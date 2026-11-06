#include "mpvplayer.h"

#include <mpv/client.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLibrary>
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
    decltype(&::mpv_wait_event) waitEvent = nullptr;
    decltype(&::mpv_free) freeString = nullptr;
    decltype(&::mpv_terminate_destroy) terminateDestroy = nullptr;
    decltype(&::mpv_error_string) errorString = nullptr;
};

MpvApi &api()
{
    static MpvApi instance;
    return instance;
}

bool loadApi(QString *error)
{
    MpvApi &a = api();
    if (a.create)
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
    a.waitEvent = reinterpret_cast<decltype(a.waitEvent)>(a.library.resolve("mpv_wait_event"));
    a.freeString = reinterpret_cast<decltype(a.freeString)>(a.library.resolve("mpv_free"));
    a.terminateDestroy = reinterpret_cast<decltype(a.terminateDestroy)>(a.library.resolve("mpv_terminate_destroy"));
    a.errorString = reinterpret_cast<decltype(a.errorString)>(a.library.resolve("mpv_error_string"));

    if (!a.create || !a.initialize || !a.setOptionString || !a.setPropertyString
        || !a.getPropertyString || !a.command || !a.observeProperty || !a.waitEvent
        || !a.freeString || !a.terminateDestroy) {
        if (error)
            *error = QObject::tr("libmpv-2.dll 缺少必要的导出符号：%1").arg(path);
        return false;
    }
    return true;
}

const char kInputConf[] =
    "SPACE      cycle pause\n"
    "LEFT       seek -5\n"
    "RIGHT      seek 5\n"
    "Shift+LEFT seek -1\n"
    "Shift+RIGHT seek 1\n"
    "UP         add volume 5\n"
    "DOWN       add volume -5\n"
    "n          playlist-next\n"
    "p          playlist-prev\n"
    "i          script-binding stats/display-stats-toggle\n"
    "f          script-message app-toggle-fullscreen\n"
    "ESC        script-message app-exit-fullscreen\n"
    // 用中键做探针，避免覆盖 OSC 的左键点击绑定。
    "MBTN_MID script-message app-mouse-probe\n";

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

// mpv.log 与 app.log 同目录（QStandardPaths::AppLocalDataLocation），
// 便于 Windows 实机验收时整个日志目录直接打包回传。
QString mpvLogFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString base = dir.isEmpty() ? QDir::homePath() + QStringLiteral("/.emby-pc") : dir;
    QDir().mkpath(base);
    return base + QStringLiteral("/mpv.log");
}

} // namespace

MpvPlayer::MpvPlayer(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, &MpvPlayer::pollEvents);
}

MpvPlayer::~MpvPlayer()
{
    m_timer.stop();
    if (m_mpv) {
        api().terminateDestroy(m_mpv);
        m_mpv = nullptr;
    }
}

bool MpvPlayer::initialize(quintptr windowId, QString *error)
{
    return initialize(windowId, QStringList(), error);
}

bool MpvPlayer::initialize(quintptr windowId, const QStringList &httpHeaderLines, QString *error)
{
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

    // ---- 嵌入窗口 ----
    // 注意：setOption 的 name 参数必须是稳定的 C 字符串，不要传临时 QByteArray 的指针。
    setOption("wid", QString::number(windowId));

    // ---- 视频输出 ----
    setOption("vo", QStringLiteral("gpu-next"));
#ifdef Q_OS_WIN
    setOption("gpu-api", QStringLiteral("d3d11"));
    setOption("gpu-context", QStringLiteral("d3d11"));
    setOption("hwdec", QStringLiteral("d3d11va"));
#endif
    // HDR/DV 片源统一 tone-map 到 SDR（不发灰）
    setOption("target-colorspace-hint", QStringLiteral("no"));
    setOption("target-trc", QStringLiteral("srgb"));
    setOption("target-prim", QStringLiteral("bt.709"));
    setOption("tone-mapping", QStringLiteral("auto"));
    setOption("hdr-compute-peak", QStringLiteral("auto"));
    setOption("dither-depth", QStringLiteral("auto"));

    // ---- 字幕 ----
    setOption("blend-subtitles", QStringLiteral("video"));
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
    setOption("input-vo-keyboard", QStringLiteral("yes"));
    setOption("input-cursor", QStringLiteral("yes"));
    setOption("input-conf", writeInputConf());
    if (oscEnabled)
        setOption("script-opts", QStringLiteral("osc-hidetimeout=1800,osc-boxalpha=170"));

    // ---- 隔离用户配置 / 缓存 ----
    setOption("config", QStringLiteral("no"));
    setOption("load-scripts", QStringLiteral("yes"));
    qInfo() << "mpv options: wid=" << windowId << "osc=" << oscEnabled
            << "load-scripts=yes input-vo-keyboard=yes input-cursor=yes";
    setOption("keep-open", QStringLiteral("yes"));
    setOption("idle", QStringLiteral("yes"));
    setOption("force-window", QStringLiteral("yes"));
    setOption("terminal", QStringLiteral("no"));

    // ---- Emby 直连 HTTP 头（T8）----
    // Authorization 等头只在 mpv 初始化前通过 http-header-fields-append 注入，
    // 不拼进播放 URL。
    if (!httpHeaderLines.isEmpty()) {
        QStringList escapedHeaders;
        escapedHeaders.reserve(httpHeaderLines.size());
        for (const QString &line : httpHeaderLines)
            escapedHeaders.append(escapeMpvListValue(line));
        setOption("http-header-fields-append", escapedHeaders.join(QLatin1Char(',')));
    }

    // ---- 文件日志（T9 验收支撑）----
    // log-file 必须在 mpv_initialize() 之前设置；每次启动会截断旧 mpv.log。
    setOption("log-file", mpvLogFilePath());
    setOption("msg-level", QStringLiteral("all=v"));
    setOption("cache", QStringLiteral("yes"));
    setOption("demuxer-max-bytes", QStringLiteral("512MiB"));

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

    m_timer.start();
    return true;
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

    command({QStringLiteral("loadfile"), files.first(), QStringLiteral("replace")});
    for (int i = 1; i < files.size(); ++i)
        command({QStringLiteral("loadfile"), files.at(i), QStringLiteral("append")});

    const int index = qBound(0, startIndex, files.size() - 1);
    if (index > 0)
        setPropertyString("playlist-pos", QString::number(index));

    setPropertyString("pause", QStringLiteral("no"));
}

void MpvPlayer::togglePause()
{
    command({QStringLiteral("cycle"), QStringLiteral("pause")});
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
    case MPV_EVENT_FILE_LOADED:
        emit fileLoaded();
        break;
    case MPV_EVENT_END_FILE: {
        auto *end = static_cast<mpv_event_end_file *>(event->data);
        if (end && end->reason == MPV_END_FILE_REASON_ERROR && end->error < 0) {
            const char *text = api().errorString ? api().errorString(end->error) : nullptr;
            emit errorOccurred(QStringLiteral("mpv end-file error: %1")
                                   .arg(QString::fromUtf8(text ? text : "unknown")));
        }
        emit endReached();
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
            emit errorOccurred(QStringLiteral("mpv: %1").arg(text));
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
        else if (name == QLatin1String("app-mouse-probe"))
            qInfo() << "mpv mouse probe: MBTN_MID received in embedded window";
        break;
    }
    default:
        break;
    }
}
