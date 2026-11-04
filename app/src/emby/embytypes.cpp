#include "embytypes.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QUuid>

namespace emby {

namespace {

QString stringField(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toString();
}

bool boolField(const QJsonObject &obj, const char *key, bool defaultValue = false)
{
    const QJsonValue value = obj.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : defaultValue;
}

int intField(const QJsonObject &obj, const char *key, int defaultValue = 0)
{
    const QJsonValue value = obj.value(QLatin1String(key));
    return value.isDouble() ? value.toInt(defaultValue) : defaultValue;
}

qint64 int64Field(const QJsonObject &obj, const char *key, qint64 defaultValue = 0)
{
    const QJsonValue value = obj.value(QLatin1String(key));
    return value.isDouble() ? value.toInteger(defaultValue) : defaultValue;
}

double doubleField(const QJsonObject &obj, const char *key, double defaultValue = 0.0)
{
    const QJsonValue value = obj.value(QLatin1String(key));
    return value.isDouble() ? value.toDouble(defaultValue) : defaultValue;
}

QStringList stringListField(const QJsonObject &obj, const char *key)
{
    QStringList result;
    const QJsonValue value = obj.value(QLatin1String(key));
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        result.reserve(array.size());
        for (const QJsonValue &entry : array) {
            if (entry.isString())
                result.append(entry.toString());
        }
    } else if (value.isString()) {
        result.append(value.toString());
    }
    return result;
}

QMap<QString, QString> stringMapField(const QJsonObject &obj, const char *key)
{
    QMap<QString, QString> result;
    const QJsonObject map = obj.value(QLatin1String(key)).toObject();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        result.insert(it.key(), it.value().toString());
    return result;
}

QVariantMap variantMapField(const QJsonObject &obj, const char *key)
{
    QVariantMap result;
    const QJsonObject map = obj.value(QLatin1String(key)).toObject();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        result.insert(it.key(), it.value().toVariant());
    return result;
}

QString trimmedOr(const QString &value, const QString &fallback)
{
    return value.isEmpty() ? fallback : value;
}

}  // namespace

QString serverTypeToString(ServerType type)
{
    return type == ServerType::Jellyfin ? QStringLiteral("Jellyfin") : QStringLiteral("Emby");
}

ServerType serverTypeFromString(const QString &value)
{
    return value.compare(QStringLiteral("Jellyfin"), Qt::CaseInsensitive) == 0 ? ServerType::Jellyfin
                                                                               : ServerType::Emby;
}

bool ServerProfile::isValid() const
{
    return !baseUrl().isEmpty();
}

bool ServerProfile::isAuthenticated() const
{
    return isValid() && !accessToken.trimmed().isEmpty();
}

QString ServerProfile::baseUrl() const
{
    QString value = url.trimmed();
    if (value.isEmpty())
        return {};
    if (!value.contains(QStringLiteral("://")))
        value.prepend(QStringLiteral("http://"));
    while (value.endsWith(QLatin1Char('/')))
        value.chop(1);
    return value;
}

QJsonObject ServerProfile::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("Id"), id);
    obj.insert(QStringLiteral("Name"), name);
    obj.insert(QStringLiteral("Url"), url);
    obj.insert(QStringLiteral("ServerType"), serverTypeToString(serverType));
    obj.insert(QStringLiteral("ServerId"), serverId);
    obj.insert(QStringLiteral("UserId"), userId);
    obj.insert(QStringLiteral("UserName"), userName);
    obj.insert(QStringLiteral("AccessToken"), accessToken);
    obj.insert(QStringLiteral("DeviceId"), deviceId);
    obj.insert(QStringLiteral("DeviceName"), deviceName);
    obj.insert(QStringLiteral("ClientName"), clientName);
    obj.insert(QStringLiteral("ClientVersion"), clientVersion);
    obj.insert(QStringLiteral("IgnoreSslErrors"), ignoreSslErrors);
    obj.insert(QStringLiteral("LegacyEmbyHeaders"), legacyEmbyHeaders);
    return obj;
}

