#include "crashhandler.h"

#include "securetokenstore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace {

bool g_initialized = false;
bool g_previousCrash = false;
QTimer *g_heartbeat = nullptr;
QString g_lastFlagContent;

#ifdef Q_OS_WIN
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
volatile LONG g_writingCrash = 0;
#endif

QString appDataDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!dir.isEmpty())
        return dir;
    return QDir::homePath() + QStringLiteral("/.emby-pc");
}

#ifdef Q_OS_WIN
QString appLogFilePath()
{
    return appDataDirectory() + QStringLiteral("/app.log");
}
#endif

void writeRunningFlag()
{
    QDir().mkpath(appDataDirectory());
    QSaveFile file(CrashHandler::flagFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    const QString content = QStringLiteral("pid=%1\nstarted=%2\napp=%3\n")
                                .arg(QCoreApplication::applicationPid())
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
                                .arg(QCoreApplication::applicationVersion());
    file.write(content.toUtf8());
    file.commit();
}

#ifdef Q_OS_WIN
QStringList lastLogLines(int maxLines)
{
    QStringList result;
    QFile file(appLogFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const qint64 size = file.size();
    constexpr qint64 kTailBytes = 128 * 1024;
    if (size > kTailBytes)
        file.seek(size - kTailBytes);
    const QByteArray data = file.readAll();
    const QList<QByteArray> lines = data.split('\n');
    const int start = qMax(0, lines.size() - maxLines);
    for (int i = start; i < lines.size(); ++i) {
        const QString line = QString::fromUtf8(lines.at(i)).trimmed();
        if (!line.isEmpty())
            result.append(emby::redactSensitiveText(line));
    }
    return result;
}
#endif // Q_OS_WIN

#ifdef Q_OS_WIN

QString exceptionDescription(const EXCEPTION_RECORD *record)
{
    if (!record)
        return QStringLiteral("no exception record");
    return QStringLiteral("code=0x%1 address=0x%2 flags=0x%3")
        .arg(static_cast<qulonglong>(record->ExceptionCode), 0, 16)
        .arg(reinterpret_cast<qulonglong>(record->ExceptionAddress), 0, 16)
        .arg(static_cast<qulonglong>(record->ExceptionFlags), 0, 16);
}

QString moduleList()
{
    QStringList lines;
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                 ::GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Module32FirstW(snapshot, &entry)) {
            do {
                lines.append(QStringLiteral("module %1 base=0x%2 size=0x%3")
                                 .arg(QString::fromWCharArray(entry.szModule))
                                 .arg(reinterpret_cast<qulonglong>(entry.modBaseAddr), 0, 16)
                                 .arg(static_cast<qulonglong>(entry.modBaseSize), 0, 16));
            } while (::Module32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
    }
    if (lines.isEmpty()) {
        wchar_t path[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        lines.append(QStringLiteral("module %1").arg(QString::fromWCharArray(path)));
    }
    return lines.join(QLatin1Char('\n'));
}

void writeCrashFiles(EXCEPTION_POINTERS *exceptionPointers)
{
    // 防止 dump 写入过程中二次异常递归进入过滤器。
    if (::InterlockedExchange(&g_writingCrash, 1) != 0)
        return;

    QDir().mkpath(CrashHandler::crashDirectory());
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString baseName = QStringLiteral("crash-%1-%2")
                                 .arg(stamp)
                                 .arg(QCoreApplication::applicationPid());
    const QString basePath = CrashHandler::crashDirectory() + QLatin1Char('/') + baseName;

    QString report;
    QTextStream out(&report);
    out << "Emby PC crash report\n";
    out << "time: " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
    out << "app: " << QCoreApplication::applicationVersion() << "\n";
    out << "qt: " << QString::fromLatin1(qVersion()) << "\n";
    out << "os: " << QSysInfo::prettyProductName() << " / "
        << QSysInfo::currentCpuArchitecture() << "\n";
    out << "pid: " << QCoreApplication::applicationPid() << "\n";
    out << "exception: " << exceptionDescription(exceptionPointers
                                                    ? exceptionPointers->ExceptionRecord
                                                    : nullptr)
        << "\n";
    out << "crash dir: " << QDir::toNativeSeparators(CrashHandler::crashDirectory()) << "\n";
    out << "\nlast app.log lines:\n";
    const QStringList logs = lastLogLines(80);
    if (logs.isEmpty())
        out << "(no app.log available)\n";
    else
        out << logs.join(QLatin1Char('\n')) << "\n";
    out << "\nmodules:\n" << moduleList() << "\n";
    out.flush();

    QFile textFile(basePath + QStringLiteral(".txt"));
    if (textFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        textFile.write(report.toUtf8());
        textFile.close();
    }

    const QString dumpPath = basePath + QStringLiteral(".dmp");
    HANDLE file = ::CreateFileW(reinterpret_cast<LPCWSTR>(dumpPath.utf16()),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = ::GetCurrentThreadId();
        info.ExceptionPointers = exceptionPointers;
        info.ClientPointers = FALSE;
        // 不要使用 MiniDumpWithDataSegs：完整可写内存段可能包含 QSettings
        // 中的 token。MiniDumpNormal 足以定位崩溃地址和线程栈，同时显著缩小
        // 凭据进入转储的范围；崩溃文本中的 app.log 片段也已做脱敏。
        const MINIDUMP_TYPE type = MiniDumpNormal;
        ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type,
                            exceptionPointers ? &info : nullptr, nullptr, nullptr);
        ::CloseHandle(file);
    } else {
        QFile errorFile(basePath + QStringLiteral(".txt"));
        if (errorFile.open(QIODevice::Append | QIODevice::Text))
            errorFile.write("\nMiniDumpWriteDump: CreateFileW failed\n");
    }

    ::InterlockedExchange(&g_writingCrash, 0);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exceptionPointers)
{
    writeCrashFiles(exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif // Q_OS_WIN

} // namespace

namespace CrashHandler {

QString crashDirectory()
{
    return appDataDirectory() + QStringLiteral("/crashes");
}

QString flagFilePath()
{
    return appDataDirectory() + QStringLiteral("/running.flag");
}

void initialize()
{
    if (g_initialized)
        return;
    g_initialized = true;

    QDir().mkpath(appDataDirectory());
    QDir().mkpath(crashDirectory());

    QFile flag(flagFilePath());
    g_previousCrash = flag.exists();
    if (g_previousCrash) {
        if (flag.open(QIODevice::ReadOnly))
            g_lastFlagContent = QString::fromUtf8(flag.readAll()).trimmed();
        qWarning() << "CrashHandler: previous run did not exit cleanly" << flagFilePath();
    }

    writeRunningFlag();

#ifdef Q_OS_WIN
    g_previousFilter = ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif

    if (QCoreApplication *app = QCoreApplication::instance()) {
        g_heartbeat = new QTimer(app);
        g_heartbeat->setInterval(5000);
        QObject::connect(g_heartbeat, &QTimer::timeout, app, []() { writeRunningFlag(); });
        g_heartbeat->start();
    }
}

void shutdown()
{
    if (!g_initialized)
        return;
    if (g_heartbeat) {
        g_heartbeat->stop();
        g_heartbeat->deleteLater();
        g_heartbeat = nullptr;
    }
#ifdef Q_OS_WIN
    if (g_previousFilter) {
        ::SetUnhandledExceptionFilter(g_previousFilter);
        g_previousFilter = nullptr;
    }
#endif
    QFile::remove(flagFilePath());
    g_initialized = false;
    g_previousCrash = false;
    g_lastFlagContent.clear();
}

bool previousCrashDetected()
{
    return g_previousCrash;
}

void showPreviousCrashDialog(QWidget *parent)
{
    if (!g_previousCrash)
        return;
    g_previousCrash = false;

    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QObject::tr("上次异常退出"));
    box.setText(QObject::tr("Emby PC 上次没有正常退出。"));
    QString detail = QObject::tr("崩溃转储目录：%1")
                         .arg(QDir::toNativeSeparators(crashDirectory()));
    if (!g_lastFlagContent.isEmpty())
        detail += QObject::tr("\n\n上次运行标志：\n%1").arg(g_lastFlagContent);
    box.setInformativeText(detail);
    QPushButton *openButton = box.addButton(QObject::tr("打开崩溃目录"),
                                            QMessageBox::ActionRole);
    box.addButton(QObject::tr("关闭"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == openButton)
        openCrashDirectory(parent);
}

void openCrashDirectory(QWidget *parent)
{
    const QString dir = crashDirectory();
    if (QDir().mkpath(dir) && QDesktopServices::openUrl(QUrl::fromLocalFile(dir)))
        return;
    if (parent) {
        QMessageBox::warning(parent, QObject::tr("打开崩溃目录"),
                             QObject::tr("无法打开崩溃目录：%1")
                                 .arg(QDir::toNativeSeparators(dir)));
    }
}

void triggerTestCrash()
{
#ifdef Q_OS_WIN
    // EMBY_PC_TEST_CRASH=1 的验收路径：未处理 AV 交给 SetUnhandledExceptionFilter。
    ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
    qFatal("CrashHandler: EMBY_PC_TEST_CRASH is a Windows-only test switch");
#endif
}

} // namespace CrashHandler
