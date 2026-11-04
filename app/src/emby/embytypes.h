#pragma once

// Emby/Jellyfin REST API data models.
//
// The structures in this file are plain C++ structs with hand-written JSON
// parsing so that the API layer does not depend on Qt Widgets, QML or any
// code generator. Field names follow the PascalCase names used by Emby.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <QVariantMap>

namespace emby {

enum class ServerType {
    Emby,
    Jellyfin,
};

QString serverTypeToString(ServerType type);
ServerType serverTypeFromString(const QString &value);

/// Local profile of one Emby/Jellyfin server + user.
struct ServerProfile {
    QString id;              ///< Local profile UUID (not the server id).
    QString name;            ///< Human readable profile name.
    QString url;             ///< Base URL, e.g. http://192.168.1.10:8096
    ServerType serverType = ServerType::Emby;
    QString serverId;        ///< Server id returned by /System/Info/Public or auth.
    QString userId;
    QString userName;
    QString accessToken;
    QString deviceId;        ///< Stable per-install device id.
    QString deviceName;      ///< Human readable device name.
    QString clientName = QStringLiteral("EmbyPC");
    QString clientVersion = QStringLiteral("0.1.0");
    bool ignoreSslErrors = false;
    /// Send legacy Emby headers (X-Emby-Authorization / X-Emby-Token) in
    /// addition to the standard Authorization header. Jellyfin 12 removed the
    /// legacy headers, so this is only enabled for Emby by default.
    bool legacyEmbyHeaders = true;

    bool isValid() const;          ///< Base URL is non-empty.
    bool isAuthenticated() const;  ///< Has base URL + access token.
    QString baseUrl() const;       ///< Normalized URL, no trailing slash.

    QJsonObject toJson() const;
    static ServerProfile fromJson(const QJsonObject &json);
    static QString generateDeviceId();
};

struct EmbyUserPolicy {
    bool isAdministrator = false;
    bool canDownload = false;
    bool canManageServer = false;
    bool enableAllFolders = true;
    QStringList enabledFolders;
    QStringList blockedTags;

    static EmbyUserPolicy fromJson(const QJsonObject &json);
    QJsonObject toJson() const;
};

struct EmbyUser {
    QString id;
    QString name;
    QString serverId;
    QString primaryImageTag;
    bool hasPassword = false;
    bool hasConfiguredPassword = false;
    bool isDisabled = false;
    EmbyUserPolicy policy;
    QString lastLoginDate;
    QString lastActivityDate;

    bool isValid() const { return !id.isEmpty(); }
    static EmbyUser fromJson(const QJsonObject &json);
};

/// One media stream (video / audio / subtitle / embedded image).
struct MediaStream {
    QString type;               ///< "Video", "Audio", "Subtitle", "EmbeddedImage"
    QString codec;
    QString codecTag;
    QString profile;
    QString language;
    QString title;
    QString displayTitle;
    QString displayLanguage;
    QString aspectRatio;
    QString pixelFormat;
    QString channelLayout;
    QString deliveryMethod;
    QString deliveryUrl;
    QString path;
    QString protocol;
    QString videoRange;         ///< "SDR", "HDR", ...
    QString colorSpace;
    QString colorTransfer;
    QString colorPrimaries;
    QString extendedVideoType;  ///< e.g. "DolbyVision", "Hdr10"
    QString extendedVideoSubType;

    int index = -1;
    int level = 0;
    int width = 0;
    int height = 0;
    int bitDepth = 0;
    int refFrames = 0;
    int channels = 0;
    int sampleRate = 0;
    qint64 bitRate = 0;
    double realFrameRate = 0.0;

    bool isInterlaced = false;
    bool isDefault = false;
    bool isForced = false;
    bool isHearingImpaired = false;
    bool isExternal = false;
    bool isExternalUrl = false;
    bool isTextSubtitleStream = false;
    bool supportsExternalStream = false;

    bool isVideo() const { return type.compare(QStringLiteral("Video"), Qt::CaseInsensitive) == 0; }
    bool isAudio() const { return type.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0; }
    bool isSubtitle() const { return type.compare(QStringLiteral("Subtitle"), Qt::CaseInsensitive) == 0; }
    bool isHdr() const;

    static MediaStream fromJson(const QJsonObject &json);
};

/// One physical media source of an item (a movie may have several versions).
struct MediaSourceInfo {
    QString id;
    QString name;
    QString path;
    QString container;
    QString directStreamUrl;
    QString transcodingUrl;
    QString liveStreamId;
    QString playSessionId;
    QString openToken;

    qint64 size = 0;
    qint64 bitrate = 0;
    qint64 runTimeTicks = 0;

    bool supportsDirectPlay = false;
    bool supportsDirectStream = false;
    bool supportsTranscoding = false;
    bool isInfiniteStream = false;
    bool requiresOpening = false;
    bool requiresClosing = false;

    QMap<QString, QString> requiredHttpHeaders;
    QList<MediaStream> mediaStreams;
    QString dateCreated;
    QString dateModified;

    const MediaStream *videoStream() const;
    const MediaStream *audioStream() const;
    QList<MediaStream> subtitleStreams() const;

