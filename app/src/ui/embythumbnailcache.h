#pragma once

#include "thumbnailcache.h"

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QString>

class QNetworkAccessManager;

namespace emby {
class EmbyClient;
}

namespace ui {

// 海报缓存统计，供帮助→导出诊断信息使用（不包含 token）。
struct PosterCacheStats
{
    int requests = 0;      // 实际发出的网络请求数
    int successes = 0;     // 成功解码并写入 LRU 的次数
    int failures = 0;      // 失败次数（含重试失败）
    int retries = 0;       // 失败后的重试次数
    int inFlight = 0;      // 当前在途网络请求（快照）
    int queued = 0;        // 当前 FIFO 队列长度（快照）
    int maxConcurrent = 6; // 并发上限
    bool qnamReused = false; // 是否复用 EmbyClient 的 QNAM
    bool http2 = false;      // 图片请求是否允许 HTTP/2
    int timeoutCount = 0;    // 传输超时次数
    int exhaustedCount = 0;  // 达到重试上限的条目数
    int diskHits = 0;        // 本地磁盘缓存命中
    int diskMisses = 0;      // 本地磁盘缓存未命中
    int diskWrites = 0;      // 写入本地磁盘缓存
    int diskErrors = 0;
    qint64 diskBytes = 0;
    qint64 latencyTotalMs = 0;
    int latencySamples = 0;
    int lastHttpStatus = 0;
    QString lastError;

    QString summary() const;
};

// 真实 Emby 图片缓存：key 为 EmbyClient::imageUrl() 生成的 URL（不含 token），
// 加载时给 QNetworkRequest 加 Authorization 头。
//
// 失败处理（T20）：不把占位图写入永久 LRU；单独记录失败次数 + retryAt，
// 用临时占位图先顶上，TTL 到期后由定时器自动重试；成功后才 insert 并清除失败记录。
// 失败日志（itemId/URL/HTTP 状态/错误）走 qWarning，会进入 app.log。
class EmbyThumbnailCache : public ThumbnailCache
{
    Q_OBJECT
public:
    explicit EmbyThumbnailCache(QObject *parent = nullptr);

    void setClient(emby::EmbyClient *client);
    emby::EmbyClient *client() const { return m_client; }

    void setRetryTtl(int milliseconds);
    int retryTtl() const { return m_retryTtlMs; }

    // 每个图片请求的传输超时（默认 15000ms）；超时按失败处理，最多重试 2 次。
    void setTransferTimeout(int milliseconds);
    int transferTimeout() const { return m_transferTimeoutMs; }

    // 本地磁盘缓存（默认开启，QStandardPaths::CacheLocation/images，1GB LRU）。
    void setDiskCacheEnabled(bool enabled);
    bool diskCacheEnabled() const { return m_diskCacheEnabled; }
    void setDiskCacheDirectory(const QString &directory);
    QString diskCacheDirectory() const { return m_diskCacheDir; }
    void setDiskCacheMaxBytes(qint64 bytes);
    qint64 diskCacheMaxBytes() const { return m_diskCacheMaxBytes; }

    // 网络并发上限（默认 6）；超出进入 FIFO 队列。
    void setMaxConcurrent(int count);
    int maxConcurrent() const { return m_maxConcurrent; }
    int inFlightCount() const { return m_inFlight.size(); }
    int queuedCount() const { return m_queue.size(); }
    bool qnamReused() const { return m_qnamReused; }
    bool http2Enabled() const { return m_http2Enabled; }
    int failureCount() const { return m_failures.size(); }
    PosterCacheStats stats() const;
    void resetStats();

    QPixmap get(const QString &key, const QSize &size) override;
    void request(const QString &key, const QSize &size) override;
    void clear() override;

protected:
    void loadAsync(const QString &key, const QSize &size) override;

private:
    struct Failure
    {
        int attempts = 0;
        qint64 retryAtMs = 0;
        bool retryScheduled = false;
        bool exhausted = false;
        int lastHttpStatus = 0;
        QString lastError;
        QPixmap placeholder;
    };

    struct LoadTask
    {
        QString key;
        QSize size;
    };

    static QString itemIdFromUrl(const QString &url);
    QString diskCachePath(const QString &key, const QSize &size) const;
    bool readDiskCache(const QString &key, const QSize &size, QPixmap *pixmap);
    void writeDiskCache(const QString &key, const QSize &size, const QByteArray &data);
    void pruneDiskCache();
    static qint64 diskCacheUsage(const QString &directory);
    void enqueue(const QString &key, const QSize &size);
    void pumpQueue();
    void startNetwork(const QString &key, const QSize &size, const QString &id);
    void scheduleRetry(const QString &key, const QSize &size, const QString &id);

    emby::EmbyClient *m_client = nullptr;
    QNetworkAccessManager *m_ownedNetwork = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QHash<QString, Failure> m_failures;
    QList<LoadTask> m_queue;      // FIFO
    QSet<QString> m_queued;       // queue 去重
    QSet<QString> m_inFlight;     // 已发起网络请求
    PosterCacheStats m_stats;
    int m_retryTtlMs = 60000;
    int m_transferTimeoutMs = 15000;
    int m_maxConcurrent = 6;
    bool m_qnamReused = false;
    bool m_http2Enabled = false;
    bool m_diskCacheEnabled = true;
    QString m_diskCacheDir;
    qint64 m_diskCacheMaxBytes = qint64(1024) * 1024 * 1024;
    int m_loggedStarts = 0;
    int m_loggedFinishes = 0;
    quint64 m_generation = 0;
};

} // namespace ui
