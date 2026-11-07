#include "mpvplayer.h"

#include <mpv/client.h>
#include <mpv/render.h>
#ifdef Q_OS_WIN
#include <mpv/render_d3d11.h>
#endif

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <vector>

namespace {

struct MpvApi
{
    QLibrary library;
    decltype(&::mpv_create) create = nullptr;
    decltype(&::mpv_initialize) initialize = nullptr;
    decltype(&::mpv_set_option_string) setOptionString = nullptr;
    decltype(&::mpv_set_property_string) setPropertyString = nullptr;
    decltype(&::mpv_get_property_string) getPropertyString = nullptr;
    decltype(&::mpv_command) command = nullptr;
    decltype(&::mpv_observe_property) observeProperty = nullptr;
    // 只为把流层错误（curl "Timeout was reached"）送到 app.log/用于
    // NETWORK_TIMEOUT 判定；不在必需符号列表里，老构建缺失时降级。
    decltype(&::mpv_request_log_messages) requestLogMessages = nullptr;
    decltype(&::mpv_wait_event) waitEvent = nullptr;
    decltype(&::mpv_free) freeString = nullptr;
    decltype(&::mpv_terminate_destroy) terminateDestroy = nullptr;
    decltype(&::mpv_error_string) errorString = nullptr;
    // spike/render-d3d11：render API（libmpv 0.29+；D3D11 后端要较新的构建）。
    decltype(&::mpv_render_context_create) renderContextCreate = nullptr;
    decltype(&::mpv_render_context_set_update_callback) renderContextSetUpdateCallback = nullptr;
    decltype(&::mpv_render_context_update) renderContextUpdate = nullptr;
    decltype(&::mpv_render_context_render) renderContextRender = nullptr;
    decltype(&::mpv_render_context_report_swap) renderContextReportSwap = nullptr;
    decltype(&::mpv_render_context_free) renderContextFree = nullptr;
};

MpvApi &api()
{
    static MpvApi instance;
    return instance;
}

bool loadApi(QString *error)
{
    MpvApi &a = api();
    // 不能只看 a.create：老版本 libmpv 可能没有 render API 导出，第一次加载
    // 会失败但仍解析到 create，第二次调用就会误判为成功。
    if (a.create && a.renderContextFree)
        return true;

    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/libmpv-2.dll");
    a.library.setFileName(path);
    if (!a.library.load()) {
        if (error)
            *error = QObject::tr("无法加载 libmpv-2.dll：%1（%2）").arg(path, a.library.errorString());
        return false;
    }

    a.create = reinterpret_cast<decltype(a.create)>(a.library.resolve("mpv_create"));
    a.initialize = reinterpret_cast<decltype(a.initialize)>(a.library.resolve("mpv_initialize"));
    a.setOptionString = reinterpret_cast<decltype(a.setOptionString)>(a.library.resolve("mpv_set_option_string"));
    a.setPropertyString = reinterpret_cast<decltype(a.setPropertyString)>(a.library.resolve("mpv_set_property_string"));
    a.getPropertyString = reinterpret_cast<decltype(a.getPropertyString)>(a.library.resolve("mpv_get_property_string"));
    a.command = reinterpret_cast<decltype(a.command)>(a.library.resolve("mpv_command"));
    a.observeProperty = reinterpret_cast<decltype(a.observeProperty)>(a.library.resolve("mpv_observe_property"));
    a.requestLogMessages = reinterpret_cast<decltype(a.requestLogMessages)>(
        a.library.resolve("mpv_request_log_messages"));
    a.waitEvent = reinterpret_cast<decltype(a.waitEvent)>(a.library.resolve("mpv_wait_event"));
    a.freeString = reinterpret_cast<decltype(a.freeString)>(a.library.resolve("mpv_free"));
    a.terminateDestroy = reinterpret_cast<decltype(a.terminateDestroy)>(a.library.resolve("mpv_terminate_destroy"));
    a.errorString = reinterpret_cast<decltype(a.errorString)>(a.library.resolve("mpv_error_string"));
    a.renderContextCreate = reinterpret_cast<decltype(a.renderContextCreate)>(
        a.library.resolve("mpv_render_context_create"));
    a.renderContextSetUpdateCallback = reinterpret_cast<decltype(a.renderContextSetUpdateCallback)>(
        a.library.resolve("mpv_render_context_set_update_callback"));
    a.renderContextUpdate = reinterpret_cast<decltype(a.renderContextUpdate)>(
        a.library.resolve("mpv_render_context_update"));
    a.renderContextRender = reinterpret_cast<decltype(a.renderContextRender)>(
        a.library.resolve("mpv_render_context_render"));
    a.renderContextReportSwap = reinterpret_cast<decltype(a.renderContextReportSwap)>(
        a.library.resolve("mpv_render_context_report_swap"));
    a.renderContextFree = reinterpret_cast<decltype(a.renderContextFree)>(
        a.library.resolve("mpv_render_context_free"));

    if (!a.create || !a.initialize || !a.setOptionString || !a.setPropertyString
        || !a.getPropertyString || !a.command || !a.observeProperty || !a.waitEvent
        || !a.freeString || !a.terminateDestroy || !a.renderContextCreate
        || !a.renderContextSetUpdateCallback || !a.renderContextUpdate
        || !a.renderContextRender || !a.renderContextFree) {
        if (error)
            *error = QObject::tr("libmpv-2.dll 缺少必要的导出符号：%1").arg(path);
        return false;
    }
    return true;
}

// P2：键盘统一由 PlayerPage 的 KeyMap 处理，不再给 mpv 写键位（否则同一按键
// 既进 KeyMap 又进 mpv，Space/N/P 会双触发）。这里只保留鼠标中键探针。
const char kInputConf[] =
    "MBTN_MID script-message app-mouse-probe\n";
// 双击不绑定：由 VideoHost 直接交给 Qt（PlayerPage::fullscreenRequested）。

QString writeInputConf()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/input.conf");
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(kInputConf);
        file.close();
    }
    return path;
}