    static MediaSourceInfo fromJson(const QJsonObject &json);
};

struct MediaUserData {
    bool isFavorite = false;
    bool played = false;
    int playCount = 0;
    int unplayedItemCount = 0;
    qint64 playbackPositionTicks = 0;
    double playedPercentage = 0.0;
    QString lastPlayedDate;

    static MediaUserData fromJson(const QJsonObject &json);
};

/// A library item: folder, movie, series, season, episode, ...
struct MediaItem {
    QString id;
    QString name;
    QString originalTitle;
    QString sortName;
    QString type;
    QString mediaType;
    QString collectionType;
    QString parentId;
    QString seriesId;
    QString seriesName;
    QString seasonId;
    QString seasonName;
    QString overview;
    QString officialRating;
    QString premiereDate;
    QString dateCreated;
    QString container;
    QString path;
    QString locationType;
    QStringList genres;

    QString primaryImageTag;
    QString logoImageTag;
    QString thumbImageTag;
    QString primaryImageItemId;
    QString parentBackdropItemId;
    QString seriesPrimaryImageTag;
    QStringList backdropImageTags;
    QStringList parentBackdropImageTags;
    QVariantMap providerIds;

    int productionYear = 0;
    int indexNumber = 0;
    int parentIndexNumber = 0;
    int childCount = 0;
    int recursiveItemCount = 0;

    qint64 runTimeTicks = 0;
    qint64 size = 0;
    qint64 bitrate = 0;

    double communityRating = 0.0;
    double criticRating = 0.0;
    double primaryImageAspectRatio = 0.0;

    bool isFolder = false;

    MediaUserData userData;
    QList<MediaStream> mediaStreams;
    QList<MediaSourceInfo> mediaSources;

    bool isPlayable() const;
    /// The tag to use for /Items/{id}/Images/Primary, if any.
    QString primaryImageTagValue() const;
    /// The item id that owns the primary image (fall back to this item).
    QString primaryImageItemIdValue() const;

    static MediaItem fromJson(const QJsonObject &json);
};

/// Query parameters for GET /Users/{userId}/Items.
struct ItemQuery {
    QString parentId;
    bool recursive = false;
    QStringList includeItemTypes;
    QStringList excludeItemTypes;
    QStringList mediaTypes;
    QStringList fields{QStringLiteral("MediaSources"), QStringLiteral("MediaStreams"),
                       QStringLiteral("Overview"), QStringLiteral("UserData"),
                       QStringLiteral("ProductionYear"), QStringLiteral("PrimaryImageAspectRatio"),
                       QStringLiteral("DateCreated"), QStringLiteral("OfficialRating"),
                       QStringLiteral("CommunityRating")};
    QStringList sortBy{QStringLiteral("SortName")};
    QString sortOrder;  ///< "Ascending" / "Descending"; empty = server default
    int startIndex = 0;
    int limit = 0;      ///< <= 0 means "server default"
    QString searchTerm;
    QStringList genres;
    QStringList years;
    QStringList filters;  ///< IsPlayed, IsUnplayed, IsFavorite, ...
    QStringList imageTypes{QStringLiteral("Primary"), QStringLiteral("Backdrop"),
                           QStringLiteral("Thumb")};
    int imageTypeLimit = 1;
    bool enableUserData = true;

    QUrlQuery toQuery() const;
};

struct ItemQueryResult {
    QList<MediaItem> items;
    int totalRecordCount = 0;
    int startIndex = 0;

    bool hasMore(int pageSize = 0) const;
    static ItemQueryResult fromJson(const QJsonObject &json);
};

struct ServerInfo {
    QString serverName;
    QString serverId;
    QString version;
    QString productName;
    QString operatingSystem;

    bool isJellyfin() const;
    static ServerInfo fromJson(const QJsonObject &json);
};

/// Result of POST /Items/{id}/PlaybackInfo.
struct PlaybackInfoResult {
    QString playSessionId;
    QList<MediaSourceInfo> mediaSources;

    const MediaSourceInfo *mediaSourceById(const QString &id) const;
    /// First source that supports direct play, else the first source.
    const MediaSourceInfo *preferredMediaSource() const;

    static PlaybackInfoResult fromJson(const QJsonObject &json);
};

/// Result of POST /Users/AuthenticateByName.
struct AuthResult {
    EmbyUser user;
    QString accessToken;
    QString serverId;
    QString sessionId;

    bool isValid() const { return !accessToken.isEmpty() && user.isValid(); }
    static AuthResult fromJson(const QJsonObject &json);
};

/// Payload used by /Sessions/Playing, /Sessions/Playing/Progress and
/// /Sessions/Playing/Stopped. The caller owns throttling policy (e.g. report
/// progress every 10 s plus on pause/seek/track change).
struct PlaybackProgressInfo {
    QString itemId;
    QString mediaSourceId;
    QString playSessionId;
    QString liveStreamId;
    qint64 positionTicks = 0;
    bool isPaused = false;
    bool isMuted = false;
    bool canSeek = true;
    bool isAudio = false;
    QString playMethod = QStringLiteral("DirectPlay");
    QString eventName;  ///< TimeUpdate, Pause, Unpause, Seek, ...
    QStringList queueableMediaTypes;

    QJsonObject toJson() const;
};

}  // namespace emby
