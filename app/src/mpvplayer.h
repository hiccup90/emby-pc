#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

struct mpv_handle;
struct mpv_event;

// libmpv 的轻量封装：动态加载 libmpv-2.dll，通过 --wid 嵌入原生子窗口。
class MpvPlayer : public QObject
{
    Q_OBJECT
public:
    explicit MpvPlayer(QObject *parent = nullptr);
    ~MpvPlayer() override;

    bool initialize(quintptr windowId, QString *error);
    // Emby 直连播放用：httpHeaderLines 为 "Name: value" 列表，初始化前写入
    // http-header-fields-append（token 不拼进 URL）。
    bool initialize(quintptr windowId, const QStringList &httpHeaderLines, QString *error);
    bool isReady() const { return m_mpv != nullptr; }

    void loadPlaylist(const QStringList &files, int startIndex = 0);
    void togglePause();
    void seekRelative(double seconds);
    void seekAbsolute(double seconds);
    void addVolume(int delta);
    void setVolume(int volume);
    void playlistNext();
    void playlistPrevious();
    void toggleStats();
    void stop();

    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool paused() const { return m_paused; }
    QString mediaTitle() const { return m_title; }

    // 通用属性/命令入口：media-title/force-media-title、sid/aid、track-list 等。
    bool setPropertyString(const char *name, const QString &value);
    QString getPropertyString(const char *name) const;
    bool command(const QStringList &args);

signals:
    void positionChanged(double position);
    void durationChanged(double duration);
    void pausedChanged(bool paused);
    void titleChanged(const QString &title);
    void fileLoaded();
    void endReached();
    void toggleFullscreenRequested();
    void exitFullscreenRequested();
    void errorOccurred(const QString &message);

private:
    void pollEvents();
    void handleEvent(mpv_event *event);
    bool setOption(const char *name, const QString &value);

    mpv_handle *m_mpv = nullptr;
    QTimer m_timer;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_paused = false;
    QString m_title;
};
