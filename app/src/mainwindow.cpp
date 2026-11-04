#include "mainwindow.h"

#include "mpvplayer.h"
#include "videohost.h"

#include "embyclient.h"
#include "embytypes.h"

#include "ui/detailview.h"
#include "ui/homeview.h"
#include "ui/libraryview.h"
#include "ui/mainwindow.h"
#include "ui/playerpage.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMessageBox>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWidget>

namespace {

QStringList videoFilesInFolder(const QString &folder)
{
    static const QStringList filters = {
        QStringLiteral("*.mkv"), QStringLiteral("*.mp4"), QStringLiteral("*.m4v"),
        QStringLiteral("*.avi"), QStringLiteral("*.mov"), QStringLiteral("*.ts"),
        QStringLiteral("*.m2ts"), QStringLiteral("*.webm"), QStringLiteral("*.wmv")
    };

    QDir dir(folder);
    const QFileInfoList entries =
        dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Name);
    QStringList files;
    files.reserve(entries.size());
    for (const QFileInfo &info : entries)
        files.append(info.absoluteFilePath());
    return files;
}

qint64 ticksFromSeconds(double seconds)
{
    return static_cast<qint64>(qMax(0.0, seconds) * 10000000.0);
}

double secondsFromTicks(qint64 ticks)
{
    return static_cast<double>(qMax<qint64>(0, ticks)) / 10000000.0;
}

QString displayTrackTitle(const QJsonObject &track, int id)
{
    QString title = track.value(QStringLiteral("title")).toString();
    if (title.isEmpty()) {
        const QString language = track.value(QStringLiteral("lang")).toString();
        title = language.isEmpty() ? QObject::tr("轨道 %1").arg(id) : language;
    }
    const QString codec = track.value(QStringLiteral("codec")).toString();
    if (!codec.isEmpty())
        title += QStringLiteral(" · %1").arg(codec);
    return title;
}

QString mpvKeyNameFromEvent(const QKeyEvent *event)
{
    if (!event)
        return {};

    // Qt 自己的快捷键优先：Esc/F11 由 ui::MainWindow 处理，不转发给 mpv，
    // 避免同一次按键既退出全屏又触发 mpv 的 ESC 绑定。
    const int key = event->key();
    if (key == Qt::Key_Escape || key == Qt::Key_F11)
        return {};

    // Ctrl/Alt 组合留给 Qt（文件菜单等），只转发无修饰键或 Shift 组合。
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return {};

    QString base;
    switch (key) {
    case Qt::Key_Space: base = QStringLiteral("SPACE"); break;
    case Qt::Key_Left: base = QStringLiteral("LEFT"); break;
    case Qt::Key_Right: base = QStringLiteral("RIGHT"); break;
    case Qt::Key_Up: base = QStringLiteral("UP"); break;
    case Qt::Key_Down: base = QStringLiteral("DOWN"); break;
    case Qt::Key_Return:
    case Qt::Key_Enter: base = QStringLiteral("ENTER"); break;
    case Qt::Key_Backspace: base = QStringLiteral("BACKSPACE"); break;
    case Qt::Key_Delete: base = QStringLiteral("DELETE"); break;
    case Qt::Key_Insert: base = QStringLiteral("INSERT"); break;
    case Qt::Key_Home: base = QStringLiteral("HOME"); break;
    case Qt::Key_End: base = QStringLiteral("END"); break;
    case Qt::Key_PageUp: base = QStringLiteral("PGUP"); break;
    case Qt::Key_PageDown: base = QStringLiteral("PGDN"); break;
    case Qt::Key_Tab: base = QStringLiteral("TAB"); break;
    default:
        break;
    }

    if (base.isEmpty()) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            base = QString(QChar('a' + key - Qt::Key_A));
        else if (key >= Qt::Key_0 && key <= Qt::Key_9)
            base = QString(QChar('0' + key - Qt::Key_0));
        else if (key >= Qt::Key_F1 && key <= Qt::Key_F10)
            base = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
        else {
            const QString text = event->text();
            if (text.size() == 1 && text.at(0).isPrint())
                base = text.toLower();
        }
    }
    if (base.isEmpty())
        return {};

    if (event->modifiers() & Qt::ShiftModifier)
        return QStringLiteral("Shift+") + base;
    return base;
}

} // namespace

