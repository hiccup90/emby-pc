#include "mainwindow.h"

#include "crashhandler.h"
#include "ui/mainwindow.h"
#include "ui/playerpage.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstdlib>

namespace {

// ---------------------------------------------------------------------------
// 文件日志
//
// 日志目录 = QStandardPaths::AppLocalDataLocation（Windows 实机上是
// %LOCALAPPDATA%\<组织名>\<应用名>\）。app.log 与 mpv.log 都放在这里，
// 「帮助 → 打开日志目录」直接打开该目录，用户验收时整个目录打包回传即可。
// 日志内容禁止出现 token / 密码 / Authorization 头（由各调用方负责）。
// ---------------------------------------------------------------------------

QString logDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!dir.isEmpty())
        return dir;
    // 极端情况下（无 HOME/无权限）退回家目录，保证日志仍可写。
    return QDir::homePath() + QStringLiteral("/.emby-pc");
}

QString appLogFilePath()
{
    return logDirectory() + QStringLiteral("/app.log");
}

constexpr qint64 kMaxLogFileBytes = 5 * 1024 * 1024; // 5MB

// 轮转：app.log -> app.log.1 -> app.log.2（最多保留 3 个文件）。
void rotateLogFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || info.size() < kMaxLogFileBytes)
        return;

    QFile::remove(path + QStringLiteral(".2"));
    if (QFile::exists(path + QStringLiteral(".1")))
        QFile::rename(path + QStringLiteral(".1"), path + QStringLiteral(".2"));
    QFile::rename(path, path + QStringLiteral(".1"));
}

QFile g_logFile;
QMutex g_logMutex;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO";
    case QtWarningMsg:
        return "WARN";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    }
    return "INFO";
}

void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString category =
        QString::fromUtf8(context.category && *context.category ? context.category : "default");
    QString location;
    if (context.file && *context.file)
        location = QStringLiteral(" (%1:%2)").arg(QString::fromUtf8(context.file)).arg(context.line);

    const QString line = timestamp + QStringLiteral(" [") + QString::fromLatin1(levelName(type))
                         + QStringLiteral("] [") + category + QStringLiteral("] ") + message
                         + location + QLatin1Char('\n');

    {
        QMutexLocker locker(&g_logMutex);
        if (g_logFile.isOpen()) {
            g_logFile.write(line.toUtf8());
            g_logFile.flush();
        }
    }

    // 同一份内容也写到 stderr：开发机/CI 直接可见（Windows GUI 程序无控制台，看不到）。
    const QByteArray utf8 = line.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stderr);
    std::fflush(stderr);

    if (type == QtFatalMsg)
        std::abort();
}

bool initFileLogging()
{
    const QString dir = logDirectory();
    if (!QDir().mkpath(dir)) {
        std::fprintf(stderr, "emby-pc: cannot create log directory: %s\n",
                     qPrintable(QDir::toNativeSeparators(dir)));
        return false;
    }

    const QString path = appLogFilePath();
    rotateLogFile(path);

    g_logFile.setFileName(path);
    if (!g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::fprintf(stderr, "emby-pc: cannot open log file: %s (%s)\n",
                     qPrintable(QDir::toNativeSeparators(path)),
                     qPrintable(g_logFile.errorString()));
        return false;
    }

    qInstallMessageHandler(fileMessageHandler);
    return true;
}

void shutdownFileLogging()
{
    qInstallMessageHandler(nullptr);
    QMutexLocker locker(&g_logMutex);
    if (g_logFile.isOpen()) {
        g_logFile.flush();
        g_logFile.close();
    }
}

// ---------------------------------------------------------------------------
// 菜单
// ---------------------------------------------------------------------------

void openLogDirectory(QWidget *parent)
{
    const QString dir = logDirectory();
    if (!QDir().mkpath(dir) || !QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        QMessageBox::warning(parent, QObject::tr("打开日志目录"),
                             QObject::tr("无法打开日志目录：%1")
                                 .arg(QDir::toNativeSeparators(dir)));
    }
}

