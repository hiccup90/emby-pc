#pragma once

#include "uitypes.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QStackedWidget;

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
    void setControlsVisible(bool visible);

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

private:
    void buildUi();
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
};

} // namespace ui