PlayerController::PlayerController(ui::MainWindow *window, ui::PlayerPage *playerPage,
                                   QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_playerPage(playerPage)
{
    Q_ASSERT(m_window);
    Q_ASSERT(m_playerPage);

    m_emby = m_window->embyClient();
    if (!m_emby)
        m_emby = m_window->findChild<emby::EmbyClient *>();

    m_videoHost = new VideoHost(m_playerPage);
    m_videoHost->hide();
    m_player = new MpvPlayer(this);

    // mpv --wid 模式禁用自身窗口输入（mpv issue #4795）：VideoHost 子类化 mpv 子
    // HWND 后把鼠标消息转成 mpv `mouse` 命令，OSC/点击视频才有输入。
    connect(m_videoHost, &VideoHost::mouseMoved, this, [this](const QPoint &position) {
        if (m_player->isReady())
            m_player->command({QStringLiteral("mouse"), QString::number(position.x()),
                               QString::number(position.y())});
    });
    connect(m_videoHost, &VideoHost::mouseButtonPressed, this,
            [this](int button, const QPoint &position, bool doubleClick) {
                if (!m_player->isReady())
                    return;
                QStringList args = {QStringLiteral("mouse"), QString::number(position.x()),
                                    QString::number(position.y()), QString::number(button)};
                if (doubleClick)
                    args.append(QStringLiteral("double"));
                m_player->command(args);
            });
    connect(m_videoHost, &VideoHost::mouseWheel, this,
            [this](int button, const QPoint &position) {
                if (m_player->isReady())
                    m_player->command({QStringLiteral("mouse"),
                                       QString::number(position.x()),
                                       QString::number(position.y()),
                                       QString::number(button)});
            });
    preparePlayerOverlays();

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(7000);
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        if (m_reporting)
            reportPlaybackProgress(QStringLiteral("TimeUpdate"));
    });

    connect(m_player, &MpvPlayer::positionChanged, this, [this](double) { updatePositionUi(); });
    connect(m_player, &MpvPlayer::durationChanged, this, [this](double) { updatePositionUi(); });
    connect(m_player, &MpvPlayer::pausedChanged, this, [this](bool paused) {
        m_playerPage->setPlaying(!paused);
        if (m_reporting && !m_embyItemId.isEmpty())
            reportPlaybackProgress(paused ? QStringLiteral("Pause") : QStringLiteral("Unpause"));
    });
    connect(m_player, &MpvPlayer::titleChanged, this, [this](const QString &title) {
        if (!title.isEmpty())
            m_playerPage->setEpisodeInfo(title, m_currentSubtitle);
    });
    connect(m_player, &MpvPlayer::fileLoaded, this, [this]() {
        m_playerPage->setPlaying(true);
        ensurePlayerControlsVisible();
        if (m_pendingSeekSeconds > 0.0) {
            m_player->seekAbsolute(m_pendingSeekSeconds);
            m_pendingSeekSeconds = 0.0;
        }
        logMpvState(QStringLiteral("file-loaded"));
        showStatus(tr("正在播放"));
        // track-list 在 FILE_LOADED 后才稳定；稍等再取，兼顾部分格式解析延迟。
        QTimer::singleShot(150, this, &PlayerController::refreshTracks);
        QTimer::singleShot(700, this, &PlayerController::refreshTracks);
    });
    connect(m_player, &MpvPlayer::endReached, this, [this]() {
        if (!m_embyItemId.isEmpty() && m_reporting) {
            reportPlaybackStopped();
            if (m_autoNextEpisode && m_episodeIndex >= 0
                && m_episodeIndex + 1 < m_episodeQueue.size()) {
                playEpisodeAt(m_episodeIndex + 1);
                return;
            }
            showStatus(tr("播放结束"), 3000);
        }
        m_playerPage->setPlaying(false);
    });
    connect(m_player, &MpvPlayer::errorOccurred, this, [this](const QString &message) {
        showStatus(message, 5000);
    });
    // 全屏状态由 ui::MainWindow 统一维护（m_fullscreen/chrome/stack）。mpv 内的
    // F/Esc 转成 PlayerPage::fullscreenRequested，复用 UI 的 toggleFullscreen()，
    // 避免 Controller 自己 showFullScreen 后 UI 状态不同步，Esc 直接退出播放。
    connect(m_player, &MpvPlayer::toggleFullscreenRequested, this, [this]() {
        if (m_playerPage)
            QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested", Qt::DirectConnection);
    });
    connect(m_player, &MpvPlayer::exitFullscreenRequested, this, [this]() {
        if (m_playerPage && m_window->isFullScreen())
            QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested", Qt::DirectConnection);
    });

    // PlayerPage 的控制信号 -> MpvPlayer。
    // 注意：ui::MainWindow 内部也连接了这些信号（用于 mock 状态演示），
    // 这里是真实播放层附加的连接；真实 mpv 事件会再回填 setPlaying/setPosition。
    connect(m_playerPage, &ui::PlayerPage::playPauseRequested, this, [this]() {
        if (m_player->isReady())
            m_player->togglePause();
    });
    connect(m_playerPage, &ui::PlayerPage::previousEpisodeRequested, this, [this]() {
        if (!m_embyItemId.isEmpty())
            stepEmbyEpisode(-1);
        else if (m_player->isReady())
            m_player->playlistPrevious();
    });
    connect(m_playerPage, &ui::PlayerPage::nextEpisodeRequested, this, [this]() {
        if (!m_embyItemId.isEmpty())
            stepEmbyEpisode(1);
        else if (m_player->isReady())
            m_player->playlistNext();
    });
    connect(m_playerPage, &ui::PlayerPage::seekRequested, this, [this](qint64 positionMs) {
        if (!m_player->isReady())
            return;
        m_player->seekAbsolute(static_cast<double>(positionMs) / 1000.0);
        if (m_reporting && !m_embyItemId.isEmpty())
            reportPlaybackProgress(QStringLiteral("Seek"), positionMs);
    });
    connect(m_playerPage, &ui::PlayerPage::volumeChanged, this, [this](int volume) {
        if (m_player->isReady())
            m_player->setVolume(volume);
    });
    connect(m_playerPage, &ui::PlayerPage::statsRequested, this, [this]() {
        if (m_player->isReady())
            m_player->toggleStats();
    });
    connect(m_playerPage, &ui::PlayerPage::subtitleTrackSelected, this,
            [this](const QString &trackId) {
                if (!m_player->isReady())
                    return;
                const QString value = trackId.isEmpty() ? QStringLiteral("no") : trackId;
                m_player->setPropertyString("sid", value);
                showStatus(tr("已选择字幕轨道 %1").arg(value));
                reportPlaybackProgress(QStringLiteral("SubtitleTrackChange"));
                QTimer::singleShot(150, this, &PlayerController::refreshTracks);
            });
    connect(m_playerPage, &ui::PlayerPage::audioTrackSelected, this,
            [this](const QString &trackId) {
                if (!m_player->isReady())
                    return;
                if (!m_player->setPropertyString("aid", trackId)) {
                    ++m_audioSwitchSerial;
                    showStatus(tr("音轨 %1 切换失败").arg(trackId), 5000);
                    refreshTracks();
                    return;
                }
                const int switchSerial = ++m_audioSwitchSerial;
                showStatus(tr("已选择音轨 %1").arg(trackId));
                reportPlaybackProgress(QStringLiteral("AudioTrackChange"));
                QTimer::singleShot(150, this, &PlayerController::refreshTracks);
                // mpv 对无效/不可解码轨道可能先接受 aid、随后回退；延迟核对实际 aid。
                QTimer::singleShot(900, this, [this, trackId, switchSerial]() {
                    if (!m_player->isReady() || switchSerial != m_audioSwitchSerial)
                        return;
                    const QString actual = m_player->getPropertyString("aid");
                    if (!actual.isEmpty() && actual != trackId) {
                        qWarning() << "audio track switch failed:" << trackId
                                   << "actual aid:" << actual;
                        showStatus(tr("音轨 %1 不可用，mpv 已回退到 %2")
                                       .arg(trackId, actual),
                                   5000);
                        refreshTracks();
                    }
                });
            });
    // 从播放页返回时恢复 Qt 控制条（它只在真实播放时隐藏），并上报停止。
    connect(m_playerPage, &ui::PlayerPage::backRequested, this, [this]() {
        if (!m_embyItemId.isEmpty())
            clearEmbySession();
        if (m_window->isPlayerMode())
            m_window->setPlayerMode(false);
    });

    // 离开播放页（返回按钮/Esc/切页）时停止 mpv 并上报 Stopped。
    QStackedWidget *playerStack = qobject_cast<QStackedWidget *>(m_playerPage->parentWidget());
    if (!playerStack)
        playerStack = m_window->findChild<QStackedWidget *>(QStringLiteral("ContentStack"));
    if (playerStack) {
        auto *stack = playerStack;
        connect(stack, &QStackedWidget::currentChanged, this, [this, stack](int index) {
            if (stack->widget(index) == m_playerPage)
                return;
            if (m_player && m_player->isReady())
                m_player->stop();
            if (!m_embyItemId.isEmpty() || m_reporting)
                clearEmbySession();
            if (m_window->isPlayerMode())
                m_window->setPlayerMode(false);
        });
    }

    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            if (m_reporting)
                reportPlaybackStopped();
        });
    }

    // T15：ui::MainWindow 构造时 setMinimumSize(960,600) 且状态栏 size grip 关闭，
    // 在部分高 DPI/小屏上用户会觉得主窗口“不可缩放”。这里在不改 ui 源码的前提下
    // 放宽最小尺寸并启用右下角 resize grip（最大化/拖边框仍由系统窗口管理）。
    const QSize uiMinimum = m_window->minimumSize();
    m_window->installEventFilter(this);
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
    m_window->setMinimumSize(QSize(720, 480));
    m_window->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
    if (QStatusBar *status = m_window->statusBar())
        status->setSizeGripEnabled(true);
    if (QScreen *screen = m_window->screen()) {
        // 默认 1280x760 在 1366x768/高 DPI 小屏上会超出可用区域，用户容易误以为
        // 窗口“不能调整”。先按可用区域收紧初始尺寸，再允许自由缩放。
        const QSize available = screen->availableGeometry().size();
        if (available.isValid()) {
            QSize target = m_window->size().boundedTo(available - QSize(48, 96));
            target = target.expandedTo(QSize(720, 480));
            if (target != m_window->size())
                m_window->resize(target);
        }
        qInfo() << "window: screen available" << screen->availableGeometry()
                << "dpr" << screen->devicePixelRatio();
    }
    qInfo() << "window: minimum" << uiMinimum << "->" << m_window->minimumSize()
            << "maximum" << m_window->maximumSize() << "current" << m_window->size()
            << "sizeGrip" << (m_window->statusBar()
                                   && m_window->statusBar()->isSizeGripEnabled());

    connectUiPlaybackSignals();
}

