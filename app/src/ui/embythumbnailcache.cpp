#include "embythumbnailcache.h"

#include "embyclient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslError>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

namespace ui {
namespace {

// app.log / 诊断里的错误串可能包含完整 URL，必须去掉 api_key/token。
QString redactSensitive(QString text, const QString &token)
{
    if (!token.isEmpty())
        text.replace(token, QStringLiteral("***"));
    static const QRegularExpression apiKeyPattern(QStringLiteral("(api_key=)[^&\\s]+"));
    text.replace(apiKeyPattern, QStringLiteral("\\1***"));
    return text;
}

} // namespace

QString PosterCacheStats::summary() const
{
    const qint64 averageMs = latencySamples > 0 ? latencyTotalMs / latencySamples : 0;
    QString text = QStringLiteral("poster cache: requests=%1 success=%2 failed=%3 retries=%4 "
                                  "in_flight=%5 queued=%6 max=%7 qnam_reused=%8 http2=%9 "
                                  "timeouts=%10 avg_ms=%11 exhausted=%12 "
                                  "disk_hits=%13 disk_misses=%14 disk_writes=%15 "
                                  "disk_errors=%16 disk_bytes=%17")
                       .arg(requests)
                       .arg(successes)
                       .arg(failures)
                       .arg(retries)
                       .arg(inFlight)
                       .arg(queued)
                       .arg(maxConcurrent)
                       .arg(qnamReused ? 1 : 0)
                       .arg(http2 ? 1 : 0)
                       .arg(timeoutCount)
                       .arg(averageMs)
                       .arg(exhaustedCount)
                       .arg(diskHits)
                       .arg(diskMisses)
                       .arg(diskWrites)
                       .arg(diskErrors)
                       .arg(diskBytes);
    if (lastHttpStatus != 0)
        text += QStringLiteral(" last_http=%1").arg(lastHttpStatus);
    if (!lastError.isEmpty())
        text += QStringLiteral(" last_error=%1").arg(lastError);
    return text;
}

EmbyThumbnailCache::EmbyThumbnailCache(QObject *parent)
    : ThumbnailCache(parent)
    , m_ownedNetwork(new QNetworkAccessManager(this))
    , m_network(m_ownedNetwork)
{
    // 默认关闭 HTTP/2（EMBY_PC_IMAGE_HTTP2=1 时才允许），用于对比 Caddy/Emby 反代。
    m_http2Enabled = qEnvironmentVariableIntValue("EMBY_PC_IMAGE_HTTP2") != 0;

    // 默认开启本地图片缓存；EMBY_PC_IMAGE_DISK_CACHE=0 可关闭。
    m_diskCacheEnabled =
        qEnvironmentVariable("EMBY_PC_IMAGE_DISK_CACHE") != QLatin1String("0");
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        m_diskCacheEnabled = false;
    } else {
        m_diskCacheDir = cacheRoot + QStringLiteral("/images");
    }
    qInfo().noquote() << QStringLiteral("poster disk cache: enabled=%1 dir=%2")
                             .arg(m_diskCacheEnabled ? 1 : 0)
                             .arg(m_diskCacheDir);
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
    m_loggedStarts = 0;
    m_loggedFinishes = 0;

    m_network = (m_client && m_client->networkManager()) ? m_client->networkManager()
                                                         : m_ownedNetwork;
    m_qnamReused = m_network != m_ownedNetwork;
    // 便于把实机日志与 QNAM/HTTP2 配置对应起来。
    qInfo().noquote()
        << QStringLiteral("poster qnam: reused=%1 http2=%2 timeout_ms=%3 max_retries=2")
               .arg(m_qnamReused ? 1 : 0)
               .arg(m_http2Enabled ? 1 : 0)
               .arg(m_transferTimeoutMs);
}

void EmbyThumbnailCache::setRetryTtl(int milliseconds)
{
    m_retryTtlMs = qMax(0, milliseconds);
}

void EmbyThumbnailCache::setTransferTimeout(int milliseconds)
{
    m_transferTimeoutMs = qMax(0, milliseconds);
}

void EmbyThumbnailCache::setDiskCacheEnabled(bool enabled)
{
    m_diskCacheEnabled = enabled;
}

void EmbyThumbnailCache::setDiskCacheDirectory(const QString &directory)
{
    m_diskCacheDir = directory;
    if (m_diskCacheDir.isEmpty())
        m_diskCacheEnabled = false;
}

void EmbyThumbnailCache::setDiskCacheMaxBytes(qint64 bytes)
{
    m_diskCacheMaxBytes = qMax(qint64(0), bytes);
    pruneDiskCache();
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
    result.qnamReused = m_qnamReused;
    result.http2 = m_http2Enabled;
    result.diskBytes = diskCacheUsage(m_diskCacheDir);
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
        // 重试上限耗尽后长期占位，避免 delegate 反复请求。
        if (it->exhausted)
            return it->placeholder;
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
        if (failure->exhausted)
            return;
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
    m_loggedStarts = 0;
    m_loggedFinishes = 0;
}