ServerProfile ServerProfile::fromJson(const QJsonObject &json)
{
    ServerProfile profile;
    profile.id = stringField(json, "Id");
    profile.name = stringField(json, "Name");
    profile.url = stringField(json, "Url");
    profile.serverType = serverTypeFromString(stringField(json, "ServerType"));
    profile.serverId = stringField(json, "ServerId");
    profile.userId = stringField(json, "UserId");
    profile.userName = stringField(json, "UserName");
    profile.accessToken = stringField(json, "AccessToken");
    profile.deviceId = stringField(json, "DeviceId");
    profile.deviceName = stringField(json, "DeviceName");
    profile.clientName = trimmedOr(stringField(json, "ClientName"), profile.clientName);
    profile.clientVersion = trimmedOr(stringField(json, "ClientVersion"), profile.clientVersion);
    profile.ignoreSslErrors = boolField(json, "IgnoreSslErrors");
    profile.legacyEmbyHeaders = boolField(json, "LegacyEmbyHeaders", true);
    if (profile.id.isEmpty())
        profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (profile.deviceId.isEmpty())
        profile.deviceId = generateDeviceId();
    return profile;
}

QString ServerProfile::generateDeviceId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

EmbyUserPolicy EmbyUserPolicy::fromJson(const QJsonObject &json)
{
    EmbyUserPolicy policy;
    policy.isAdministrator = boolField(json, "IsAdministrator");
    policy.canDownload = boolField(json, "CanDownload");
    policy.canManageServer = boolField(json, "CanManageServer");
    policy.enableAllFolders = boolField(json, "EnableAllFolders", true);
    policy.enabledFolders = stringListField(json, "EnabledFolders");
    policy.blockedTags = stringListField(json, "BlockedTags");
    return policy;
}

QJsonObject EmbyUserPolicy::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("IsAdministrator"), isAdministrator);
    obj.insert(QStringLiteral("CanDownload"), canDownload);
    obj.insert(QStringLiteral("CanManageServer"), canManageServer);
    obj.insert(QStringLiteral("EnableAllFolders"), enableAllFolders);
    obj.insert(QStringLiteral("EnabledFolders"), QJsonArray::fromStringList(enabledFolders));
    obj.insert(QStringLiteral("BlockedTags"), QJsonArray::fromStringList(blockedTags));
    return obj;
}

EmbyUser EmbyUser::fromJson(const QJsonObject &json)
{
    EmbyUser user;
    user.id = stringField(json, "Id");
    user.name = stringField(json, "Name");
    user.serverId = stringField(json, "ServerId");
    user.primaryImageTag = stringField(json, "PrimaryImageTag");
    user.hasPassword = boolField(json, "HasPassword");
    user.hasConfiguredPassword = boolField(json, "HasConfiguredPassword");
    user.isDisabled = boolField(json, "IsDisabled");
    user.policy = EmbyUserPolicy::fromJson(json.value(QStringLiteral("Policy")).toObject());
    user.lastLoginDate = stringField(json, "LastLoginDate");
    user.lastActivityDate = stringField(json, "LastActivityDate");
    return user;
}

bool MediaStream::isHdr() const
{
    const QString range = videoRange.trimmed().toUpper();
    if (range == QStringLiteral("HDR") || range == QStringLiteral("HDR10")
        || range == QStringLiteral("HDR10+") || range == QStringLiteral("HLG")
        || range == QStringLiteral("DOVI") || range == QStringLiteral("DOLBYVISION")) {
        return true;
    }
    const QString extended = extendedVideoType.trimmed().toLower();
    return extended.contains(QStringLiteral("hdr")) || extended.contains(QStringLiteral("dolby"))
        || extended.contains(QStringLiteral("dovi"));
}