// mpv 列表型选项以逗号分隔，值内的反斜杠和逗号需要转义。
QString escapeMpvListValue(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size() + 8);
    for (const QChar character : value) {
        if (character == QLatin1Char('\\') || character == QLatin1Char(','))
            escaped.append(QLatin1Char('\\'));
        escaped.append(character);
    }
    return escaped;
}

// T48：日志脱敏。mpv 的 log-message 可能带 http 请求头/URL token，
// 写 app.log 前统一去掉 Authorization/api_key/X-Emby-Token 的值。
QString redactSensitiveLog(const QString &text)
{
    QString redacted = text;
    static const QRegularExpression auth(
        QStringLiteral("(?i)(Authorization\\s*:\\s*)[^\\r\\n]+"));
    redacted.replace(auth, QStringLiteral("\\1<redacted>"));
    static const QRegularExpression bearer(
        QStringLiteral("(?i)(Bearer\\s+)[A-Za-z0-9._~+/=-]+"));
    redacted.replace(bearer, QStringLiteral("\\1<redacted>"));
    static const QRegularExpression tokens(
        QStringLiteral("(?i)((?:api_key|X-Emby-Token|X-MediaBrowser-Token|Token)\\s*[=:]\\s*)[^&,\\s;]+"));
    redacted.replace(tokens, QStringLiteral("\\1<redacted>"));
    return redacted;
}

// MPV_END_FILE_REASON_* -> 稳定可 grep 的名字（日志验收用）。
QString endFileReasonName(int reason)
{
    switch (reason) {
    case MPV_END_FILE_REASON_EOF: return QStringLiteral("eof");
    case MPV_END_FILE_REASON_STOP: return QStringLiteral("stop");
    case MPV_END_FILE_REASON_QUIT: return QStringLiteral("quit");
    case MPV_END_FILE_REASON_ERROR: return QStringLiteral("error");
    case MPV_END_FILE_REASON_REDIRECT: return QStringLiteral("redirect");
    default: return QStringLiteral("unknown");
    }
}

// mpv.log 与 app.log 同目录（QStandardPaths::AppLocalDataLocation），
// 便于 Windows 实机验收时整个日志目录直接打包回传。
QString mpvLogFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString base = dir.isEmpty() ? QDir::homePath() + QStringLiteral("/.emby-pc") : dir;
    QDir().mkpath(base);
    return base + QStringLiteral("/mpv.log");
}

// T63/A-P0-02：上次运行/旧版 verbose 日志可能已含 token；启动时原地脱敏。
void sanitizeExistingMpvLog()
{
    const QString path = mpvLogFilePath();
    QFileInfo info(path);
    if (!info.exists() || info.size() <= 0 || info.size() > 64 * 1024 * 1024)
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    const QString original = QString::fromUtf8(file.readAll());
    file.close();
    const QString redacted = redactSensitiveLog(original);
    if (redacted == original)
        return;
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(redacted.toUtf8());
        file.close();
        qInfo() << "mpv.log sanitized on startup";
    }
}

} // namespace

MpvPlayer::MpvPlayer(QObject *parent)
    : QObject(parent)
{
    sanitizeExistingMpvLog();
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, &MpvPlayer::pollEvents);
}

MpvPlayer::~MpvPlayer()
{
    shutdown();
}

