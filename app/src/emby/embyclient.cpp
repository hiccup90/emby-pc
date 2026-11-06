#include "embyclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QSslError>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>

namespace emby {

namespace {

template <typename Handler, typename Result>
void postTypedError(QObject *context, const QString &operation, const QString &message,
                    Handler handler, const Result &emptyResult)
{
    if (!handler)
        return;
    ApiError error;
    error.operation = operation;
    error.message = message;
    QTimer::singleShot(0, context,
                       [handler = std::move(handler), error, emptyResult]() {
                           handler(emptyResult, error);
                       });
}

QString escapeQuoted(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

QString escapeMpvListValue(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size() + 8);
    for (const QChar character : value) {
        if (character == QLatin1Char('\\') || character == QLatin1Char(','))
            escaped.append(QLatin1Char('\\'));
        escaped.append(character);
    }
    return escaped;
}

QString jsonErrorMessage(const QJsonObject &payload)
{
    static const char *keys[] = {"Message", "ErrorMessage", "error", "message"};
    for (const char *key : keys) {
        const QString value = payload.value(QLatin1String(key)).toString();
        if (!value.trimmed().isEmpty())
            return value;
    }
    return {};
}

void removeTokenQueryItems(QUrlQuery &query, bool *changed)
{
    static const QStringList keys = {
        QStringLiteral("api_key"), QStringLiteral("X-Emby-Token"),
        QStringLiteral("ApiKey"), QStringLiteral("access_token")};
    for (const QString &key : keys) {
        if (query.hasQueryItem(key)) {
            query.removeAllQueryItems(key);
            *changed = true;
        }
    }
}

}  // namespace

bool ApiError::isError() const
{
    return networkError != QNetworkReply::NoError || httpStatus >= 400 || !message.isEmpty();
}

QString ApiError::toString() const
{
    if (!isError())
        return QStringLiteral("OK");
    QString text = operation.isEmpty() ? QStringLiteral("request") : operation;
    if (httpStatus > 0)
        text += QStringLiteral(" [HTTP %1]").arg(httpStatus);
    if (networkError != QNetworkReply::NoError)
        text += QStringLiteral(" [network %1]").arg(static_cast<int>(networkError));
    if (!message.isEmpty())
        text += QStringLiteral(": ") + message;
    return text;
}

EmbyClient::EmbyClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    m_profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_profile.deviceId = ServerProfile::generateDeviceId();
    m_profile.deviceName = QSysInfo::machineHostName();
}