MediaStream MediaStream::fromJson(const QJsonObject &json)
{
    MediaStream stream;
    stream.type = stringField(json, "Type");
    stream.codec = stringField(json, "Codec");
    stream.codecTag = stringField(json, "CodecTag");
    stream.profile = stringField(json, "Profile");
    stream.language = stringField(json, "Language");
    stream.title = stringField(json, "Title");
    stream.displayTitle = stringField(json, "DisplayTitle");
    stream.displayLanguage = stringField(json, "DisplayLanguage");
    stream.aspectRatio = stringField(json, "AspectRatio");
    stream.pixelFormat = stringField(json, "PixelFormat");
    stream.channelLayout = stringField(json, "ChannelLayout");
    stream.deliveryMethod = stringField(json, "DeliveryMethod");
    stream.deliveryUrl = stringField(json, "DeliveryUrl");
    stream.path = stringField(json, "Path");
    stream.protocol = stringField(json, "Protocol");
    stream.videoRange = stringField(json, "VideoRange");
    stream.colorSpace = stringField(json, "ColorSpace");
    stream.colorTransfer = stringField(json, "ColorTransfer");
    stream.colorPrimaries = stringField(json, "ColorPrimaries");
    stream.extendedVideoType = stringField(json, "ExtendedVideoType");
    stream.extendedVideoSubType = stringField(json, "ExtendedVideoSubType");

    stream.index = intField(json, "Index", -1);
    stream.level = intField(json, "Level");
    stream.width = intField(json, "Width");
    stream.height = intField(json, "Height");
    stream.bitDepth = intField(json, "BitDepth");
    stream.refFrames = intField(json, "RefFrames");
    stream.channels = intField(json, "Channels");
    stream.sampleRate = intField(json, "SampleRate");
    stream.bitRate = int64Field(json, "BitRate");
    stream.realFrameRate = doubleField(json, "RealFrameRate");

    stream.isInterlaced = boolField(json, "IsInterlaced");
    stream.isDefault = boolField(json, "IsDefault");
    stream.isForced = boolField(json, "IsForced");
    stream.isHearingImpaired = boolField(json, "IsHearingImpaired");
    stream.isExternal = boolField(json, "IsExternal");
    stream.isExternalUrl = boolField(json, "IsExternalUrl");
    stream.isTextSubtitleStream = boolField(json, "IsTextSubtitleStream");
    stream.supportsExternalStream = boolField(json, "SupportsExternalStream");

    if (stream.isSubtitle() && !json.contains(QStringLiteral("IsTextSubtitleStream"))) {
        static const QSet<QString> textCodecs = {
            QStringLiteral("srt"),    QStringLiteral("subrip"), QStringLiteral("ass"),
            QStringLiteral("ssa"),    QStringLiteral("webvtt"), QStringLiteral("vtt"),
            QStringLiteral("mov_text"), QStringLiteral("ttml")};
        stream.isTextSubtitleStream = textCodecs.contains(stream.codec.toLower());
    }
    return stream;
}

const MediaStream *MediaSourceInfo::videoStream() const
{
    for (const MediaStream &stream : mediaStreams) {
        if (stream.isVideo())
            return &stream;
    }
    return nullptr;
}

const MediaStream *MediaSourceInfo::audioStream() const
{
    for (const MediaStream &stream : mediaStreams) {
        if (stream.isAudio())
            return &stream;
    }
    return nullptr;
}

QList<MediaStream> MediaSourceInfo::subtitleStreams() const
{
    QList<MediaStream> result;
    for (const MediaStream &stream : mediaStreams) {
        if (stream.isSubtitle())
            result.append(stream);
    }
    return result;
}