void MpvPlayer::shutdown()
{
    m_timer.stop();
    // render context 必须在 mpv core 之前释放（render.h "Context and handle
    // lifecycle"：core 销毁前没有 free 是未定义行为）。
    destroyRenderContext();
    m_renderDevice = 0;
    if (m_mpv) {
        api().terminateDestroy(m_mpv);
        m_mpv = nullptr;
    }
    m_pendingLoad = false;
    m_pendingFiles.clear();
    m_pendingStartIndex = 0;
    m_loadStartPending = false;
    m_lastStreamError.clear();
    m_updatePosted.store(false);
    m_renderErrorLogged = false;
    m_hasVideoFrame = false;
    m_position = 0.0;
    m_duration = 0.0;
    m_paused = false;
    m_title.clear();
    m_httpHeaderLines.clear();
}

void MpvPlayer::destroyRenderContext()
{
    if (!m_renderContext)
        return;
    // 先摘掉 update callback，避免 free 期间还有回调排队到 GUI 线程。
    if (api().renderContextSetUpdateCallback) {
        api().renderContextSetUpdateCallback(m_renderContext, nullptr, nullptr);
    }
    api().renderContextFree(m_renderContext);
    m_renderContext = nullptr;
    m_hasVideoFrame = false;
    m_updatePosted.store(false);
}

bool MpvPlayer::clearHttpHeaderFields()
{
    if (!m_mpv)
        return false;
    const int rc = api().setPropertyString(m_mpv, "http-header-fields", "");
    m_httpHeaderLines.clear();
    qInfo() << "mpv clear http-header-fields rc=" << rc;
    return rc >= 0;
}

bool MpvPlayer::reinitialize(quintptr renderDevice, const QStringList &httpHeaderLines,
                             QString *error)
{
    qInfo() << "mpv reinitialize: headers" << httpHeaderLines.size()
            << "renderDevice" << renderDevice;
    shutdown();
    return initialize(renderDevice, httpHeaderLines, error);
}

bool MpvPlayer::initialize(quintptr renderDevice, QString *error)
{
    return initialize(renderDevice, QStringList(), error);
}