EmbyClient::~EmbyClient()
{
    const QSet<QNetworkReply *> replies = m_inFlight;
    for (QNetworkReply *reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
    m_inFlight.clear();
}

void EmbyClient::setProfile(const ServerProfile &profile)
{
    m_profile = profile;
    m_authExpiredNotified = false;
    if (m_profile.id.isEmpty())
        m_profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (m_profile.deviceId.isEmpty())
        m_profile.deviceId = ServerProfile::generateDeviceId();
    if (m_profile.deviceName.isEmpty())
        m_profile.deviceName = QSysInfo::machineHostName();
    if (m_profile.clientName.isEmpty())
        m_profile.clientName = QStringLiteral("EmbyPC");
    if (m_profile.clientVersion.isEmpty())
        m_profile.clientVersion = QStringLiteral("0.1.0");
}

void EmbyClient::setServer(const QString &baseUrl, const QString &accessToken)
{
    m_profile.url = baseUrl.trimmed();
    m_profile.accessToken = accessToken.trimmed();
    if (accessToken.isEmpty()) {
        m_profile.userId.clear();
        m_profile.userName.clear();
    }
    if (m_profile.deviceId.isEmpty())
        m_profile.deviceId = ServerProfile::generateDeviceId();
}

void EmbyClient::setAccessToken(const QString &accessToken)
{
    m_profile.accessToken = accessToken.trimmed();
    if (!m_profile.accessToken.isEmpty())
        m_authExpiredNotified = false;
}

void EmbyClient::setUserId(const QString &userId, const QString &userName)
{
    m_profile.userId = userId;
    if (!userName.isEmpty())
        m_profile.userName = userName;
}

void EmbyClient::setDeviceProfileOptions(const DeviceProfileOptions &options)
{
    m_deviceProfile = options;
}

QString EmbyClient::authorizationHeader() const
{
    QString deviceName = m_profile.deviceName;
    if (deviceName.isEmpty())
        deviceName = QSysInfo::machineHostName();
    if (deviceName.isEmpty())
        deviceName = QStringLiteral("PC");
    QString value = QStringLiteral("MediaBrowser Client=\"%1\", Device=\"%2\", "
                                   "DeviceId=\"%3\", Version=\"%4\"")
                        .arg(escapeQuoted(m_profile.clientName), escapeQuoted(deviceName),
                             escapeQuoted(m_profile.deviceId),
                             escapeQuoted(m_profile.clientVersion));
    if (!m_profile.accessToken.isEmpty()) {
        value += QStringLiteral(", Token=\"%1\"")
                     .arg(escapeQuoted(m_profile.accessToken));
    }
    return value;
}

QList<QPair<QString, QString>> EmbyClient::streamHeaders() const
{
    QList<QPair<QString, QString>> headers;
    headers.append(qMakePair(QStringLiteral("Authorization"), authorizationHeader()));
    if (m_profile.serverType == ServerType::Emby && m_profile.legacyEmbyHeaders
        && !m_profile.accessToken.isEmpty()) {
        headers.append(qMakePair(QStringLiteral("X-Emby-Token"), m_profile.accessToken));
    }
    return headers;
}

QStringList EmbyClient::streamHeaderLines() const
{
    QStringList lines;
    const auto headers = streamHeaders();
    lines.reserve(headers.size());
    for (const auto &header : headers)
        lines.append(header.first + QStringLiteral(": ") + header.second);
    return lines;
}

QString EmbyClient::mpvHeaderFields() const
{
    QStringList escaped;
    const auto headers = streamHeaders();
    escaped.reserve(headers.size());
    for (const auto &header : headers) {
        escaped.append(escapeMpvListValue(header.first + QStringLiteral(": ") + header.second));
    }
    return escaped.join(QLatin1Char(','));
}

bool EmbyClient::isSameOrigin(const QUrl &target) const
{
    const QUrl base(m_profile.baseUrl());
    if (!base.isValid() || base.scheme().isEmpty() || base.host().isEmpty())
        return false;

    QUrl resolved = target;
    if (resolved.isRelative())
        resolved = base.resolved(target);
    if (!resolved.isValid() || resolved.scheme().isEmpty() || resolved.host().isEmpty())
        return false;

    const QString scheme = resolved.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    if (scheme != base.scheme().toLower())
        return false;
    if (resolved.host().compare(base.host(), Qt::CaseInsensitive) != 0)
        return false;

    const int targetPort = resolved.port(scheme == QLatin1String("https") ? 443 : 80);
    const int basePort = base.port(base.scheme().toLower() == QLatin1String("https") ? 443 : 80);
    return targetPort == basePort;
}

QList<QPair<QString, QString>> EmbyClient::streamHeaders(const QUrl &target) const
{
    if (!isSameOrigin(target))
        return {};
    return streamHeaders();
}

QStringList EmbyClient::streamHeaderLines(const QUrl &target) const
{
    const auto headers = streamHeaders(target);
    QStringList lines;
    lines.reserve(headers.size());
    for (const auto &header : headers)
        lines.append(header.first + QStringLiteral(": ") + header.second);
    return lines;
}

QString EmbyClient::mpvHeaderFields(const QUrl &target) const
{
    QStringList escaped;
    const auto headers = streamHeaders(target);
    escaped.reserve(headers.size());
    for (const auto &header : headers)
        escaped.append(escapeMpvListValue(header.first + QStringLiteral(": ") + header.second));
    return escaped.join(QLatin1Char(','));
}

void EmbyClient::clearAuthentication()
{
    m_profile.accessToken.clear();
    m_profile.userId.clear();
    m_profile.userName.clear();
    m_authExpiredNotified = true;
}

QUrl EmbyClient::buildUrl(const QString &path, const QUrlQuery &query) const
{
    const QString base = m_profile.baseUrl();
    if (base.isEmpty())
        return {};
    QUrl url(base);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    QString basePath = url.path();
    while (basePath.endsWith(QLatin1Char('/')))
        basePath.chop(1);
    QString relative = path;
    if (!relative.startsWith(QLatin1Char('/')))
        relative.prepend(QLatin1Char('/'));
    url.setPath(basePath + relative);
    url.setQuery(query);
    return url;
}

QNetworkRequest EmbyClient::makeRequest(const QString &path, const QUrlQuery &query) const
{
    QNetworkRequest request(buildUrl(path, query));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("%1/%2").arg(m_profile.clientName, m_profile.clientVersion));
    request.setRawHeader("Accept", "application/json");
    const QByteArray authorization = authorizationHeader().toUtf8();
    request.setRawHeader("Authorization", authorization);
    if (m_profile.serverType == ServerType::Emby && m_profile.legacyEmbyHeaders) {
        request.setRawHeader("X-Emby-Authorization", authorization);
        if (!m_profile.accessToken.isEmpty())
            request.setRawHeader("X-Emby-Token", m_profile.accessToken.toUtf8());
    }
    if (m_requestTimeoutMs > 0)
        request.setTransferTimeout(m_requestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void EmbyClient::trackReply(QNetworkReply *reply)
{
    m_inFlight.insert(reply);
}

void EmbyClient::untrackReply(QNetworkReply *reply)
{
    m_inFlight.remove(reply);
}

bool EmbyClient::ensureConfigured(const QString &operation, JsonHandler handler)
{
    if (isConfigured())
        return true;
    deliverError(operation, QStringLiteral("Server URL is not configured"), std::move(handler));
    return false;
}

void EmbyClient::deliverResult(JsonHandler handler, const QJsonObject &object,
                               const ApiError &error)
{
    if (handler)
        handler(object, error);
}

void EmbyClient::deliverError(const QString &operation, const QString &message,
                              JsonHandler handler)
{
    if (!handler)
        return;
    ApiError error;
    error.operation = operation;
    error.message = message;
    QTimer::singleShot(0, this, [handler = std::move(handler), error]() {
        handler(QJsonObject(), error);
    });
}

void EmbyClient::deliverErrorStatus(const QString &operation, const QString &message,
                                    StatusHandler handler)
{
    if (!handler)
        return;
    ApiError error;
    error.operation = operation;
    error.message = message;
    QTimer::singleShot(0, this, [handler = std::move(handler), error]() { handler(error); });
}

void EmbyClient::handleReply(QNetworkReply *reply, const QString &operation,
                             JsonHandler handler)
{
    const QByteArray data = reply->readAll();
    QJsonObject object;
    bool parsed = true;
    if (!data.trimmed().isEmpty()) {
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error == QJsonParseError::NoError) {
            if (document.isObject())
                object = document.object();
        } else {
            parsed = false;
        }
    }

    ApiError error;
    error.operation = operation;
    error.httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    error.networkError = reply->error();
    error.payload = object;

    if (reply->error() != QNetworkReply::NoError) {
        error.message = jsonErrorMessage(object);
        if (error.message.isEmpty())
            error.message = reply->errorString();
    } else if (error.httpStatus >= 400) {
        error.message = jsonErrorMessage(object);
        if (error.message.isEmpty())
            error.message = QStringLiteral("HTTP %1").arg(error.httpStatus);
    } else if (!parsed) {
        error.message = QStringLiteral("Invalid JSON response from server");
    }

    if ((error.httpStatus == 401 || error.httpStatus == 403)
        && operation != QLatin1String("AuthenticateByName") && !m_authExpiredNotified) {
        m_authExpiredNotified = true;
        emit authenticationExpired(error.httpStatus, operation, error.message);
    }

    deliverResult(std::move(handler), object, error);
}

QNetworkReply *EmbyClient::sendGet(const QString &path, const QUrlQuery &query,
                                   const QString &operation, JsonHandler handler)
{
    if (!ensureConfigured(operation, handler))
        return nullptr;
    QNetworkReply *reply = m_network->get(makeRequest(path, query));
    trackReply(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, operation, handler = std::move(handler)]() mutable {
                untrackReply(reply);
                handleReply(reply, operation, std::move(handler));
                reply->deleteLater();
            });
    if (m_profile.ignoreSslErrors)
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
            reply->ignoreSslErrors();
        });
    return reply;
}

