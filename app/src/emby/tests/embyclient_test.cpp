// Self-test for the Emby API client. Runs a local QTcpServer fake Emby
// server and exercises authentication, library browsing, PlaybackInfo,
// direct-play URL construction and progress reporting.
//
// Build/run:
//   cmake -S app/src/emby -B build/emby -DEMBY_BUILD_TESTS=ON
//   cmake --build build/emby -j && ctest --test-dir build/emby --output-on-failure

#include "deviceprofile.h"
#include "embyclient.h"
#include "securetokenstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtDebug>

using namespace emby;

namespace {

int g_failures = 0;

void check(bool condition, const QString &what)
{
    if (condition) {
        qInfo().noquote() << "ok  :" << what;
    } else {
        ++g_failures;
        qCritical().noquote() << "FAIL:" << what;
    }
}

struct Waiter {
    QEventLoop loop;
    bool done = false;

    void finish()
    {
        done = true;
        loop.quit();
    }

    bool wait(int timeoutMs = 5000)
    {
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
        return done;
    }
};

struct HttpRequest {
    QString method;
    QString target;
    QMap<QString, QString> headers;
    QByteArray body;

    QUrl url() const { return QUrl(QStringLiteral("http://localhost") + target); }
    QString path() const { return url().path(); }
    QUrlQuery query() const { return QUrlQuery(url()); }
};

class FakeEmbyServer : public QTcpServer
{
public:
    explicit FakeEmbyServer(QObject *parent = nullptr) : QTcpServer(parent) {}

    QList<HttpRequest> requests;

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(socketDescriptor);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

private:
    QHash<QTcpSocket *, QByteArray> m_buffers;