MediaSourceInfo MediaSourceInfo::fromJson(const QJsonObject &json)
{
    MediaSourceInfo source;
    source.id = stringField(json, "Id");
    source.name = stringField(json, "Name");
    source.path = stringField(json, "Path");
    source.container = stringField(json, "Container");
    source.directStreamUrl = stringField(json, "DirectStreamUrl");
    source.transcodingUrl = stringField(json, "TranscodingUrl");
    source.liveStreamId = stringField(json, "LiveStreamId");
    source.playSessionId = stringField(json, "PlaySessionId");
    source.openToken = stringField(json, "OpenToken");
    source.size = int64Field(json, "Size");
    source.bitrate = int64Field(json, "Bitrate");
    source.runTimeTicks = int64Field(json, "RunTimeTicks");
    source.supportsDirectPlay = boolField(json, "SupportsDirectPlay");
    source.supportsDirectStream = boolField(json, "SupportsDirectStream");
    source.supportsTranscoding = boolField(json, "SupportsTranscoding");
    source.isInfiniteStream = boolField(json, "IsInfiniteStream");
    source.requiresOpening = boolField(json, "RequiresOpening");
    source.requiresClosing = boolField(json, "RequiresClosing");
    source.requiredHttpHeaders = stringMapField(json, "RequiredHttpHeaders");
    source.dateCreated = stringField(json, "DateCreated");
    source.dateModified = stringField(json, "DateModified");

    const QJsonArray streams = json.value(QStringLiteral("MediaStreams")).toArray();
    source.mediaStreams.reserve(streams.size());
    for (const QJsonValue &value : streams)
        source.mediaStreams.append(MediaStream::fromJson(value.toObject()));
    return source;
}

MediaUserData MediaUserData::fromJson(const QJsonObject &json)
{
    MediaUserData data;
    data.isFavorite = boolField(json, "IsFavorite");
    data.played = boolField(json, "Played");
    data.playCount = intField(json, "PlayCount");
    data.unplayedItemCount = intField(json, "UnplayedItemCount");
    data.playbackPositionTicks = int64Field(json, "PlaybackPositionTicks");
    data.playedPercentage = doubleField(json, "PlayedPercentage");
    data.lastPlayedDate = stringField(json, "LastPlayedDate");
    return data;
}

bool MediaItem::isPlayable() const
{
    return !mediaSources.isEmpty() || mediaType.compare(QStringLiteral("Video"), Qt::CaseInsensitive) == 0
        || mediaType.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0;
}

QString MediaItem::primaryImageTagValue() const
{
    return primaryImageTag;
}

QString MediaItem::primaryImageItemIdValue() const
{
    return primaryImageItemId.isEmpty() ? id : primaryImageItemId;
}

MediaItem MediaItem::fromJson(const QJsonObject &json)
{
    MediaItem item;
    item.id = stringField(json, "Id");
    item.name = stringField(json, "Name");
    item.originalTitle = stringField(json, "OriginalTitle");
    item.sortName = stringField(json, "SortName");
    item.type = stringField(json, "Type");
    item.mediaType = stringField(json, "MediaType");
    item.collectionType = stringField(json, "CollectionType");
    item.parentId = stringField(json, "ParentId");
    item.seriesId = stringField(json, "SeriesId");
    item.seriesName = stringField(json, "SeriesName");
    item.seasonId = stringField(json, "SeasonId");
    item.seasonName = stringField(json, "SeasonName");
    item.overview = stringField(json, "Overview");
    item.officialRating = stringField(json, "OfficialRating");
    item.premiereDate = stringField(json, "PremiereDate");
    item.dateCreated = stringField(json, "DateCreated");
    item.container = stringField(json, "Container");
    item.path = stringField(json, "Path");
    item.locationType = stringField(json, "LocationType");
    item.genres = stringListField(json, "Genres");

    const QJsonObject imageTags = json.value(QStringLiteral("ImageTags")).toObject();
    item.primaryImageTag = imageTags.value(QStringLiteral("Primary")).toString();
    item.logoImageTag = imageTags.value(QStringLiteral("Logo")).toString();
    item.thumbImageTag = imageTags.value(QStringLiteral("Thumb")).toString();
    if (item.primaryImageTag.isEmpty())
        item.primaryImageTag = stringField(json, "PrimaryImageTag");
    item.primaryImageItemId = stringField(json, "PrimaryImageItemId");
    item.parentBackdropItemId = stringField(json, "ParentBackdropItemId");
    item.seriesPrimaryImageTag = stringField(json, "SeriesPrimaryImageTag");
    item.backdropImageTags = stringListField(json, "BackdropImageTags");
    item.parentBackdropImageTags = stringListField(json, "ParentBackdropImageTags");
    item.providerIds = variantMapField(json, "ProviderIds");

    item.productionYear = intField(json, "ProductionYear");
    item.indexNumber = intField(json, "IndexNumber");
    item.parentIndexNumber = intField(json, "ParentIndexNumber");
    item.childCount = intField(json, "ChildCount");
    item.recursiveItemCount = intField(json, "RecursiveItemCount");
    item.runTimeTicks = int64Field(json, "RunTimeTicks");
    item.size = int64Field(json, "Size");
    item.bitrate = int64Field(json, "Bitrate");
    item.communityRating = doubleField(json, "CommunityRating");
    item.criticRating = doubleField(json, "CriticRating");
    item.primaryImageAspectRatio = doubleField(json, "PrimaryImageAspectRatio");
    item.isFolder = boolField(json, "IsFolder");

    item.userData = MediaUserData::fromJson(json.value(QStringLiteral("UserData")).toObject());

    const QJsonArray streams = json.value(QStringLiteral("MediaStreams")).toArray();
    item.mediaStreams.reserve(streams.size());
    for (const QJsonValue &value : streams)
        item.mediaStreams.append(MediaStream::fromJson(value.toObject()));

    const QJsonArray sources = json.value(QStringLiteral("MediaSources")).toArray();
    item.mediaSources.reserve(sources.size());
    for (const QJsonValue &value : sources)
        item.mediaSources.append(MediaSourceInfo::fromJson(value.toObject()));
    return item;
}

