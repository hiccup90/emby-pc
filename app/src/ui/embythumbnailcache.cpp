#include "embythumbnailcache.h"

#include "embyclient.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QDebug>

namespace ui {

QString PosterCacheStats::summary() const
{
    QString text = QStringLiteral("poster cache: requests=%1 success=%2 failed=%3 retries=%4 "
                                  "in_flight=%5 queued=%6 max=%7")
                       .arg(requests)
                       .arg(successes)
                       .arg(failures)
                       .arg(retries)
                       .arg(inFlight)
                       .arg(queued)
                       .arg(maxConcurrent);
    if (lastHttpStatus != 0)
        text += QStringLiteral(" last_http=%1").arg(lastHttpStatus);
    if (!lastError.isEmpty())
        text += QStringLiteral(" last_error=%1").arg(lastError);
    return text;
}

EmbyThumbnailCache::EmbyThumbnailCache(QObject *parent)
    : ThumbnailCache(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void EmbyThumbnailCache::setClient(emby::EmbyClient *client)
{
    if (m_client == client)
        return;
    m_client = client;
    ++m_generation;
    m_queue.clear();
    m_queued.clear();
    m_inFlight.clear();
    m_failures.clear();
    resetStats();
}

void EmbyThumbnailCache::setRetryTtl(int milliseconds)
{
    m_retryTtlMs = qMax(0, milliseconds);
}

void EmbyThumbnailCache::setMaxConcurrent(int count)
{
    m_maxConcurrent = qBound(1, count, 64);
    pumpQueue();
}

PosterCacheStats EmbyThumbnailCache::stats() const
{
    PosterCacheStats result = m_stats;
    result.inFlight = m_inFlight.size();
    result.queued = m_queue.size();
    result.maxConcurrent = m_maxConcurrent;
    return result;
}

void EmbyThumbnailCache::resetStats()
{
    m_stats = PosterCacheStats();
}

QPixmap EmbyThumbnailCache::get(const QString &key, const QSize &size)
{
    QPixmap cached = ThumbnailCache::get(key, size);
    if (!cached.isNull())
        return cached;

    const auto it = m_failures.constFind(cacheKey(key, size));
    if (it != m_failures.constEnd()) {
        // TTL 内先显示临时占位；TTL 到期后返回空，让 delegate 的 request()
        // 触发“下次可见时重试”（定时器也会兜底重试）。
        if (QDateTime::currentMSecsSinceEpoch() < it->retryAtMs)
            return it->placeholder;
        return {};
    }
    return {};
}

void EmbyThumbnailCache::request(const QString &key, const QSize &size)
{
    if (!key.startsWith(QLatin1String("http"))) {
        ThumbnailCache::request(key, size);
        return;
    }
    if (key.isEmpty() || size.isEmpty())
        return;

    const QString id = cacheKey(key, size);
    // 已发起/已排队：合并重复请求（这是 paint 风暴的关键修复）。
    if (m_inFlight.contains(id) || m_queued.contains(id))
        return;

    const auto failure = m_failures.constFind(id);
    if (failure != m_failures.constEnd()) {
        // 失败 TTL 内抑制重复请求；TTL 到期后由定时器/重绘触发重试。
        if (QDateTime::currentMSecsSinceEpoch() < failure->retryAtMs)
            return;
    } else if (!ThumbnailCache::get(key, size).isNull()) {
        return; // 已有真实缓存
    }

    enqueue(key, size);
}

void EmbyThumbnailCache::enqueue(const QString &key, const QSize &size)
{
    const QString id = cacheKey(key, size);
    if (m_inFlight.contains(id) || m_queued.contains(id))
        return;
    m_queued.insert(id);
    m_queue.append(LoadTask{key, size});
    pumpQueue();
}

void EmbyThumbnailCache::pumpQueue()
{
    if (!m_client)
        return;
    while (m_inFlight.size() < m_maxConcurrent && !m_queue.isEmpty()) {
        const LoadTask task = m_queue.takeFirst();
        const QString id = cacheKey(task.key, task.size);
        m_queued.remove(id);
        if (m_inFlight.contains(id))
            continue;
        m_inFlight.insert(id);
        startNetwork(task.key, task.size, id);
    }
}

void EmbyThumbnailCache::clear()
{
    ++m_generation;
    ThumbnailCache::clear();
    m_queue.clear();
    m_queued.clear();
    m_inFlight.clear();
    m_failures.clear();
    resetStats();
}

QString EmbyThumbnailCache::itemIdFromUrl(const QString &url)
{
    const QUrl parsed(url);
    const QStringList segments = parsed.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const int index = segments.indexOf(QStringLiteral("Items"));
    if (index >= 0 && index + 1 < segments.size())
        return QUrl::fromPercentEncoding(segments.at(index + 1).toUtf8());
    return {};
}

void EmbyThumbnailCache::scheduleRetry(const QString &key, const QSize &size,
                                       const QString &id)
{
    auto it = m_failures.find(id);
    if (it == m_failures.end() || it->retryScheduled)
        return;
    it->retryScheduled = true;

    const int delay = m_retryTtlMs;
    QTimer::singleShot(delay, this, [this, key, size, id]() {
        auto failure = m_failures.find(id);
        if (failure == m_failures.end())
            return;
        failure->retryScheduled = false;
        failure->retryAtMs = 0;
        if (!m_client)
            return;
        // 走本类 request()，统一经过 in-flight/队列去重与并发上限。
        request(key, size);
    });
}

void EmbyThumbnailCache::startNetwork(const QString &key, const QSize &size,
                                      const QString &id)
{
    if (!m_client || !key.startsWith(QLatin1String("http"))) {
        m_inFlight.remove(id);
        pumpQueue();
        return;
    }

    if (m_failures.contains(id))
        ++m_stats.retries;
    // requests 只统计真正发起的网络请求，不统计 enqueue/paint 调用。
    ++m_stats.requests;

    QNetworkRequest request{QUrl(key)};
    request.setRawHeader("Authorization", m_client->authorizationHeader().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    const quint64 generation = m_generation;
    QNetworkReply *reply = m_network->get(request);
    if (m_client->profile().ignoreSslErrors) {
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
            reply->ignoreSslErrors();
        });
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, size, generation]() {
        const QByteArray data = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString errorText = reply->errorString();
        reply->deleteLater();

        const QString id = cacheKey(key, size);

        // 登录切换/退出后清过缓存：旧响应直接丢弃，避免串号（也不能误删
        // 新代际中同 key 的 in-flight 标记）。
        if (generation != m_generation) {
            pumpQueue();
            return;
        }
        m_inFlight.remove(id);

        QPixmap pixmap;
        if (error == QNetworkReply::NoError && pixmap.loadFromData(data)) {
            const bool recovered = m_failures.remove(id) > 0;
            insert(key, size, pixmap);
            ++m_stats.successes;
            m_stats.lastHttpStatus = httpStatus;
            m_stats.lastError.clear();
            if (recovered)
                qInfo().noquote() << QStringLiteral("poster recovered url=%1").arg(key);
            pumpQueue();
            return;
        }

        Failure failure = m_failures.value(id);
        ++failure.attempts;
        failure.lastHttpStatus = httpStatus;
        failure.lastError = errorText;
        failure.retryAtMs = QDateTime::currentMSecsSinceEpoch() + m_retryTtlMs;
        failure.placeholder = generate(key, size);
        m_failures.insert(id, failure);

        ++m_stats.failures;
        m_stats.lastHttpStatus = httpStatus;
        m_stats.lastError = errorText;

        // app.log：记录 itemId / 去掉 token 的 URL / HTTP 状态 / 错误串。
        qWarning().noquote()
            << QStringLiteral("poster load failed item=%1 url=%2 status=%3 error=%4 ttl_ms=%5")
                   .arg(itemIdFromUrl(key), key)
                   .arg(httpStatus)
                   .arg(errorText)
                   .arg(m_retryTtlMs);
        qInfo().noquote() << QStringLiteral("poster stats: %1").arg(stats().summary());

        scheduleRetry(key, size, id);
        pumpQueue();
    });
}

void EmbyThumbnailCache::loadAsync(const QString &key, const QSize &size)
{
    if (!m_client || !key.startsWith(QLatin1String("http"))) {
        ThumbnailCache::loadAsync(key, size);
        return;
    }
    // 兼容父类 request() 的调用路径：统一进入并发队列。
    enqueue(key, size);
}

} // namespace ui
