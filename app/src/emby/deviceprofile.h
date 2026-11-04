#pragma once

// Emby DeviceProfile construction.
//
// The profile describes what this client can play without server-side
// transcoding. The defaults are intentionally "wide direct play": the client
// embeds libmpv with FFmpeg, so Matroska/MP4/TS, H.264/HEVC/AV1/VP9,
// AAC/AC-3/E-AC-3/DTS/TrueHD/FLAC/Opus and ASS/SRT subtitles can all be
// played directly. Every list and every profile group has a switch so a
// future settings page can narrow the profile (or disable transcoding) per
// machine.

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace emby {

struct DeviceProfileOptions {
    QString name = QStringLiteral("EmbyPC");
    qint64 maxStreamingBitrate = 120000000;  // 120 Mbps
    qint64 maxStaticBitrate = 120000000;
    qint64 musicStreamingTranscodingBitrate = 384000;

    // PlaybackInfo request switches (copied into the request body).
    // enableDirectPlay also gates DirectPlayProfiles and enableTranscoding
    // gates TranscodingProfiles inside the DeviceProfile JSON.
    bool enableDirectPlay = true;
    bool enableDirectStream = true;
    bool enableTranscoding = true;

    // Profile-level switches.
    bool includeVideoDirectPlayProfile = true;
    bool includeAudioDirectPlayProfile = true;
    bool includeHlsTranscodingProfile = true;
    bool includeSubtitleProfiles = true;

    // Direct play capabilities.
    QStringList videoContainers{QStringLiteral("mkv"), QStringLiteral("mp4"),
                                QStringLiteral("m4v"), QStringLiteral("ts"),
                                QStringLiteral("mpegts"), QStringLiteral("webm")};
    QStringList videoCodecs{QStringLiteral("h264"), QStringLiteral("hevc"),
                            QStringLiteral("av1"), QStringLiteral("vp9")};
    QStringList audioCodecs{QStringLiteral("aac"), QStringLiteral("ac3"),
                            QStringLiteral("eac3"), QStringLiteral("dts"),
                            QStringLiteral("truehd"), QStringLiteral("flac"),
                            QStringLiteral("opus")};
    QStringList audioContainers{QStringLiteral("mp3"), QStringLiteral("aac"),
                                QStringLiteral("flac"), QStringLiteral("opus"),
                                QStringLiteral("wav")};
    QStringList audioOnlyCodecs{QStringLiteral("mp3"), QStringLiteral("aac"),
                               QStringLiteral("flac"), QStringLiteral("opus"),
                               QStringLiteral("pcm_s16le")};

    // Subtitle formats advertised for direct play (embedded and external).
    QStringList subtitleFormats{QStringLiteral("ass"), QStringLiteral("srt")};
    QStringList subtitleMethods{QStringLiteral("Embed"), QStringLiteral("External")};

    // Fallback HLS transcode profile (only used when the server decides it
    // cannot direct play).
    QString hlsContainer = QStringLiteral("ts");
    QString hlsVideoCodec = QStringLiteral("h264");
    QString hlsAudioCodec = QStringLiteral("aac");
    int hlsMaxAudioChannels = 6;
    int hlsMinSegments = 1;
    bool hlsBreakOnNonKeyFrames = true;

    static DeviceProfileOptions wideDirectPlay() { return DeviceProfileOptions{}; }
};

class DeviceProfile {
public:
    DeviceProfile() = default;
    explicit DeviceProfile(const DeviceProfileOptions &options) : m_options(options) {}

    const DeviceProfileOptions &options() const { return m_options; }
    void setOptions(const DeviceProfileOptions &options) { m_options = options; }

    QJsonObject toJson() const;

    static QJsonObject build(const DeviceProfileOptions &options = DeviceProfileOptions());

private:
    DeviceProfileOptions m_options;
};

}  // namespace emby