bool MpvPlayer::initialize(quintptr renderDevice, const QStringList &httpHeaderLines,
                           QString *error)
{
    if (m_mpv)
        shutdown();
    QString loadError;
    if (!loadApi(&loadError)) {
        if (error)
            *error = loadError;
        return false;
    }

    MpvApi &a = api();
    m_mpv = a.create();
    if (!m_mpv) {
        if (error)
            *error = QObject::tr("mpv_create() 失败");
        return false;
    }

    // ---- 视频输出（render API）----
    // spike/render-d3d11：不再设置 wid / force-window / input-vo-keyboard /
    // input-cursor。视频由 libmpv 的 libmpv VO 渲染进 Qt 的 D3D11 纹理。
    setOption("vo", QStringLiteral("libmpv"));
#ifdef Q_OS_WIN
    setOption("gpu-api", QStringLiteral("d3d11"));
    setOption("gpu-context", QStringLiteral("d3d11"));
    setOption("hwdec", m_hwdecMode.isEmpty() ? QStringLiteral("d3d11va") : m_hwdecMode);
#endif
    // render API 在 GUI 线程调用 mpv_render_context_render；默认 50ms 的
    // target-time 等待会卡住 UI（render.h MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME /
    // "video-timing-offset"）。这里置 0，由 Qt 的合成/paint 节流。
    setOption("video-timing-offset", QStringLiteral("0"));
    // HDR/DV 片源统一 tone-map 到 SDR（不发灰）
    setOption("target-colorspace-hint", QStringLiteral("no"));
    setOption("target-trc", QStringLiteral("srgb"));
    setOption("target-prim", QStringLiteral("bt.709"));
    // ---- 性能实验（第三步：只开 cscale + 两个下采样修正的二分）----
    // PERF-EXPERIMENT: 前两步已确认片源是 SDR（prim=bt.709/gamma=bt.1886/sig=0），
    // 且「缩放恢复默认后 GPU 回到 30%」说明约 15% 成本在缩放相关 pass 上。
    // 本步保留 tone-mapping=clip、hdr-compute-peak=no、dither-depth=no（SDR 下
    // tone/peak 本来就是 no-op），只打开三项：cscale=bilinear、
    // correct-downscaling=no、linear-downscaling=no；scale/dscale 保持默认不动。
    // 要回答：GPU 明显下降 -> 成本在色度上采样与两个修正 pass；
    //         仍 30% -> 成本在 dscale 主缩放滤镜本身。
    setOption("tone-mapping", QStringLiteral("clip"));
    setOption("hdr-compute-peak", QStringLiteral("no"));
    setOption("dither-depth", QStringLiteral("no"));
    setOption("cscale", QStringLiteral("bilinear"));
    setOption("correct-downscaling", QStringLiteral("no"));
    setOption("linear-downscaling", QStringLiteral("no"));

    // ---- 字幕 ----
    // 4K 片源下 blend-subtitles=video 会先按视频分辨率（如 3840x2160）把字幕
    // 混进每一帧再做缩放，每帧都付一次 4K 混合成本；改为 no 后字幕不再混入
    // 视频帧，由 libass 渲染后作为 OSD 在输出分辨率叠加。sub-ass-override 保持
    // no，ASS 样式/特效不被覆盖。
    setOption("blend-subtitles", QStringLiteral("no"));
    setOption("sub-ass-override", QStringLiteral("no"));
    setOption("embeddedfonts", QStringLiteral("yes"));
    setOption("sub-auto", QStringLiteral("fuzzy"));

    // ---- OSD / OSC / 输入 ----
    // T15：Qt PlayerControlBar 是主控制条。libmpv 的 builtin scripts 在部分发行包中
    // 会被裁剪/禁用，OSC 不一定存在；默认关闭 OSC，避免用户“看不到任何控制条”。
    // 需要排查 OSC 时可设置环境变量 EMBY_PC_MPV_OSC=1 启用，Qt 控制条仍然保留。
    const bool oscEnabled = qEnvironmentVariableIntValue("EMBY_PC_MPV_OSC") > 0;
    setOption("osc", oscEnabled ? QStringLiteral("yes") : QStringLiteral("no"));
    setOption("osd-bar", QStringLiteral("no"));
    setOption("input-default-bindings", QStringLiteral("yes"));
    setOption("input-conf", writeInputConf());
    if (oscEnabled)
        setOption("script-opts", QStringLiteral("osc-hidetimeout=1800,osc-boxalpha=170"));

    // ---- 隔离用户配置 / 缓存 ----
    setOption("config", QStringLiteral("no"));
    setOption("load-scripts", QStringLiteral("yes"));
    qInfo() << "mpv options: vo=libmpv render-api=d3d11 osc=" << oscEnabled
            << "renderDevice=" << renderDevice
            << "load-scripts=yes video-timing-offset=0";
    setOption("keep-open", QStringLiteral("yes"));
    setOption("idle", QStringLiteral("yes"));
    setOption("terminal", QStringLiteral("no"));

    // ---- Emby 直连 HTTP 头（T8）----
    // Authorization 等头只在 mpv 初始化前通过 http-header-fields 注入，
    // 不拼进播放 URL。
    if (!httpHeaderLines.isEmpty()) {
        QStringList escapedHeaders;
        escapedHeaders.reserve(httpHeaderLines.size());
        for (const QString &line : httpHeaderLines)
            escapedHeaders.append(escapeMpvListValue(line));
        // T50：-append 只存在于 mpv 命令行展开；libmpv API 必须用 http-header-fields。
        const bool headerOptionSet =
            setOption("http-header-fields", escapedHeaders.join(QLatin1Char(',')));
        qInfo() << "mpv option http-header-fields set=" << headerOptionSet
                << "headers" << escapedHeaders.size();
        if (!headerOptionSet) {
            if (error)
                *error = QObject::tr("mpv 不接受 http-header-fields 选项");
            a.terminateDestroy(m_mpv);
            m_mpv = nullptr;
            return false;
        }
    }

    // ---- 文件日志（T9 验收支撑）----
    // log-file 必须在 mpv_initialize() 之前设置；每次启动会截断旧 mpv.log。
    // T63/A-P0-02：默认 info，不再把 header setter/HTTP 细节写进发行日志；
    // 排障时用 EMBY_PC_MPV_VERBOSE=1 临时开 all=v。
    const QString logPath = mpvLogFilePath();
    const QFileInfo logInfo(logPath);
    if (logInfo.exists() && logInfo.size() > 32 * 1024 * 1024)
        QFile::remove(logPath);
    setOption("log-file", logPath);
    const bool verboseLog = qEnvironmentVariableIntValue("EMBY_PC_MPV_VERBOSE") > 0;
    setOption("msg-level", verboseLog ? QStringLiteral("all=v") : QStringLiteral("info"));
    setOption("cache", QStringLiteral("yes"));
    setOption("demuxer-max-bytes", QStringLiteral("512MiB"));
    // T48 后续：连接/读取超时 15s（mpv 默认 60s），比 start-file 后看门狗
    // 重置的 25s 早 10s，保证 curl 的 "Timeout was reached" 先到；失败时
    // 不再拉起 yt-dlp 子进程。
    setOption("network-timeout", QStringLiteral("15"));
    setOption("ytdl", QStringLiteral("no"));

    const int initResult = a.initialize(m_mpv);
    if (initResult < 0) {
        if (error) {
            const char *text = a.errorString ? a.errorString(initResult) : nullptr;
            *error = QObject::tr("mpv_initialize() 失败：%1")
                         .arg(QString::fromUtf8(text ? text : "unknown error"));
        }
        a.terminateDestroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // libmpv 默认不投递任何 log-message 事件（min level = "no"）。不打开的话
    // handleEvent 里的 MPV_EVENT_LOG_MESSAGE 分支永远不会触发，end-file 的
    // error 又只有 "loading failed"，网络超时会被误判成普通失败。这里只要
    // error/fatal 级（terminal=no 不影响客户端日志），避免刷屏。
    if (a.requestLogMessages)
        a.requestLogMessages(m_mpv, "error");

    a.observeProperty(m_mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    a.observeProperty(m_mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    a.observeProperty(m_mpv, 3, "pause", MPV_FORMAT_FLAG);
    a.observeProperty(m_mpv, 4, "media-title", MPV_FORMAT_STRING);
    // T29：hwdec-current 会随片源/硬解初始化变化，设置页只读显示。
    a.observeProperty(m_mpv, 5, "hwdec-current", MPV_FORMAT_STRING);

    // T29：初始化完成即把 libmpv 版本和当前渲染信息提供给设置/关于页。
    emit libmpvVersionChanged(getPropertyString("mpv-version"));
    emit hardwareDecoderChanged(getPropertyString("hwdec-current"),
                                getPropertyString("vo"),
                                getPropertyString("gpu-api"));

    m_httpHeaderLines = httpHeaderLines;

    // render context 必须赶在任何 loadfile（VO 初始化）之前创建：
    // render.h 明确“Video initialization will fail if the render context was not
    // initialized yet”。device 为 0 时先挂起，等 VideoHost 的 QRhi 初始化。
    if (renderDevice != 0) {
        if (!createRenderContext(renderDevice, error)) {
            shutdown();
            return false;
        }
    } else {
        qInfo() << "mpv render context deferred: QRhi D3D11 device not ready yet";
    }

    m_timer.start();
    return true;
}

bool MpvPlayer::createRenderContext(quintptr renderDevice, QString *error)
{
#ifdef Q_OS_WIN
    if (!m_mpv || !api().renderContextCreate) {
        if (error)
            *error = QObject::tr("mpv render context 不可用（mpv 未初始化）");
        return false;
    }
    if (!renderDevice) {
        if (error)
            *error = QObject::tr("QRhi 尚未提供 ID3D11Device");
        return false;
    }
    if (m_renderContext) {
        if (m_renderDevice == renderDevice)
            return true;
        // 设备变了：旧 context 只能销毁重建（同一 mpv core 同时只允许 1 个）。
        qWarning() << "mpv render context device changed" << m_renderDevice << "->"
                   << renderDevice;
        destroyRenderContext();
    }

    // MPV_RENDER_API_TYPE_D3D11 的 data 是 char*；用可写数组避免 const 转换。
    char apiType[] = MPV_RENDER_API_TYPE_D3D11;
    mpv_d3d11_init_params initParams{};
    initParams.device = reinterpret_cast<void *>(renderDevice);
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, apiType},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &initParams},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    mpv_render_context *context = nullptr;
    const int result = api().renderContextCreate(&context, m_mpv, params);
    if (result < 0 || !context) {
        const char *text = api().errorString ? api().errorString(result) : nullptr;
        if (error) {
            *error = QObject::tr("mpv_render_context_create(D3D11) 失败：%1")
                         .arg(QString::fromUtf8(text ? text : "unknown error"));
        }
        qWarning() << "mpv render context create failed rc=" << result
                   << (text ? text : "unknown error");
        return false;
    }

    m_renderContext = context;
    m_renderDevice = renderDevice;
    m_renderErrorLogged = false;
    // 回调在 mpv 线程触发；里面不能调任何 mpv API，只做队列投递。
    api().renderContextSetUpdateCallback(m_renderContext, &MpvPlayer::renderUpdateCallback, this);
    qInfo() << "mpv render context created: api=d3d11 device=" << Qt::hex << renderDevice;
    flushPendingLoad();
    return true;
#else
    Q_UNUSED(renderDevice);
    if (error)
        *error = QObject::tr("D3D11 render API 仅在 Windows 可用");
    return false;
#endif
}

bool MpvPlayer::attachRenderDevice(quintptr renderDevice, QString *error)
{
    if (!m_mpv) {
        if (error)
            *error = QObject::tr("mpv 尚未初始化");
        return false;
    }
    return createRenderContext(renderDevice, error);
}

void MpvPlayer::renderUpdateCallback(void *callbackContext)
{
    auto *self = static_cast<MpvPlayer *>(callbackContext);
    if (!self)
        return;
    // 合并高频回调：只排一个 GUI 线程事件；不阻塞 mpv 线程。
    bool expected = false;
    if (!self->m_updatePosted.compare_exchange_strong(expected, true))
        return;
    QMetaObject::invokeMethod(self, "handleRenderUpdate", Qt::QueuedConnection);
}

void MpvPlayer::handleRenderUpdate()
{
    m_updatePosted.store(false);
    emit frameUpdate();
}

bool MpvPlayer::renderFrame(void *texture, bool forceRedraw)
{
#ifdef Q_OS_WIN
    if (!m_mpv || !m_renderContext || !texture)
        return false;

    // update() 不阻塞，只处理 queued 请求并返回 MPV_RENDER_UPDATE_FRAME bit。
    // 没有新帧且不是纹理重建时，保留上一帧的纹理内容，不重复 blit。
    const uint64_t flags = api().renderContextUpdate(m_renderContext);
    const bool newFrame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    if (!newFrame && !forceRedraw)
        return false;
    if (!newFrame && !m_hasVideoFrame)
        return false; // 纹理刚建、还没出过帧：让 VideoHost 用 QRhi 清黑

    mpv_d3d11_fbo fbo{};
    fbo.tex = texture;
    int blockForTargetTime = 0; // 不要在 Qt GUI 线程上等 target time
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_D3D11_FBO, &fbo},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int result = api().renderContextRender(m_renderContext, params);
    if (result < 0) {
        if (!m_renderErrorLogged) {
            m_renderErrorLogged = true;
            const char *text = api().errorString ? api().errorString(result) : nullptr;
            qWarning() << "mpv_render_context_render(D3D11) failed rc=" << result
                       << (text ? text : "unknown error");
        }
        return false;
    }
    if (newFrame)
        m_hasVideoFrame = true;
    // 有意不调 mpv_render_context_report_swap()：render.h 说明它是 optional，
    // 而且我们的“present”发生在 Qt backingstore 合成时（本函数之后、同一帧内），
    // 在这里报 flip 时间并不准确；不一致地调用反而会让 mpv 的时序判断变差。
    return true;
#else
    Q_UNUSED(texture);
    Q_UNUSED(forceRedraw);
    return false;
#endif
}

