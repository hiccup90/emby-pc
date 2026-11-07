#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>

struct mpv_handle;
struct mpv_event;
struct mpv_render_context;

// libmpv 的轻量封装：动态加载 libmpv-2.dll。
//
// spike/render-d3d11：不再用 `--wid` 嵌入子窗口，而是用 libmpv 的 render API
// （MPV_RENDER_API_TYPE_D3D11）把每一帧渲染进 VideoHost（QRhiWidget）提供的
// D3D11 2D 纹理。设备由 Qt 的 QRhi 创建，mpv 的 d3d11va 解码也跑在同一个
// device 上（render_d3d11.h "Hardware decoding"），因此不需要跨设备共享纹理。
class MpvPlayer : public QObject
{
    Q_OBJECT
public:
    explicit MpvPlayer(QObject *parent = nullptr);
    ~MpvPlayer() override;

    // renderDevice 是 VideoHost 的 ID3D11Device*（以 quintptr 传递）。
    // 传 0 表示 QRhi 还没就绪：先创建 mpv core，等 attachRenderDevice() 再建
    // mpv_render_context；这期间的 loadPlaylist() 会挂起，context 就绪后自动执行。
    bool initialize(quintptr renderDevice, QString *error);
    // Emby 直连播放用：httpHeaderLines 为 "Name: value" 列表，初始化前写入
    // http-header-fields（token 不拼进 URL）。
    bool initialize(quintptr renderDevice, const QStringList &httpHeaderLines, QString *error);
    // T63/A-P0-02：不要在运行中设置 http-header-fields（mpv 日志会记录 token）。
    // 头变化时由 Controller 用 reinitialize 在 log-file 之前重新注入。
    const QStringList &httpHeaderLines() const { return m_httpHeaderLines; }
    // 清空运行时 http-header-fields（空值不含 token；用于退出登录/清空会话）。
    bool clearHttpHeaderFields();
    bool reinitialize(quintptr renderDevice, const QStringList &httpHeaderLines, QString *error);
    void shutdown();
    bool isReady() const { return m_mpv != nullptr; }
    // T63/A-P0-10：hwdec 配置（d3d11va / auto-safe / no），初始化前设置。
    void setHardwareDecoderMode(const QString &mode) { m_hwdecMode = mode; }
    QString hardwareDecoderMode() const { return m_hwdecMode; }
    // spike/render-d3d11：Qt/QRhi 的 ID3D11Device 就绪（或换了设备）时挂接
    // mpv_render_context。重复调用同一设备是 no-op。
    bool attachRenderDevice(quintptr renderDevice, QString *error = nullptr);
    bool hasRenderContext() const { return m_renderContext != nullptr; }
    quintptr renderDevice() const { return m_renderDevice; }

    // 每次 loadPlaylist（Emby 候选）分配一个自增 generation；deferred 的
    // loadfile 也先占用，用于过滤换候选后到达的上一条目 END_FILE。
    quint64 loadGeneration() const { return m_loadGeneration; }

    // 把当前视频帧渲染进 texture（ID3D11Texture2D*，由 Qt 的 QRhi 创建并合成）。
    // forceRedraw=true 用于纹理刚重建（首帧/窗口缩放/DPR 变化）；
    // 返回 true 表示这一帧已提交，调用方不要再用自己的命令覆盖该纹理。
    bool renderFrame(void *texture, bool forceRedraw);

    void loadPlaylist(const QStringList &files, int startIndex = 0);
    void togglePause();
    void toggleMute();
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
    // START_FILE：mpv 已开始打开/探测该条目（网络请求已发出）。
    void loadStarted(quint64 generation);
    // END_FILE：generation 是“触发该事件的条目所属的 load 请求”。换候选后
    // 上一个条目被 abort 的 END_FILE 会用旧 generation，调用方应忽略。
    // reason 为 mpv_end_file_reason，error 为 END_FILE(ERROR) 的文本（可能为空）。
    void endReached(quint64 generation, int reason, const QString &error);
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
    // mpv 有新帧/重绘请求。由 render update callback（mpv 线程）合并后
    // 队列投递到 GUI 线程，VideoHost 收到后 update() 触发一帧渲染。
    void frameUpdate();

private slots:
    void handleRenderUpdate();

private:
    void pollEvents();
    void handleEvent(mpv_event *event);
    bool setOption(const char *name, const QString &value);
    static void renderUpdateCallback(void *callbackContext);
    bool createRenderContext(quintptr renderDevice, QString *error);
    void destroyRenderContext();
    void flushPendingLoad();
    void loadPlaylistNow(const QStringList &files, int startIndex);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_renderContext = nullptr;
    quintptr m_renderDevice = 0;
    QTimer m_timer;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_paused = false;
    QString m_title;
    QStringList m_httpHeaderLines;
    QString m_hwdecMode = QStringLiteral("d3d11va");

    // render update callback -> GUI 线程的合并投递（callback 里不能调 mpv API）。
    std::atomic_bool m_updatePosted{false};
    bool m_renderErrorLogged = false;

    // render context 尚未就绪时挂起的 loadfile（首帧前 QRhi device 可能未就绪）。
    bool m_pendingLoad = false;
    QStringList m_pendingFiles;
    int m_pendingStartIndex = 0;

    // load generation：loadPlaylist 入口自增；START_FILE 记录实际开播的
    // generation，END_FILE 用它区分“上一条目被 replace 掉”的陈旧事件。
    quint64 m_loadGeneration = 0;
    quint64 m_startedGeneration = 0;
    bool m_loadStartPending = false;
    // 本 generation 内最后一条流层 timeout 错误（mpv.log 里的 curl 文本），
    // end->error 只有 "loading failed"，靠它识别 NETWORK_TIMEOUT。
    QString m_lastStreamError;

    // 是否已经真正渲染过视频帧：纹理重建但 mpv 还没出帧时，不要用 dummy
    // frame 去“重画”，交给 VideoHost 清黑更可靠。
    bool m_hasVideoFrame = false;
};
