#pragma once

#include "ui/uitypes.h"

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
struct MediaSourceInfo;
struct PlaybackInfoResult;
}

namespace ui {
class MainWindow;
class PlayerPage;
}

// 播放控制层：把 libmpv（--wid 嵌入 VideoHost）接到 ui::PlayerPage。
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

    void playFiles(const QStringList &files, int startIndex = 0);
    bool ensurePlayer(const QStringList &httpHeaderLines = QStringList());
    void updatePositionUi();
    void showStatus(const QString &message, int timeoutMs = 3000);
    void ensurePlayerControlsVisible();
    void preparePlayerOverlays();
    void updatePlayerWindowChrome();
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
    void startEmbyPlayback(const QString &itemId, const QString &title,
                           const emby::MediaSourceInfo &source, const QString &playSessionId,
                           qint64 startPositionTicks);
    void loadEpisodeQueue(const QString &seriesId, const QString &seriesTitle, int season,
                          int episode);
    void playEpisodeAt(int index);
    void stepEmbyEpisode(int delta);
    void refreshTracks();
    void showPlayerPage();
    void reportPlaybackStart();
    void reportPlaybackProgress(const QString &eventName, qint64 positionMs = -1);
    void reportPlaybackStopped();
    void clearEmbySession();
    void handleApiError(const emby::ApiError &error);

    ui::MainWindow *m_window = nullptr;
    ui::PlayerPage *m_playerPage = nullptr;
    VideoHost *m_videoHost = nullptr;
    MpvPlayer *m_player = nullptr;
    emby::EmbyClient *m_emby = nullptr;
    QTimer *m_progressTimer = nullptr;

    QString m_currentSubtitle;
    bool m_surfaceAttached = false;
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
    int m_requestSerial = 0;
    int m_audioSwitchSerial = 0;
    double m_pendingSeekSeconds = 0.0;
    qint64 m_lastWindowResizeLogMs = 0;

    QList<EpisodeEntry> m_episodeQueue;
    int m_episodeIndex = -1;
};