bool MpvPlayer::setOption(const char *name, const QString &value)
{
    if (!m_mpv)
        return false;
    const QByteArray utf8 = value.toUtf8();
    return api().setOptionString(m_mpv, name, utf8.constData()) >= 0;
}

bool MpvPlayer::setPropertyString(const char *name, const QString &value)
{
    if (!m_mpv)
        return false;
    const QByteArray utf8 = value.toUtf8();
    return api().setPropertyString(m_mpv, name, utf8.constData()) >= 0;
}

QString MpvPlayer::getPropertyString(const char *name) const
{
    if (!m_mpv || !api().getPropertyString)
        return {};
    char *value = api().getPropertyString(m_mpv, name);
    if (!value)
        return {};
    const QString result = QString::fromUtf8(value);
    if (api().freeString)
        api().freeString(value);
    return result;
}

bool MpvPlayer::command(const QStringList &args)
{
    if (!m_mpv || args.isEmpty())
        return false;

    // Qt6 里 QVector 是 QList 的别名，QList<const char*>::constData() 返回
    // const char* const*，不能直接传给 mpv_command(const char**)，因此用 std::vector
    // 管理指针数组（QByteArray 先全部生成，保证 constData() 指针在调用期间有效）。
    std::vector<QByteArray> storage;
    storage.reserve(static_cast<size_t>(args.size()));
    for (const QString &arg : args)
        storage.push_back(arg.toUtf8());

    std::vector<const char *> argv;
    argv.reserve(storage.size() + 1);
    for (const QByteArray &arg : storage)
        argv.push_back(arg.constData());
    argv.push_back(nullptr);

    return api().command(m_mpv, argv.data()) >= 0;
}

