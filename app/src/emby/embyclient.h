#pragma once

// Asynchronous Emby/Jellyfin REST API client built on QNetworkAccessManager.
//
// Design notes:
//  * Every call is asynchronous and reports its result through a std::function
//    callback invoked on the thread that owns the EmbyClient (normally the GUI
//    thread). A callback is invoked exactly once - also on failure.
//  * The access token is sent in the Authorization header (and optionally the
//    legacy Emby headers); it is never appended to an API URL.
//  * Methods return the underlying QNetworkReply so the caller may abort a
//    request. EmbyClient keeps ownership of the reply.
//  * Progress-report throttling is intentionally left to the caller; this
//    class only exposes the three /Sessions/Playing endpoints.
//
// Typical use:
//     auto *client = new EmbyClient(this);
//     client->setServer("http://192.168.1.10:8096");
//     client->authenticate("user", "pass", [client](const AuthResult &auth,
//                                                  const ApiError &error) {
//         if (error.isError()) { ... }
//         ...
//     });

#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

#include "deviceprofile.h"
#include "embytypes.h"

class QNetworkAccessManager;

namespace emby {

struct ApiError {
    QString operation;
    int httpStatus = 0;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    QString message;
    QJsonObject payload;

    bool isError() const;
    bool isOk() const { return !isError(); }
    QString toString() const;
};

class EmbyClient : public QObject
{
    Q_OBJECT
public:
    using AuthHandler = std::function<void(const AuthResult &, const ApiError &)>;
    using ItemListHandler = std::function<void(const ItemQueryResult &, const ApiError &)>;
    using ItemHandler = std::function<void(const MediaItem &, const ApiError &)>;
    using PlaybackInfoHandler = std::function<void(const PlaybackInfoResult &, const ApiError &)>;
    using ServerInfoHandler = std::function<void(const ServerInfo &, const ApiError &)>;
    using StatusHandler = std::function<void(const ApiError &)>;
    using JsonHandler = std::function<void(const QJsonObject &, const ApiError &)>;

    explicit EmbyClient(QObject *parent = nullptr);
    ~EmbyClient() override;

    // ---- Configuration -------------------------------------------------
    void setProfile(const ServerProfile &profile);
    const ServerProfile &profile() const { return m_profile; }

    /// Sets the server base URL and (optionally) the access token. A missing
    /// scheme is assumed to be http://. The token is stored, never put in a URL.
    void setServer(const QString &baseUrl, const QString &accessToken = QString());
    void setAccessToken(const QString &accessToken);
    void setUserId(const QString &userId, const QString &userName = QString());

    QString baseUrl() const { return m_profile.baseUrl(); }
    bool isConfigured() const { return m_profile.isValid(); }
    bool isAuthenticated() const { return m_profile.isAuthenticated(); }

    /// Shared QNAM used for metadata and (T25) poster image downloads. The
    /// client owns it; do not delete or re-parent it.
    QNetworkAccessManager *networkManager() const { return m_network; }

    /// Per-request transfer timeout in milliseconds (0 disables the timeout).
    void setRequestTimeout(int milliseconds) { m_requestTimeoutMs = milliseconds; }
    int requestTimeout() const { return m_requestTimeoutMs; }

    void setDeviceProfileOptions(const DeviceProfileOptions &options);
    const DeviceProfileOptions &deviceProfileOptions() const { return m_deviceProfile; }

    // ---- Authentication ------------------------------------------------
    /// POST /Users/AuthenticateByName. On success the access token, user id and
    /// user name are stored in profile().
    QNetworkReply *authenticate(const QString &username, const QString &password,
                                AuthHandler handler);

    /// GET /System/Info/Public - useful for "test connection" and for
    /// detecting Jellyfin vs Emby before/after login.
    QNetworkReply *fetchPublicSystemInfo(ServerInfoHandler handler);

    // ---- Library browsing ----------------------------------------------
    /// GET /Users/{userId}/Views
    QNetworkReply *fetchViews(ItemListHandler handler);

    /// GET /Users/{userId}/Items with all query fields from ItemQuery.
    QNetworkReply *fetchItems(const ItemQuery &query, ItemListHandler handler);

    /// GET /Users/{userId}/Items/{itemId}
    QNetworkReply *fetchItem(const QString &itemId, ItemHandler handler);