    void onReadyRead(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QByteArray headerBlock = buffer.left(headerEnd);
            const QList<QByteArray> lines = headerBlock.split('\n');
            const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
            if (requestLine.size() < 2) {
                socket->disconnectFromHost();
                return;
            }
            HttpRequest request;
            request.method = QString::fromLatin1(requestLine.at(0));
            request.target = QString::fromLatin1(requestLine.at(1));
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines.at(i).trimmed();
                const int colon = line.indexOf(':');
                if (colon <= 0)
                    continue;
                request.headers.insert(
                    QString::fromLatin1(line.left(colon)).trimmed().toLower(),
                    QString::fromLatin1(line.mid(colon + 1)).trimmed());
            }
            const int contentLength = request.headers.value(QStringLiteral("content-length"),
                                                           QStringLiteral("0")).toInt();
            const int total = headerEnd + 4 + contentLength;
            if (buffer.size() < total)
                return;
            request.body = buffer.mid(headerEnd + 4, contentLength);
            buffer.remove(0, total);
            requests.append(request);
            respond(socket, request);
        }
    }

    void respond(QTcpSocket *socket, const HttpRequest &request)
    {
        int status = 200;
        QByteArray reason = "OK";
        QByteArray body = "{}";
        const QString path = request.path();

        if (request.method == "POST" && path == "/Users/AuthenticateByName") {
            const QJsonObject payload = QJsonDocument::fromJson(request.body).object();
            const QString authorization = request.headers.value(QStringLiteral("authorization"));
            if (!authorization.contains(QStringLiteral("MediaBrowser"))
                || !authorization.contains(QStringLiteral("Client=\"TestClient\""))
                || payload.value(QStringLiteral("Username")).toString() != "tester"
                || payload.value(QStringLiteral("Pw")).toString() != "secret") {
                status = 401;
                reason = "Unauthorized";
                body = R"({"Message":"bad credentials"})";
            } else {
                body = R"({"AccessToken":"token-abc","ServerId":"srv-1","User":{"Id":"u1","Name":"tester","ServerId":"srv-1","Policy":{"IsAdministrator":true,"CanDownload":true}},"SessionInfo":{"Id":"session-1"}})";
            }
        } else if (request.method == "GET" && path == "/System/Info/Public") {
            body = R"({"ServerName":"Fake Emby","Version":"4.9.0","ProductName":"Emby Server","Id":"srv-1","OperatingSystem":"Linux"})";
        } else if (request.method == "GET" && path == "/Users/u1/Views") {
            body = R"({"Items":[{"Id":"v1","Name":"Movies","Type":"CollectionFolder","CollectionType":"movies"}],"TotalRecordCount":1})";
        } else if (request.method == "GET" && path == "/Users/u1/Items/Resume") {
            body = R"({"Items":[{"Id":"item-r","Name":"Resume Movie","Type":"Movie","MediaType":"Video","RunTimeTicks":72000000000,"UserData":{"PlaybackPositionTicks":1234567890,"PlayedPercentage":17.5}}],"TotalRecordCount":1})";
        } else if (request.method == "GET" && path == "/Users/u1/Items/item1") {
            body = R"({"Id":"item1","Name":"Big Buck Bunny","Type":"Movie","MediaType":"Video","ProductionYear":2008,"RunTimeTicks":5960000000,"Overview":"A rabbit.","ImageTags":{"Primary":"tag-p"},"MediaSources":[{"Id":"src1","Name":"1080p MKV","Container":"mkv","SupportsDirectPlay":true,"SupportsDirectStream":true,"SupportsTranscoding":true,"RunTimeTicks":5960000000,"MediaStreams":[{"Type":"Video","Codec":"hevc","Width":1920,"Height":1080,"VideoRange":"HDR10","Index":0},{"Type":"Audio","Codec":"eac3","Channels":6,"Index":1},{"Type":"Subtitle","Codec":"ass","Index":2,"IsTextSubtitleStream":true,"IsDefault":true}]}],"UserData":{"PlaybackPositionTicks":50000000,"Played":false}})";
        } else if (request.method == "GET" && path == "/Users/u1/Items") {
            body = R"({"Items":[{"Id":"item1","Name":"Big Buck Bunny","Type":"Movie","MediaType":"Video","ProductionYear":2008,"RunTimeTicks":5960000000,"ImageTags":{"Primary":"tag-p"},"UserData":{"PlaybackPositionTicks":50000000,"Played":false},"MediaSources":[{"Id":"src1","Name":"1080p MKV","Container":"mkv","SupportsDirectPlay":true,"RunTimeTicks":5960000000,"MediaStreams":[{"Type":"Video","Codec":"hevc","Width":1920,"Height":1080,"VideoRange":"HDR10","Index":0},{"Type":"Audio","Codec":"eac3","Channels":6,"Index":1},{"Type":"Subtitle","Codec":"ass","Index":2,"IsTextSubtitleStream":true}]}]}],"TotalRecordCount":42})";
        } else if (request.method == "POST" && path == "/Items/item1/PlaybackInfo") {
            const QJsonObject payload = QJsonDocument::fromJson(request.body).object();
            const QJsonObject deviceProfile =
                payload.value(QStringLiteral("DeviceProfile")).toObject();
            if (payload.value(QStringLiteral("UserId")).toString() != "u1"
                || deviceProfile.isEmpty()) {
                status = 400;
                reason = "Bad Request";
                body = R"({"Message":"missing UserId or DeviceProfile"})";
            } else {
                body = R"({"PlaySessionId":"ps-1","MediaSources":[{"Id":"src1","Name":"1080p MKV","Container":"mkv","SupportsDirectPlay":true,"SupportsDirectStream":true,"SupportsTranscoding":true,"RunTimeTicks":5960000000,"MediaStreams":[{"Type":"Video","Codec":"hevc","Index":0},{"Type":"Audio","Codec":"eac3","Index":1}]}]})";
            }
        } else if (request.method == "POST"
                   && (path == "/Sessions/Playing" || path == "/Sessions/Playing/Progress"
                       || path == "/Sessions/Playing/Stopped")) {
            status = 204;
            reason = "No Content";
            body.clear();
        } else if (request.method == "GET" && path == "/Users/u1/Items/missing") {
            status = 404;
            reason = "Not Found";
            body = R"({"Message":"not found"})";
        } else if (request.method == "GET" && path == "/Users/u1/Items/unauthorized") {
            status = 401;
            reason = "Unauthorized";
            body = R"({"Message":"token expired"})";
        } else if (request.method == "GET" && path == "/Users/u1/Items/forbidden") {
            status = 403;
            reason = "Forbidden";
            body = R"({"Message":"forbidden"})";
        } else {
            status = 404;
            reason = "Not Found";
            body = R"({"Message":"unexpected request"})";
        }

        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        response += "Connection: keep-alive\r\n\r\n";
        response += body;
        socket->write(response);
        socket->flush();
    }
};

