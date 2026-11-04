#pragma once

#include "uitypes.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace emby {
class EmbyClient;
struct MediaItem;
}

namespace ui {

// 一个 Emby 媒体库视图（/Users/{userId}/Views 的一项）。
struct LibrarySection
{
    QString id;
    QString name;
    QString collectionType; // movies / tvshows / boxsets / ...
};

// UI 数据层：把 emby::MediaItem 转成 ui::MediaItem，负责 Views / Items 分页 /
// 首页 Resume+最近添加 / 详情（含季集）的请求编排。只依赖 emby 模块，不碰 Widgets。
class LibraryData : public QObject
{
    Q_OBJECT
public:
    using ViewsHandler = std::function<void(const QList<LibrarySection> &, const QString &error)>;
    using ItemsHandler = std::function<void(const QList<MediaItem> &, int totalCount, bool hasMore,
                                            const QString &error)>;
    using HomeHandler = std::function<void(const QList<MediaItem> &resume,
                                           const QList<MediaItem> &recent, const QString &error)>;
    using DetailHandler = std::function<void(const MediaItem &, const QString &error)>;

    explicit LibraryData(emby::EmbyClient *client = nullptr, QObject *parent = nullptr);

    void setClient(emby::EmbyClient *client);
    emby::EmbyClient *client() const { return m_client; }

    void fetchViews(ViewsHandler handler);
    void fetchItems(const LibrarySection &section, int startIndex, int limit, int sortMode,
                    const QString &searchTerm, ItemsHandler handler);
    void fetchHome(int resumeLimit, int recentLimit, HomeHandler handler);
    void fetchDetail(const QString &itemId, DetailHandler handler);

private:
    MediaItem toMediaItem(const emby::MediaItem &source) const;
    Episode toEpisode(const emby::MediaItem &source) const;
    QString primaryImageUrl(const emby::MediaItem &source, int maxHeight) const;
    QString backdropImageUrl(const emby::MediaItem &source) const;

    emby::EmbyClient *m_client = nullptr;
};

} // namespace ui