QString EmbyThumbnailCache::diskCachePath(const QString &key, const QSize &size) const
{
    if (!m_diskCacheEnabled || m_diskCacheDir.isEmpty())
        return {};
    const QByteArray stableKey =
        (key + QLatin1Char('|') + QString::number(size.width()) + QLatin1Char('x')
         + QString::number(size.height()))
            .toUtf8();
    const QByteArray digest = QCryptographicHash::hash(stableKey, QCryptographicHash::Sha1).toHex();
    return m_diskCacheDir + QLatin1Char('/') + QString::fromLatin1(digest)
           + QStringLiteral(".img");
}

bool EmbyThumbnailCache::readDiskCache(const QString &key, const QSize &size,
                                       QPixmap *pixmap)
{
    if (!m_diskCacheEnabled || !pixmap)
        return false;
    const QString path = diskCachePath(key, size);
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.exists()) {
        ++m_stats.diskMisses;
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        ++m_stats.diskErrors;
        ++m_stats.diskMisses;
        return false;
    }
    const QByteArray data = file.readAll();
    // 读命中刷新 mtime，让 1GB 淘汰按最近访问而不是最近写入。
    file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    if (!pixmap->loadFromData(data)) {
        ++m_stats.diskErrors;
        ++m_stats.diskMisses;
        QFile::remove(path);
        return false;
    }
    ++m_stats.diskHits;
    return true;
}

qint64 EmbyThumbnailCache::diskCacheUsage(const QString &directory)
{
    if (directory.isEmpty())
        return 0;
    QDir dir(directory);
    if (!dir.exists())
        return 0;
    qint64 total = 0;
    const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::NoSort);
    for (const QFileInfo &file : files)
        total += file.size();
    return total;
}

void EmbyThumbnailCache::pruneDiskCache()
{
    if (!m_diskCacheEnabled || m_diskCacheDir.isEmpty())
        return;
    QDir dir(m_diskCacheDir);
    if (!dir.exists())
        return;

    qint64 usage = diskCacheUsage(m_diskCacheDir);
    if (usage <= m_diskCacheMaxBytes) {
        m_stats.diskBytes = usage;
        return;
    }

    // 最旧文件优先删除（QDir::Time 新→旧，反向遍历）。
    const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Time);
    for (int i = files.size() - 1; i >= 0 && usage > m_diskCacheMaxBytes; --i) {
        const QFileInfo &file = files.at(i);
        const qint64 fileSize = file.size();
        if (QFile::remove(file.absoluteFilePath())) {
            usage -= fileSize;
        } else {
            ++m_stats.diskErrors;
        }
    }
    m_stats.diskBytes = qMax(qint64(0), usage);
}

