#include "deviceprofile.h"

#include <QJsonArray>
#include <QJsonValue>

namespace emby {

namespace {

QString csv(const QStringList &values)
{
    return values.join(QLatin1Char(','));
}

}  // namespace

QJsonObject DeviceProfile::toJson() const
{
    QJsonObject profile;
    profile.insert(QStringLiteral("Name"), m_options.name);
    profile.insert(QStringLiteral("MaxStreamingBitrate"),
                   static_cast<double>(m_options.maxStreamingBitrate));
    profile.insert(QStringLiteral("MaxStaticBitrate"),
                   static_cast<double>(m_options.maxStaticBitrate));
    profile.insert(QStringLiteral("MusicStreamingTranscodingBitrate"),
                   static_cast<double>(m_options.musicStreamingTranscodingBitrate));

    QJsonArray directPlayProfiles;
    if (m_options.enableDirectPlay && m_options.includeVideoDirectPlayProfile
        && !m_options.videoContainers.isEmpty() && !m_options.videoCodecs.isEmpty()) {
        QJsonObject video;
        video.insert(QStringLiteral("Type"), QStringLiteral("Video"));
        video.insert(QStringLiteral("Container"), csv(m_options.videoContainers));
        video.insert(QStringLiteral("VideoCodec"), csv(m_options.videoCodecs));
        if (!m_options.audioCodecs.isEmpty())
            video.insert(QStringLiteral("AudioCodec"), csv(m_options.audioCodecs));
        directPlayProfiles.append(video);
    }
    if (m_options.enableDirectPlay && m_options.includeAudioDirectPlayProfile
        && !m_options.audioContainers.isEmpty()) {
        QJsonObject audio;
        audio.insert(QStringLiteral("Type"), QStringLiteral("Audio"));
        audio.insert(QStringLiteral("Container"), csv(m_options.audioContainers));
        if (!m_options.audioOnlyCodecs.isEmpty())
            audio.insert(QStringLiteral("AudioCodec"), csv(m_options.audioOnlyCodecs));
        directPlayProfiles.append(audio);
    }
    if (!directPlayProfiles.isEmpty())
        profile.insert(QStringLiteral("DirectPlayProfiles"), directPlayProfiles);

    QJsonArray transcodingProfiles;
    if (m_options.enableTranscoding && m_options.includeHlsTranscodingProfile) {
        QJsonObject hls;
        hls.insert(QStringLiteral("Type"), QStringLiteral("Video"));
        hls.insert(QStringLiteral("Context"), QStringLiteral("Streaming"));
        hls.insert(QStringLiteral("Protocol"), QStringLiteral("hls"));
        hls.insert(QStringLiteral("Container"), m_options.hlsContainer);
        hls.insert(QStringLiteral("VideoCodec"), m_options.hlsVideoCodec);
        hls.insert(QStringLiteral("AudioCodec"), m_options.hlsAudioCodec);
        hls.insert(QStringLiteral("MaxAudioChannels"), m_options.hlsMaxAudioChannels);
        hls.insert(QStringLiteral("MinSegments"), m_options.hlsMinSegments);
        hls.insert(QStringLiteral("BreakOnNonKeyFrames"), m_options.hlsBreakOnNonKeyFrames);
        transcodingProfiles.append(hls);
    }
    if (!transcodingProfiles.isEmpty())
        profile.insert(QStringLiteral("TranscodingProfiles"), transcodingProfiles);

    if (m_options.includeSubtitleProfiles && !m_options.subtitleFormats.isEmpty()) {
        QJsonArray subtitleProfiles;
        const QStringList methods = m_options.subtitleMethods.isEmpty()
            ? QStringList{QStringLiteral("Embed")}
            : m_options.subtitleMethods;
        for (const QString &format : m_options.subtitleFormats) {
            for (const QString &method : methods) {
                QJsonObject subtitle;
                subtitle.insert(QStringLiteral("Format"), format);
                subtitle.insert(QStringLiteral("Method"), method);
                subtitleProfiles.append(subtitle);
            }
        }
        if (!subtitleProfiles.isEmpty())
            profile.insert(QStringLiteral("SubtitleProfiles"), subtitleProfiles);
    }
    return profile;
}

QJsonObject DeviceProfile::build(const DeviceProfileOptions &options)
{
    return DeviceProfile(options).toJson();
}

}  // namespace emby