    /// POST /Items/{itemId}/PlaybackInfo?UserId={userId} with a DeviceProfile.
    QNetworkReply *fetchPlaybackInfo(const QString &itemId, PlaybackInfoHandler handler);
    QNetworkReply *fetchPlaybackInfo(const QString &itemId,
                                     const DeviceProfileOptions &options,
                                     PlaybackInfoHandler handler);

    /// GET /Users/{userId}/Items/Resume
    QNetworkReply *fetchResumeItems(int limit = 12, int startIndex = 0,
                                    ItemListHandler handler = {});

    // ---- Playback URL / headers ---------------------------------------
    /// Builds /Videos/{itemId}/stream?static=true&mediaSourceId={id}.
    /// The access token is NOT added to the URL; use streamHeaderLines() /
    /// mpvHeaderFields() when handing the URL to mpv.
    QUrl directStreamUrl(const QString &itemId,
                         const QString &mediaSourceId = QString()) const;

    /// Builds /Items/{itemId}/Images/{imageType} with optional tag and size.
    /// The token is not added to the URL; use streamHeaderLines() on the image
    /// QNetworkRequest instead.
    QUrl imageUrl(const QString &itemId,
                  const QString &imageType = QStringLiteral("Primary"),
                  const QString &imageTag = QString(), int maxWidth = 0,
                  int maxHeight = 0) const;

    /// Chooses direct play / direct stream / transcode URL for a media source.
    /// Server-provided URLs are resolved against the base URL; token query
    /// parameters (api_key, X-Emby-Token) are stripped from them.
    QUrl playbackUrl(const QString &itemId, const MediaSourceInfo &source) const;
    /// T48：把 PlaybackInfo 返回的原始 DirectStreamUrl/TranscodingUrl 解析为绝对 URL，
    /// 并去除 query 中的 token；供直连失败后的备选 URL 使用。
    QUrl resolveMediaUrl(const QString &rawUrl) const;

    /// Headers for fetching media: "Authorization: MediaBrowser ...".
    QList<QPair<QString, QString>> streamHeaders() const;
    /// Same headers as full lines ("Name: value").
    QStringList streamHeaderLines() const;
    /// Value for mpv's --http-header-fields option (list separators escaped).
    /// For libmpv, streamHeaderLines() + the "http-header-fields-append"
    /// option is safer because append does not interpret escapes.
    QString mpvHeaderFields() const;

    /// Authorization header value ("MediaBrowser Client=..., Device=...,
    /// DeviceId=..., Version=..., Token=...").
    QString authorizationHeader() const;

    // ---- Playback progress --------------------------------------------
    QNetworkReply *reportPlaybackStart(const PlaybackProgressInfo &info,
                                       StatusHandler handler = {});
    QNetworkReply *reportPlaybackProgress(const PlaybackProgressInfo &info,
                                          StatusHandler handler = {});
    QNetworkReply *reportPlaybackStopped(const PlaybackProgressInfo &info,
                                         StatusHandler handler = {});

    // ---- Low-level escape hatch ---------------------------------------
    QNetworkReply *getJson(const QString &path, const QUrlQuery &query, JsonHandler handler);
    QNetworkReply *postJson(const QString &path, const QJsonObject &body, JsonHandler handler);

    /// Aborts all in-flight requests. Their callbacks receive an
    /// OperationCanceledError ApiError.
    void abortAll();

private:
    QUrl buildUrl(const QString &path, const QUrlQuery &query = QUrlQuery()) const;
    QNetworkRequest makeRequest(const QString &path, const QUrlQuery &query) const;
    QNetworkReply *sendGet(const QString &path, const QUrlQuery &query,
                           const QString &operation, JsonHandler handler);
    QNetworkReply *sendPost(const QString &path, const QUrlQuery &query,
                            const QJsonObject &body, const QString &operation,
                            JsonHandler handler);
    void handleReply(QNetworkReply *reply, const QString &operation, JsonHandler handler);
    void deliverError(const QString &operation, const QString &message, JsonHandler handler);
    void deliverErrorStatus(const QString &operation, const QString &message,
                            StatusHandler handler);
    void deliverResult(JsonHandler handler, const QJsonObject &object, const ApiError &error);
    void trackReply(QNetworkReply *reply);
    void untrackReply(QNetworkReply *reply);
    bool ensureConfigured(const QString &operation, JsonHandler handler);

    ServerProfile m_profile;
    DeviceProfileOptions m_deviceProfile;
    QNetworkAccessManager *m_network = nullptr;
    QSet<QNetworkReply *> m_inFlight;
    int m_requestTimeoutMs = 20000;
};

}  // namespace emby