void MpvPlayer::loadPlaylist(const QStringList &files, int startIndex)
{
    if (!m_mpv || files.isEmpty())
        return;

    // 每次 loadfile 请求一个自增 generation。deferred 时也先占用（否则 flush
    // 前 Controller 记下的 generation 会与 START_FILE/END_FILE 对不上）。
    ++m_loadGeneration;
    m_loadStartPending = true;
    m_lastStreamError.clear();

    // render context 未就绪（例如首次播放时 PlayerPage 还没 paint，QRhi 的
    // ID3D11Device 尚不可用）时不能 loadfile：vo=libmpv 的 preinit 会因为没有
    // render context 直接失败。先挂起，等 attachRenderDevice() 后原样执行。
    if (!m_renderContext) {
        m_pendingLoad = true;
        m_pendingFiles = files;
        m_pendingStartIndex = startIndex;
        qInfo() << "mpv loadfile deferred until render context is ready: generation"
                << m_loadGeneration << "files" << files.size()
                << "startIndex" << startIndex;
        return;
    }

    loadPlaylistNow(files, startIndex);
}

void MpvPlayer::flushPendingLoad()
{
    if (!m_pendingLoad || !m_renderContext)
        return;
    m_pendingLoad = false;
    const QStringList files = m_pendingFiles;
    const int startIndex = m_pendingStartIndex;
    m_pendingFiles.clear();
    m_pendingStartIndex = 0;
    qInfo() << "mpv loadfile flush after render context ready: generation"
            << m_loadGeneration << "files" << files.size()
            << "startIndex" << startIndex;
    loadPlaylistNow(files, startIndex);
}

