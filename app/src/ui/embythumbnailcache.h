#pragma once

#include "thumbnailcache.h"

#include <QHash>
#include <QPixmap>
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
        int lastHttpStatus = 0;
        QString lastError;
        QPixmap placeholder;
    };

    static QString itemIdFromUrl(const QString &url);
    void scheduleRetry(const QString &key, const QSize &size, const QString &id);

    emby::EmbyClient *m_client = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QHash<QString, Failure> m_failures;
    PosterCacheStats m_stats;
    int m_retryTtlMs = 60000;
    quint64 m_generation = 0;
};

} // namespace ui