PlayerController::~PlayerController() = default;

bool PlayerController::eventFilter(QObject *watched, QEvent *event)
{
    // 键盘转发：mpv 子窗口在 --wid 下收不到输入，Qt 主窗口把按键转成
    // mpv `keypress`，空格/方向键/F/n/p/i 等 input.conf 绑定才会生效。
    // Esc/F11 留给 Qt 自己的快捷键；Ctrl/Alt 组合也不转发。
    if (event->type() == QEvent::KeyPress && m_window && m_window->isPlayerMode()
        && m_window->isActiveWindow() && m_player && m_player->isReady()
        && !QApplication::activeModalWidget()) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const QString keyName = mpvKeyNameFromEvent(keyEvent);
        if (!keyName.isEmpty()) {
            m_player->command({QStringLiteral("keypress"), keyName});
            if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_INPUT"))
                qInfo() << "forward key to mpv" << keyName;
        }
    }

    if (watched == m_window && event->type() == QEvent::Resize) {
        auto *resizeEvent = static_cast<QResizeEvent *>(event);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastWindowResizeLogMs >= 500) {
            m_lastWindowResizeLogMs = now;
            qInfo() << "window resize event" << resizeEvent->oldSize() << "->"
                    << resizeEvent->size() << "minimum" << m_window->minimumSize();
        }
    }

    // 全屏时点“返回”先退出全屏，不直接退出播放。PlayerPage 的返回按钮
    // 直接发 backRequested，UI 的槽会优先退出 player mode；这里在按钮事件层
    // 拦截真实鼠标释放，保证全屏状态下第一次点击只退出全屏。
    if (watched == m_backButton && event->type() == QEvent::MouseButtonRelease
        && m_window && m_window->isFullScreen() && m_playerPage) {
        QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested", Qt::DirectConnection);
        return true;
    }

    return QObject::eventFilter(watched, event);
}