void EmbyThumbnailCache::writeDiskCache(const QString &key, const QSize &size,
                                        const QByteArray &data)
{
    if (!m_diskCacheEnabled || data.isEmpty())
        return;
    const QString path = diskCachePath(key, size);
    if (path.isEmpty())
        return;

    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        ++m_stats.diskErrors;
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ++m_stats.diskErrors;
        return;
    }
    if (file.write(data) != data.size()) {
        ++m_stats.diskErrors;
        file.close();
        QFile::remove(path);
        return;
    }
    file.close();
    ++m_stats.diskWrites;
    pruneDiskCache();
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
    if (it == m_failures.end() || it->retryScheduled || it->exhausted)
        return;
    if (it->attempts > 2) {
        // 初次 + 最多 2 次重试（共 3 次网络尝试），之后不再打服务器。
        it->exhausted = true;
        ++m_stats.exhaustedCount;
        qWarning().noquote()
            << QStringLiteral("poster retry exhausted item=%1 attempts=%2")
                   .arg(itemIdFromUrl(key))
                   .arg(it->attempts);
        return;
    }
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

    // 先读本地磁盘缓存（跨启动复用），命中则不发网络请求。
    QPixmap diskCached;
    if (readDiskCache(key, size, &diskCached)) {
        m_inFlight.remove(id);
        insert(key, size, diskCached);
        pumpQueue();
        return;
    }

    if (m_failures.contains(id))
        ++m_stats.retries;
    // requests 只统计真正发起的网络请求，不统计 enqueue/paint 调用。
    ++m_stats.requests;

    // qEmby/Jellyfin 兼容：图片 URL 带 api_key（token）查询参数；Authorization
    // 头保留兼容。日志只记录 token-free 的 key，绝不把 api_key 写进 app.log。
    QUrl requestUrl(key);
    QUrlQuery requestQuery(requestUrl);
    requestQuery.removeAllQueryItems(QStringLiteral("api_key"));
    if (!m_client->profile().accessToken.isEmpty())
        requestQuery.addQueryItem(QStringLiteral("api_key"), m_client->profile().accessToken);
    requestUrl.setQuery(requestQuery);

    QNetworkRequest request{requestUrl};
    request.setRawHeader("Authorization", m_client->authorizationHeader().toUtf8());
    if (m_transferTimeoutMs > 0)
        request.setTransferTimeout(m_transferTimeoutMs);
    // 默认关闭 HTTP/2，便于对比 Caddy/Emby 反代上的连接复用问题。
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, m_http2Enabled);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    const QString itemId = itemIdFromUrl(key);
    const QString accessToken = m_client->profile().accessToken;
    if (m_loggedStarts < 20) {
        qInfo().noquote()
            << QStringLiteral("poster start item=%1 size=%2x%3 in_flight=%4 queued=%5 "
                              "qnam_reused=%6 http2=%7")
                   .arg(itemId)
                   .arg(size.width())
                   .arg(size.height())
                   .arg(m_inFlight.size())
                   .arg(m_queue.size())
                   .arg(m_qnamReused ? 1 : 0)
                   .arg(m_http2Enabled ? 1 : 0);
        ++m_loggedStarts;
    }

    QElapsedTimer timer;
    timer.start();
    const quint64 generation = m_generation;
    QNetworkReply *reply = m_network->get(request);
    if (m_client->profile().ignoreSslErrors) {
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
            reply->ignoreSslErrors();
        });
    }
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, key, size, generation, timer, accessToken]() {
        const QNetworkReply::NetworkError error = reply->error();
        const QByteArray data = error == QNetworkReply::NoError ? reply->readAll()
                                                                : QByteArray();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString errorText = redactSensitive(reply->errorString(), accessToken);
        const qint64 elapsedMs = timer.elapsed();
        reply->deleteLater();

        const QString id = cacheKey(key, size);

        // 登录切换/退出后清过缓存：旧响应直接丢弃，避免串号（也不能误删
        // 新代际中同 key 的 in-flight 标记）。
        if (generation != m_generation) {
            pumpQueue();
            return;
        }
        m_inFlight.remove(id);

        m_stats.latencyTotalMs += elapsedMs;
        ++m_stats.latencySamples;

        QPixmap pixmap;
        if (error == QNetworkReply::NoError && pixmap.loadFromData(data)) {
            const bool recovered = m_failures.remove(id) > 0;
            writeDiskCache(key, size, data);
            insert(key, size, pixmap);
            ++m_stats.successes;
            m_stats.lastHttpStatus = httpStatus;
            m_stats.lastError.clear();
            if (m_loggedFinishes < 20) {
                qInfo().noquote()
                    << QStringLiteral("poster finish item=%1 status=%2 bytes=%3 elapsed_ms=%4 "
                                      "result=ok")
                           .arg(itemIdFromUrl(key))
                           .arg(httpStatus)
                           .arg(data.size())
                           .arg(elapsedMs);
                ++m_loggedFinishes;
            }
            if (recovered)
                qInfo().noquote() << QStringLiteral("poster recovered item=%1 elapsed_ms=%2")
                                         .arg(itemIdFromUrl(key))
                                         .arg(elapsedMs);
            pumpQueue();
            return;
        }

        // Qt 的 transfer timeout 在部分 QNAM 后端报 OperationCanceledError；
        // 配置了传输超时时把该错误按 timeout 统计。
        const bool timedOut = error == QNetworkReply::TimeoutError
                              || (m_transferTimeoutMs > 0
                                  && error == QNetworkReply::OperationCanceledError);
        if (timedOut)
            ++m_stats.timeoutCount;

        Failure failure = m_failures.value(id);
        ++failure.attempts;
        failure.retryAtMs = QDateTime::currentMSecsSinceEpoch() + m_retryTtlMs;
        failure.lastHttpStatus = httpStatus;
        failure.lastError = errorText;
        failure.placeholder = generate(key, size);
        m_failures.insert(id, failure);

        ++m_stats.failures;
        m_stats.lastHttpStatus = httpStatus;
        m_stats.lastError = errorText;

        // 失败/超时全部记录；成功只记录前 20 条，避免日志洪水。
        qWarning().noquote()
            << QStringLiteral("poster finish item=%1 status=%2 bytes=%3 elapsed_ms=%4 "
                              "result=%5 timeout=%6 error=%7 url=%8")
                   .arg(itemIdFromUrl(key))
                   .arg(httpStatus)
                   .arg(data.size())
                   .arg(elapsedMs)
                   .arg(timedOut ? QStringLiteral("timeout") : QStringLiteral("error"))
                   .arg(timedOut ? 1 : 0)
                   .arg(errorText, key);
        if (timedOut)
            qWarning().noquote()
                << QStringLiteral("poster timeout item=%1 elapsed_ms=%2 timeout_ms=%3")
                       .arg(itemIdFromUrl(key))
                       .arg(elapsedMs)
                       .arg(m_transferTimeoutMs);
        if (m_loggedFinishes < 20)
            ++m_loggedFinishes;

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