void MpvPlayer::loadPlaylistNow(const QStringList &files, int startIndex)
{
    // 新片源的第一帧到达前不要把上一部的末帧当成“可重画帧”贴到新纹理上。
    m_hasVideoFrame = false;
    m_loadStartPending = true;
    qInfo() << "mpv loadfile issued: generation" << m_loadGeneration
            << "startIndex" << startIndex << "files" << files.size();
    bool ok = command({QStringLiteral("loadfile"), files.first(), QStringLiteral("replace")});
    if (!ok) {
        emit errorOccurred(QStringLiteral("mpv loadfile failed: %1").arg(files.first()));
        return;
    }
    for (int i = 1; i < files.size(); ++i) {
        if (!command({QStringLiteral("loadfile"), files.at(i), QStringLiteral("append")})) {
            emit errorOccurred(QStringLiteral("mpv append failed: %1").arg(files.at(i)));
            return;
        }
    }

    const int index = qBound(0, startIndex, files.size() - 1);
    if (index > 0 && !setPropertyString("playlist-pos", QString::number(index))) {
        emit errorOccurred(QStringLiteral("mpv playlist-pos failed: %1").arg(index));
        return;
    }

    setPropertyString("pause", QStringLiteral("no"));
}

void MpvPlayer::togglePause()
{
    command({QStringLiteral("cycle"), QStringLiteral("pause")});
}

void MpvPlayer::toggleMute()
{
    command({QStringLiteral("cycle"), QStringLiteral("mute")});
}

void MpvPlayer::seekRelative(double seconds)
{
    command({QStringLiteral("seek"), QString::number(seconds), QStringLiteral("relative")});
}

void MpvPlayer::seekAbsolute(double seconds)
{
    command({QStringLiteral("seek"), QString::number(seconds), QStringLiteral("absolute")});
}

void MpvPlayer::addVolume(int delta)
{
    command({QStringLiteral("add"), QStringLiteral("volume"), QString::number(delta)});
}

void MpvPlayer::setVolume(int volume)
{
    setPropertyString("volume", QString::number(volume));
}

void MpvPlayer::playlistNext()
{
    command({QStringLiteral("playlist-next")});
}

void MpvPlayer::playlistPrevious()
{
    command({QStringLiteral("playlist-prev")});
}

void MpvPlayer::toggleStats()
{
    command({QStringLiteral("script-binding"), QStringLiteral("stats/display-stats-toggle")});
}

void MpvPlayer::stop()
{
    // stop 语义就是“不要再开始”：丢掉尚未执行的挂起 loadfile。
    m_pendingLoad = false;
    m_pendingFiles.clear();
    m_pendingStartIndex = 0;
    m_hasVideoFrame = false;
    command({QStringLiteral("stop")});
}

