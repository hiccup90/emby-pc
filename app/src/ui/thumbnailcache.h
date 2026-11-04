#pragma once

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QSize>
#include <QString>

namespace ui {

// 缩略图缓存：delegate 只做同步 get()，未命中时 request() 异步回填，
// 回填后发 thumbnailReady，让视图局部重绘——这样滚动时不会因为等待图片而卡顿。
//
// 当前 skeleton 用 postergenerator 生成离线占位图；后续接入 Emby 时，
// 继承 ThumbnailCache 并重写 generate()（内部用 QNetworkAccessManager 拉
// /Items/{id}/Images/Primary?maxWidth=...），其它逻辑不用改。
class ThumbnailCache : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailCache(QObject *parent = nullptr);
    ~ThumbnailCache() override;

    // 同步查询。命中返回图片；未命中返回空 QPixmap（调用方应继续 request）。
    QPixmap get(const QString &key, const QSize &size);

    // 请求异步加载；对同一 key+size 去重。
    void request(const QString &key, const QSize &size);

    // 网络层/生成器回填入口（线程安全要求：必须在 UI 线程调用）。
    void insert(const QString &key, const QSize &size, const QPixmap &pixmap);

    // mock 阶段给占位海报补充标题文字；真实网络图片不需要。
    void setTitleHint(const QString &key, const QString &title);

    void clear();
    void setCapacity(int entries);
    int cachedCount() const { return m_cache.size(); }

signals:
    void thumbnailReady(const QString &key, const QSize &size);

protected:
    // 默认实现：调用 postergenerator 生成占位海报/背景图。
    // 子类可重写为真正的网络加载（加载完成后调用 insert）。
    virtual void loadAsync(const QString &key, const QSize &size);

    // 占位图生成入口（EmbyThumbnailCache 加载失败时复用）。
    virtual QPixmap generate(const QString &key, const QSize &size);

private:
    static QString cacheKey(const QString &key, const QSize &size);
    void evictIfNeeded();

    struct Entry
    {
        QPixmap pixmap;
        quint64 lastUsed = 0;
    };

    QHash<QString, Entry> m_cache;
    QHash<QString, QString> m_titleHints;
    QSet<QString> m_pending;
    quint64 m_tick = 0;
    int m_capacity = 320;
};

} // namespace ui