void installHelpMenu(QMainWindow *window)
{
    QMenu *helpMenu = window->menuBar()->addMenu(QObject::tr("帮助(&H)"));

    QAction *openLogsAction = helpMenu->addAction(QObject::tr("打开日志目录"));
    openLogsAction->setObjectName(QStringLiteral("OpenLogDirectoryAction"));
    QObject::connect(openLogsAction, &QAction::triggered, window,
                     [window]() { openLogDirectory(window); });

    helpMenu->addSeparator();
    QAction *aboutAction = helpMenu->addAction(QObject::tr("关于 Emby PC"));
    QObject::connect(aboutAction, &QAction::triggered, window, [window]() {
        QMessageBox::about(
            window, QObject::tr("关于 Emby PC"),
            QStringLiteral("<b>Emby PC</b> %1<br>Qt %2<br>构建时间：%3 %4<br><br>"
                           "日志目录：<br>%5")
                .arg(QCoreApplication::applicationVersion())
                .arg(QString::fromLatin1(qVersion()))
                .arg(QString::fromLatin1(__DATE__))
                .arg(QString::fromLatin1(__TIME__))
                .arg(QDir::toNativeSeparators(logDirectory())));
    });
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("emby-pc"));
    app.setOrganizationName(QStringLiteral("emby-pc"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setApplicationDisplayName(QStringLiteral("Emby PC"));

    // 尽早初始化文件日志：之后的 Qt 日志（含 UI 层）都会写入 app.log。
    const bool logReady = initFileLogging();
    if (logReady) {
        qInfo() << "======== Emby PC 启动 ========";
        qInfo() << "版本:" << app.applicationVersion() << "Qt:" << qVersion();
        qInfo() << "系统:" << QSysInfo::prettyProductName() << QSysInfo::currentCpuArchitecture();
        qInfo() << "app.log:" << QDir::toNativeSeparators(appLogFilePath());
        qInfo() << "mpv.log:" << QDir::toNativeSeparators(logDirectory() + QStringLiteral("/mpv.log"));
    }

    // T34：早期崩溃检测/未处理异常过滤器；running.flag 在正常退出时清除。
    CrashHandler::initialize();

    // 正式应用壳：ui::MainWindow（侧边栏 + 页面栈 + mock 媒体库）。
    ui::MainWindow window;
    window.setWindowTitle(QStringLiteral("Emby PC"));
    window.resize(1280, 760);

    auto *playerPage = window.findChild<ui::PlayerPage *>();
    if (!playerPage) {
        qCritical() << "ui::PlayerPage not found in ui::MainWindow";
        CrashHandler::shutdown();
        shutdownFileLogging();
        return 1;
    }

    // 播放层：VideoHost + MpvPlayer（--wid 嵌入），接到 PlayerPage 的控制信号。
    PlayerController controller(&window, playerPage);
    controller.installMenus();

    // 帮助菜单：打开日志目录（app.log / mpv.log 所在目录）。
    installHelpMenu(&window);

    window.show();

    // 上次异常退出提示：窗口显示后弹一次；GUI 启动只提示一次。
    if (CrashHandler::previousCrashDetected()) {
        QTimer::singleShot(0, &window, [&window]() {
            CrashHandler::showPreviousCrashDialog(&window);
        });
    }
    // M4-2 测试开关：EMBY_PC_TEST_CRASH=1 时故意触发一次未处理异常，
    // 生成 dump 后重启验证“上次异常退出”提示；正常运行不受影响。
    if (qEnvironmentVariableIntValue("EMBY_PC_TEST_CRASH") > 0) {
        QTimer::singleShot(1200, &window, []() { CrashHandler::triggerTestCrash(); });
    }

    const int exitCode = app.exec();

    CrashHandler::shutdown();
    if (logReady) {
        qInfo() << "======== Emby PC 退出，exitCode:" << exitCode << "========";
        shutdownFileLogging();
    }
    return exitCode;
}