QNetworkReply *EmbyClient::sendPost(const QString &path, const QUrlQuery &query,
                                    const QJsonObject &body, const QString &operation,
                                    JsonHandler handler)
{
    if (!ensureConfigured(operation, handler))
        return nullptr;
    QNetworkRequest request = makeRequest(path, query);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply =
        m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    trackReply(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, operation, handler = std::move(handler)]() mutable {
                untrackReply(reply);
                handleReply(reply, operation, std::move(handler));
                reply->deleteLater();
            });
    if (m_profile.ignoreSslErrors)
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
            reply->ignoreSslErrors();
        });
    return reply;
}

QNetworkReply *EmbyClient::authenticate(const QString &username, const QString &password,
                                        AuthHandler handler)
{
    QJsonObject body;
    body.insert(QStringLiteral("Username"), username);
    body.insert(QStringLiteral("Pw"), password);
    return sendPost(
        QStringLiteral("/Users/AuthenticateByName"), QUrlQuery(), body,
        QStringLiteral("AuthenticateByName"),
        [this, handler = std::move(handler)](const QJsonObject &object,
                                             const ApiError &error) {
            if (error.isError()) {
                if (handler)
                    handler(AuthResult(), error);
                return;
            }
            AuthResult result = AuthResult::fromJson(object);
            if (!result.isValid()) {
                ApiError invalid;
                invalid.operation = QStringLiteral("AuthenticateByName");
                invalid.httpStatus = error.httpStatus;
                invalid.message =
                    QStringLiteral("Authentication response is missing AccessToken or User.Id");
                invalid.payload = object;
                if (handler)
                    handler(AuthResult(), invalid);
                return;
            }
            m_profile.accessToken = result.accessToken;
            m_profile.userId = result.user.id;
            m_profile.userName = result.user.name;
            m_authExpiredNotified = false;
            if (!result.serverId.isEmpty())
                m_profile.serverId = result.serverId;
            if (handler)
                handler(result, error);
        });
}

