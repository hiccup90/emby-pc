#pragma once

#include "uitypes.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QWidget>

class QLabel;
class QPaintEvent;
class QPushButton;
class QResizeEvent;
class QSlider;
class QStackedWidget;
class QTimer;

namespace ui {

class VideoHostPlaceholder;

// 播放页容器：视频区域 + 控制条。
// 视频区里的 VideoHostPlaceholder 只是占位，真正嵌入由主窗口/播放器模块通过
// attachVideoSurface() 传入（Windows 下就是 mpv --wid 的原生子窗口宿主）。
//
// 本类只发信号，不直接调用 libmpv：playPauseRequested / previousEpisodeRequested /
// nextEpisodeRequested / seekRequested / subtitleTrackSelected / audioTrackSelected /
// fullscreenRequested / volumeChanged —— 由播放控制层连接。
class PlayerPage : public QWidget
{
    Q_OBJECT
public:
    explicit PlayerPage(QWidget *parent = nullptr);
    ~PlayerPage() override;

    // 挂载真实视频宿主；传 nullptr 恢复占位图。
    // 不接管 surface 的所有权（由主窗口/播放器模块负责生命周期）。
    void attachVideoSurface(QWidget *surface);
    void releaseVideoSurface();
    QWidget *videoSurfaceContainer() const;

    void setEpisodeInfo(const QString &title, const QString &subtitle);
    void setPosition(qint64 positionMs, qint64 durationMs);
    void setPlaying(bool playing);
    bool isPlaying() const { return m_playing; }
    void setSubtitleTracks(const QList<TrackInfo> &tracks);
    void setAudioTracks(const QList<TrackInfo> &tracks);
    void setVolume(int volume);
    int volume() const;
    void setFullscreen(bool fullscreen);

    // T32：队列为空/到首尾时禁用上一集/下一集，并更新 tooltip。
    // 由 ui::MainWindow(mock) 或 PlayerController(Emby 队列) 在切集/加载队列后调用。
    void setEpisodeNavigationEnabled(bool hasPrevious, bool hasNext);

    // 播放器模式：显示/隐藏顶层 overlay，并启停自动隐藏轮询。
    // MainWindow::setPlayerMode() 负责调用。
    void setPlayerModeActive(bool active);
    bool isPlayerModeActive() const { return m_playerModeActive; }

    // 手动显示/隐藏（false 时自动隐藏不会再把浮层弹出来）。
    void setControlsVisible(bool visible);
    bool controlsVisible() const { return m_overlaysVisible; }

    // 自动隐藏：QTimer 轮询 QCursor::pos()，鼠标在主窗口内移动 -> 显示并重置计时；
    // 静止 hideDelay（默认 2500ms）-> 隐藏；暂停时保持显示。
    void setOverlayAutoHide(bool enabled);
    bool overlayAutoHideEnabled() const { return m_autoHide; }
    void setOverlayHideDelay(int milliseconds);
    int overlayHideDelay() const { return m_hideDelayMs; }

    // 测试/无鼠标环境注入光标位置（全局坐标）。
    void setOverlayCursorPositionForTest(const QPoint &globalPos, bool hasPosition = true);

    // true：浮层是独立顶层窗口（默认）；false：EMBY_PC_PLAYER_OVERLAY=0 的页面内兜底。
    bool usesTopLevelOverlay() const { return m_topLevelOverlay; }
    bool overlaysVisible() const { return m_overlaysVisible; }
    QWidget *topOverlay() const { return m_topBar; }
    QWidget *bottomOverlay() const { return m_controlBar; }

signals:
    void backRequested();
    void playPauseRequested();
    void previousEpisodeRequested();
    void nextEpisodeRequested();
    void seekRequested(qint64 positionMs);
    void subtitleTrackSelected(const QString &trackId);
    void audioTrackSelected(const QString &trackId);
    void statsRequested();
    void fullscreenRequested();
    void volumeChanged(int volume);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void createOverlayWindows();
    void ensureHostWindow();
    void applyVideoMargins();
    void syncOverlayGeometry();
    void updateDockedOverlayGeometry();
    void showOverlays();
    void hideOverlays();
    void applyOverlayVisibility();
    void pollOverlayCursor();
    QPoint currentCursorPosition() const;
    QRect hostGlobalRect() const;
    void startAutoHideTimer();
    void stopAutoHideTimer();
    void showTrackMenu(QPushButton *button, const QList<TrackInfo> &tracks, bool audio);

    QWidget *m_topBar = nullptr;
    QWidget *m_controlBar = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_positionLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    QStackedWidget *m_surfaceStack = nullptr;
    VideoHostPlaceholder *m_placeholder = nullptr;
    QWidget *m_surface = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_prevButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_subtitleButton = nullptr;
    QPushButton *m_audioButton = nullptr;
    QPushButton *m_statsButton = nullptr;
    QPushButton *m_fullscreenButton = nullptr;
    QSlider *m_seekSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;

    QList<TrackInfo> m_subtitleTracks;
    QList<TrackInfo> m_audioTracks;
    qint64 m_durationMs = 0;
    bool m_playing = false;
    bool m_fullscreen = false;
    bool m_updatingSlider = false;

    // overlay 模式与自动隐藏状态
    bool m_topLevelOverlay = true;
    bool m_playerModeActive = false;
    bool m_autoHide = true;
    bool m_controlsRequested = true;
    bool m_overlaysVisible = false;
    int m_hideDelayMs = 2500;
    QTimer *m_overlayTimer = nullptr;
    QWidget *m_hostWindow = nullptr;
    QPoint m_lastCursorPos;
    QElapsedTimer m_lastInteraction;
    bool m_hasTestCursor = false;
    QPoint m_testCursorPos;
};

} // namespace ui