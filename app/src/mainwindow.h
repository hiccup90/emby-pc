#pragma once

#include "ui/uitypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QEvent;
class QWidget;
class QTimer;

class VideoHost;
class MpvPlayer;

namespace emby {
class EmbyClient;
struct ApiError;
struct MediaItem;
struct MediaSourceInfo;
struct PlaybackInfoResult;
}

namespace ui {
class MainWindow;
class PlayerPage;
}

// 播放控制层：把 libmpv（render API → QRhiWidget 的 D3D11 纹理）接到 ui::PlayerPage。
//
// 两种来源：
//  - 本地文件/文件夹（文件菜单）；
//  - Emby 条目（T8）：PlaybackInfo -> 直连 URL + Authorization 头 -> mpv，
//    并上报 Playing/Progress/Stopped。
class PlayerController : public QObject
{
    Q_OBJECT
public:
    explicit PlayerController(ui::MainWindow *window, ui::PlayerPage *playerPage,
                              QObject *parent = nullptr);
    ~PlayerController() override;

    // 在 ui::MainWindow 的菜单栏上安装“文件”菜单（打开文件/打开文件夹/退出）。
    void installMenus();

    // T17 自测：从 mpv track-list JSON 解析可用音轨，过滤 codec 为空/非 audio 的轨道。
    // filteredCount 返回被过滤掉的 audio 轨道数（可为 nullptr）。
    static QList<ui::TrackInfo> audioTracksFromTrackList(const QString &json,
                                                         int *filteredCount = nullptr);
    // T63/A-P0-07：统一续播位置规则（关闭续播或 >=95% 从头）。
    static qint64 adjustedResumeTicks(qint64 positionTicks, qint64 durationTicks,
                                     bool autoResume)
    {
        if (!autoResume || positionTicks <= 0 || durationTicks <= 0)
            return 0;
        if (positionTicks >= (durationTicks * 95) / 100)
            return 0;
        return positionTicks;
    }

    // T36 测试钩子（假 Emby 服务器 selftest 使用；不改变生产路径）。
    void requestEpisodeSiblingsForTest(const QString &episodeId);
    int episodeQueueSizeForTest() const { return m_episodeQueue.size(); }
    int episodeIndexForTest() const { return m_episodeIndex; }

public slots:
    void openFile();
    void openFolder();

private:
    struct EpisodeEntry {
        QString itemId;
        QString title;
        int season = 0;
        int episode = 0;
        qint64 runTimeTicks = 0;
        qint64 startPositionTicks = 0;
    };
    struct CachedEpisodeList {
        QList<EpisodeEntry> entries;
        qint64 fetchedAtMs = 0;
    };

    void playFiles(const QStringList &files, int startIndex = 0);
    bool ensurePlayer(const QStringList &httpHeaderLines = QStringList());
    void updatePositionUi();
    void showStatus(const QString &message, int timeoutMs = 3000);
    void ensurePlayerControlsVisible();
    // spike/render-d3d11：VideoHost 的 QRhi 就绪后，用同一个 ID3D11Device
    // 创建 mpv render context；首次播放时 device 可能晚于 ensurePlayer。
    void handleRenderDeviceReady(quintptr device);
    // spike/render-d3d11 诊断：ensurePlayer 后 2 秒仍拿不到 QRhi 设备
    // （通常意味着 QRhiWidget::initialize() 从未执行）时打一条明确日志。
    void armRenderDeviceWaitDiagnostic();
    void preparePlayerOverlays();
    void updatePlayerWindowChrome();
    void scheduleHomeRefresh(int delayMs = 0);
    void applyPlayerSettings();
    void applyMpvSettings();
    QString normalizedSubtitleLanguage() const;
    void updateHardwareInfo();
    bool isDuplicateKeyForward(const QString &name, int source);
    void forwardKeyPressToMpv(const QString &name);
    void logMpvState(const QString &reason);
    void exportDiagnostics();
    QString diagnosticSummary();
    bool eventFilter(QObject *watched, QEvent *event) override;