QNetworkReply *EmbyClient::fetchPublicSystemInfo(ServerInfoHandler handler)
{
    return sendGet(QStringLiteral("/System/Info/Public"), QUrlQuery(),
                   QStringLiteral("SystemInfoPublic"),
                   [handler = std::move(handler)](const QJsonObject &object,
                                                  const ApiError &error) {
                       if (handler)
                           handler(ServerInfo::fromJson(object), error);
                   });
}

QNetworkReply *EmbyClient::fetchViews(ItemListHandler handler)
{
    const QString operation = QStringLiteral("Views");
    if (m_profile.userId.trimmed().isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Not authenticated: missing user id"),
                       std::move(handler), ItemQueryResult());
        return nullptr;
    }
    const QString path = QStringLiteral("/Users/%1/Views").arg(m_profile.userId);
    return sendGet(path, QUrlQuery(), operation,
                   [handler = std::move(handler)](const QJsonObject &object,
                                                  const ApiError &error) {
                       if (handler)
                           handler(ItemQueryResult::fromJson(object), error);
                   });
}

QNetworkReply *EmbyClient::fetchItems(const ItemQuery &query, ItemListHandler handler)
{
    const QString operation = QStringLiteral("Items");
    if (m_profile.userId.trimmed().isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Not authenticated: missing user id"),
                       std::move(handler), ItemQueryResult());
        return nullptr;
    }
    const QString path = QStringLiteral("/Users/%1/Items").arg(m_profile.userId);
    return sendGet(path, query.toQuery(), operation,
                   [handler = std::move(handler)](const QJsonObject &object,
                                                  const ApiError &error) {
                       if (handler)
                           handler(ItemQueryResult::fromJson(object), error);
                   });
}

