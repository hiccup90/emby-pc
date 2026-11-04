// Live probe for a real Emby server.
//
// Credentials are read from the environment and are never written to disk:
//   EMBY_URL       e.g. http://192.168.1.10:8096
//   EMBY_USER      user name
//   EMBY_PASSWORD  password (may be empty)
//
// Build (standalone, no root CMake changes needed):
//   cmake -S app/src/emby/tests -B /tmp/emby-live-build
//   cmake --build /tmp/emby-live-build -j
//   EMBY_URL=... EMBY_USER=... EMBY_PASSWORD=... /tmp/emby-live-build/live_probe
//
// Output intentionally never prints the access token or password.

#include "embyclient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

using namespace emby;

namespace {

int g_failures = 0;

void out(const QString &text)
{
    QTextStream stream(stdout);
    stream << text << Qt::endl;
}

void check(bool condition, const QString &what)
{
    if (condition) {
        out(QStringLiteral("[OK]   ") + what);
    } else {
        ++g_failures;
        out(QStringLiteral("[FAIL] ") + what);
    }
}

template <typename Result, typename Starter>
bool awaitResult(Starter starter, Result &result, ApiError &error, int timeoutMs = 30000)
{
    QEventLoop loop;
    bool done = false;
    starter([&](const Result &value, const ApiError &apiError) {
        result = value;
        error = apiError;
        done = true;
        loop.quit();
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    return done;
}

QString itemBrief(const MediaItem &item)
{
    return QStringLiteral("id=%1 name=%2 type=%3 mediaType=%4 year=%5")
        .arg(item.id, item.name, item.type, item.mediaType)
        .arg(item.productionYear);
}

QString streamBrief(const MediaStream &stream)
{
    return QStringLiteral("type=%1 codec=%2 lang=%3 w=%4 h=%5 ch=%6 range=%7")
        .arg(stream.type, stream.codec, stream.language)
        .arg(stream.width)
        .arg(stream.height)
        .arg(stream.channels)
        .arg(stream.videoRange);
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString url = qEnvironmentVariable("EMBY_URL");
    const QString user = qEnvironmentVariable("EMBY_USER");
    const QString password = qEnvironmentVariable("EMBY_PASSWORD");
    if (url.isEmpty() || user.isEmpty()) {
        QTextStream(stderr) << "Set EMBY_URL and EMBY_USER first (EMBY_PASSWORD may be empty).\n";
        return 2;
    }

    EmbyClient client;
    ServerProfile profile;
    profile.url = url;
    profile.clientName = QStringLiteral("EmbyPC-LiveProbe");
    profile.deviceName = QStringLiteral("dsh-linux-probe");
    profile.deviceId = QStringLiteral("emby-pc-live-probe");
    profile.clientVersion = QStringLiteral("0.1.0");
    profile.serverType = ServerType::Emby;
    profile.legacyEmbyHeaders = true;
    client.setProfile(profile);

    out(QStringLiteral("=== Public system info ==="));
    ServerInfo serverInfo;
    ApiError error;
    const bool infoDone = awaitResult<ServerInfo>(
        [&](auto callback) { client.fetchPublicSystemInfo(callback); }, serverInfo, error);
    check(infoDone && !error.isError(), QStringLiteral("GET /System/Info/Public"));
    out(QStringLiteral("serverName=%1 version=%2 product=%3")
            .arg(serverInfo.serverName, serverInfo.version, serverInfo.productName));
    check(!serverInfo.serverName.isEmpty(), QStringLiteral("ServerInfo fields parsed"));

    out(QStringLiteral("=== Authentication ==="));
    AuthResult auth;
    const bool authDone = awaitResult<AuthResult>(
        [&](auto callback) { client.authenticate(user, password, callback); }, auth, error);
    check(authDone && !error.isError(), QStringLiteral("POST /Users/AuthenticateByName"));
    check(auth.isValid(), QStringLiteral("AuthResult has token + user"));
    out(QStringLiteral("auth user=%1 tokenLen=%2")
            .arg(auth.user.name)
            .arg(auth.accessToken.size()));
    check(!auth.accessToken.isEmpty(), QStringLiteral("access token stored (not printed)"));
    check(client.profile().userId == auth.user.id, QStringLiteral("client stores user id"));

    out(QStringLiteral("=== Views ==="));
    ItemQueryResult views;
    const bool viewsDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchViews(callback); }, views, error);
    check(viewsDone && !error.isError(), QStringLiteral("GET /Users/{userId}/Views"));
    out(QStringLiteral("views=%1").arg(views.items.size()));
    bool hasMoviesView = false;
    bool hasTvView = false;
    bool hasBoxSetView = false;
    for (const MediaItem &view : views.items) {
        out(QStringLiteral("view id=%1 name=%2 type=%3 collectionType=%4")
                .arg(view.id, view.name, view.type, view.collectionType));
        if (view.collectionType == QStringLiteral("movies"))
            hasMoviesView = true;
        if (view.collectionType == QStringLiteral("tvshows"))
            hasTvView = true;
        if (view.collectionType == QStringLiteral("boxsets"))
            hasBoxSetView = true;
    }
    check(hasMoviesView && hasTvView, QStringLiteral("views include movies + tvshows"));
    check(hasBoxSetView, QStringLiteral("views include boxsets"));

    const QString movieViewId = [&]() {
        for (const MediaItem &view : views.items) {
            if (view.collectionType == QStringLiteral("movies"))
                return view.id;
        }
        return QString();
    }();
    const QString tvViewId = [&]() {
        for (const MediaItem &view : views.items) {
            if (view.collectionType == QStringLiteral("tvshows"))
                return view.id;
        }
        return QString();
    }();

    const QStringList itemFields = {
        QStringLiteral("MediaSources"), QStringLiteral("MediaStreams"),
        QStringLiteral("Overview"), QStringLiteral("UserData"),
        QStringLiteral("ProductionYear"), QStringLiteral("PrimaryImageAspectRatio"),
        QStringLiteral("DateCreated"), QStringLiteral("OfficialRating"),
        QStringLiteral("CommunityRating")};

    out(QStringLiteral("=== Playable item count ==="));
    ItemQuery playableQuery;
    playableQuery.recursive = true;
    playableQuery.includeItemTypes = {QStringLiteral("Movie"), QStringLiteral("Episode")};
    playableQuery.limit = 1;
    playableQuery.fields = itemFields;
    ItemQueryResult playable;
    const bool playableDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(playableQuery, callback); }, playable, error);
    check(playableDone && !error.isError(), QStringLiteral("GET /Users/{userId}/Items recursive"));
    out(QStringLiteral("playableTotal=%1").arg(playable.totalRecordCount));
    check(playable.totalRecordCount > 5000, QStringLiteral("large playable library detected"));

    out(QStringLiteral("=== Movies page (Limit=10, SortBy=SortName) ==="));
    ItemQuery movieQuery;
    movieQuery.parentId = movieViewId;
    movieQuery.recursive = true;
    movieQuery.includeItemTypes = {QStringLiteral("Movie")};
    movieQuery.sortBy = {QStringLiteral("SortName")};
    movieQuery.limit = 10;
    movieQuery.fields = itemFields;
    ItemQueryResult movies;
    const bool moviesDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(movieQuery, callback); }, movies, error);
    check(moviesDone && !error.isError(), QStringLiteral("GET movie Items page"));
    out(QStringLiteral("moviesTotal=%1 returned=%2").arg(movies.totalRecordCount).arg(movies.items.size()));
    check(movies.items.size() == 10, QStringLiteral("Limit=10 honored"));
    for (int i = 0; i < movies.items.size() && i < 3; ++i)
        out(QStringLiteral("movie[%1] %2").arg(i).arg(itemBrief(movies.items.at(i))));
    if (!movies.items.isEmpty()) {
        const MediaItem &first = movies.items.first();
        check(!first.id.isEmpty() && !first.name.isEmpty(), QStringLiteral("MediaItem id/name parsed"));
        check(first.productionYear > 0, QStringLiteral("ProductionYear parsed when requested"));
        check(first.primaryImageTagValue().size() > 0, QStringLiteral("Primary image tag parsed"));
        check(first.userData.playbackPositionTicks >= 0, QStringLiteral("UserData parsed"));
    }

    out(QStringLiteral("=== Pagination (StartIndex=10, Limit=10) ==="));
    ItemQuery page2Query = movieQuery;
    page2Query.startIndex = 10;
    ItemQueryResult page2;
    const bool page2Done = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(page2Query, callback); }, page2, error);
    check(page2Done && !error.isError(), QStringLiteral("GET movie Items page 2"));
    check(page2.items.size() == 10, QStringLiteral("page 2 Limit=10 honored"));
    QSet<QString> firstPageIds;
    for (const MediaItem &item : movies.items)
        firstPageIds.insert(item.id);
    bool overlap = false;
    for (const MediaItem &item : page2.items) {
        if (firstPageIds.contains(item.id))
            overlap = true;
    }
    check(!overlap, QStringLiteral("page 1 and page 2 do not overlap"));
    if (!page2.items.isEmpty())
        out(QStringLiteral("page2[0] %1").arg(itemBrief(page2.items.first())));

    out(QStringLiteral("=== Default ItemQuery fields ==="));
    ItemQuery defaultQuery;
    defaultQuery.parentId = movieViewId;
    defaultQuery.recursive = true;
    defaultQuery.includeItemTypes = {QStringLiteral("Movie")};
    defaultQuery.limit = 2;
    ItemQueryResult defaultPage;
    const bool defaultDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(defaultQuery, callback); }, defaultPage, error);
    check(defaultDone && !error.isError() && !defaultPage.items.isEmpty(),
          QStringLiteral("ItemQuery default Fields request"));
    if (!defaultPage.items.isEmpty()) {
        out(QStringLiteral("defaultFields[0] %1").arg(itemBrief(defaultPage.items.first())));
        check(defaultPage.items.first().productionYear > 0,
              QStringLiteral("default Fields include ProductionYear"));
    }

    out(QStringLiteral("=== Search (SearchTerm=边境) ==="));
    ItemQuery searchQuery;
    searchQuery.recursive = true;
    searchQuery.searchTerm = QStringLiteral("边境");
    searchQuery.includeItemTypes = {QStringLiteral("Movie")};
    searchQuery.limit = 10;
    searchQuery.fields = itemFields;
    ItemQueryResult searchResult;
    const bool searchDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(searchQuery, callback); }, searchResult, error);
    check(searchDone && !error.isError(), QStringLiteral("GET Items with SearchTerm"));
    out(QStringLiteral("searchTotal=%1 returned=%2").arg(searchResult.totalRecordCount).arg(searchResult.items.size()));
    for (const MediaItem &item : searchResult.items)
        out(QStringLiteral("search hit %1").arg(itemBrief(item)));
    check(!searchResult.items.isEmpty(), QStringLiteral("search returns items"));

    out(QStringLiteral("=== Latest movies (SortBy=DateCreated Desc) ==="));
    ItemQuery latestQuery;
    latestQuery.recursive = true;
    latestQuery.includeItemTypes = {QStringLiteral("Movie")};
    latestQuery.sortBy = {QStringLiteral("DateCreated")};
    latestQuery.sortOrder = QStringLiteral("Descending");
    latestQuery.limit = 5;
    latestQuery.fields = itemFields;
    ItemQueryResult latest;
    const bool latestDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(latestQuery, callback); }, latest, error);
    check(latestDone && !error.isError(), QStringLiteral("GET latest movies sorted by DateCreated"));
    out(QStringLiteral("latestTotal=%1 returned=%2").arg(latest.totalRecordCount).arg(latest.items.size()));
    for (const MediaItem &item : latest.items) {
        out(QStringLiteral("latest %1 dateCreated=%2").arg(itemBrief(item), item.dateCreated));
    }
    if (!latest.items.isEmpty())
        check(!latest.items.first().dateCreated.isEmpty(), QStringLiteral("DateCreated parsed"));

    out(QStringLiteral("=== Movie detail ==="));
    MediaItem detail;
    const bool detailDone = awaitResult<MediaItem>(
        [&](auto callback) { client.fetchItem(movies.items.isEmpty() ? QString() : movies.items.first().id, callback); },
        detail, error);
    check(detailDone && !error.isError(), QStringLiteral("GET /Users/{userId}/Items/{id}"));
    out(QStringLiteral("detail %1 originalTitle=%2 rating=%3 community=%4 premiere=%5")
            .arg(itemBrief(detail), detail.originalTitle, detail.officialRating)
            .arg(detail.communityRating)
            .arg(detail.premiereDate));
    out(QStringLiteral("detail path=%1 container=%2 runtime=%3 mediaSources=%4 imageTag=%5")
            .arg(detail.path, detail.container)
            .arg(detail.runTimeTicks)
            .arg(detail.mediaSources.size())
            .arg(detail.primaryImageTagValue()));
    check(!detail.name.isEmpty() && !detail.overview.isEmpty(),
          QStringLiteral("detail name/overview parsed"));
    check(detail.mediaSources.size() > 0, QStringLiteral("detail MediaSources parsed"));
    check(detail.runTimeTicks > 0, QStringLiteral("detail RunTimeTicks parsed"));
    check(!detail.genres.isEmpty(), QStringLiteral("detail Genres parsed"));
    check(!detail.logoImageTag.isEmpty(), QStringLiteral("detail Logo image tag parsed"));
    if (!detail.mediaSources.isEmpty()) {
        out(QStringLiteral("detail source container=%1 streams=%2")
                .arg(detail.mediaSources.first().container)
                .arg(detail.mediaSources.first().mediaStreams.size()));
        check(!detail.mediaSources.first().mediaStreams.isEmpty(),
              QStringLiteral("detail MediaStreams parsed"));
    }

    out(QStringLiteral("=== Series -> Seasons -> Episodes ==="));
    ItemQuery seriesQuery;
    seriesQuery.parentId = tvViewId;
    seriesQuery.recursive = true;
    seriesQuery.includeItemTypes = {QStringLiteral("Series")};
    seriesQuery.limit = 1;
    seriesQuery.sortBy = {QStringLiteral("SortName")};
    seriesQuery.fields = {QStringLiteral("Overview"), QStringLiteral("UserData")};
    ItemQueryResult series;
    const bool seriesDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(seriesQuery, callback); }, series, error);
    check(seriesDone && !error.isError() && !series.items.isEmpty(), QStringLiteral("fetch first Series"));
    const QString seriesId = series.items.isEmpty() ? QString() : series.items.first().id;
    out(QStringLiteral("series %1").arg(series.items.isEmpty() ? QStringLiteral("-") : series.items.first().name));
    if (!series.items.isEmpty()) {
        out(QStringLiteral("series unplayedItemCount=%1").arg(series.items.first().userData.unplayedItemCount));
        check(series.items.first().userData.unplayedItemCount > 0,
              QStringLiteral("UnplayedItemCount parsed for series"));
    }

    ItemQuery seasonQuery;
    seasonQuery.parentId = seriesId;
    seasonQuery.recursive = false;
    seasonQuery.includeItemTypes = {QStringLiteral("Season")};
    seasonQuery.sortBy = {QStringLiteral("SortName")};
    seasonQuery.fields = {QStringLiteral("Overview"), QStringLiteral("UserData")};
    ItemQueryResult seasons;
    const bool seasonsDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(seasonQuery, callback); }, seasons, error);
    check(seasonsDone && !error.isError() && !seasons.items.isEmpty(),
          QStringLiteral("Items?ParentId={series}&IncludeItemTypes=Season"));
    out(QStringLiteral("seasons=%1").arg(seasons.items.size()));
    for (const MediaItem &season : seasons.items)
        out(QStringLiteral("season id=%1 name=%2 index=%3").arg(season.id, season.name).arg(season.indexNumber));
    const QString seasonId = seasons.items.isEmpty() ? QString() : seasons.items.first().id;

    ItemQuery episodeQuery;
    episodeQuery.parentId = seasonId;
    episodeQuery.recursive = false;
    episodeQuery.includeItemTypes = {QStringLiteral("Episode")};
    episodeQuery.sortBy = {QStringLiteral("SortName")};
    episodeQuery.fields = itemFields;
    ItemQueryResult episodes;
    const bool episodesDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchItems(episodeQuery, callback); }, episodes, error);
    check(episodesDone && !error.isError() && !episodes.items.isEmpty(),
          QStringLiteral("Items?ParentId={season}&IncludeItemTypes=Episode"));
    out(QStringLiteral("episodes=%1").arg(episodes.items.size()));
    if (!episodes.items.isEmpty())
        out(QStringLiteral("firstEpisode id=%1 name=%2 season=%3 episode=%4")
                .arg(episodes.items.first().id, episodes.items.first().name)
                .arg(episodes.items.first().parentIndexNumber)
                .arg(episodes.items.first().indexNumber));

    QUrlQuery showsQuery;
    showsQuery.addQueryItem(QStringLiteral("Limit"), QStringLiteral("1"));
    QJsonObject showsObject;
    const bool showsDone = awaitResult<QJsonObject>(
        [&](auto callback) { client.getJson(QStringLiteral("/Shows/%1/Episodes").arg(seriesId), showsQuery, callback); },
        showsObject, error);
    const ItemQueryResult showsEpisodes = ItemQueryResult::fromJson(showsObject);
    check(showsDone && !error.isError(), QStringLiteral("GET /Shows/{id}/Episodes"));
    out(QStringLiteral("showsEpisodes total=%1 returned=%2")
            .arg(showsEpisodes.totalRecordCount)
            .arg(showsEpisodes.items.size()));
    check(!showsEpisodes.items.isEmpty(), QStringLiteral("/Shows/{id}/Episodes returns items"));

    out(QStringLiteral("=== PlaybackInfo ==="));
    const QString playbackItemId = movies.items.isEmpty() ? QString() : movies.items.first().id;
    PlaybackInfoResult playback;
    const bool playbackDone = awaitResult<PlaybackInfoResult>(
        [&](auto callback) { client.fetchPlaybackInfo(playbackItemId, callback); }, playback, error);
    check(playbackDone && !error.isError(), QStringLiteral("POST /Items/{id}/PlaybackInfo"));
    check(!playback.playSessionId.isEmpty(), QStringLiteral("PlaySessionId parsed"));
    check(!playback.mediaSources.isEmpty(), QStringLiteral("PlaybackInfo MediaSources parsed"));
    const MediaSourceInfo *source = playback.preferredMediaSource();
    check(source != nullptr, QStringLiteral("preferred MediaSource selected"));
    if (source) {
        out(QStringLiteral("playback source id=%1 container=%2 supportsDirectPlay=%3 directStreamUrl=%4 transcodingUrl=%5")
                .arg(source->id, source->container)
                .arg(source->supportsDirectPlay)
                .arg(!source->directStreamUrl.isEmpty())
                .arg(!source->transcodingUrl.isEmpty()));
        out(QStringLiteral("playback size=%1 bitrate=%2 runTimeTicks=%3")
                .arg(source->size)
                .arg(source->bitrate)
                .arg(source->runTimeTicks));
        if (const MediaStream *video = source->videoStream())
            out(QStringLiteral("video %1").arg(streamBrief(*video)));
        if (const MediaStream *audio = source->audioStream())
            out(QStringLiteral("audio %1").arg(streamBrief(*audio)));
        for (const MediaStream &subtitle : source->subtitleStreams())
            out(QStringLiteral("subtitle %1").arg(streamBrief(subtitle)));
        check(source->videoStream() != nullptr, QStringLiteral("video stream parsed from PlaybackInfo"));
        check(source->audioStream() != nullptr, QStringLiteral("audio stream parsed from PlaybackInfo"));
        check(!source->id.isEmpty() && !source->container.isEmpty(),
              QStringLiteral("PlaybackInfo source id/container parsed"));

        const QUrl direct = client.directStreamUrl(playbackItemId, source->id);
        check(direct.path().endsWith(QStringLiteral("/stream")), QStringLiteral("directStreamUrl path"));
        check(!direct.toString().contains(QStringLiteral("api_key")),
              QStringLiteral("directStreamUrl excludes api_key/token"));
        const QUrl resolved = client.playbackUrl(playbackItemId, *source);
        check(!resolved.isEmpty(), QStringLiteral("playbackUrl resolves source"));
        check(!resolved.toString().contains(QStringLiteral("api_key")),
              QStringLiteral("playbackUrl strips api_key"));
        out(QStringLiteral("resolved path=%1 queryKeys=%2")
                .arg(resolved.path(), resolved.query(QUrl::FullyEncoded).section(QLatin1Char('='), 0, 0)));

        DeviceProfileOptions narrow;
        narrow.enableDirectPlay = false;
        narrow.includeVideoDirectPlayProfile = false;
        narrow.enableDirectStream = true;
        narrow.enableTranscoding = true;
        PlaybackInfoResult narrowPlayback;
        const bool narrowDone = awaitResult<PlaybackInfoResult>(
            [&](auto callback) { client.fetchPlaybackInfo(playbackItemId, narrow, callback); },
            narrowPlayback, error);
        check(narrowDone && !error.isError(), QStringLiteral("PlaybackInfo with narrow DeviceProfile"));
        if (const MediaSourceInfo *narrowSource = narrowPlayback.preferredMediaSource()) {
            out(QStringLiteral("narrow source supportsDirectPlay=%1 directStreamUrl=%2 transcodingUrl=%3")
                    .arg(narrowSource->supportsDirectPlay)
                    .arg(!narrowSource->directStreamUrl.isEmpty())
                    .arg(!narrowSource->transcodingUrl.isEmpty()));
            const QUrl narrowUrl = client.playbackUrl(playbackItemId, *narrowSource);
            check(!narrowUrl.isEmpty(), QStringLiteral("narrow playbackUrl resolves"));
            check(!narrowUrl.toString().contains(QStringLiteral("api_key")),
                  QStringLiteral("narrow playbackUrl strips api_key"));
        } else {
            check(false, QStringLiteral("narrow PlaybackInfo has media source"));
        }
    }

    out(QStringLiteral("=== Resume ==="));
    ItemQueryResult resume;
    const bool resumeDone = awaitResult<ItemQueryResult>(
        [&](auto callback) { client.fetchResumeItems(20, 0, callback); }, resume, error);
    check(resumeDone && !error.isError(), QStringLiteral("GET /Users/{userId}/Items/Resume"));
    out(QStringLiteral("resume total=%1 returned=%2").arg(resume.totalRecordCount).arg(resume.items.size()));

    out(QStringLiteral("=== Error path ==="));
    MediaItem missing;
    ApiError missingError;
    const bool missingDone = awaitResult<MediaItem>(
        [&](auto callback) { client.fetchItem(QStringLiteral("this-id-does-not-exist"), callback); },
        missing, missingError);
    check(missingDone && missingError.isError(), QStringLiteral("invalid item id returns ApiError"));
    out(QStringLiteral("error httpStatus=%1 message=%2").arg(missingError.httpStatus).arg(missingError.message));

    out(QStringLiteral("=== RESULT ==="));
    out(QStringLiteral("failures=%1").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
