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
    // http-header-fields（token 不拼进 URL）。
    bool initialize(quintptr windowId, const QStringList &httpHeaderLines, QString *error);
    // T63/A-P0-02：不要在运行中设置 http-header-fields（mpv 日志会记录 token）。
    // 头变化时由 Controller 用 reinitialize 在 log-file 之前重新注入。
    const QStringList &httpHeaderLines() const { return m_httpHeaderLines; }
    // 清空运行时 http-header-fields（空值不含 token；用于本地播放/退出登录）。
    bool clearHttpHeaderFields();
    bool reinitialize(quintptr windowId, const QStringList &httpHeaderLines, QString *error);
    void shutdown();
    bool isReady() const { return m_mpv != nullptr; }
    // T63/A-P0-10：hwdec 配置（d3d11va / auto-safe / no），初始化前设置。
    void setHardwareDecoderMode(const QString &mode) { m_hwdecMode = mode; }
    QString hardwareDecoderMode() const { return m_hwdecMode; }
    // T63 自测：input.conf 的 N/P/双击绑定（不启动 mpv）。
    static QString inputConfForTest();

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
    // T29：设置/关于页只读信息。hwdec-current 可能随片源变化。
    void hardwareDecoderChanged(const QString &hwdecCurrent, const QString &vo,
                                const QString &gpuApi);
    void libmpvVersionChanged(const QString &version);
    void toggleFullscreenRequested();
    void exitFullscreenRequested();
    // T63/A-P0-05：mpv n/p 键在 Emby 单集播放列表下转发到应用层上下集。
    void nextEpisodeRequested();
    void previousEpisodeRequested();
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
    QStringList m_httpHeaderLines;
    QString m_hwdecMode = QStringLiteral("d3d11va");
};