void PlayerController::connectUiPlaybackSignals()
{
    if (auto *detail = m_window->findChild<ui::DetailView *>()) {
        connect(detail, &ui::DetailView::playRequested, this,
                &PlayerController::onDetailPlayRequested);
        connect(detail, &ui::DetailView::episodeActivated, this,
                &PlayerController::onDetailPlayRequested);
    }

    // HomeView（继续观看/最近添加）与主媒体库 LibraryView 都直接发 playRequested。
    // HomeView 内部的 recentView 也会转发一份，这里跳过嵌套在 HomeView 里的实例，避免重复请求。
    const QList<ui::LibraryView *> libraryViews = m_window->findChildren<ui::LibraryView *>();
    for (ui::LibraryView *view : libraryViews) {
        bool insideHomeView = false;
        for (QWidget *parent = view->parentWidget(); parent; parent = parent->parentWidget()) {
            if (qobject_cast<ui::HomeView *>(parent)) {
                insideHomeView = true;
                break;
            }
        }
        if (insideHomeView)
            continue;
        connect(view, &ui::LibraryView::playRequested, this,
                &PlayerController::onLibraryPlayRequested);
    }
    const QList<ui::HomeView *> homeViews = m_window->findChildren<ui::HomeView *>();
    for (ui::HomeView *view : homeViews) {
        connect(view, &ui::HomeView::playRequested, this,
                &PlayerController::onLibraryPlayRequested);
    }

    connect(m_window, &ui::MainWindow::loginStateChanged, this, [this](bool loggedIn) {
        if (loggedIn) {
            m_emby = m_window->embyClient();
            showStatus(tr("已登录 Emby，可直接播放媒体库条目"), 3000);
        } else {
            clearEmbySession();
            if (m_player->isReady())
                m_player->stop();
        }
    });
}