QNetworkReply *EmbyClient::fetchItem(const QString &itemId, ItemHandler handler)
{
    const QString operation = QStringLiteral("ItemDetails");
    if (m_profile.userId.trimmed().isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Not authenticated: missing user id"),
                       std::move(handler), MediaItem());
        return nullptr;
    }
    if (itemId.isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Item id is required"),
                       std::move(handler), MediaItem());
        return nullptr;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("MediaSources,MediaStreams,Overview,UserData,ProductionYear,"
                       "PrimaryImageAspectRatio,DateCreated,OfficialRating,CommunityRating,"
                       "Genres,RunTimeTicks,Container,Path"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    const QString path = QStringLiteral("/Users/%1/Items/%2").arg(m_profile.userId, itemId);
    return sendGet(path, query, operation,
                   [handler = std::move(handler)](const QJsonObject &object,
                                                  const ApiError &error) {
                       if (handler)
                           handler(MediaItem::fromJson(object), error);
                   });
}

QNetworkReply *EmbyClient::fetchPlaybackInfo(const QString &itemId,
                                            PlaybackInfoHandler handler)
{
    return fetchPlaybackInfo(itemId, m_deviceProfile, std::move(handler));
}

QNetworkReply *EmbyClient::fetchPlaybackInfo(const QString &itemId,
                                            const DeviceProfileOptions &options,
                                            PlaybackInfoHandler handler)
{
    const QString operation = QStringLiteral("PlaybackInfo");
    if (m_profile.userId.trimmed().isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Not authenticated: missing user id"),
                       std::move(handler), PlaybackInfoResult());
        return nullptr;
    }
    if (itemId.isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Item id is required"),
                       std::move(handler), PlaybackInfoResult());
        return nullptr;
    }

    QJsonObject body;
    body.insert(QStringLiteral("UserId"), m_profile.userId);
    body.insert(QStringLiteral("StartTimeTicks"), 0.0);
    body.insert(QStringLiteral("IsPlayback"), true);
    body.insert(QStringLiteral("AutoOpenLiveStream"), false);
    body.insert(QStringLiteral("MaxStreamingBitrate"),
                static_cast<double>(options.maxStreamingBitrate));
    body.insert(QStringLiteral("EnableDirectPlay"), options.enableDirectPlay);
    body.insert(QStringLiteral("EnableDirectStream"), options.enableDirectStream);
    body.insert(QStringLiteral("EnableTranscoding"), options.enableTranscoding);
    body.insert(QStringLiteral("DeviceProfile"), DeviceProfile(options).toJson());

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), m_profile.userId);
    return sendPost(QStringLiteral("/Items/%1/PlaybackInfo").arg(itemId), query, body,
                    operation,
                    [handler = std::move(handler)](const QJsonObject &object,
                                                   const ApiError &error) {
                        if (handler)
                            handler(PlaybackInfoResult::fromJson(object), error);
                    });
}