QUrlQuery ItemQuery::toQuery() const
{
    QUrlQuery query;
    if (!parentId.isEmpty())
        query.addQueryItem(QStringLiteral("ParentId"), parentId);
    if (recursive)
        query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    if (!includeItemTypes.isEmpty())
        query.addQueryItem(QStringLiteral("IncludeItemTypes"), includeItemTypes.join(QLatin1Char(',')));
    if (!excludeItemTypes.isEmpty())
        query.addQueryItem(QStringLiteral("ExcludeItemTypes"), excludeItemTypes.join(QLatin1Char(',')));
    if (!mediaTypes.isEmpty())
        query.addQueryItem(QStringLiteral("MediaTypes"), mediaTypes.join(QLatin1Char(',')));
    if (!fields.isEmpty())
        query.addQueryItem(QStringLiteral("Fields"), fields.join(QLatin1Char(',')));
    if (!sortBy.isEmpty())
        query.addQueryItem(QStringLiteral("SortBy"), sortBy.join(QLatin1Char(',')));
    if (!sortOrder.isEmpty())
        query.addQueryItem(QStringLiteral("SortOrder"), sortOrder);
    if (startIndex > 0)
        query.addQueryItem(QStringLiteral("StartIndex"), QString::number(startIndex));
    if (limit > 0)
        query.addQueryItem(QStringLiteral("Limit"), QString::number(limit));
    if (!searchTerm.isEmpty())
        query.addQueryItem(QStringLiteral("SearchTerm"), searchTerm);
    if (!genres.isEmpty())
        query.addQueryItem(QStringLiteral("Genres"), genres.join(QLatin1Char(',')));
    if (!years.isEmpty())
        query.addQueryItem(QStringLiteral("Years"), years.join(QLatin1Char(',')));
    if (!filters.isEmpty())
        query.addQueryItem(QStringLiteral("Filters"), filters.join(QLatin1Char(',')));
    if (!imageTypes.isEmpty()) {
        query.addQueryItem(QStringLiteral("EnableImageTypes"),
                           imageTypes.join(QLatin1Char(',')));
        if (imageTypeLimit > 0)
            query.addQueryItem(QStringLiteral("ImageTypeLimit"), QString::number(imageTypeLimit));
    }
    if (enableUserData)
        query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    return query;
}

bool ItemQueryResult::hasMore(int pageSize) const
{
    const int count = pageSize > 0 ? pageSize : items.size();
    if (totalRecordCount > 0)
        return startIndex + count < totalRecordCount;
    // Some Emby versions return TotalRecordCount=0 together with a non-empty
    // Items array (observed for SearchTerm requests). In that case a full page
    // means "try the next page" and a short page means "end of list".
    return pageSize > 0 && items.size() >= pageSize;
}