void PlayerController::installMenus()
{
    QMenu *fileMenu = m_window->menuBar()->addMenu(tr("文件(&F)"));

    QAction *openFileAction = fileMenu->addAction(tr("打开文件…"));
    openFileAction->setShortcut(QKeySequence::Open);
    connect(openFileAction, &QAction::triggered, this, &PlayerController::openFile);

    QAction *openFolderAction = fileMenu->addAction(tr("打开文件夹（作为播放列表）…"));
    openFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(openFolderAction, &QAction::triggered, this, &PlayerController::openFolder);

    fileMenu->addSeparator();
    QAction *quitAction = fileMenu->addAction(tr("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, m_window, &QWidget::close);

    // main.cpp 里的“帮助”菜单在本函数之后才创建；延迟到事件循环首轮再挂
    // “导出诊断信息”，这样不需要改 main.cpp 也能保证菜单只出现一次。
    QTimer::singleShot(0, this, [this]() {
        QMenu *helpMenu = nullptr;
        const QList<QAction *> actions = m_window->menuBar()->actions();
        for (QAction *action : actions) {
            if (action->menu() && action->text().contains(tr("帮助"))) {
                helpMenu = action->menu();
                break;
            }
        }
        if (!helpMenu)
            helpMenu = m_window->menuBar()->addMenu(tr("帮助(&H)"));
        if (helpMenu->findChild<QAction *>(QStringLiteral("ExportDiagnosticsAction")))
            return;
        QAction *exportAction = helpMenu->addAction(tr("导出诊断信息…"));
        exportAction->setObjectName(QStringLiteral("ExportDiagnosticsAction"));
        connect(exportAction, &QAction::triggered, this, &PlayerController::exportDiagnostics);
    });
}

void PlayerController::openFile()
{
    const QString file = QFileDialog::getOpenFileName(
        m_window, tr("打开视频"), QString(),
        tr("视频文件 (*.mkv *.mp4 *.m4v *.avi *.mov *.ts *.m2ts *.webm *.wmv);;所有文件 (*.*)"));
    if (file.isEmpty())
        return;
    playFiles({file}, 0);
}

void PlayerController::openFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(m_window, tr("选择文件夹"));
    if (folder.isEmpty())
        return;
    const QStringList files = videoFilesInFolder(folder);
    if (files.isEmpty()) {
        QMessageBox::information(m_window, tr("打开文件夹"),
                                 tr("这个文件夹里没有找到视频文件。"));
        return;
    }
    playFiles(files, 0);
}

void PlayerController::playFiles(const QStringList &files, int startIndex)
{
    if (files.isEmpty())
        return;

    if (!m_embyItemId.isEmpty() || m_reporting)
        clearEmbySession();

    if (!ensurePlayer())
        return;

    const QFileInfo firstInfo(files.first());
    m_currentSubtitle = tr("本地播放 · %1 个文件 · %2")
                            .arg(files.size())
                            .arg(firstInfo.absolutePath());
    m_player->loadPlaylist(files, qBound(0, startIndex, static_cast<int>(files.size()) - 1));
    m_player->setPropertyString("force-media-title", firstInfo.fileName());
    showPlayerPage();
    m_playerPage->setEpisodeInfo(firstInfo.fileName(), m_currentSubtitle);
    m_playerPage->setPlaying(true);
    m_playerPage->setVolume(m_playerPage->volume());
    ensurePlayerControlsVisible();

    m_window->setWindowTitle(QStringLiteral("Emby PC — %1").arg(firstInfo.fileName()));
    showStatus(tr("已加载 %1 个文件").arg(files.size()), 3000);
}

bool PlayerController::ensurePlayer(const QStringList &httpHeaderLines)
{
    if (m_player->isReady()) {
        // mpv 已初始化：本地播放之后再播 Emby 时尝试追加 Authorization 头。
        if (!httpHeaderLines.isEmpty() && m_emby)
            m_player->setPropertyString("http-header-fields-append",
                                        m_emby->mpvHeaderFields());
        return true;
    }

    if (!m_surfaceAttached) {
        m_playerPage->attachVideoSurface(m_videoHost);
        m_surfaceAttached = true;
    }

    QString error;
    if (!m_player->initialize(m_videoHost->nativeWindow(), httpHeaderLines, &error)) {
        QMessageBox::critical(m_window, tr("播放内核初始化失败"), error);
        m_playerPage->releaseVideoSurface();
        m_videoHost->setParent(m_playerPage);
        m_videoHost->hide();
        m_surfaceAttached = false;
        return false;
    }

    m_player->setVolume(m_playerPage->volume());
    logMpvState(QStringLiteral("initialized"));
    return true;
}

void PlayerController::onDetailPlayRequested(const QString &itemId, int season, int episode)
{
    if (!m_window->hasSession() || !m_emby) {
        showStatus(tr("未登录 Emby，无法播放该条目（可先用文件菜单播放本地视频）"), 4000);
        return;
    }

    ui::MediaItem item;
    if (auto *detail = m_window->findChild<ui::DetailView *>())
        item = detail->currentItem();
    if (item.id != itemId) {
        item = ui::MediaItem();
        item.id = itemId;
    }

    if (item.isSeries()) {
        loadEpisodeQueue(item.id, item.title, season, episode);
    } else {
        qint64 startTicks = 0;
        if (item.progress > 0.0 && item.runtimeMinutes > 0) {
            startTicks = static_cast<qint64>(item.progress
                                             * item.runtimeMinutes * 60.0 * 10000000.0);
        }
        playEmbyItem(item.id, item.title, -1, -1, startTicks);
    }
}

void PlayerController::onLibraryPlayRequested(const QString &itemId)
{
    if (!m_window->hasSession() || !m_emby) {
        showStatus(tr("未登录 Emby，无法播放该条目"), 3000);
        return;
    }

    const int request = ++m_requestSerial;
    QPointer<PlayerController> self(this);
    m_emby->fetchItem(itemId, [self, request, itemId](const emby::MediaItem &item,
                                                      const emby::ApiError &error) {
        if (!self || request != self->m_requestSerial)
            return;
        if (error.isError()) {
            self->handleApiError(error);
            return;
        }
        if (item.type == QLatin1String("Series")) {
            self->loadEpisodeQueue(item.id, item.name, -1, -1);
        } else {
            self->playEmbyItem(item.id, item.name, -1, -1,
                               item.userData.playbackPositionTicks);
        }
    });
}

void PlayerController::loadEpisodeQueue(const QString &seriesId, const QString &seriesTitle,
                                        int season, int episode)
{
    if (!m_emby)
        return;

    const int request = ++m_requestSerial;
    emby::ItemQuery query;
    query.parentId = seriesId;
    query.recursive = true;
    query.includeItemTypes = {QStringLiteral("Episode")};
    query.sortBy = {QStringLiteral("ParentIndexNumber"), QStringLiteral("IndexNumber")};
    query.sortOrder = QStringLiteral("Ascending");
    query.limit = 500;

    showStatus(tr("正在获取剧集列表…"), 3000);
    QPointer<PlayerController> self(this);
    m_emby->fetchItems(query, [self, request, seriesTitle, season, episode](
                                  const emby::ItemQueryResult &result,
                                  const emby::ApiError &error) {
        if (!self || request != self->m_requestSerial)
            return;
        if (error.isError()) {
            self->handleApiError(error);
            return;
        }

        self->m_episodeQueue.clear();
        for (const emby::MediaItem &item : result.items) {
            if (item.type != QLatin1String("Episode"))
                continue;
            EpisodeEntry entry;
            entry.itemId = item.id;
            entry.title = item.name;
            entry.season = item.parentIndexNumber;
            entry.episode = item.indexNumber;
            entry.runTimeTicks = item.runTimeTicks;
            entry.startPositionTicks = item.userData.playbackPositionTicks;
            self->m_episodeQueue.append(entry);
        }

        if (self->m_episodeQueue.isEmpty()) {
            self->showStatus(QObject::tr("没有找到可播放的剧集"), 4000);
            return;
        }

        int index = 0;
        for (int i = 0; i < self->m_episodeQueue.size(); ++i) {
            const EpisodeEntry &entry = self->m_episodeQueue.at(i);
            if (entry.season == season && entry.episode == episode) {
                index = i;
                break;
            }
        }
        self->m_episodeIndex = index;
        const EpisodeEntry entry = self->m_episodeQueue.at(index);
        self->playEmbyItem(entry.itemId,
                           entry.title.isEmpty() ? seriesTitle : entry.title,
                           entry.season, entry.episode, entry.startPositionTicks);
    });
}

void PlayerController::playEmbyItem(const QString &itemId, const QString &title,
                                    int /*season*/, int /*episode*/,
                                    qint64 startPositionTicks)
{
    if (!m_emby || !m_window->hasSession()) {
        showStatus(tr("未登录 Emby，无法播放"), 3000);
        return;
    }

    const int request = ++m_requestSerial;
    showStatus(tr("正在准备播放…"), 3000);
    QPointer<PlayerController> self(this);
    m_emby->fetchPlaybackInfo(
        itemId, [self, request, itemId, title, startPositionTicks](
                    const emby::PlaybackInfoResult &info, const emby::ApiError &error) {
            if (!self || request != self->m_requestSerial)
                return;
            self->handlePlaybackInfo(itemId, title, -1, -1, startPositionTicks, info, error);
        });
}

void PlayerController::handlePlaybackInfo(const QString &itemId, const QString &title,
                                          int /*season*/, int /*episode*/,
                                          qint64 startPositionTicks,
                                          const emby::PlaybackInfoResult &info,
                                          const emby::ApiError &error)
{
    if (error.isError()) {
        handleApiError(error);
        return;
    }

    const emby::MediaSourceInfo *source = info.preferredMediaSource();
    if (!source) {
        showStatus(tr("服务器没有返回可播放的媒体源"), 4000);
        return;
    }

    if (!m_emby) {
        showStatus(tr("Emby 客户端不可用"), 4000);
        return;
    }

    const QUrl url = m_emby->playbackUrl(itemId, *source);
    if (!url.isValid() || url.isEmpty()) {
        showStatus(tr("无法构造播放地址"), 4000);
        return;
    }

    const QString playMethod = source->supportsDirectPlay
                                   ? QStringLiteral("DirectPlay")
                                   : (source->supportsDirectStream ? QStringLiteral("DirectStream")
                                                                   : QStringLiteral("Transcode"));

    if (m_reporting)
        reportPlaybackStopped();

    m_embyItemId = itemId;
    m_embyTitle = title;
    m_mediaSourceId = source->id;
    m_playSessionId = info.playSessionId.isEmpty() ? source->playSessionId : info.playSessionId;
    m_playMethod = playMethod;
    m_embyDurationTicks = source->runTimeTicks;
    m_currentSubtitle = tr("Emby · %1 · %2")
                            .arg(m_window->sessionProfile().userName, playMethod);

    if (!ensurePlayer(m_emby->streamHeaderLines()))
        return;

    m_player->setPropertyString("force-media-title", title.isEmpty() ? itemId : title);
    if (source->runTimeTicks > 0)
        m_playerPage->setPosition(0, source->runTimeTicks / 10000);
    m_pendingSeekSeconds = startPositionTicks > 0 ? secondsFromTicks(startPositionTicks) : 0.0;
    m_player->loadPlaylist({url.toString()}, 0);

    showPlayerPage();
    m_playerPage->setEpisodeInfo(title.isEmpty() ? itemId : title, m_currentSubtitle);
    m_playerPage->setPlaying(true);
    m_playerPage->setVolume(m_playerPage->volume());
    ensurePlayerControlsVisible();

    m_window->setWindowTitle(QStringLiteral("Emby PC — %1").arg(title.isEmpty() ? itemId : title));
    reportPlaybackStart();
    showStatus(tr("开始播放：%1").arg(playMethod), 2500);
}

void PlayerController::showPlayerPage()
{
    if (!m_playerPage)
        return;

    // T17：播放时让主窗口进入 player mode（隐藏 sidebar/topbar/statusbar，
    // PlayerPage 铺满客户区），退出由 backRequested/stack currentChanged 恢复。
    if (!m_window->isPlayerMode())
        m_window->setPlayerMode(true);

    ensurePlayerControlsVisible();
    QStackedWidget *stack = qobject_cast<QStackedWidget *>(m_playerPage->parentWidget());
    if (!stack)
        stack = m_window->findChild<QStackedWidget *>(QStringLiteral("ContentStack"));
    if (stack)
        stack->setCurrentWidget(m_playerPage);

    if (QWidget *container = m_playerPage->videoSurfaceContainer()) {
        qInfo() << "player mode: page" << m_playerPage->geometry()
                << "videoArea" << container->geometry()
                << "videoHost" << (m_videoHost ? m_videoHost->geometry() : QRect());
    }
    if (auto *controlBar = m_playerPage->findChild<QWidget *>(QStringLiteral("PlayerControlBar"))) {
        qInfo() << "controls: PlayerControlBar visible" << controlBar->isVisible()
                << "geometry" << controlBar->geometry();
    }
}

void PlayerController::stepEmbyEpisode(int delta)
{
    if (m_episodeQueue.isEmpty() || m_episodeIndex < 0)
        return;
    const int nextIndex = m_episodeIndex + delta;
    if (nextIndex < 0 || nextIndex >= m_episodeQueue.size()) {
        showStatus(tr("已经是第一集/最后一集"), 2000);
        return;
    }
    playEpisodeAt(nextIndex);
}

void PlayerController::playEpisodeAt(int index)
{
    if (index < 0 || index >= m_episodeQueue.size())
        return;
    m_episodeIndex = index;
    const EpisodeEntry entry = m_episodeQueue.at(index);
    playEmbyItem(entry.itemId, entry.title, entry.season, entry.episode,
                 entry.startPositionTicks);
}

QList<ui::TrackInfo> PlayerController::audioTracksFromTrackList(const QString &json,
                                                                 int *filteredCount)
{
    if (filteredCount)
        *filteredCount = 0;

    QList<ui::TrackInfo> audio;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return audio;

    for (const QJsonValue &value : doc.array()) {
        const QJsonObject track = value.toObject();
        if (track.value(QStringLiteral("type")).toString() != QLatin1String("audio"))
            continue;

        const int id = track.value(QStringLiteral("id")).toInt();
        if (id <= 0)
            continue;

        const QString codec = track.value(QStringLiteral("codec")).toString().trimmed();
        // 实机问题：aid=1 的 codec 为空，mpv 解码失败回退到 aid=2，但菜单仍列出
        // 这条坏轨道。codec 为空（或 none）的 audio 轨道不进入菜单。
        if (codec.isEmpty()
            || codec.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
            if (filteredCount)
                ++(*filteredCount);
            continue;
        }

        ui::TrackInfo info;
        info.id = QString::number(id);
        info.language = track.value(QStringLiteral("lang")).toString();
        info.codec = codec;
        info.title = displayTrackTitle(track, id);
        info.selected = track.value(QStringLiteral("selected")).toBool();
        audio.append(info);
    }
    return audio;
}

void PlayerController::refreshTracks()
{
    if (!m_player->isReady())
        return;
    const QString json = m_player->getPropertyString("track-list");
    if (json.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return;

    QList<ui::TrackInfo> subtitles;
    bool subtitleSelected = false;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject track = value.toObject();
        if (track.value(QStringLiteral("type")).toString() != QLatin1String("sub"))
            continue;
        const int id = track.value(QStringLiteral("id")).toInt();
        if (id <= 0)
            continue;
        ui::TrackInfo info;
        info.id = QString::number(id);
        info.language = track.value(QStringLiteral("lang")).toString();
        info.codec = track.value(QStringLiteral("codec")).toString();
        info.title = displayTrackTitle(track, id);
        info.selected = track.value(QStringLiteral("selected")).toBool();
        subtitles.append(info);
        subtitleSelected = subtitleSelected || info.selected;
    }

    int filteredAudio = 0;
    const QList<ui::TrackInfo> audio = audioTracksFromTrackList(json, &filteredAudio);
    if (filteredAudio > 0)
        qInfo() << "track-list: hidden" << filteredAudio
                << "audio track(s) with empty/none codec";

    ui::TrackInfo off;
    off.id = QStringLiteral("no");
    off.title = tr("关闭字幕");
    off.selected = !subtitleSelected;
    subtitles.prepend(off);

    m_playerPage->setSubtitleTracks(subtitles);
    m_playerPage->setAudioTracks(audio);
}

void PlayerController::updatePositionUi()
{
    if (!m_player || !m_player->isReady())
        return;
    double durationSeconds = m_player->duration();
    if (durationSeconds <= 0.0 && m_embyDurationTicks > 0)
        durationSeconds = secondsFromTicks(m_embyDurationTicks);
    m_playerPage->setPosition(ticksFromSeconds(m_player->position()) / 10000,
                              ticksFromSeconds(durationSeconds) / 10000);
}

void PlayerController::showStatus(const QString &message, int timeoutMs)
{
    if (m_window && m_window->statusBar())
        m_window->statusBar()->showMessage(message, timeoutMs);
}

void PlayerController::ensurePlayerControlsVisible()
{
    if (!m_playerPage)
        return;
    // T18：PlayerTopBar/PlayerControlBar 是独立顶层半透明窗口，显隐与自动隐藏
    // 由 PlayerPage 统一管理，这里只请求显示，不要再直接 show()/raise()。
    m_playerPage->setControlsVisible(true);
}

void PlayerController::preparePlayerOverlays()
{
    if (!m_playerPage)
        return;

    m_backButton = m_playerPage->findChild<QWidget *>(QStringLiteral("PlayerBackButton"));
    if (m_backButton)
        m_backButton->installEventFilter(this);

    // T18：浮层是独立顶层窗口，定位/显隐/自动隐藏由 PlayerPage 管理；
    // 这里只挂返回按钮的全屏拦截事件过滤器。
    qInfo() << "player overlays: top" << (m_playerPage->topOverlay() != nullptr)
            << "bottom" << (m_playerPage->bottomOverlay() != nullptr)
            << "backButton" << (m_backButton != nullptr);
}

void PlayerController::logMpvState(const QString &reason)
{
    if (!m_player || !m_player->isReady())
        return;

    // 不要记录 http-header-fields：其中含 Authorization/token。
    static const QStringList keys = {
        QStringLiteral("mpv-version"), QStringLiteral("vo"), QStringLiteral("gpu-api"),
        QStringLiteral("gpu-context"), QStringLiteral("hwdec"),
        QStringLiteral("hwdec-current"), QStringLiteral("options/osc"),
        QStringLiteral("options/load-scripts"), QStringLiteral("options/input-vo-keyboard"),
        QStringLiteral("options/input-cursor"), QStringLiteral("options/wid"),
        QStringLiteral("options/script-opts"), QStringLiteral("script-list")
    };
    QStringList values;
    for (const QString &key : keys) {
        const QString value = m_player->getPropertyString(key.toUtf8().constData());
        if (!value.isEmpty())
            values.append(key + QLatin1Char('=') + value);
    }
    qInfo().noquote() << "mpv state [" << reason << "]:" << values.join(QStringLiteral("; "));
    if (m_videoHost)
        qInfo().noquote() << m_videoHost->diagnosticSummary();
}

QString PlayerController::diagnosticSummary()
{
    QStringList lines;
    lines.append(QStringLiteral("Emby PC diagnostics"));
    lines.append(QStringLiteral("time: %1").arg(
        QDateTime::currentDateTime().toString(Qt::ISODate)));
    lines.append(QStringLiteral("app: %1 Qt %2")
                     .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    lines.append(QStringLiteral("os: %1 / %2")
                     .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()));

    lines.append(QStringLiteral("window: size=%1x%2 minimum=%3x%4 maximum=%5x%6")
                     .arg(m_window->width())
                     .arg(m_window->height())
                     .arg(m_window->minimumWidth())
                     .arg(m_window->minimumHeight())
                     .arg(m_window->maximumWidth())
                     .arg(m_window->maximumHeight()));
    lines.append(QStringLiteral("window: maximized=%1 fullscreen=%2 sizeGrip=%3")
                     .arg(m_window->isMaximized() ? QStringLiteral("yes")
                                                  : QStringLiteral("no"))
                     .arg(m_window->isFullScreen() ? QStringLiteral("yes")
                                                   : QStringLiteral("no"))
                     .arg(m_window->statusBar() && m_window->statusBar()->isSizeGripEnabled()
                              ? QStringLiteral("yes")
                              : QStringLiteral("no")));
    if (QScreen *screen = m_window->screen()) {
        lines.append(QStringLiteral("screen: available=%1x%2 dpr=%3")
                         .arg(screen->availableGeometry().width())
                         .arg(screen->availableGeometry().height())
                         .arg(screen->devicePixelRatio()));
    }

    if (m_playerPage) {
        lines.append(QStringLiteral("PlayerPage: size=%1x%2 visible=%3")
                         .arg(m_playerPage->width())
                         .arg(m_playerPage->height())
                         .arg(m_playerPage->isVisible() ? QStringLiteral("yes")
                                                        : QStringLiteral("no")));
        if (auto *bar = m_playerPage->findChild<QWidget *>(QStringLiteral("PlayerControlBar"))) {
            lines.append(QStringLiteral("PlayerControlBar: visible=%1 geometry=%2,%3 %4x%5")
                             .arg(bar->isVisible() ? QStringLiteral("yes")
                                                   : QStringLiteral("no"))
                             .arg(bar->x())
                             .arg(bar->y())
                             .arg(bar->width())
                             .arg(bar->height()));
        }
    }

    if (m_player && m_player->isReady()) {
        static const QStringList keys = {
            QStringLiteral("mpv-version"), QStringLiteral("vo"), QStringLiteral("gpu-api"),
            QStringLiteral("gpu-context"), QStringLiteral("hwdec"),
            QStringLiteral("hwdec-current"), QStringLiteral("options/osc"),
            QStringLiteral("options/load-scripts"), QStringLiteral("options/input-vo-keyboard"),
            QStringLiteral("options/input-cursor"), QStringLiteral("options/wid"),
            QStringLiteral("options/script-opts"), QStringLiteral("script-list")
        };
        for (const QString &key : keys) {
            const QString value = m_player->getPropertyString(key.toUtf8().constData());
            if (!value.isEmpty())
                lines.append(QStringLiteral("mpv %1=%2").arg(key, value));
        }
    } else {
        lines.append(QStringLiteral("mpv: not initialized"));
    }

    if (m_videoHost)
        lines.append(m_videoHost->diagnosticSummary());

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    lines.append(QStringLiteral("log directory: %1").arg(QDir::toNativeSeparators(dir)));
    lines.append(QStringLiteral("app.log: %1").arg(
        QDir::toNativeSeparators(dir + QStringLiteral("/app.log"))));
    lines.append(QStringLiteral("mpv.log: %1").arg(
        QDir::toNativeSeparators(dir + QStringLiteral("/mpv.log"))));
    return lines.join(QLatin1Char('\n'));
}

void PlayerController::exportDiagnostics()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        QMessageBox::warning(m_window, tr("导出诊断信息"),
                             tr("无法创建日志目录：%1").arg(QDir::toNativeSeparators(dir)));
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString path = dir + QStringLiteral("/diagnostics-") + stamp + QStringLiteral(".txt");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(m_window, tr("导出诊断信息"),
                             tr("无法写入诊断文件：%1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    QTextStream stream(&file);
    stream << diagnosticSummary() << QLatin1Char('\n');
    file.close();

    // 同时把 app.log / mpv.log 复制到诊断目录，用户只需打包这个目录。
    const QString bundleDir = dir + QStringLiteral("/diagnostics-") + stamp;
    if (QDir().mkpath(bundleDir)) {
        QFile::copy(dir + QStringLiteral("/app.log"),
                    bundleDir + QStringLiteral("/app.log"));
        QFile::copy(dir + QStringLiteral("/mpv.log"),
                    bundleDir + QStringLiteral("/mpv.log"));
        QFile::copy(path, bundleDir + QStringLiteral("/diagnostics.txt"));
    }

    const QString shown = bundleDir.isEmpty() ? path : bundleDir;
    QMessageBox::information(m_window, tr("导出诊断信息"),
                             tr("诊断信息已导出到：\n%1\n\n请把整个目录打包回传。")
                                 .arg(QDir::toNativeSeparators(shown)));
    QDesktopServices::openUrl(QUrl::fromLocalFile(shown));
}

void PlayerController::reportPlaybackStart()
{
    if (!m_emby || m_embyItemId.isEmpty())
        return;

    emby::PlaybackProgressInfo info;
    info.itemId = m_embyItemId;
    info.mediaSourceId = m_mediaSourceId;
    info.playSessionId = m_playSessionId;
    info.positionTicks = ticksFromSeconds(m_player->position());
    info.playMethod = m_playMethod;
    info.eventName = QStringLiteral("TimeUpdate");

    m_reporting = true;
    m_progressTimer->start();
    QPointer<PlayerController> self(this);
    m_emby->reportPlaybackStart(info, [self](const emby::ApiError &error) {
        if (!self)
            return;
        if (error.isError())
            self->handleApiError(error);
    });
}

void PlayerController::reportPlaybackProgress(const QString &eventName, qint64 positionMs)
{
    if (!m_emby || m_embyItemId.isEmpty() || !m_reporting)
        return;

    emby::PlaybackProgressInfo info;
    info.itemId = m_embyItemId;
    info.mediaSourceId = m_mediaSourceId;
    info.playSessionId = m_playSessionId;
    info.positionTicks = positionMs >= 0 ? ticksFromSeconds(positionMs / 1000.0)
                                         : ticksFromSeconds(m_player->position());
    info.isPaused = m_player->paused();
    info.playMethod = m_playMethod;
    info.eventName = eventName;

    QPointer<PlayerController> self(this);
    m_emby->reportPlaybackProgress(info, [self](const emby::ApiError &error) {
        if (!self)
            return;
        if (error.isError())
            self->handleApiError(error);
    });
}

void PlayerController::reportPlaybackStopped()
{
    if (!m_emby || m_embyItemId.isEmpty())
        return;

    emby::PlaybackProgressInfo info;
    info.itemId = m_embyItemId;
    info.mediaSourceId = m_mediaSourceId;
    info.playSessionId = m_playSessionId;
    info.positionTicks = ticksFromSeconds(m_player->position());
    info.playMethod = m_playMethod;
    info.eventName = QStringLiteral("Stopped");

    m_reporting = false;
    if (m_progressTimer)
        m_progressTimer->stop();
    QPointer<PlayerController> self(this);
    m_emby->reportPlaybackStopped(info, [self](const emby::ApiError &error) {
        if (!self)
            return;
        if (error.isError())
            self->handleApiError(error);
    });
}

void PlayerController::clearEmbySession()
{
    if (m_reporting)
        reportPlaybackStopped();
    m_progressTimer->stop();
    m_embyItemId.clear();
    m_embyTitle.clear();
    m_mediaSourceId.clear();
    m_playSessionId.clear();
    m_playMethod.clear();
    m_embyDurationTicks = 0;
    m_episodeQueue.clear();
    m_episodeIndex = -1;
    m_pendingSeekSeconds = 0.0;
    m_reporting = false;
}

void PlayerController::handleApiError(const emby::ApiError &error)
{
    if (error.httpStatus == 401 || error.httpStatus == 403) {
        showStatus(tr("Emby 登录已失效，请重新登录"), 5000);
        m_window->requestLogin();
    } else {
        showStatus(error.toString(), 5000);
    }
}