QNetworkReply *EmbyClient::fetchResumeItems(int limit, int startIndex,
                                           ItemListHandler handler)
{
    const QString operation = QStringLiteral("ResumeItems");
    if (m_profile.userId.trimmed().isEmpty()) {
        postTypedError(this, operation, QStringLiteral("Not authenticated: missing user id"),
                       std::move(handler), ItemQueryResult());
        return nullptr;
    }
    QUrlQuery query;
    if (limit > 0)
        query.addQueryItem(QStringLiteral("Limit"), QString::number(limit));
    if (startIndex > 0)
        query.addQueryItem(QStringLiteral("StartIndex"), QString::number(startIndex));
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("MediaSources,MediaStreams,Overview,UserData,ProductionYear,"
                       "PrimaryImageAspectRatio,DateCreated,OfficialRating,CommunityRating,"
                       "Genres,RunTimeTicks,Container,Path"));
    query.addQueryItem(QStringLiteral("MediaTypes"), QStringLiteral("Video"));
    query.addQueryItem(QStringLiteral("EnableImageTypes"),
                       QStringLiteral("Primary,Backdrop,Thumb"));
    query.addQueryItem(QStringLiteral("ImageTypeLimit"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    const QString path = QStringLiteral("/Users/%1/Items/Resume").arg(m_profile.userId);
    return sendGet(path, query, operation,
                   [handler = std::move(handler)](const QJsonObject &object,
                                                  const ApiError &error) {
                       if (handler)
                           handler(ItemQueryResult::fromJson(object), error);
                   });
}

QUrl EmbyClient::directStreamUrl(const QString &itemId,
                                 const QString &mediaSourceId) const
{
    if (itemId.isEmpty())
        return {};
    QUrl url = buildUrl(QStringLiteral("/Videos/%1/stream").arg(itemId));
    if (!url.isValid())
        return {};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
    if (!mediaSourceId.isEmpty())
        query.addQueryItem(QStringLiteral("mediaSourceId"), mediaSourceId);
    url.setQuery(query);
    return url;
}

QUrl EmbyClient::resolveMediaUrl(const QString &rawUrl) const
{
    const QString raw = rawUrl.trimmed();
    if (raw.isEmpty())
        return {};
    QUrl url(raw);
    if (url.scheme().isEmpty()) {
        const QUrlQuery originalQuery(url);
        const QString path = url.path();
        url = buildUrl(path.startsWith(QLatin1Char('/')) ? path : QStringLiteral("/") + path);
        if (!originalQuery.isEmpty())
            url.setQuery(originalQuery);
    }
    if (!url.isValid())
        return {};
    QUrlQuery query(url);
    bool changed = false;
    removeTokenQueryItems(query, &changed);
    if (changed)
        url.setQuery(query);
    return url;
}

QUrl EmbyClient::imageUrl(const QString &itemId, const QString &imageType,
                           const QString &imageTag, int maxWidth, int maxHeight) const
{
    if (itemId.isEmpty() || imageType.isEmpty())
        return {};
    QUrl url = buildUrl(QStringLiteral("/Items/%1/Images/%2").arg(itemId, imageType));
    if (!url.isValid())
        return {};
    QUrlQuery query;
    if (!imageTag.isEmpty())
        query.addQueryItem(QStringLiteral("tag"), imageTag);
    if (maxWidth > 0)
        query.addQueryItem(QStringLiteral("maxWidth"), QString::number(maxWidth));
    if (maxHeight > 0)
        query.addQueryItem(QStringLiteral("maxHeight"), QString::number(maxHeight));
    if (!query.isEmpty())
        url.setQuery(query);
    return url;
}

QUrl EmbyClient::playbackUrl(const QString &itemId, const MediaSourceInfo &source) const
{
    // T48：SupportsDirectPlay 时优先使用服务器给的 DirectStreamUrl（实体文件通常是
    // /videos/{id}/original.mp4?DeviceId=...&MediaSourceId=...&PlaySessionId=...）。
    // 以前无条件用自拼 static URL，实体库因此可能失败而 strm 正常。
    // resolveMediaUrl 会解析相对路径并只去掉 token query，保留 DeviceId/MediaSourceId/
    // PlaySessionId。
    if (source.supportsDirectPlay) {
        if (!source.directStreamUrl.trimmed().isEmpty()) {
            const QUrl serverUrl = resolveMediaUrl(source.directStreamUrl);
            if (!serverUrl.isEmpty() && serverUrl.isValid())
                return serverUrl;
        }
        if (!source.id.isEmpty())
            return directStreamUrl(itemId, source.id);
    }

    const QString negotiated =
        !source.directStreamUrl.isEmpty() ? source.directStreamUrl : source.transcodingUrl;
    if (negotiated.isEmpty())
        return {};

    QUrl url(negotiated);
    if (url.scheme().isEmpty()) {
        const QUrlQuery originalQuery(url);
        const QString path = url.path();
        url = buildUrl(path.startsWith(QLatin1Char('/')) ? path
                                                          : QStringLiteral("/") + path);
        if (!originalQuery.isEmpty())
            url.setQuery(originalQuery);
    }
    if (!url.isValid())
        return {};

    QUrlQuery query(url);
    bool changed = false;
    removeTokenQueryItems(query, &changed);
    if (changed)
        url.setQuery(query);
    return url;
}

QNetworkReply *EmbyClient::reportPlaybackStart(const PlaybackProgressInfo &info,
                                               StatusHandler handler)
{
    const QString operation = QStringLiteral("PlaybackStart");
    if (info.itemId.isEmpty()) {
        deliverErrorStatus(operation, QStringLiteral("ItemId is required"), handler);
        return nullptr;
    }
    return sendPost(QStringLiteral("/Sessions/Playing"), QUrlQuery(), info.toJson(), operation,
                    [handler = std::move(handler)](const QJsonObject &, const ApiError &error) {
                        if (handler)
                            handler(error);
                    });
}

QNetworkReply *EmbyClient::reportPlaybackProgress(const PlaybackProgressInfo &info,
                                                  StatusHandler handler)
{
    const QString operation = QStringLiteral("PlaybackProgress");
    if (info.itemId.isEmpty()) {
        deliverErrorStatus(operation, QStringLiteral("ItemId is required"), handler);
        return nullptr;
    }
    PlaybackProgressInfo progress = info;
    if (progress.eventName.isEmpty())
        progress.eventName = QStringLiteral("TimeUpdate");
    return sendPost(QStringLiteral("/Sessions/Playing/Progress"), QUrlQuery(),
                    progress.toJson(), operation,
                    [handler = std::move(handler)](const QJsonObject &, const ApiError &error) {
                        if (handler)
                            handler(error);
                    });
}

QNetworkReply *EmbyClient::reportPlaybackStopped(const PlaybackProgressInfo &info,
                                                 StatusHandler handler)
{
    const QString operation = QStringLiteral("PlaybackStopped");
    if (info.itemId.isEmpty()) {
        deliverErrorStatus(operation, QStringLiteral("ItemId is required"), handler);
        return nullptr;
    }
    return sendPost(QStringLiteral("/Sessions/Playing/Stopped"), QUrlQuery(), info.toJson(),
                    operation,
                    [handler = std::move(handler)](const QJsonObject &, const ApiError &error) {
                        if (handler)
                            handler(error);
                    });
}

QNetworkReply *EmbyClient::getJson(const QString &path, const QUrlQuery &query,
                                   JsonHandler handler)
{
    return sendGet(path, query, QStringLiteral("GET ") + path, std::move(handler));
}

QNetworkReply *EmbyClient::postJson(const QString &path, const QJsonObject &body,
                                    JsonHandler handler)
{
    return sendPost(path, QUrlQuery(), body, QStringLiteral("POST ") + path,
                    std::move(handler));
}

void EmbyClient::abortAll()
{
    const QSet<QNetworkReply *> replies = m_inFlight;
    for (QNetworkReply *reply : replies)
        reply->abort();
}

}  // namespace emby
