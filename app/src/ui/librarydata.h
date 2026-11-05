#pragma once

#include "uitypes.h"

#include <QObject>
#include <QSet>
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

// 海报来源统计（T20）：区分客户端回退链与源数据缺图。
struct PosterSourceStats
{
    int items = 0;
    int ownPrimary = 0;
    int seriesPrimary = 0;
    int thumb = 0;
    int backdrop = 0;
    int noImage = 0;
    int missingPrimary = 0;

    QString summary() const;
};

// 首页分栏数据（T37）。HomeView 只消费有序的 HomeSection 列表，
// 顺序/隐藏库/刷新缓存由数据层按 QSettings 解析。
struct HomeQuery
{
    int resumeLimit = 12;
    int recentLimit = 24;
    int perLibraryLimit = 18;
    // 非空时覆盖 QSettings.hiddenLibraries；实际会与 home/sidebar 隐藏集合合并。
    QStringList hiddenLibraryIds;
    // 非空时覆盖 QSettings.home/sectionOrder；token: resume / recent / lib:<viewId>。
    QStringList sectionOrder;
};

struct HomeSection
{
    enum class Kind { Resume, Recent, Library };
    Kind kind = Kind::Resume;
    QString libraryId;
    QString title;
    QList<MediaItem> items;
    int totalCount = 0;
    bool hasMore = false;
    QString error;
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
    using HomeSectionsHandler = std::function<void(const QList<HomeSection> &, const QString &error)>;
    using DetailHandler = std::function<void(const MediaItem &, const QString &error)>;

    explicit LibraryData(emby::EmbyClient *client = nullptr, QObject *parent = nullptr);

    void setClient(emby::EmbyClient *client);
    emby::EmbyClient *client() const { return m_client; }

    void fetchViews(ViewsHandler handler);
    void fetchItems(const LibrarySection &section, int startIndex, int limit, int sortMode,
                    const QString &searchTerm, ItemsHandler handler);
    void fetchHome(int resumeLimit, int recentLimit, HomeHandler handler);
    void fetchHomeSections(const HomeQuery &query, HomeSectionsHandler handler);
    // 登录/退出/设置变更时调用；60 秒缓存用。
    void clearHomeSectionsCache();
    void fetchDetail(const QString &itemId, DetailHandler handler);

    PosterSourceStats posterStats() const { return m_posterStats; }
    void resetPosterStats();
    QString posterStatsSummary() const;

private:
    // kind: 0=own primary, 1=series primary, 2=thumb, 3=backdrop, 4=none
    MediaItem toMediaItem(const emby::MediaItem &source) const;
    Episode toEpisode(const emby::MediaItem &source) const;
    QString posterImageUrl(const emby::MediaItem &source, int maxHeight, int *kind) const;
    QString backdropImageUrl(const emby::MediaItem &source) const;
    void recordPosterSource(const emby::MediaItem &source, int kind) const;

    emby::EmbyClient *m_client = nullptr;
    mutable PosterSourceStats m_posterStats;
    QString m_homeCacheKey;
    qint64 m_homeCacheMs = 0;
    QList<HomeSection> m_homeCacheSections;
    mutable QSet<QString> m_posterCounted;
};

} // namespace ui