void MpvPlayer::pollEvents()
{
    if (!m_mpv)
        return;

    MpvApi &a = api();
    for (;;) {
        mpv_event *event = a.waitEvent(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        handleEvent(event);
    }
}

void MpvPlayer::handleEvent(mpv_event *event)
{
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
        auto *prop = static_cast<mpv_event_property *>(event->data);
        if (!prop || !prop->name || !prop->data)
            break;

        const QByteArray name(prop->name);
        if (name == "time-pos" && prop->format == MPV_FORMAT_DOUBLE) {
            m_position = *static_cast<double *>(prop->data);
            emit positionChanged(m_position);
        } else if (name == "duration" && prop->format == MPV_FORMAT_DOUBLE) {
            m_duration = *static_cast<double *>(prop->data);
            emit durationChanged(m_duration);
        } else if (name == "pause" && prop->format == MPV_FORMAT_FLAG) {
            m_paused = *static_cast<int *>(prop->data) != 0;
            emit pausedChanged(m_paused);
        } else if (name == "media-title" && prop->format == MPV_FORMAT_STRING) {
            const char *value = *static_cast<char **>(prop->data);
            m_title = QString::fromUtf8(value ? value : "");
            emit titleChanged(m_title);
        } else if (name == "hwdec-current" && prop->format == MPV_FORMAT_STRING) {
            const char *value = *static_cast<char **>(prop->data);
            emit hardwareDecoderChanged(QString::fromUtf8(value ? value : ""),
                                        getPropertyString("vo"),
                                        getPropertyString("gpu-api"));
        }
        break;
    }
    case MPV_EVENT_START_FILE: {
        // 新条目的网络/探测已开始：从此 END_FILE 属于本 generation。
        m_loadStartPending = false;
        m_startedGeneration = m_loadGeneration;
        m_lastStreamError.clear();
        qInfo() << "mpv start-file: generation" << m_loadGeneration;
        emit loadStarted(m_loadGeneration);
        break;
    }
    case MPV_EVENT_FILE_LOADED:
        emit fileLoaded();
        break;
    case MPV_EVENT_END_FILE: {
        auto *end = static_cast<mpv_event_end_file *>(event->data);
        const int reason = end ? static_cast<int>(end->reason) : int(MPV_END_FILE_REASON_EOF);
        const int errorCode = end ? end->error : 0;
        // 新 loadfile 已发出、START_FILE 尚未到达：这个 END_FILE 属于被
        // replace 掉的上一个条目，不能作为当前候选的失败依据。
        const bool stale = m_loadStartPending;
        const quint64 generation = stale ? m_startedGeneration : m_loadGeneration;

        QString errorText;
        bool networkTimeout = false;
        if (reason == MPV_END_FILE_REASON_ERROR && errorCode < 0) {
            const char *text = api().errorString ? api().errorString(errorCode) : nullptr;
            errorText = QString::fromUtf8(text ? text : "unknown");
            // END_FILE 的 error 通常只有 "loading failed"；curl 的
            // "Timeout was reached" 在 LOG_MESSAGE 里（m_lastStreamError）。
            // 这里两处都看，避免把网络超时归成普通解码/加载失败。
            const auto timeoutText = [](const QString &value) {
                return value.contains(QLatin1String("timed out"), Qt::CaseInsensitive)
                    || value.contains(QLatin1String("timeout"), Qt::CaseInsensitive);
            };
            networkTimeout = timeoutText(errorText) || timeoutText(m_lastStreamError);
            if (!stale) {
                QString message = QStringLiteral("mpv end-file error: %1").arg(errorText);
                if (networkTimeout) {
                    message.prepend(QStringLiteral("NETWORK_TIMEOUT: "));
                    if (!m_lastStreamError.isEmpty())
                        message += QStringLiteral(" [stream: %1]").arg(m_lastStreamError);
                }
                emit errorOccurred(message);
            }
        }

        qInfo() << "mpv end-file: reason" << endFileReasonName(reason) << reason
                << "error" << errorText << "networkTimeout" << networkTimeout
                << "generation" << generation << "currentGeneration" << m_loadGeneration
                << "stale" << stale;
        emit endReached(generation, reason, errorText);
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        auto *message = static_cast<mpv_event_log_message *>(event->data);
        if (!message || !message->level || !message->text)
            break;
        const QString level = QString::fromUtf8(message->level);
        if (level == QLatin1String("error") || level == QLatin1String("fatal")) {
            const QString prefix = message->prefix ? QString::fromUtf8(message->prefix) : QString();
            const QString text = redactSensitiveLog(QString::fromUtf8(message->text).trimmed());
            qWarning() << "mpv log" << prefix << level << text;
            // 只记录流层超时文本，供 END_FILE(ERROR) 标记 NETWORK_TIMEOUT；
            // 在每次 START_FILE / loadPlaylist 时清空，不跨条目串味。
            if (text.contains(QLatin1String("timed out"), Qt::CaseInsensitive)
                || text.contains(QLatin1String("timeout"), Qt::CaseInsensitive)) {
                m_lastStreamError = text;
            }
            // T63/A-P1-21：普通 error 日志（解码器/音频警告等）不触发候选切换；
            // 只有 END_FILE reason=ERROR 作为 fatal 错误 emit errorOccurred。
        }
        break;
    }
    case MPV_EVENT_CLIENT_MESSAGE: {
        auto *message = static_cast<mpv_event_client_message *>(event->data);
        if (!message || message->num_args <= 0 || !message->args || !message->args[0])
            break;
        const QString name = QString::fromUtf8(message->args[0]);
        if (name == QLatin1String("app-toggle-fullscreen"))
            emit toggleFullscreenRequested();
        else if (name == QLatin1String("app-exit-fullscreen"))
            emit exitFullscreenRequested();
        else if (name == QLatin1String("app-next-episode"))
            emit nextEpisodeRequested();
        else if (name == QLatin1String("app-prev-episode"))
            emit previousEpisodeRequested();
        else if (name == QLatin1String("app-mouse-probe"))
            qInfo() << "mpv mouse probe: MBTN_MID received in embedded window";
        break;
    }
    default:
        break;
    }
}