const HttpRequest *lastRequest(const FakeEmbyServer &server)
{
    return server.requests.isEmpty() ? nullptr : &server.requests.constLast();
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Keep QSettings test data out of the real user profile. The legacy
    // migration test below writes the old "auth/profile" key here.
    QTemporaryDir settingsRoot;
    check(settingsRoot.isValid(), QStringLiteral("temporary QSettings directory created"));
    QCoreApplication::setOrganizationName(QStringLiteral("EmbyClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("emby-client-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

    FakeEmbyServer server;
    check(server.start(), QStringLiteral("fake Emby server listens"));

    EmbyClient client;
    ServerProfile profile;
    profile.url = server.baseUrl();
    profile.clientName = QStringLiteral("TestClient");
    profile.deviceName = QStringLiteral("TestDevice");
    profile.deviceId = QStringLiteral("test-device-id");
    profile.clientVersion = QStringLiteral("9.9.9");
    profile.serverType = ServerType::Emby;
    profile.legacyEmbyHeaders = true;
    client.setProfile(profile);
    check(client.isConfigured(), QStringLiteral("client is configured"));

    // ---- DeviceProfile -------------------------------------------------
    const QJsonObject wideProfile = DeviceProfile::build();
    const QJsonArray directPlay = wideProfile.value(QStringLiteral("DirectPlayProfiles")).toArray();
    check(directPlay.size() == 2, QStringLiteral("DeviceProfile has video + audio direct play"));
    const QJsonObject videoProfile = directPlay.at(0).toObject();
    check(videoProfile.value(QStringLiteral("Container")).toString().contains(QStringLiteral("mkv")),
          QStringLiteral("DeviceProfile containers include mkv"));
    check(videoProfile.value(QStringLiteral("Container")).toString().contains(QStringLiteral("mp4")),
          QStringLiteral("DeviceProfile containers include mp4"));
    check(videoProfile.value(QStringLiteral("Container")).toString().contains(QStringLiteral("ts")),
          QStringLiteral("DeviceProfile containers include ts"));
    check(videoProfile.value(QStringLiteral("VideoCodec")).toString().contains(QStringLiteral("hevc")),
          QStringLiteral("DeviceProfile video codecs include hevc"));
    check(videoProfile.value(QStringLiteral("VideoCodec")).toString().contains(QStringLiteral("av1")),
          QStringLiteral("DeviceProfile video codecs include av1"));
    check(videoProfile.value(QStringLiteral("VideoCodec")).toString().contains(QStringLiteral("vp9")),
          QStringLiteral("DeviceProfile video codecs include vp9"));
    check(videoProfile.value(QStringLiteral("AudioCodec")).toString().contains(QStringLiteral("truehd")),
          QStringLiteral("DeviceProfile audio codecs include truehd"));
    check(videoProfile.value(QStringLiteral("AudioCodec")).toString().contains(QStringLiteral("opus")),
          QStringLiteral("DeviceProfile audio codecs include opus"));
    const QJsonArray subtitleProfiles =
        wideProfile.value(QStringLiteral("SubtitleProfiles")).toArray();
    QStringList subtitleFormats;
    for (const QJsonValue &value : subtitleProfiles)
        subtitleFormats.append(value.toObject().value(QStringLiteral("Format")).toString());
    check(subtitleFormats.contains(QStringLiteral("ass")),
          QStringLiteral("DeviceProfile subtitle formats include ass"));
    check(subtitleFormats.contains(QStringLiteral("srt")),
          QStringLiteral("DeviceProfile subtitle formats include srt"));

    DeviceProfileOptions narrowOptions;
    narrowOptions.includeVideoDirectPlayProfile = false;
    narrowOptions.includeAudioDirectPlayProfile = false;
    narrowOptions.includeHlsTranscodingProfile = false;
    narrowOptions.includeSubtitleProfiles = false;
    const QJsonObject narrow = DeviceProfile(narrowOptions).toJson();
    check(!narrow.contains(QStringLiteral("DirectPlayProfiles")),
          QStringLiteral("DeviceProfile switches can disable direct play profiles"));
    check(!narrow.contains(QStringLiteral("TranscodingProfiles")),
          QStringLiteral("DeviceProfile switches can disable transcoding profiles"));
    check(!narrow.contains(QStringLiteral("SubtitleProfiles")),
          QStringLiteral("DeviceProfile switches can disable subtitle profiles"));

    DeviceProfileOptions requestOff;
    requestOff.enableDirectPlay = false;
    requestOff.enableTranscoding = false;
    const QJsonObject requestOffProfile = DeviceProfile(requestOff).toJson();
    check(!requestOffProfile.contains(QStringLiteral("DirectPlayProfiles")),
          QStringLiteral("enableDirectPlay=false gates DirectPlayProfiles"));
    check(!requestOffProfile.contains(QStringLiteral("TranscodingProfiles")),
          QStringLiteral("enableTranscoding=false gates TranscodingProfiles"));

    // ---- Authentication ------------------------------------------------
    AuthResult auth;
    ApiError authError;
    {
        Waiter waiter;
        client.authenticate(QStringLiteral("tester"), QStringLiteral("secret"),
                            [&](const AuthResult &result, const ApiError &error) {
                                auth = result;
                                authError = error;
                                waiter.finish();
                            });
        check(waiter.wait(), QStringLiteral("authenticate callback fires"));
    }
    check(authError.isOk(), QStringLiteral("authenticate succeeds: ") + authError.toString());
    check(auth.accessToken == QStringLiteral("token-abc"),
          QStringLiteral("authenticate returns AccessToken"));
    check(auth.user.id == QStringLiteral("u1"), QStringLiteral("authenticate returns User.Id"));
    check(auth.serverId == QStringLiteral("srv-1"),
          QStringLiteral("authenticate returns ServerId"));
    check(client.profile().accessToken == QStringLiteral("token-abc"),
          QStringLiteral("client stores access token"));
    check(client.profile().userId == QStringLiteral("u1"),
          QStringLiteral("client stores user id"));

    if (const HttpRequest *request = lastRequest(server)) {
        check(request->headers.value(QStringLiteral("authorization"))
                  .contains(QStringLiteral("MediaBrowser")),
              QStringLiteral("auth request carries Authorization: MediaBrowser"));
        check(request->headers.value(QStringLiteral("x-emby-authorization"))
                  .contains(QStringLiteral("DeviceId=\"test-device-id\"")),
              QStringLiteral("auth request carries legacy Emby header"));
    }

    // ---- Public system info -------------------------------------------
    ServerInfo serverInfo;
    ApiError serverInfoError;
    {
        Waiter waiter;
        client.fetchPublicSystemInfo([&](const ServerInfo &result, const ApiError &error) {
            serverInfo = result;
            serverInfoError = error;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("fetchPublicSystemInfo callback fires"));
    }
    check(serverInfoError.isOk() && serverInfo.serverName == QStringLiteral("Fake Emby"),
          QStringLiteral("fetchPublicSystemInfo parses ServerName"));
    check(!serverInfo.isJellyfin(), QStringLiteral("ServerInfo detects Emby"));

    // ---- Views ---------------------------------------------------------
    ItemQueryResult views;
    ApiError viewsError;
    {
        Waiter waiter;
        client.fetchViews([&](const ItemQueryResult &result, const ApiError &error) {
            views = result;
            viewsError = error;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("fetchViews callback fires"));
    }
    check(viewsError.isOk() && views.items.size() == 1
              && views.items.first().name == QStringLiteral("Movies"),
          QStringLiteral("fetchViews parses items"));
    if (const HttpRequest *request = lastRequest(server))
        check(request->headers.value(QStringLiteral("authorization"))
                  .contains(QStringLiteral("Token=\"token-abc\"")),
              QStringLiteral("authenticated request carries Token in Authorization"));

    // ---- Items / query parameters -------------------------------------
    ItemQuery query;
    query.parentId = QStringLiteral("v1");
    query.recursive = true;
    query.includeItemTypes = {QStringLiteral("Movie"), QStringLiteral("Episode")};
    query.fields = {QStringLiteral("MediaSources"), QStringLiteral("MediaStreams")};
    query.sortBy = {QStringLiteral("SortName")};
    query.limit = 60;
    ItemQueryResult page;
    ApiError pageError;
    {
        Waiter waiter;
        client.fetchItems(query, [&](const ItemQueryResult &result, const ApiError &error) {
            page = result;
            pageError = error;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("fetchItems callback fires"));
    }
    check(pageError.isOk(), QStringLiteral("fetchItems succeeds: ") + pageError.toString());
    check(page.totalRecordCount == 42, QStringLiteral("fetchItems parses TotalRecordCount"));
    check(page.items.size() == 1 && page.items.first().id == QStringLiteral("item1"),
          QStringLiteral("fetchItems parses MediaItem"));
    const MediaItem &item = page.items.first();
    check(item.mediaSources.size() == 1, QStringLiteral("MediaItem has MediaSources"));
    if (!item.mediaSources.isEmpty()) {
        const MediaSourceInfo &source = item.mediaSources.first();
        check(source.id == QStringLiteral("src1") && source.supportsDirectPlay,
              QStringLiteral("MediaSource direct play info parsed"));
        check(source.videoStream() != nullptr && source.videoStream()->isHdr(),
              QStringLiteral("video stream parsed and HDR detected"));
        check(source.subtitleStreams().size() == 1,
              QStringLiteral("subtitle stream parsed"));
    }
    check(item.userData.playbackPositionTicks == 50000000,
          QStringLiteral("user data position parsed"));
    check(item.primaryImageTagValue() == QStringLiteral("tag-p"),
          QStringLiteral("image tag parsed"));
    if (const HttpRequest *request = lastRequest(server)) {
        const QUrlQuery sent = request->query();
        check(sent.queryItemValue(QStringLiteral("Recursive")) == QStringLiteral("true"),
              QStringLiteral("Items query sends Recursive=true"));
        check(sent.queryItemValue(QStringLiteral("IncludeItemTypes"))
                  == QStringLiteral("Movie,Episode"),
              QStringLiteral("Items query sends IncludeItemTypes"));
        check(sent.queryItemValue(QStringLiteral("Fields"))
                  == QStringLiteral("MediaSources,MediaStreams"),
              QStringLiteral("Items query sends Fields"));
        check(sent.queryItemValue(QStringLiteral("SortBy")) == QStringLiteral("SortName"),
              QStringLiteral("Items query sends SortBy"));
        check(sent.queryItemValue(QStringLiteral("Limit")) == QStringLiteral("60"),
              QStringLiteral("Items query sends Limit"));
    }

    // ---- Item details --------------------------------------------------
    MediaItem details;
    ApiError detailsError;
    {
        Waiter waiter;
        client.fetchItem(QStringLiteral("item1"), [&](const MediaItem &result,
                                                       const ApiError &error) {
            details = result;
            detailsError = error;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("fetchItem callback fires"));
    }
    check(detailsError.isOk() && details.name == QStringLiteral("Big Buck Bunny"),
          QStringLiteral("fetchItem parses detail"));

    // ---- PlaybackInfo --------------------------------------------------
    PlaybackInfoResult playback;
    ApiError playbackError;
    {
        Waiter waiter;
        client.fetchPlaybackInfo(QStringLiteral("item1"),
                                 [&](const PlaybackInfoResult &result, const ApiError &error) {
                                     playback = result;
                                     playbackError = error;
                                     waiter.finish();
                                 });
        check(waiter.wait(), QStringLiteral("fetchPlaybackInfo callback fires"));
    }
    check(playbackError.isOk(), QStringLiteral("fetchPlaybackInfo succeeds: ")
                                    + playbackError.toString());
    check(playback.playSessionId == QStringLiteral("ps-1"),
          QStringLiteral("PlaybackInfo parses PlaySessionId"));
    check(playback.preferredMediaSource() != nullptr
              && playback.preferredMediaSource()->id == QStringLiteral("src1"),
          QStringLiteral("PlaybackInfo picks direct-play source"));
    if (const HttpRequest *request = lastRequest(server)) {
        const QJsonObject payload = QJsonDocument::fromJson(request->body).object();
        const QJsonObject deviceProfile =
            payload.value(QStringLiteral("DeviceProfile")).toObject();
        check(payload.value(QStringLiteral("UserId")).toString() == QStringLiteral("u1"),
              QStringLiteral("PlaybackInfo body sends UserId"));
        check(payload.value(QStringLiteral("EnableDirectPlay")).toBool(),
              QStringLiteral("PlaybackInfo body enables direct play"));
        check(!deviceProfile.isEmpty(), QStringLiteral("PlaybackInfo body sends DeviceProfile"));
        check(request->query().queryItemValue(QStringLiteral("UserId"))
                  == QStringLiteral("u1"),
              QStringLiteral("PlaybackInfo query sends UserId"));
    }

    // ---- Resume --------------------------------------------------------
    ItemQueryResult resume;
    ApiError resumeError;
    {
        Waiter waiter;
        client.fetchResumeItems(10, 0, [&](const ItemQueryResult &result,
                                            const ApiError &error) {
            resume = result;
            resumeError = error;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("fetchResumeItems callback fires"));
    }
    check(resumeError.isOk() && resume.items.size() == 1
              && resume.items.first().id == QStringLiteral("item-r"),
          QStringLiteral("fetchResumeItems parses items"));
    if (const HttpRequest *request = lastRequest(server)) {
        check(request->path() == QStringLiteral("/Users/u1/Items/Resume"),
              QStringLiteral("Resume uses correct path"));
        check(request->query().queryItemValue(QStringLiteral("Limit")) == QStringLiteral("10"),
              QStringLiteral("Resume sends Limit"));
    }

    // ---- Direct play URL + stream headers -----------------------------
    const QUrl streamUrl = client.directStreamUrl(QStringLiteral("item1"),
                                                  QStringLiteral("src1"));
    check(streamUrl.path() == QStringLiteral("/Videos/item1/stream"),
          QStringLiteral("directStreamUrl path is /Videos/{id}/stream"));
    const QUrlQuery streamQuery(streamUrl);
    check(streamQuery.queryItemValue(QStringLiteral("static")) == QStringLiteral("true"),
          QStringLiteral("directStreamUrl has static=true"));
    check(streamQuery.queryItemValue(QStringLiteral("mediaSourceId"))
              == QStringLiteral("src1"),
          QStringLiteral("directStreamUrl has mediaSourceId"));
    check(!streamUrl.toString().contains(QStringLiteral("token-abc")),
          QStringLiteral("directStreamUrl does not contain token"));
    check(!streamUrl.toString().contains(QStringLiteral("api_key")),
          QStringLiteral("directStreamUrl does not contain api_key"));

    const QUrl imageUrl = client.imageUrl(QStringLiteral("item1"), QStringLiteral("Primary"),
                                          QStringLiteral("tag-p"), 300, 0);
    check(imageUrl.path() == QStringLiteral("/Items/item1/Images/Primary"),
          QStringLiteral("imageUrl builds image path"));
    check(QUrlQuery(imageUrl).queryItemValue(QStringLiteral("tag")) == QStringLiteral("tag-p"),
          QStringLiteral("imageUrl sends tag"));
    check(!imageUrl.toString().contains(QStringLiteral("token-abc")),
          QStringLiteral("imageUrl does not contain token"));

    const QStringList headerLines = client.streamHeaderLines();
    check(!headerLines.isEmpty()
              && headerLines.first().startsWith(QStringLiteral("Authorization: MediaBrowser")),
          QStringLiteral("streamHeaderLines returns Authorization header"));
    check(headerLines.first().contains(QStringLiteral("Token=\"token-abc\"")),
          QStringLiteral("stream header carries token"));
    check(client.mpvHeaderFields().contains(QStringLiteral("\\, Device=")),
          QStringLiteral("mpvHeaderFields escapes list separators"));

    // Server-provided URL token query must be stripped.
    MediaSourceInfo transcodeSource;
    transcodeSource.id = QStringLiteral("src2");
    transcodeSource.supportsDirectPlay = false;
    transcodeSource.transcodingUrl =
        QStringLiteral("/Videos/item1/master.m3u8?api_key=should-be-removed&PlaySessionId=ps-9");
    const QUrl transcodeUrl = client.playbackUrl(QStringLiteral("item1"), transcodeSource);
    check(transcodeUrl.path() == QStringLiteral("/Videos/item1/master.m3u8"),
          QStringLiteral("playbackUrl resolves server URL"));
    check(!transcodeUrl.toString().contains(QStringLiteral("api_key")),
          QStringLiteral("playbackUrl strips api_key token"));
    check(QUrlQuery(transcodeUrl).queryItemValue(QStringLiteral("PlaySessionId"))
              == QStringLiteral("ps-9"),
          QStringLiteral("playbackUrl preserves non-token query parameters"));

    // ---- Playback progress --------------------------------------------
    PlaybackProgressInfo progress;
    progress.itemId = QStringLiteral("item1");
    progress.mediaSourceId = QStringLiteral("src1");
    progress.playSessionId = QStringLiteral("ps-1");
    progress.positionTicks = 10000000;
    progress.isPaused = false;

    {
        ApiError error;
        Waiter waiter;
        client.reportPlaybackStart(progress, [&](const ApiError &value) {
            error = value;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("reportPlaybackStart callback fires"));
        check(error.isOk(), QStringLiteral("reportPlaybackStart succeeds: ") + error.toString());
        if (const HttpRequest *request = lastRequest(server)) {
            const QJsonObject payload = QJsonDocument::fromJson(request->body).object();
            check(request->path() == QStringLiteral("/Sessions/Playing"),
                  QStringLiteral("start posts to /Sessions/Playing"));
            check(payload.value(QStringLiteral("ItemId")).toString() == QStringLiteral("item1"),
                  QStringLiteral("start payload has ItemId"));
            check(payload.value(QStringLiteral("PlaySessionId")).toString()
                      == QStringLiteral("ps-1"),
                  QStringLiteral("start payload has PlaySessionId"));
            check(payload.value(QStringLiteral("PositionTicks")).toInteger() == 10000000,
                  QStringLiteral("start payload has PositionTicks"));
        }
    }
    {
        ApiError error;
        Waiter waiter;
        PlaybackProgressInfo paused = progress;
        paused.positionTicks = 20000000;
        paused.isPaused = true;
        client.reportPlaybackProgress(paused, [&](const ApiError &value) {
            error = value;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("reportPlaybackProgress callback fires"));
        check(error.isOk(), QStringLiteral("reportPlaybackProgress succeeds: ") + error.toString());
        if (const HttpRequest *request = lastRequest(server)) {
            const QJsonObject payload = QJsonDocument::fromJson(request->body).object();
            check(request->path() == QStringLiteral("/Sessions/Playing/Progress"),
                  QStringLiteral("progress posts to /Sessions/Playing/Progress"));
            check(payload.value(QStringLiteral("IsPaused")).toBool(),
                  QStringLiteral("progress payload has IsPaused"));
            check(payload.value(QStringLiteral("EventName")).toString()
                      == QStringLiteral("TimeUpdate"),
                  QStringLiteral("progress payload defaults EventName=TimeUpdate"));
        }
    }
    {
        ApiError error;
        Waiter waiter;
        PlaybackProgressInfo stopped = progress;
        stopped.positionTicks = 30000000;
        client.reportPlaybackStopped(stopped, [&](const ApiError &value) {
            error = value;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("reportPlaybackStopped callback fires"));
        check(error.isOk(), QStringLiteral("reportPlaybackStopped succeeds: ") + error.toString());
        if (const HttpRequest *request = lastRequest(server))
            check(request->path() == QStringLiteral("/Sessions/Playing/Stopped"),
                  QStringLiteral("stopped posts to /Sessions/Playing/Stopped"));
    }

    // ---- Error paths ---------------------------------------------------
    {
        EmbyClient anonymousClient;
        anonymousClient.setServer(server.baseUrl());
        ApiError error;
        Waiter waiter;
        anonymousClient.fetchViews([&](const ItemQueryResult &, const ApiError &value) {
            error = value;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("missing user id callback fires"));
        check(error.isError() && error.message.contains(QStringLiteral("Not authenticated")),
              QStringLiteral("fetch without userId reports Not authenticated"));
    }
    {
        ApiError error;
        Waiter waiter;
        client.fetchItem(QStringLiteral("missing"), [&](const MediaItem &, const ApiError &value) {
            error = value;
            waiter.finish();
        });
        check(waiter.wait(), QStringLiteral("404 callback fires"));
        check(error.httpStatus == 404 && error.message == QStringLiteral("not found"),
              QStringLiteral("HTTP error payload parsed: ") + error.toString());
    }

    // ---- T65: same-origin header filtering ----------------------------
    {
        const QUrl sameOrigin(server.baseUrl() + QStringLiteral("/Videos/item1/stream"));
        check(client.isSameOrigin(sameOrigin), QStringLiteral("same-origin media URL accepted"));
        check(!client.streamHeaderLines(sameOrigin).isEmpty(),
              QStringLiteral("same-origin media URL gets Authorization header"));
        check(client.mpvHeaderFields(sameOrigin).contains(QStringLiteral("Authorization")),
              QStringLiteral("same-origin media URL gets mpv header fields"));

        const QUrl otherHost(QStringLiteral("https://cdn.example.invalid/video.mkv"));
        check(!client.isSameOrigin(otherHost), QStringLiteral("cross-host URL rejected"));
        check(client.streamHeaderLines(otherHost).isEmpty(),
              QStringLiteral("cross-host URL gets no Authorization header"));
        check(client.mpvHeaderFields(otherHost).isEmpty(),
              QStringLiteral("cross-host URL gets no mpv token header"));
        check(!client.streamHeaders(otherHost).size(),
              QStringLiteral("cross-host URL has empty header list"));
        check(!client.isSameOrigin(QUrl(QStringLiteral("file:///tmp/movie.mkv"))),
              QStringLiteral("file URL is not same-origin"));
        check(!client.isSameOrigin(QUrl(QStringLiteral("http://127.0.0.1:1/video"))),
              QStringLiteral("different port is not same-origin"));
    }

    // ---- T65: 401/403 authenticationExpired + clearAuthentication -----
    {
        int expiredCount = 0;
        int expiredStatus = 0;
        QString expiredOperation;
        QObject::connect(&client, &EmbyClient::authenticationExpired,
                         [&](int status, const QString &operation, const QString &) {
                             ++expiredCount;
                             expiredStatus = status;
                             expiredOperation = operation;
                         });
        ApiError error;
        Waiter waiter;
        client.fetchItem(QStringLiteral("unauthorized"),
                         [&](const MediaItem &, const ApiError &value) {
                             error = value;
                             waiter.finish();
                         });
        check(waiter.wait(), QStringLiteral("401 callback fires"));
        check(error.httpStatus == 401, QStringLiteral("401 error status parsed"));
        check(expiredCount == 1 && expiredStatus == 401,
              QStringLiteral("authenticationExpired emitted once with 401"));
        check(expiredOperation == QStringLiteral("ItemDetails"),
              QStringLiteral("authenticationExpired carries operation"));

        // The signal is a notification; the UI owns session teardown. It must
        // not fire again for a second failed request in the same session.
        ApiError secondError;
        Waiter secondWaiter;
        client.fetchItem(QStringLiteral("unauthorized"),
                         [&](const MediaItem &, const ApiError &value) {
                             secondError = value;
                             secondWaiter.finish();
                         });
        check(secondWaiter.wait(), QStringLiteral("second 401 callback fires"));
        check(secondError.httpStatus == 401 && expiredCount == 1,
              QStringLiteral("authenticationExpired is deduplicated per profile"));

        check(client.isAuthenticated(),
              QStringLiteral("401 signal does not implicitly clear token"));
        client.clearAuthentication();
        check(!client.isAuthenticated(), QStringLiteral("clearAuthentication clears token"));
        check(client.profile().accessToken.isEmpty() && client.profile().userId.isEmpty(),
              QStringLiteral("clearAuthentication clears token and user id"));
        check(client.baseUrl() == server.baseUrl(),
              QStringLiteral("clearAuthentication keeps server URL"));
    }
    {
        EmbyClient forbiddenClient;
        ServerProfile forbiddenProfile = profile;
        forbiddenProfile.userId = QStringLiteral("u1");
        forbiddenProfile.accessToken = QStringLiteral("forbidden-token");
        forbiddenClient.setProfile(forbiddenProfile);
        int forbiddenCount = 0;
        int forbiddenStatus = 0;
        QObject::connect(&forbiddenClient, &EmbyClient::authenticationExpired,
                         [&](int status, const QString &, const QString &) {
                             ++forbiddenCount;
                             forbiddenStatus = status;
                         });
        ApiError error;
        Waiter waiter;
        forbiddenClient.fetchItem(QStringLiteral("forbidden"),
                                  [&](const MediaItem &, const ApiError &value) {
                                      error = value;
                                      waiter.finish();
                                  });
        check(waiter.wait(), QStringLiteral("403 callback fires"));
        check(error.httpStatus == 403 && forbiddenCount == 1 && forbiddenStatus == 403,
              QStringLiteral("authenticationExpired handles 403"));
    }

    {
        // Bad credentials are a login failure, not an expired existing session;
        // the UI login dialog handles this directly.
        EmbyClient badLoginClient;
        badLoginClient.setProfile(profile);
        int expiredCount = 0;
        QObject::connect(&badLoginClient, &EmbyClient::authenticationExpired,
                         [&](int, const QString &, const QString &) { ++expiredCount; });
        ApiError error;
        Waiter waiter;
        badLoginClient.authenticate(QStringLiteral("tester"), QStringLiteral("wrong"),
                                    [&](const AuthResult &, const ApiError &value) {
                                        error = value;
                                        waiter.finish();
                                    });
        check(waiter.wait(), QStringLiteral("bad login callback fires"));
        check(error.httpStatus == 401 && expiredCount == 0,
              QStringLiteral("login 401 does not emit sessionExpired"));
    }

    // ---- T65: secure profile storage + legacy migration ----------------
    {
        QTemporaryDir secureDir;
        check(secureDir.isValid(), QStringLiteral("secure store temp dir created"));
        SecureProfileStore store(secureDir.path());
        ServerProfile stored = profile;
        stored.accessToken = QStringLiteral("unit-test-secure-token");
        stored.userId = QStringLiteral("u-secure");
        stored.userName = QStringLiteral("secure-user");
        check(store.saveProfile(stored), QStringLiteral("secure profile save succeeds"));
        check(store.hasStoredProfile(), QStringLiteral("secure profile exists"));
        check(store.backendName().contains(QStringLiteral("DPAPI"))
                  || store.backendName().contains(QStringLiteral("Linux")),
              QStringLiteral("secure backend name available"));
#ifndef Q_OS_WIN
        const QString path = store.storagePath();
        check(QFileInfo::exists(path), QStringLiteral("secure profile file exists"));
        QFile file(path);
        check(file.open(QIODevice::ReadOnly), QStringLiteral("secure profile file readable"));
        const QByteArray raw = file.readAll();
        file.close();
        check(!raw.contains("unit-test-secure-token"),
              QStringLiteral("secure profile file does not contain plaintext token"));
        const QFileDevice::Permissions permissions = QFileInfo(path).permissions();
        check(permissions.testFlag(QFileDevice::ReadOwner)
                  && permissions.testFlag(QFileDevice::WriteOwner)
                  && !permissions.testFlag(QFileDevice::ReadGroup)
                  && !permissions.testFlag(QFileDevice::ReadOther),
              QStringLiteral("secure profile file is owner-only (0600)"));
#endif
        const ServerProfile loaded = store.loadProfile();
        check(loaded.accessToken == QStringLiteral("unit-test-secure-token")
                  && loaded.userId == QStringLiteral("u-secure"),
              QStringLiteral("secure profile roundtrip"));
        store.clear();
        check(!store.hasStoredProfile(), QStringLiteral("secure profile clear removes storage"));
        check(store.loadProfile().accessToken.isEmpty(),
              QStringLiteral("load after clear returns empty profile"));
    }
    {
        QTemporaryDir migrateDir;
        ServerProfile legacy = profile;
        legacy.accessToken = QStringLiteral("legacy-plain-token");
        legacy.userId = QStringLiteral("u-legacy");
        QSettings settings;
        settings.setValue(QStringLiteral("auth/profile"),
                          QJsonDocument(legacy.toJson()).toJson(QJsonDocument::Compact));
        settings.sync();
        check(settings.contains(QStringLiteral("auth/profile")),
              QStringLiteral("legacy plaintext profile seeded"));
        SecureProfileStore store(migrateDir.path());
        const ServerProfile migrated = store.loadProfile();
        check(migrated.accessToken == QStringLiteral("legacy-plain-token"),
              QStringLiteral("legacy profile is migrated"));
        check(!QSettings().contains(QStringLiteral("auth/profile")),
              QStringLiteral("legacy plaintext profile key removed after migration"));
        check(store.hasStoredProfile(), QStringLiteral("migrated secure profile exists"));
    }
#ifndef Q_OS_WIN
    {
        QTemporaryDir tamperDir;
        check(QDir().mkpath(tamperDir.path()), QStringLiteral("tamper test dir created"));
        QFile file(tamperDir.path() + QStringLiteral("/profile.dat"));
        check(file.open(QIODevice::WriteOnly), QStringLiteral("tamper test file writable"));
        file.write("not-a-secure-profile");
        file.close();
        SecureProfileStore store(tamperDir.path());
        check(store.loadProfile().accessToken.isEmpty(),
              QStringLiteral("corrupt secure profile falls back safely"));
    }
#endif

    // ---- T65: diagnostic redaction ------------------------------------
    {
        const QString text = QStringLiteral(
            "Authorization: MediaBrowser Client=\"C\", Token=\"auth-token\"\n"
            "GET /x?api_key=unit-test-query-token\n"
            "{\"AccessToken\":\"json-token\",\"Pw\":\"pw-token\"}");
        const QString redacted = redactSensitiveText(text);
        check(!redacted.contains(QStringLiteral("auth-token")),
              QStringLiteral("redaction removes Authorization token"));
        check(!redacted.contains(QStringLiteral("unit-test-query-token")),
              QStringLiteral("redaction removes api_key query value"));
        check(!redacted.contains(QStringLiteral("json-token")),
              QStringLiteral("redaction removes AccessToken JSON value"));
        check(!redacted.contains(QStringLiteral("pw-token")),
              QStringLiteral("redaction removes password JSON value"));
        check(redacted.contains(QStringLiteral("<redacted>")),
              QStringLiteral("redaction leaves marker"));

        const QString mpvLine = redactSensitiveText(QStringLiteral(
            "Set property: http-header-fields=\"Authorization: MediaBrowser "
            "Token=\\\"mpv-header-token\\\"\""));
        check(!mpvLine.contains(QStringLiteral("mpv-header-token"))
                  && mpvLine.contains(QStringLiteral("<redacted>")),
              QStringLiteral("redaction handles mpv http-header-fields log line"));
    }

    qInfo().noquote() << (g_failures == 0 ? QStringLiteral("ALL TESTS PASSED")
                                          : QStringLiteral("%1 TEST(S) FAILED").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
