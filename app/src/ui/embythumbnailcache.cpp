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
    QString text = QStringLiteral("poster cache: requests=%1 success=%2 failed=%3 retries=%4")
                       .arg(requests)
                       .arg(successes)
                       .arg(failures)
                       .arg(retries);
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
    m_failures.clear();
    resetStats();
}

void EmbyThumbnailCache::setRetryTtl(int milliseconds)
{
    m_retryTtlMs = qMax(0, milliseconds);
}

PosterCacheStats EmbyThumbnailCache::stats() const
{
    return m_stats;
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

    // 失败 TTL 内抑制重复请求；TTL 到期后由定时器主动重试，避免 paint 风暴。
    const auto it = m_failures.constFind(cacheKey(key, size));
    if (it != m_failures.constEnd()
        && QDateTime::currentMSecsSinceEpoch() < it->retryAtMs) {
        return;
    }

    ThumbnailCache::request(key, size);
}

void EmbyThumbnailCache::clear()
{
    ++m_generation;
    ThumbnailCache::clear();
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
        if (!m_client)
            return;
        // 直接走基类 request，绕过本类的 TTL 抑制；loadAsync 仍由虚函数分派。
        ThumbnailCache::request(key, size);
    });
}

void EmbyThumbnailCache::loadAsync(const QString &key, const QSize &size)
{
    if (!m_client || !key.startsWith(QLatin1String("http"))) {
        ThumbnailCache::loadAsync(key, size);
        return;
    }

    QNetworkRequest request{QUrl(key)};
    request.setRawHeader("Authorization", m_client->authorizationHeader().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    ++m_stats.requests;
    if (m_failures.contains(cacheKey(key, size)))
        ++m_stats.retries;
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

        // 登录切换/退出后清过缓存：旧响应直接丢弃，避免串号。
        if (generation != m_generation)
            return;

        const QString id = cacheKey(key, size);
        QPixmap pixmap;
        if (error == QNetworkReply::NoError && pixmap.loadFromData(data)) {
            const bool recovered = m_failures.remove(id) > 0;
            insert(key, size, pixmap);
            ++m_stats.successes;
            m_stats.lastHttpStatus = httpStatus;
            m_stats.lastError.clear();
            if (recovered)
                qInfo().noquote() << QStringLiteral("poster recovered url=%1").arg(key);
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
        qInfo().noquote() << QStringLiteral("poster stats: %1").arg(m_stats.summary());

        scheduleRetry(key, size, id);
    });
}

} // namespace ui