ItemQueryResult ItemQueryResult::fromJson(const QJsonObject &json)
{
    ItemQueryResult result;
    result.totalRecordCount = intField(json, "TotalRecordCount");
    result.startIndex = intField(json, "StartIndex");
    const QJsonArray items = json.value(QStringLiteral("Items")).toArray();
    result.items.reserve(items.size());
    for (const QJsonValue &value : items)
        result.items.append(MediaItem::fromJson(value.toObject()));
    return result;
}

bool ServerInfo::isJellyfin() const
{
    return productName.contains(QStringLiteral("jellyfin"), Qt::CaseInsensitive)
        || serverName.contains(QStringLiteral("jellyfin"), Qt::CaseInsensitive);
}

ServerInfo ServerInfo::fromJson(const QJsonObject &json)
{
    ServerInfo info;
    info.serverName = stringField(json, "ServerName");
    info.serverId = stringField(json, "Id");
    info.version = stringField(json, "Version");
    info.productName = stringField(json, "ProductName");
    info.operatingSystem = stringField(json, "OperatingSystem");
    return info;
}

const MediaSourceInfo *PlaybackInfoResult::mediaSourceById(const QString &id) const
{
    for (const MediaSourceInfo &source : mediaSources) {
        if (source.id == id)
            return &source;
    }
    return nullptr;
}

const MediaSourceInfo *PlaybackInfoResult::preferredMediaSource() const
{
    for (const MediaSourceInfo &source : mediaSources) {
        if (source.supportsDirectPlay)
            return &source;
    }
    return mediaSources.isEmpty() ? nullptr : &mediaSources.first();
}

PlaybackInfoResult PlaybackInfoResult::fromJson(const QJsonObject &json)
{
    PlaybackInfoResult result;
    result.playSessionId = stringField(json, "PlaySessionId");
    const QJsonArray sources = json.value(QStringLiteral("MediaSources")).toArray();
    result.mediaSources.reserve(sources.size());
    for (const QJsonValue &value : sources)
        result.mediaSources.append(MediaSourceInfo::fromJson(value.toObject()));
    return result;
}

AuthResult AuthResult::fromJson(const QJsonObject &json)
{
    AuthResult result;
    result.accessToken = stringField(json, "AccessToken");
    result.serverId = stringField(json, "ServerId");
    result.user = EmbyUser::fromJson(json.value(QStringLiteral("User")).toObject());
    result.sessionId = json.value(QStringLiteral("SessionInfo"))
                           .toObject()
                           .value(QStringLiteral("Id"))
                           .toString();
    if (result.serverId.isEmpty())
        result.serverId = result.user.serverId;
    return result;
}

QJsonObject PlaybackProgressInfo::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("ItemId"), itemId);
    if (!mediaSourceId.isEmpty())
        obj.insert(QStringLiteral("MediaSourceId"), mediaSourceId);
    if (!playSessionId.isEmpty())
        obj.insert(QStringLiteral("PlaySessionId"), playSessionId);
    if (!liveStreamId.isEmpty())
        obj.insert(QStringLiteral("LiveStreamId"), liveStreamId);
    obj.insert(QStringLiteral("PositionTicks"), static_cast<double>(positionTicks));
    obj.insert(QStringLiteral("IsPaused"), isPaused);
    obj.insert(QStringLiteral("IsMuted"), isMuted);
    obj.insert(QStringLiteral("CanSeek"), canSeek);
    obj.insert(QStringLiteral("PlayMethod"), playMethod);
    if (!eventName.isEmpty())
        obj.insert(QStringLiteral("EventName"), eventName);

    QStringList mediaTypes = queueableMediaTypes;
    if (mediaTypes.isEmpty())
        mediaTypes.append(isAudio ? QStringLiteral("Audio") : QStringLiteral("Video"));
    obj.insert(QStringLiteral("QueueableMediaTypes"), QJsonArray::fromStringList(mediaTypes));
    return obj;
}

}  // namespace emby