    // ---- Emby 播放链路 ----
    void connectUiPlaybackSignals();
    void onDetailPlayRequested(const QString &itemId, int season, int episode);
    void onLibraryPlayRequested(const QString &itemId);
    void playEmbyItem(const QString &itemId, const QString &title, int season, int episode,
                      qint64 startPositionTicks = 0);
    void handlePlaybackInfo(const QString &itemId, const QString &title, int season, int episode,
                            qint64 startPositionTicks, const emby::PlaybackInfoResult &info,
                            const emby::ApiError &error);
    void startPlaybackCandidate(int index);
    void tryNextPlaybackCandidate(const QString &reason);
    void handlePlaybackTimeout();
    void handlePlaybackFailure(const QString &reason);
    void showPlaybackErrorDialog(const QString &code, const QString &detail, bool retryable);
    void retryPlaybackAfterFailure();
    void leavePlaybackAfterFailure();
    void resetLocalPlaybackState();
    void applyHardwareFallback(const QString &hwdec);
    QString sanitizedPlaybackUrl(const QString &urlString) const;
    void startEmbyPlayback(const QString &itemId, const QString &title,
                           const emby::MediaSourceInfo &source, const QString &playSessionId,
                           qint64 startPositionTicks);
    void loadEpisodeQueue(const QString &seriesId, const QString &seriesTitle, int season,
                          int episode);
    void playEpisodeAt(int index);
    void stepEmbyEpisode(int delta);
    void requestEpisodeSiblings(const QString &episodeId);
    void applyEpisodeList(const QList<emby::MediaItem> &items, const QString &currentItemId,
                        const QString &seriesId, const QString &seasonId);
    void applyEpisodeEntries(const QList<EpisodeEntry> &entries, const QString &currentItemId,
                             const QString &seriesId, const QString &seasonId);
    void refreshTracks();
    void showPlayerPage();
    void reportPlaybackStart();
    void reportPlaybackProgress(const QString &eventName, qint64 positionMs = -1);
    void reportPlaybackStopped(bool waitForCompletion = false);
    void clearEmbySession();
    void handleApiError(const emby::ApiError &error);

    ui::MainWindow *m_window = nullptr;
    ui::PlayerPage *m_playerPage = nullptr;
    VideoHost *m_videoHost = nullptr;
    MpvPlayer *m_player = nullptr;
    emby::EmbyClient *m_emby = nullptr;
    QTimer *m_progressTimer = nullptr;
    QTimer *m_playbackWatchdog = nullptr;
    QStringList m_playbackCandidates;
    int m_playbackCandidateIndex = 0;
    QString m_currentPlaybackUrl;
    bool m_playbackFileLoaded = false;
    bool m_playbackFailurePrompted = false;
    int m_playbackRetryCount = 0;
    QString m_lastPlaybackErrorCode;
    QString m_lastPlaybackErrorDetail;
    QStringList m_localFiles;
    int m_localStartIndex = 0;
    bool m_localPlayback = false;
    bool m_autoResume = true;
    QString m_hwdecMode = QStringLiteral("auto");
    int m_hwdecFallbackStage = 0;

    QString m_currentSubtitle;
    bool m_surfaceAttached = false;
    bool m_renderDeviceWaitArmed = false;
    QWidget *m_backButton = nullptr;

    // Emby 播放会话状态（m_embyItemId 非空表示当前是 Emby 播放）。
    QString m_embyItemId;
    QString m_embyTitle;
    QString m_mediaSourceId;
    QString m_playSessionId;
    QString m_playMethod;
    qint64 m_embyDurationTicks = 0;
    bool m_reporting = false;
    bool m_autoNextEpisode = true;
    bool m_rememberVolume = true;
    int m_savedVolume = 80;
    QString m_subtitleLanguage;
    int m_requestSerial = 0;
    int m_audioSwitchSerial = 0;
    QString m_lastForwardedKeyName;
    qint64 m_lastForwardedKeyMs = 0;
    int m_lastForwardedKeySource = -1;
    double m_lastKnownPositionSeconds = 0.0;
    double m_pendingSeekSeconds = 0.0;
    qint64 m_lastWindowResizeLogMs = 0;

    QList<EpisodeEntry> m_episodeQueue;
    QHash<QString, CachedEpisodeList> m_episodeCache;
    int m_episodeIndex = -1;
    int m_episodeRequestSerial = 0;
};
