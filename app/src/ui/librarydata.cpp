#include "librarydata.h"

#include "authutils.h"
#include "embyclient.h"

#include <QDebug>
#include <QSharedPointer>

#include <algorithm>

namespace ui {

QString PosterSourceStats::summary() const
{
    return QStringLiteral("poster sources: items=%1 own_primary=%2 missing_primary=%3 "
                          "series_fallback=%4 thumb=%5 backdrop=%6 no_image=%7")
        .arg(items)
        .arg(ownPrimary)
        .arg(missingPrimary)
        .arg(seriesPrimary)
        .arg(thumb)
        .arg(backdrop)
        .arg(noImage);
}

namespace {

QStringList listFields()
{
    return {
        QStringLiteral("ProductionYear"), QStringLiteral("PrimaryImageAspectRatio"),
        QStringLiteral("DateCreated"), QStringLiteral("PremiereDate"),
        QStringLiteral("OfficialRating"), QStringLiteral("CommunityRating"),
        QStringLiteral("Genres"), QStringLiteral("RunTimeTicks"), QStringLiteral("Overview"),
        QStringLiteral("ImageTags"), QStringLiteral("BackdropImageTags"), QStringLiteral("UserData"),
    };
}

QStringList itemTypesForCollection(const QString &collectionType)
{
    if (collectionType == QLatin1String("movies"))
        return {QStringLiteral("Movie")};
    if (collectionType == QLatin1String("tvshows"))
        return {QStringLiteral("Series")};
    if (collectionType == QLatin1String("boxsets"))
        return {QStringLiteral("BoxSet")};
    if (collectionType == QLatin1String("music"))
        return {QStringLiteral("MusicAlbum")};
    return {QStringLiteral("Movie"), QStringLiteral("Series"), QStringLiteral("BoxSet")};
}

int collectionRank(const QString &collectionType)
{
    if (collectionType == QLatin1String("movies"))
        return 0;
    if (collectionType == QLatin1String("tvshows"))
        return 1;
    if (collectionType == QLatin1String("boxsets"))
        return 2;
    return 3;
}

int runtimeMinutesFromTicks(qint64 ticks)
{
    if (ticks <= 0)
        return 0;
    return qMax(1, int(ticks / 600000000LL));
}

} // namespace

LibraryData::LibraryData(emby::EmbyClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
}

void LibraryData::setClient(emby::EmbyClient *client)
{
    m_client = client;
}

QString LibraryData::posterImageUrl(const emby::MediaItem &source, int maxHeight,
                                 int *kind) const
{
    if (kind)
        *kind = 4;
    if (!m_client)
        return {};

    auto imageUrl = [this, maxHeight](const QString &itemId, const QString &imageType,
                                      const QString &tag) {
        if (itemId.isEmpty())
            return QString();
        return m_client->imageUrl(itemId, imageType, tag, 0, maxHeight).toString();
    };

    // 1) 自身 Primary
    if (!source.primaryImageTag.isEmpty()) {
        const QString url = imageUrl(source.primaryImageItemIdValue(),
                                     QStringLiteral("Primary"), source.primaryImageTag);
        if (!url.isEmpty()) {
            if (kind)
                *kind = 0;
            return url;
        }
    }

    // 2) 剧集/季使用 Series（或 Parent）的 Primary；Emby 有时不给 tag，仍尝试无 tag URL。
    QString seriesId = source.seriesId;
    if (seriesId.isEmpty() && source.type == QLatin1String("Season"))
        seriesId = source.parentId;
    if (!seriesId.isEmpty()) {
        const QString url = imageUrl(seriesId, QStringLiteral("Primary"),
                                     source.seriesPrimaryImageTag);
        if (!url.isEmpty()) {
            if (kind)
                *kind = 1;
            return url;
        }
    }

    // 3) 自身 Thumb（BoxSet 常见）
    if (!source.thumbImageTag.isEmpty()) {
        const QString url = imageUrl(source.id, QStringLiteral("Thumb"), source.thumbImageTag);
        if (!url.isEmpty()) {
            if (kind)
                *kind = 2;
            return url;
        }
    }

    // 4) 自身/父级 Backdrop
    if (!source.backdropImageTags.isEmpty()) {
        const QString url = imageUrl(source.id, QStringLiteral("Backdrop"),
                                     source.backdropImageTags.first());
        if (!url.isEmpty()) {
            if (kind)
                *kind = 3;
            return url;
        }
    }
    if (!source.parentBackdropImageTags.isEmpty()) {
        const QString backdropId = source.parentBackdropItemId.isEmpty() ? seriesId
                                                                        : source.parentBackdropItemId;
        const QString url = imageUrl(backdropId, QStringLiteral("Backdrop"),
                                     source.parentBackdropImageTags.first());
        if (!url.isEmpty()) {
            if (kind)
                *kind = 3;
            return url;
        }
    }

    return {};
}

void LibraryData::recordPosterSource(const emby::MediaItem &source, int kind) const
{
    if (source.id.isEmpty() || m_posterCounted.contains(source.id))
        return;
    m_posterCounted.insert(source.id);
    ++m_posterStats.items;
    if (kind != 0)
        ++m_posterStats.missingPrimary;
    switch (kind) {
    case 0:
        ++m_posterStats.ownPrimary;
        break;
    case 1:
        ++m_posterStats.seriesPrimary;
        break;
    case 2:
        ++m_posterStats.thumb;
        break;
    case 3:
        ++m_posterStats.backdrop;
        break;
    default:
        ++m_posterStats.noImage;
        break;
    }
}

void LibraryData::resetPosterStats()
{
    m_posterStats = PosterSourceStats();
    m_posterCounted.clear();
}

QString LibraryData::posterStatsSummary() const
{
    return m_posterStats.summary();
}

QString LibraryData::backdropImageUrl(const emby::MediaItem &source) const
{
    if (!m_client)
        return {};
    QString id = source.id;
    QString tag;
    if (!source.backdropImageTags.isEmpty()) {
        tag = source.backdropImageTags.first();
    } else if (!source.parentBackdropImageTags.isEmpty()) {
        tag = source.parentBackdropImageTags.first();
        id = source.parentBackdropItemId.isEmpty() ? source.seriesId : source.parentBackdropItemId;
    }
    if (tag.isEmpty() || id.isEmpty())
        return {};
    return m_client->imageUrl(id, QStringLiteral("Backdrop"), tag, 1400, 0).toString();
}

MediaItem LibraryData::toMediaItem(const emby::MediaItem &source) const
{
    MediaItem item;
    item.id = source.id;
    item.type = source.type;
    item.title = source.name;
    item.originalTitle = source.originalTitle;
    item.year = source.productionYear;
    item.officialRating = source.officialRating;
    item.genres = source.genres;
    item.overview = source.overview;
    item.communityRating = source.communityRating;
    item.runtimeMinutes = runtimeMinutesFromTicks(source.runTimeTicks);
    int posterKind = 4;
    item.posterKey = posterImageUrl(source, 320, &posterKind);
    recordPosterSource(source, posterKind);
    item.backdropKey = backdropImageUrl(source);
    item.played = source.userData.played;
    double playedPercentage = source.userData.playedPercentage;
    // 某些 Emby Resume 响应只给 PlaybackPositionTicks、不给 PlayedPercentage；
    // 用 RunTimeTicks 回算，避免继续观看卡片进度为 0。
    if (playedPercentage <= 0.0 && source.userData.playbackPositionTicks > 0
        && source.runTimeTicks > 0) {
        playedPercentage = 100.0 * static_cast<double>(source.userData.playbackPositionTicks)
                           / static_cast<double>(source.runTimeTicks);
    }
    item.progress = qBound(0.0, playedPercentage / 100.0, 1.0);
    if (item.played && item.progress <= 0.0)
        item.progress = 1.0;
    return item;
}

Episode LibraryData::toEpisode(const emby::MediaItem &source) const
{
    Episode episode;
    episode.number = source.indexNumber;
    episode.title = source.name;
    episode.overview = source.overview;
    episode.runtimeMinutes = runtimeMinutesFromTicks(source.runTimeTicks);
    episode.played = source.userData.played;
    double playedPercentage = source.userData.playedPercentage;
    if (playedPercentage <= 0.0 && source.userData.playbackPositionTicks > 0
        && source.runTimeTicks > 0) {
        playedPercentage = 100.0 * static_cast<double>(source.userData.playbackPositionTicks)
                           / static_cast<double>(source.runTimeTicks);
    }
    episode.progress = qBound(0.0, playedPercentage / 100.0, 1.0);
    if (episode.played && episode.progress <= 0.0)
        episode.progress = 1.0;
    int posterKind = 4;
    episode.imageKey = posterImageUrl(source, 200, &posterKind);
    recordPosterSource(source, posterKind);
    return episode;
}

void LibraryData::fetchViews(ViewsHandler handler)
{
    if (!handler)
        return;
    if (!m_client) {
        handler({}, QStringLiteral("未登录，无法加载媒体库。"));
        return;
    }
    m_client->fetchViews([this, handler](const emby::ItemQueryResult &result,
                                         const emby::ApiError &error) {
        if (error.isError()) {
            handler({}, friendlyApiError(error));
            return;
        }
        QList<LibrarySection> sections;
        for (const emby::MediaItem &source : result.items) {
            if (source.type != QLatin1String("CollectionFolder")
                && source.type != QLatin1String("UserView"))
                continue;
            LibrarySection section;
            section.id = source.id;
            section.name = source.name;
            section.collectionType = source.collectionType;
            if (section.id.isEmpty())
                continue;
            sections.append(section);
        }
        std::stable_sort(sections.begin(), sections.end(),
                         [](const LibrarySection &a, const LibrarySection &b) {
                             const int rankA = collectionRank(a.collectionType);
                             const int rankB = collectionRank(b.collectionType);
                             if (rankA != rankB)
                                 return rankA < rankB;
                             return a.name.localeAwareCompare(b.name) < 0;
                         });
        handler(sections, QString());
    });
}

void LibraryData::fetchItems(const LibrarySection &section, int startIndex, int limit,
                             int sortMode, const QString &searchTerm, ItemsHandler handler)
{
    if (!handler)
        return;
    if (!m_client) {
        handler({}, 0, false, QStringLiteral("未登录，无法加载媒体库。"));
        return;
    }

    const int pageSize = qBound(1, limit, 200);

    // 注意：step 可能在 fetchItems() 返回后才执行，必须按值捕获所有局部变量，
    // 否则 section/searchTerm/sortMode 会变成悬空引用（MSVC Debug 下更易暴露）。
    auto makeQuery = [section, sortMode, searchTerm, pageSize](int start) {
        emby::ItemQuery query;
        query.parentId = section.id;
        query.recursive = true;
        query.includeItemTypes = itemTypesForCollection(section.collectionType);
        query.fields = listFields();
        query.startIndex = qMax(0, start);
        query.limit = pageSize;
        query.searchTerm = searchTerm.trimmed();
        switch (sortMode) {
        case 1:
            query.sortBy = {QStringLiteral("SortName")};
            query.sortOrder = QStringLiteral("Ascending");
            break;
        case 2:
            query.sortBy = {QStringLiteral("CommunityRating")};
            query.sortOrder = QStringLiteral("Descending");
            break;
        case 3:
            query.sortBy = {QStringLiteral("ProductionYear")};
            query.sortOrder = QStringLiteral("Descending");
            break;
        default:
            query.sortBy = {QStringLiteral("DateCreated")};
            query.sortOrder = QStringLiteral("Descending");
            break;
        }
        return query;
    };

    // Emby 对 ParentId 查询会把 IncludeItemTypes 同时作用于子项：例如 IncludeItemTypes=Series
    // 时，剧集会按“集数”而不是“剧集数量”计入 TotalRecordCount，导致单次 60 条里只剩 5 个剧集。
    // 因此这里循环补齐到 limit 条，并把 hasMore 收敛到正确语义。
    struct ItemsState
    {
        QList<MediaItem> items;
        int total = -1;
        int next = 0;
        int initial = 0;
        int pages = 0;
        bool exhausted = false;
        ItemsHandler handler;
    };
    auto state = QSharedPointer<ItemsState>::create();
    state->handler = std::move(handler);
    state->next = qMax(0, startIndex);
    state->initial = state->next;

    auto step = QSharedPointer<std::function<void()>>::create();
    auto finish = [state, step, pageSize](bool failed, const QString &message) {
        // 断开 step 的自引用，避免异步递归持有的 shared_ptr 循环泄漏。
        *step = std::function<void()>();
        if (!state->handler)
            return;
        if (failed && state->items.isEmpty() && state->initial == 0) {
            auto callback = std::move(state->handler);
            state->handler = nullptr;
            callback({}, 0, false, message);
            return;
        }
        const int total = state->total >= 0 ? state->total : state->initial + state->items.size();
        const bool hasMore = !failed && !state->exhausted;
        auto callback = std::move(state->handler);
        state->handler = nullptr;
        callback(state->items, total, hasMore, QString());
    };

    *step = [this, makeQuery, state, step, pageSize, finish]() {
        const int requestStart = state->next;
        m_client->fetchItems(
            makeQuery(requestStart),
            [this, state, step, requestStart, pageSize, finish](const emby::ItemQueryResult &page,
                                                                const emby::ApiError &error) {
                if (error.isError()) {
                    finish(true, friendlyApiError(error));
                    return;
                }

                if (page.totalRecordCount > state->total)
                    state->total = page.totalRecordCount;
                for (const emby::MediaItem &source : page.items)
                    state->items.append(toMediaItem(source));

                state->next = requestStart + page.items.size();
                ++state->pages;

                const bool pageFull = page.items.size() >= pageSize;
                const bool enoughItems = state->items.size() >= pageSize;
                const bool totalReached = state->total > 0 && state->next >= state->total;
                if (!pageFull || page.items.isEmpty() || state->pages >= 20 || totalReached) {
                    state->exhausted = true;
                    finish(false, QString());
                    return;
                }
                if (enoughItems) {
                    finish(false, QString());
                    return;
                }
                (*step)();
            });
    };
    (*step)();
}

void LibraryData::fetchHome(int resumeLimit, int recentLimit, HomeHandler handler)
{
    if (!handler)
        return;
    if (!m_client) {
        handler({}, {}, QStringLiteral("未登录，无法加载首页。"));
        return;
    }

    struct HomeState
    {
        QList<MediaItem> resume;
        QList<MediaItem> recent;
        bool resumeDone = false;
        bool recentDone = false;
        QString resumeError;
        QString recentError;
        HomeHandler handler;
    };
    auto state = QSharedPointer<HomeState>::create();
    state->handler = std::move(handler);

    auto finish = [this, state]() {
        if (!state->resumeDone || !state->recentDone) {
            qInfo() << "home finish waiting: resumeDone" << state->resumeDone
                    << "recentDone" << state->recentDone
                    << "resume" << state->resume.size()
                    << "recent" << state->recent.size();
            return;
        }
        if (!state->handler)
            return;
        // 最近添加失败不应把已经拿到的继续观看一起丢掉；只有 resume 失败，
        // 或 resume 为空且 recent 失败时才把错误交给首页显示。
        QString error = state->resumeError;
        if (error.isEmpty() && state->resume.isEmpty())
            error = state->recentError;
        qInfo() << "home finish: resumeDone" << state->resumeDone
                << "recentDone" << state->recentDone
                << "resume" << state->resume.size()
                << "recent" << state->recent.size()
                << "error" << error;
        auto callback = std::move(state->handler);
        state->handler = nullptr;
        callback(state->resume, state->recent, error);
    };

    qInfo() << "resume request: userId" << m_client->profile().userId
            << "limit" << qMax(1, resumeLimit);
    m_client->fetchResumeItems(qMax(1, resumeLimit), 0,
                               [this, state, finish](const emby::ItemQueryResult &page,
                                                     const emby::ApiError &error) {
        qInfo() << "resume response: userId" << (m_client ? m_client->profile().userId : QString())
                << "httpStatus" << error.httpStatus
                << "totalRecordCount" << page.totalRecordCount
                << "items" << page.items.size()
                << "error" << error.isError()
                << "message" << error.message.left(200);
        if (error.isError())
            state->resumeError = friendlyApiError(error);

        state->resume.reserve(page.items.size());
        for (const emby::MediaItem &source : page.items)
            state->resume.append(toMediaItem(source));
        qInfo() << "resume parsed:" << state->resume.size()
                << "error" << state->resumeError;
        if (!page.items.isEmpty()) {
            const emby::MediaItem &first = page.items.first();
            qInfo() << "resume first: id" << first.id << "name" << first.name
                    << "type" << first.type << "played" << first.userData.played
                    << "positionTicks" << first.userData.playbackPositionTicks
                    << "playedPercentage" << first.userData.playedPercentage
                    << "runTimeTicks" << first.runTimeTicks;
        }
        state->resumeDone = true;
        finish();
    });

    emby::ItemQuery query;
    query.recursive = true;
    query.includeItemTypes = {QStringLiteral("Movie"), QStringLiteral("Series")};
    query.fields = listFields();
    query.limit = qMax(1, recentLimit);
    query.sortBy = {QStringLiteral("DateCreated")};
    query.sortOrder = QStringLiteral("Descending");
    qInfo() << "home recent request: userId" << m_client->profile().userId
            << "limit" << qMax(1, recentLimit);
    m_client->fetchItems(query, [this, state, finish](const emby::ItemQueryResult &page,
                                                      const emby::ApiError &error) {
        qInfo() << "home recent response: httpStatus" << error.httpStatus
                << "totalRecordCount" << page.totalRecordCount
                << "items" << page.items.size()
                << "error" << error.isError()
                << "message" << error.message.left(200);
        if (error.isError()) {
            state->recentError = friendlyApiError(error);
            qWarning() << "home recent response error: httpStatus" << error.httpStatus
                       << "message" << error.message.left(200);
        }
        state->recent.reserve(page.items.size());
        for (const emby::MediaItem &source : page.items)
            state->recent.append(toMediaItem(source));
        qInfo() << "home recent parsed:" << state->recent.size()
                << "error" << state->recentError;
        state->recentDone = true;
        finish();
    });
}


void LibraryData::fetchDetail(const QString &itemId, DetailHandler handler)
{
    if (!handler)
        return;
    if (!m_client) {
        handler({}, QStringLiteral("未登录，无法加载详情。"));
        return;
    }

    m_client->fetchItem(itemId, [this, handler, itemId](const emby::MediaItem &source,
                                                        const emby::ApiError &error) {
        if (error.isError()) {
            handler({}, friendlyApiError(error));
            return;
        }

        const MediaItem baseItem = toMediaItem(source);
        if (source.type != QLatin1String("Series")) {
            handler(baseItem, QString());
            return;
        }

        struct DetailState
        {
            MediaItem item;
            QList<emby::MediaItem> seasons;
            QList<emby::MediaItem> episodes;
            bool seasonsDone = false;
            bool episodesDone = false;
            QString error;
            DetailHandler handler;
        };
        auto state = QSharedPointer<DetailState>::create();
        state->item = baseItem;
        state->handler = std::move(handler);

        auto finish = [this, state]() {
            if (!state->seasonsDone || !state->episodesDone)
                return;
            if (!state->handler)
                return;

            QMap<int, Season> seasonMap;
            for (const emby::MediaItem &sourceSeason : state->seasons) {
                Season season;
                season.number = sourceSeason.indexNumber > 0 ? sourceSeason.indexNumber : 1;
                season.name = sourceSeason.name;
                if (season.name.isEmpty())
                    season.name = QStringLiteral("第 %1 季").arg(season.number);
                seasonMap.insert(season.number, season);
            }
            for (const emby::MediaItem &sourceEpisode : state->episodes) {
                const int seasonNumber = sourceEpisode.parentIndexNumber > 0
                                             ? sourceEpisode.parentIndexNumber
                                             : 1;
                if (!seasonMap.contains(seasonNumber)) {
                    Season season;
                    season.number = seasonNumber;
                    season.name = QStringLiteral("第 %1 季").arg(seasonNumber);
                    seasonMap.insert(seasonNumber, season);
                }
                seasonMap[seasonNumber].episodes.append(toEpisode(sourceEpisode));
            }
            for (Season &season : seasonMap) {
                std::stable_sort(season.episodes.begin(), season.episodes.end(),
                                 [](const Episode &a, const Episode &b) {
                                     return a.number < b.number;
                                 });
            }
            state->item.seasons = seasonMap.values();

            auto callback = std::move(state->handler);
            state->handler = nullptr;
            callback(state->item, state->error);
        };

        emby::ItemQuery seasonQuery;
        seasonQuery.parentId = source.id;
        seasonQuery.recursive = false;
        seasonQuery.includeItemTypes = {QStringLiteral("Season")};
        seasonQuery.sortBy = {QStringLiteral("SortName")};
        seasonQuery.fields = {QStringLiteral("Overview"), QStringLiteral("ImageTags"),
                              QStringLiteral("BackdropImageTags"), QStringLiteral("UserData")};
        m_client->fetchItems(seasonQuery, [state, finish](const emby::ItemQueryResult &page,
                                                          const emby::ApiError &error) {
            if (error.isError() && state->error.isEmpty())
                state->error = friendlyApiError(error);
            state->seasons = page.items;
            state->seasonsDone = true;
            finish();
        });

        emby::ItemQuery episodeQuery;
        episodeQuery.parentId = source.id;
        episodeQuery.recursive = true;
        episodeQuery.includeItemTypes = {QStringLiteral("Episode")};
        episodeQuery.sortBy = {QStringLiteral("ParentIndexNumber"), QStringLiteral("IndexNumber")};
        episodeQuery.sortOrder = QStringLiteral("Ascending");
        episodeQuery.fields = {QStringLiteral("Overview"), QStringLiteral("RunTimeTicks"),
                               QStringLiteral("ImageTags"), QStringLiteral("BackdropImageTags"),
                               QStringLiteral("UserData"), QStringLiteral("ProductionYear"),
                               QStringLiteral("CommunityRating")};
        m_client->fetchItems(episodeQuery, [state, finish](const emby::ItemQueryResult &page,
                                                           const emby::ApiError &error) {
            if (error.isError() && state->error.isEmpty())
                state->error = friendlyApiError(error);
            state->episodes = page.items;
            state->episodesDone = true;
            finish();
        });
    });
}

} // namespace ui
