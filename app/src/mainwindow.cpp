#include "mainwindow.h"

#include "mpvplayer.h"
#include "videohost.h"

#include "embyclient.h"
#include "embytypes.h"

#include "ui/detailview.h"
#include "ui/homeview.h"
#include "ui/libraryview.h"
#include "ui/mainwindow.h"
#include "ui/playbackerrordialog.h"
#include "ui/playerpage.h"
#include "ui/settingspage.h"

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
#include <QAbstractButton>
#include <QEventLoop>
#include <QPushButton>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QSharedPointer>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWidget>

namespace {

QString hwdecMpvValue(const QString &mode)
{
    if (mode == QLatin1String("off"))
        return QStringLiteral("no");
    if (mode == QLatin1String("prefer"))
        return QStringLiteral("d3d11va");
    return QStringLiteral("auto-safe");
}

QString redactDiagnosticText(const QString &text)
{
    QString redacted = text;
    static const QRegularExpression auth(
        QStringLiteral("(?i)(Authorization\\s*:\\s*)[^\\r\\n]+"));
    redacted.replace(auth, QStringLiteral("\\1<redacted>"));
    static const QRegularExpression bearer(
        QStringLiteral("(?i)(Bearer\\s+)[A-Za-z0-9._~+/=-]+"));
    redacted.replace(bearer, QStringLiteral("\\1<redacted>"));
    static const QRegularExpression tokens(QStringLiteral(
        "(?i)((?:api_key|access_token|X-Emby-Token|X-MediaBrowser-Token|Token)\\s*[=:]\\s*)[^&,\\s]+"));
    redacted.replace(tokens, QStringLiteral("\\1<redacted>"));
    return redacted;
}

bool copyRedactedLog(const QString &source, const QString &target)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString text = QString::fromUtf8(in.readAll());
    in.close();
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    out.write(redactDiagnosticText(text).toUtf8());
    out.close();
    return true;
}

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

    // spike/render-d3d11：VideoHost 是 QRhiWidget，mpv 直接渲染进它的合成纹理；
    // 鼠标事件仍转成 mpv `mouse` 命令（OSC/脚本绑定），双击改由 Qt 触发全屏。
    connect(m_videoHost, &VideoHost::mouseMoved, this, [this](const QPoint &position) {
        if (m_player->isReady())
            m_player->command({QStringLiteral("mouse"), QString::number(position.x()),
                               QString::number(position.y())});
    });
    connect(m_videoHost, &VideoHost::mouseButtonPressed, this,
            [this](int button, const QPoint &position, bool doubleClick) {
                if (!m_player->isReady())
                    return;
                if (doubleClick && button == 0) {
                    // 双击全屏由 Qt 侧发起：复用 PlayerPage::fullscreenRequested
                    // （不新增全屏信号），mpv 的 MBTN_LEFT_DBL 绑定已删除。
                    if (m_playerPage) {
                        QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested",
                                                  Qt::DirectConnection);
                    }
                    return;
                }
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

    // mpv 每帧渲染到 VideoHost 当前正在合成的 D3D11 纹理。返回 false 时
    // VideoHost 才会用 QRhi 清黑（render context 尚未就绪/渲染失败）。
    m_videoHost->setFrameRenderer([this](void *texture, bool forceRedraw) {
        if (!m_player || !m_player->isReady())
            return false;
        return m_player->renderFrame(texture, forceRedraw);
    });
    connect(m_player, &MpvPlayer::frameUpdate, m_videoHost, &VideoHost::requestFrame);
    // 队列连接：不在 QRhiWidget::initialize()/paint 里同步创建 mpv render context。
    connect(m_videoHost, &VideoHost::d3d11DeviceReady, this,
            &PlayerController::handleRenderDeviceReady, Qt::QueuedConnection);

    // 键盘继续走 Qt 的 eventFilter（mpvKeyNameFromEvent → keypress），render API
    // 下没有 VO 窗口，不需要再激活/回焦 mpv 子窗口。
    preparePlayerOverlays();

    // T48：Emby 播放 8 秒无 file-loaded 视为失败，自动尝试备选 URL。
    m_playbackWatchdog = new QTimer(this);
    m_playbackWatchdog->setSingleShot(true);
    m_playbackWatchdog->setInterval(8000);
    connect(m_playbackWatchdog, &QTimer::timeout, this,
            &PlayerController::handlePlaybackTimeout);

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(10000); // T29：按规格 10 秒；pause/seek/切轨仍立即上报
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        if (m_reporting)
            reportPlaybackProgress(QStringLiteral("TimeUpdate"));
    });

    // T29：设置页变化实时应用；QSettings 默认值由 SettingsPage 落盘。
    if (auto *settingsPage = m_window->settingsPage()) {
        connect(settingsPage, &ui::SettingsPage::settingsChanged, this,
                &PlayerController::applyPlayerSettings);
    }
    applyPlayerSettings();

    connect(m_player, &MpvPlayer::hardwareDecoderChanged, this,
            [this](const QString &hwdec, const QString &vo, const QString &gpuApi) {
                if (auto *settings = m_window->settingsPage()) {
                    settings->setHardwareDecoderText(
                        QStringLiteral("hwdec-current=%1 · vo=%2 · gpu-api=%3")
                            .arg(hwdec.isEmpty() ? QStringLiteral("no") : hwdec, vo, gpuApi));
                }
                // T63/A-P0-10：文件已加载后 hwdec 为空/no/-copy 时自动降级并提示。
                if (m_playbackFileLoaded)
                    applyHardwareFallback(hwdec);
            });
    if (m_emby) {
        // T65/A-P0-08：token 过期（401/403）时结束播放会话、清 token、请求重登录。
        connect(m_emby, &emby::EmbyClient::authenticationExpired, this,
                [this](int httpStatus, const QString &operation, const QString &message) {
                    qWarning() << "PlayerController: authentication expired" << httpStatus
                               << operation << message;
                    if (m_player && m_player->isReady())
                        m_player->stop();
                    if (m_reporting)
                        reportPlaybackStopped(false);
                    clearEmbySession();
                    if (m_playerPage)
                        m_playerPage->setPlaying(false);
                    if (m_player && m_player->isReady())
                        m_player->clearHttpHeaderFields();
                    // T65：UI 统一清 tokenStore/profile/session/abortAll 后再弹登录；
                    // 不能只 requestLogin，否则 hasSession()/旧 token 仍残留。
                    m_window->sessionExpired(tr("登录状态已失效，请重新登录。"));
                });
    }
    connect(m_player, &MpvPlayer::libmpvVersionChanged, this, [this](const QString &version) {
        if (auto *settings = m_window->settingsPage())
            settings->setLibmpvVersionText(version);
    });

    connect(m_player, &MpvPlayer::positionChanged, this, [this](double position) {
        m_lastKnownPositionSeconds = position;
        updatePositionUi();
    });
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
        m_playbackFileLoaded = true;
        if (m_playbackWatchdog)
            m_playbackWatchdog->stop();
        m_playerPage->setPlaying(true);
        ensurePlayerControlsVisible();
        if (m_pendingSeekSeconds > 0.0) {
            m_player->seekAbsolute(m_pendingSeekSeconds);
            m_pendingSeekSeconds = 0.0;
        }
        logMpvState(QStringLiteral("file-loaded"));
        updateHardwareInfo();
        QTimer::singleShot(150, this, [this]() {
            if (m_playbackFileLoaded && m_player)
                applyHardwareFallback(m_player->getPropertyString("hwdec-current"));
        });
        showStatus(tr("正在播放"));
        // track-list 在 FILE_LOADED 后才稳定；稍等再取，兼顾部分格式解析延迟。
        QTimer::singleShot(150, this, &PlayerController::refreshTracks);
        QTimer::singleShot(700, this, &PlayerController::refreshTracks);
    });
    connect(m_player, &MpvPlayer::endReached, this, [this]() {
        const bool hadLoaded = m_playbackFileLoaded;
        if (m_playbackWatchdog)
            m_playbackWatchdog->stop();
        if (!hadLoaded && !m_embyItemId.isEmpty() && !m_playbackCandidates.isEmpty()) {
            // 首次直连失败时 mpv 可能先 ERROR 再 END_FILE；不要把失败当成正常结束去切集。
            qWarning() << "PlayerController::endReached without file-loaded item"
                       << m_embyItemId << "candidate" << m_playbackCandidateIndex + 1
                       << "/" << m_playbackCandidates.size();
            return;
        }
        m_playbackFileLoaded = false;
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
        qWarning() << "PlayerController::mpv error" << message
                   << "item" << m_embyItemId << "url"
                   << sanitizedPlaybackUrl(m_currentPlaybackUrl)
                   << "fileLoaded" << m_playbackFileLoaded;
        if (!m_playbackFileLoaded) {
            if (!m_embyItemId.isEmpty() && !m_playbackCandidates.isEmpty()) {
                tryNextPlaybackCandidate(QStringLiteral("fatal mpv error: %1").arg(message));
                return;
            }
            if (m_localPlayback && !m_localFiles.isEmpty()) {
                handlePlaybackFailure(QStringLiteral("local mpv fatal error: %1").arg(message));
                return;
            }
        }
        showStatus(message, 5000);
    });
    // 全屏状态由 ui::MainWindow 统一维护（m_fullscreen/chrome/stack）。mpv 内的
    // F/Esc 转成 PlayerPage::fullscreenRequested，复用 UI 的 toggleFullscreen()，
    // 避免 Controller 自己 showFullScreen 后 UI 状态不同步，Esc 直接退出播放。
    connect(m_player, &MpvPlayer::toggleFullscreenRequested, this, [this]() {
        if (m_playerPage)
            QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested", Qt::DirectConnection);
    });
    connect(m_player, &MpvPlayer::nextEpisodeRequested, this, [this]() {
        qInfo() << "PlayerController: mpv next episode item=" << m_embyItemId
                << "queue=" << m_episodeQueue.size() << "index=" << m_episodeIndex;
        if (!m_embyItemId.isEmpty())
            stepEmbyEpisode(1);
        else if (m_player->isReady())
            m_player->playlistNext();
    });
    connect(m_player, &MpvPlayer::previousEpisodeRequested, this, [this]() {
        qInfo() << "PlayerController: mpv previous episode item=" << m_embyItemId
                << "queue=" << m_episodeQueue.size() << "index=" << m_episodeIndex;
        if (!m_embyItemId.isEmpty())
            stepEmbyEpisode(-1);
        else if (m_player->isReady())
            m_player->playlistPrevious();
    });
    connect(m_player, &MpvPlayer::exitFullscreenRequested, this, [this]() {
        if (m_playerPage && m_window->isFullScreen()) {
            QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested", Qt::DirectConnection);
        } else if (m_window->isPlayerMode()) {
            // 焦点在 mpv 子窗口时 Qt 的 Esc 快捷键收不到；由 mpv 的 ESC
            // 绑定回到这里，行为与 UI 一致：没全屏才退出播放页。
            m_window->setPlayerMode(false);
            updatePlayerWindowChrome();
        }
    });
    // 全屏按钮/mpv F 走 PlayerPage 信号；Qt F11 走 WindowStateChange。
    // 延迟到 toggleFullscreen 完成后再同步菜单栏，避免读到旧状态。
    connect(m_playerPage, &ui::PlayerPage::fullscreenRequested, this, [this]() {
        qInfo() << "PlayerController: fullscreenRequested received; window fullscreen="
                << m_window->isFullScreen() << "playerMode=" << m_window->isPlayerMode();
        QTimer::singleShot(0, this, &PlayerController::updatePlayerWindowChrome);
    });

    // PlayerPage 的控制信号 -> MpvPlayer。
    // 注意：ui::MainWindow 内部也连接了这些信号（用于 mock 状态演示），
    // 这里是真实播放层附加的连接；真实 mpv 事件会再回填 setPlaying/setPosition。
    connect(m_playerPage, &ui::PlayerPage::playPauseRequested, this, [this]() {
        if (m_player->isReady())
            m_player->togglePause();
    });
    connect(m_playerPage, &ui::PlayerPage::previousEpisodeRequested, this, [this]() {
        qInfo() << "PlayerController: previousEpisodeRequested item=" << m_embyItemId
                << "queue=" << m_episodeQueue.size() << "index=" << m_episodeIndex;
        if (!m_embyItemId.isEmpty())
            stepEmbyEpisode(-1);
        else if (m_player->isReady())
            m_player->playlistPrevious();
        else
            m_playerPage->setEpisodeNavigationEnabled(false, false);
    });
    connect(m_playerPage, &ui::PlayerPage::nextEpisodeRequested, this, [this]() {
        qInfo() << "PlayerController: nextEpisodeRequested item=" << m_embyItemId
                << "queue=" << m_episodeQueue.size() << "index=" << m_episodeIndex;
        if (!m_embyItemId.isEmpty())
            stepEmbyEpisode(1);
        else if (m_player->isReady())
            m_player->playlistNext();
        else
            m_playerPage->setEpisodeNavigationEnabled(false, false);
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
        updatePlayerWindowChrome();
        scheduleHomeRefresh(600);
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
            if (m_player && m_player->isReady()) {
                // 先记住位置再 stop；否则 mpv 的 position 可能已经归零，
                // Stopped 上报 0 会让服务器把条目从 Continue Watching 移除。
                m_lastKnownPositionSeconds = m_player->position();
                m_player->stop();
            }
            if (!m_embyItemId.isEmpty() || m_reporting)
                clearEmbySession();
            if (m_window->isPlayerMode())
                m_window->setPlayerMode(false);
            updatePlayerWindowChrome();
            scheduleHomeRefresh(600);
        });
    }

    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            if (m_reporting)
                reportPlaybackStopped(true);
        });
    }

    // T30/WIN-01：主窗口最小尺寸统一由 ui::MainWindow 的 800x480 决定；
    // PlayerController 不再覆盖 widget minimumSize（T15 的 720x480 已删除）。
    // 保留 size grip 与小屏初始尺寸收紧逻辑。
    const QSize uiMinimum = m_window->minimumSize();
    m_window->installEventFilter(this);
    connect(m_window, &ui::MainWindow::playerModeChanged, this,
            [this](bool) { updatePlayerWindowChrome(); });
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
    m_window->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
    if (QStatusBar *status = m_window->statusBar())
        status->setSizeGripEnabled(true);
    if (QScreen *screen = m_window->screen()) {
        // 默认 1280x760 在 1366x768/高 DPI 小屏上会超出可用区域，用户容易误以为
        // 窗口“不能调整”。先按可用区域收紧初始尺寸，再允许自由缩放。
        const QSize available = screen->availableGeometry().size();
        if (available.isValid()) {
            QSize target = m_window->size().boundedTo(available - QSize(48, 96));
            target = target.expandedTo(QSize(800, 480));
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

PlayerController::~PlayerController()
{
    // T29/SYN-03：窗口关闭/Controller 析构时兜底上报 Stopped；aboutToQuit 和
    // MainWindow closeEvent 路径也会调用，reportPlaybackStopped() 自身有 m_reporting 防重。
    if (m_reporting)
        reportPlaybackStopped(true);

    // spike/render-d3d11：先摘掉 VideoHost 的渲染回调（std::function 捕获 this，
    // 不会随 QObject 自动断开），再释放 mpv。MpvPlayer::shutdown() 内部保证
    // mpv_render_context_free() 在 mpv_terminate_destroy() 之前。
    if (m_videoHost)
        m_videoHost->setFrameRenderer(VideoHost::FrameRenderer());
    if (m_player)
        m_player->shutdown();
}

void PlayerController::applyPlayerSettings()
{
    QSettings settings;
    m_autoNextEpisode = settings.value(QStringLiteral("player/autoPlayNext"), true).toBool();
    m_autoResume = settings.value(QStringLiteral("player/autoResume"), true).toBool();
    m_hwdecMode = settings.value(QStringLiteral("player/hwdecMode"), QStringLiteral("auto"))
                      .toString()
                      .trimmed()
                      .toLower();
    if (m_hwdecMode != QLatin1String("off") && m_hwdecMode != QLatin1String("prefer"))
        m_hwdecMode = QStringLiteral("auto");
    m_rememberVolume = settings.value(QStringLiteral("player/rememberVolume"), true).toBool();
    m_savedVolume = qBound(0, settings.value(QStringLiteral("player/volume"), 80).toInt(), 100);
    m_subtitleLanguage = settings.value(QStringLiteral("player/subtitleLanguage"),
                                        QStringLiteral("chi,zho,zh"))
                              .toString();
    const int hideDelay = qBound(500,
                                 settings.value(QStringLiteral("player/autoHideDelayMs"), 2500).toInt(),
                                 10000);

    if (m_playerPage) {
        m_playerPage->setOverlayHideDelay(hideDelay);
        if (m_rememberVolume)
            m_playerPage->setVolume(m_savedVolume);
    }
    if (m_player && m_player->isReady())
        applyMpvSettings();
    qInfo() << "player settings applied: slang" << normalizedSubtitleLanguage()
            << "autoNext" << m_autoNextEpisode << "autoResume" << m_autoResume
            << "hwdecMode" << m_hwdecMode << "rememberVolume" << m_rememberVolume
            << "volume" << m_savedVolume << "hideDelay" << hideDelay;
}

QString PlayerController::normalizedSubtitleLanguage() const
{
    QString language = m_subtitleLanguage.trimmed();
    if (language.isEmpty() || language.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("no");
    return language;
}

void PlayerController::applyMpvSettings()
{
    if (!m_player || !m_player->isReady())
        return;
    // mpv 的 slang 为空/不自动开启时用 "no"，避免空字符串被当成无效语言列表。
    m_player->setPropertyString("slang", normalizedSubtitleLanguage());
    m_player->setPropertyString("hwdec", hwdecMpvValue(m_hwdecMode));
    m_hwdecFallbackStage = 0;
    if (m_playerPage)
        m_player->setVolume(m_playerPage->volume());
}

void PlayerController::updateHardwareInfo()
{
    if (!m_player || !m_player->isReady())
        return;
    if (auto *settings = m_window->settingsPage()) {
        const QString hwdec = m_player->getPropertyString("hwdec-current");
        const QString vo = m_player->getPropertyString("vo");
        const QString gpuApi = m_player->getPropertyString("gpu-api");
        settings->setHardwareDecoderText(
            QStringLiteral("hwdec-current=%1 · vo=%2 · gpu-api=%3")
                .arg(hwdec.isEmpty() ? QStringLiteral("no") : hwdec, vo, gpuApi));
    }
}

void PlayerController::applyHardwareFallback(const QString &hwdec)
{
    if (m_hwdecMode == QLatin1String("off") || m_hwdecFallbackStage >= 2)
        return;
    const QString current = hwdec.trimmed().toLower();
    const bool bad = current.isEmpty() || current == QLatin1String("no")
                     || current.contains(QLatin1String("-copy"));
    if (!bad) {
        m_hwdecFallbackStage = 0;
        return;
    }
    const QString next = m_hwdecFallbackStage == 0 ? QStringLiteral("auto-safe")
                                                   : QStringLiteral("no");
    const bool applied = m_player && m_player->setPropertyString("hwdec", next);
    ++m_hwdecFallbackStage;
    qWarning() << "PlayerController: hwdec fallback" << hwdec << "->" << next
               << "stage=" << m_hwdecFallbackStage << "applied=" << applied;
    const QString message = m_hwdecFallbackStage == 1
                                ? tr("硬件解码不可用，已切换兼容模式（auto-safe）")
                                : tr("硬件解码不可用，已切换软解");
    showStatus(message, 5000);
    // 播放模式状态栏隐藏，额外用 mpv OSD 保证用户能看到降级提示。
    if (m_player && m_player->isReady() && m_player->command({QStringLiteral("show-text"), message,
                                                              QStringLiteral("5000")}))
        qInfo() << "hardware fallback OSD shown";
}

bool PlayerController::eventFilter(QObject *watched, QEvent *event)
{
    // T29/SYN-03：窗口 closeEvent 先于对象析构；保证上报最后已知位置而不是 0。
    if (watched == m_window && event->type() == QEvent::Close) {
        if (m_reporting)
            reportPlaybackStopped(true);
    }

    // 键盘转发：render API 下视频没有独立的 VO 窗口，Qt 主窗口把按键转成
    // mpv `keypress`，空格/方向键/F/n/p/i 等 input.conf 绑定才会生效。
    // Esc/F11 留给 Qt 自己的快捷键；Ctrl/Alt 组合也不转发。
    // T63/A-P1-20：同时安装在 QApplication 和 MainWindow 上的过滤器会对同一
    // KeyPress 各转发一次；只允许 app 层转发，消除空格/F/N/P 的双触发。
    if (event->type() == QEvent::KeyPress && watched == QCoreApplication::instance()
        && m_window && m_window->isPlayerMode() && m_window->isActiveWindow() && m_player
        && m_player->isReady() && !QApplication::activeModalWidget()) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const QString keyName = mpvKeyNameFromEvent(keyEvent);
        if (!keyName.isEmpty())
            forwardKeyPressToMpv(keyName);
    }

    if (watched == m_window && event->type() == QEvent::WindowStateChange)
        updatePlayerWindowChrome();

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
    // T40：不再创建 QMenuBar；保留文件动作的快捷键和诊断导出 QAction，
    // 供设置页按钮触发。Ctrl+O / Ctrl+Shift+O / Ctrl+Q 仍然可用。
    auto *openFileAction = new QAction(tr("打开文件…"), m_window);
    openFileAction->setShortcut(QKeySequence::Open);
    m_window->addAction(openFileAction);
    connect(openFileAction, &QAction::triggered, this, &PlayerController::openFile);

    auto *openFolderAction = new QAction(tr("打开文件夹（作为播放列表）…"), m_window);
    openFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    m_window->addAction(openFolderAction);
    connect(openFolderAction, &QAction::triggered, this, &PlayerController::openFolder);

    auto *quitAction = new QAction(tr("退出"), m_window);
    quitAction->setShortcut(QKeySequence::Quit);
    m_window->addAction(quitAction);
    connect(quitAction, &QAction::triggered, m_window, &QWidget::close);

    auto *exportAction = new QAction(tr("导出诊断信息…"), m_window);
    exportAction->setObjectName(QStringLiteral("ExportDiagnosticsAction"));
    m_window->addAction(exportAction);
    connect(exportAction, &QAction::triggered, this, &PlayerController::exportDiagnostics);
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

    // T65：本地播放不再携带旧 Emby Authorization（空值 setter 不含 token）。
    if (m_player && m_player->isReady() && !m_player->httpHeaderLines().isEmpty())
        m_player->clearHttpHeaderFields();
    if (!ensurePlayer())
        return;
    if (m_playbackWatchdog)
        m_playbackWatchdog->stop();
    m_playbackCandidates.clear();
    m_playbackCandidateIndex = 0;
    m_currentPlaybackUrl.clear();
    m_playbackFileLoaded = false;
    m_playbackFailurePrompted = false;
    m_playbackRetryCount = 0;
    m_localPlayback = true;
    m_localFiles = files;
    m_localStartIndex = qBound(0, startIndex, static_cast<int>(files.size()) - 1);
    m_lastKnownPositionSeconds = 0.0;

    const QFileInfo firstInfo(files.first());
    m_currentSubtitle = tr("本地播放 · %1 个文件 · %2")
                            .arg(files.size())
                            .arg(firstInfo.absolutePath());
    m_player->loadPlaylist(files, qBound(0, startIndex, static_cast<int>(files.size()) - 1));
    if (m_playbackWatchdog)
        m_playbackWatchdog->start(15000);
    if (m_playerPage) {
        const bool multiple = files.size() > 1;
        m_playerPage->setEpisodeNavigationEnabled(multiple && startIndex > 0,
                                                  multiple && startIndex + 1 < files.size());
    }
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
    // spike/render-d3d11：Qt 的 QRhi 可能还没初始化（首次播放时 PlayerPage 还
    // 没 paint），此时 renderDevice=0，MpvPlayer 会先把 loadfile 挂起，等
    // VideoHost::d3d11DeviceReady → handleRenderDeviceReady 再建 context。
    const quintptr renderDevice = m_videoHost ? m_videoHost->d3d11Device() : 0;
    if (m_player->isReady()) {
        // T63/A-P0-02：运行时 http-header-fields setter 会把完整 token 写进 mpv.log。
        // 只在 header 变化时销毁重建（新实例在 log-file 生效前注入 header）。
        if (m_player->httpHeaderLines() != httpHeaderLines) {
            qInfo() << "mpv headers changed; reinitializing before load (no runtime token setter)"
                    << "renderDevice=" << renderDevice;
            m_player->setHardwareDecoderMode(hwdecMpvValue(m_hwdecMode));
            QString error;
            if (!m_player->reinitialize(renderDevice, httpHeaderLines, &error)) {
                QMessageBox::critical(m_window, tr("播放内核初始化失败"), error);
                return false;
            }
            applyMpvSettings();
            updateHardwareInfo();
            logMpvState(QStringLiteral("reinitialized-http-headers"));
        }
        // 重试路径：上一次 render context 创建失败/设备后到时，这里补一次 attach。
        // （设备已缓存但 context 不在时，不会再有 d3d11DeviceReady 信号。）
        if (!m_player->hasRenderContext() && renderDevice != 0) {
            QString attachError;
            if (!m_player->attachRenderDevice(renderDevice, &attachError)) {
                QMessageBox::critical(m_window, tr("播放内核初始化失败"), attachError);
                return false;
            }
            logMpvState(QStringLiteral("render-context-attached"));
        }
        return true;
    }

    // T63/A-P0-10：初始化前应用硬解模式（auto→auto-safe、prefer→d3d11va、off→no）。
    m_player->setHardwareDecoderMode(hwdecMpvValue(m_hwdecMode));

    if (!m_surfaceAttached) {
        m_playerPage->attachVideoSurface(m_videoHost);
        m_surfaceAttached = true;
    }

    QString error;
    if (!m_player->initialize(renderDevice, httpHeaderLines, &error)) {
        QMessageBox::critical(m_window, tr("播放内核初始化失败"), error);
        m_playerPage->releaseVideoSurface();
        m_videoHost->setParent(m_playerPage);
        m_videoHost->hide();
        m_surfaceAttached = false;
        return false;
    }

    applyMpvSettings();
    updateHardwareInfo();
    logMpvState(QStringLiteral("initialized"));
    return true;
}

void PlayerController::handleRenderDeviceReady(quintptr device)
{
    if (!device)
        return;
    if (!m_player || !m_player->isReady()) {
        // 首次播放时 device 常常先于 ensurePlayer；VideoHost 已缓存，
        // 下次 initialize(renderDevice, ...) 直接用。
        qInfo() << "PlayerController: QRhi D3D11 device ready before mpv init; device"
                << Qt::hex << device;
        return;
    }
    if (m_player->hasRenderContext() && m_player->renderDevice() == device)
        return;

    QString error;
    if (!m_player->attachRenderDevice(device, &error)) {
        qWarning() << "PlayerController: attach D3D11 render device failed" << error;
        // 正在起播（已有 item/本地文件/候选）才走失败弹窗；否则只提示。
        const bool playbackStarting =
            !m_playbackFileLoaded
            && (!m_embyItemId.isEmpty() || m_localPlayback || !m_playbackCandidates.isEmpty());
        if (playbackStarting)
            handlePlaybackFailure(tr("D3D11 渲染上下文创建失败：%1").arg(error));
        else
            showStatus(tr("D3D11 渲染上下文创建失败：%1").arg(error), 8000);
        return;
    }

    qInfo() << "PlayerController: mpv D3D11 render context ready; device" << Qt::hex << device
            << "hwdec" << m_player->hardwareDecoderMode();
    logMpvState(QStringLiteral("render-context-ready"));
    if (m_videoHost)
        m_videoHost->requestFrame();
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
    m_emby->fetchItems(query, [self, request, seriesId, seriesTitle, season, episode](
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
            qInfo() << "PlayerController::loadEpisodeQueue empty series=" << seriesId;
            if (self->m_playerPage)
                self->m_playerPage->setEpisodeNavigationEnabled(false, false);
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
        qInfo() << "PlayerController::loadEpisodeQueue series=" << seriesId
                << "queue=" << self->m_episodeQueue.size() << "index=" << index;
        if (self->m_playerPage)
            self->m_playerPage->setEpisodeNavigationEnabled(
                index > 0, index + 1 < self->m_episodeQueue.size());
        const EpisodeEntry entry = self->m_episodeQueue.at(index);
        self->playEmbyItem(entry.itemId,
                           entry.title.isEmpty() ? seriesTitle : entry.title,
                           entry.season, entry.episode, entry.startPositionTicks);
    });
}

void PlayerController::applyEpisodeEntries(const QList<EpisodeEntry> &entries,
                                                   const QString &currentItemId,
                                                   const QString &seriesId,
                                                   const QString &seasonId)
{
    m_episodeQueue = entries;
    m_episodeIndex = -1;
    for (int i = 0; i < m_episodeQueue.size(); ++i) {
        if (m_episodeQueue.at(i).itemId == currentItemId) {
            m_episodeIndex = i;
            break;
        }
    }
    const bool hasPrev = m_episodeIndex > 0;
    const bool hasNext = m_episodeIndex >= 0 && m_episodeIndex + 1 < m_episodeQueue.size();
    if (m_playerPage)
        m_playerPage->setEpisodeNavigationEnabled(hasPrev, hasNext);
    qInfo() << "PlayerController::episodeSiblings series=" << seriesId
            << "season=" << seasonId << "current=" << currentItemId
            << "count=" << m_episodeQueue.size() << "index=" << m_episodeIndex
            << "prev=" << hasPrev << "next=" << hasNext;
}

void PlayerController::applyEpisodeList(const QList<emby::MediaItem> &items,
                                        const QString &currentItemId,
                                        const QString &seriesId, const QString &seasonId)
{
    QList<EpisodeEntry> entries;
    entries.reserve(items.size());
    for (const emby::MediaItem &item : items) {
        if (item.type != QLatin1String("Episode"))
            continue;
        EpisodeEntry entry;
        entry.itemId = item.id;
        entry.title = item.name;
        entry.season = item.parentIndexNumber;
        entry.episode = item.indexNumber;
        entry.runTimeTicks = item.runTimeTicks;
        entry.startPositionTicks = item.userData.playbackPositionTicks;
        entries.append(entry);
    }
    applyEpisodeEntries(entries, currentItemId, seriesId, seasonId);
}

void PlayerController::requestEpisodeSiblings(const QString &episodeId)
{
    if (!m_emby || episodeId.isEmpty())
        return;

    // 已在队列中（详情页 loadEpisodeQueue / 上一集下一集）直接同步按钮，避免重复请求。
    for (int i = 0; i < m_episodeQueue.size(); ++i) {
        if (m_episodeQueue.at(i).itemId == episodeId) {
            m_episodeIndex = i;
            if (m_playerPage)
                m_playerPage->setEpisodeNavigationEnabled(
                    i > 0, i + 1 < m_episodeQueue.size());
            return;
        }
    }

    const int serial = ++m_episodeRequestSerial;
    QPointer<PlayerController> self(this);
    m_emby->fetchItem(episodeId, [self, serial, episodeId](const emby::MediaItem &item,
                                                          const emby::ApiError &error) {
        if (!self || serial != self->m_episodeRequestSerial)
            return;
        if (error.isError()) {
            qWarning() << "PlayerController::requestEpisodeSiblings item failed"
                       << episodeId << error.toString();
            // 保守：获取失败也允许点击，点击时再重试；电影会在成功拿到 type 后禁用。
            if (self->m_playerPage && self->m_episodeQueue.isEmpty())
                self->m_playerPage->setEpisodeNavigationEnabled(true, true);
            return;
        }
        if (item.type != QLatin1String("Episode")) {
            qInfo() << "PlayerController::episodeSiblings not episode type=" << item.type
                    << "item=" << episodeId;
            if (self->m_playerPage)
                self->m_playerPage->setEpisodeNavigationEnabled(false, false);
            return;
        }

        const QString seriesId = item.seriesId;
        const QString seasonId = item.seasonId.isEmpty() ? item.parentId : item.seasonId;
        const QString cacheKey = !seriesId.isEmpty() ? seriesId : seasonId;
        if (cacheKey.isEmpty()) {
            qWarning() << "PlayerController::requestEpisodeSiblings no series/season for"
                       << episodeId;
            if (self->m_playerPage)
                self->m_playerPage->setEpisodeNavigationEnabled(false, false);
            return;
        }

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto cached = self->m_episodeCache.find(cacheKey);
        if (cached != self->m_episodeCache.end()
            && now - cached->fetchedAtMs < 5 * 60 * 1000) {
            qInfo() << "PlayerController::episodeSiblings cache hit" << cacheKey
                    << "count=" << cached->entries.size();
            self->applyEpisodeEntries(cached->entries, episodeId, seriesId, seasonId);
            return;
        }

        qInfo() << "PlayerController::requestEpisodeSiblings series=" << seriesId
                << "season=" << seasonId << "item=" << episodeId;
        emby::ItemQuery query;
        query.parentId = !seriesId.isEmpty() ? seriesId : seasonId;
        query.recursive = !seriesId.isEmpty();
        query.includeItemTypes = {QStringLiteral("Episode")};
        query.sortBy = {QStringLiteral("ParentIndexNumber"), QStringLiteral("IndexNumber")};
        query.sortOrder = QStringLiteral("Ascending");
        query.limit = 1000;

        self->m_emby->fetchItems(
            query, [self, serial, episodeId, seriesId, seasonId, cacheKey](
                        const emby::ItemQueryResult &result, const emby::ApiError &error) {
                if (!self || serial != self->m_episodeRequestSerial)
                    return;
                if (error.isError()) {
                    qWarning() << "PlayerController::requestEpisodeSiblings list failed"
                               << cacheKey << error.toString();
                    if (self->m_playerPage && self->m_episodeQueue.isEmpty())
                        self->m_playerPage->setEpisodeNavigationEnabled(true, true);
                    return;
                }
                self->applyEpisodeList(result.items, episodeId, seriesId, seasonId);
                CachedEpisodeList cache;
                cache.fetchedAtMs = QDateTime::currentMSecsSinceEpoch();
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
                    cache.entries.append(entry);
                }
                self->m_episodeCache.insert(cacheKey, cache);
            });
    });
}

void PlayerController::requestEpisodeSiblingsForTest(const QString &episodeId)
{
    requestEpisodeSiblings(episodeId);
}

void PlayerController::playEmbyItem(const QString &itemId, const QString &title,
                                    int /*season*/, int /*episode*/,
                                    qint64 startPositionTicks)
{
    if (!m_emby || !m_window->hasSession()) {
        showStatus(tr("未登录 Emby，无法播放"), 3000);
        return;
    }

    if (m_playerPage) {
        if (m_episodeQueue.isEmpty())
            m_playerPage->setEpisodeNavigationEnabled(false, false);
        else
            m_playerPage->setEpisodeNavigationEnabled(
                m_episodeIndex > 0, m_episodeIndex + 1 < m_episodeQueue.size());
    }
    // T36：单集点播/继续观看也异步补兄弟集；已在队列中时直接同步按钮。
    requestEpisodeSiblings(itemId);

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

    // T63/A-P0-07：所有 Emby 续播入口统一过 >95%/autoResume 规则。
    startPositionTicks = adjustedResumeTicks(startPositionTicks, source->runTimeTicks, m_autoResume);
    qInfo() << "PlaybackInfo: resume startPositionTicks=" << startPositionTicks
            << "autoResume=" << m_autoResume << "duration=" << source->runTimeTicks;

    m_localPlayback = false;
    m_localFiles.clear();
    m_localStartIndex = 0;
    m_playbackRetryCount = 0;

    const QString playMethod = source->supportsDirectPlay
                                   ? QStringLiteral("DirectPlay")
                                   : (source->supportsDirectStream ? QStringLiteral("DirectStream")
                                                                   : QStringLiteral("Transcode"));

    // T48：候选 URL。直连优先服务器 DirectStreamUrl（playbackUrl 内已优先），
    // 再补自拼 static 和服务器原始 URL；首次失败自动切下一个。
    QList<QUrl> candidateUrls;
    auto addCandidate = [&candidateUrls](const QUrl &url) {
        if (!url.isValid() || url.isEmpty())
            return;
        const QString text = url.toString();
        for (const QUrl &existing : candidateUrls) {
            if (existing.toString() == text)
                return;
        }
        candidateUrls.append(url);
    };
    addCandidate(m_emby->playbackUrl(itemId, *source));
    addCandidate(m_emby->directStreamUrl(itemId, source->id));
    addCandidate(m_emby->resolveMediaUrl(source->directStreamUrl));
    addCandidate(m_emby->resolveMediaUrl(source->transcodingUrl));

    QStringList candidates;
    for (const QUrl &candidate : candidateUrls)
        candidates.append(candidate.toString());
    // T63/A-P0-08：候选最多 3 个（首播 + 2 次重试），不超过 PLY-12。
    if (candidates.size() > 3)
        candidates = candidates.mid(0, 3);
    if (candidates.isEmpty()) {
        qWarning() << "PlaybackInfo: no URL item=" << itemId << "method=" << playMethod
                   << "sourceId=" << source->id;
        showStatus(tr("无法构造播放地址"), 4000);
        handlePlaybackFailure(QStringLiteral("no candidate URL"));
        return;
    }

    const emby::MediaStream *video = source->videoStream();
    const emby::MediaStream *audio = source->audioStream();
    qInfo() << "PlaybackInfo: item" << itemId << "title" << title << "method" << playMethod
            << "supportsDirectPlay" << source->supportsDirectPlay
            << "supportsDirectStream" << source->supportsDirectStream
            << "supportsTranscoding" << source->supportsTranscoding
            << "mediaSourceId" << source->id
            << "hasDirectStreamUrl" << !source->directStreamUrl.isEmpty()
            << "hasTranscodingUrl" << !source->transcodingUrl.isEmpty()
            << "url" << sanitizedPlaybackUrl(candidates.first())
            << "runTimeTicks" << source->runTimeTicks << "container" << source->container
            << "videoCodec" << (video ? video->codec : QString())
            << "videoSize" << (video ? QSize(video->width, video->height) : QSize())
            << "audioCodec" << (audio ? audio->codec : QString())
            << "audioChannels" << (audio ? audio->channels : 0)
            << "candidates" << candidates.size();
    for (int i = 0; i < candidates.size(); ++i) {
        qInfo() << "PlaybackInfo: candidate" << i << "/" << candidates.size()
                << sanitizedPlaybackUrl(candidates.at(i));
    }

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

    // T63/T65：header 按候选 URL 同源判断；CDN/第三方候选不带 Authorization。
    if (!ensurePlayer(m_emby->streamHeaderLines(QUrl(candidates.first()))))
        return;

    m_player->setPropertyString("force-media-title", title.isEmpty() ? itemId : title);
    if (source->runTimeTicks > 0)
        m_playerPage->setPosition(0, source->runTimeTicks / 10000);
    m_lastKnownPositionSeconds = 0.0;
    m_pendingSeekSeconds = startPositionTicks > 0 ? secondsFromTicks(startPositionTicks) : 0.0;
    m_playbackCandidates = candidates;
    m_playbackCandidateIndex = 0;
    m_playbackFileLoaded = false;
    m_playbackFailurePrompted = false;
    startPlaybackCandidate(0);

    showPlayerPage();
    m_playerPage->setEpisodeInfo(title.isEmpty() ? itemId : title, m_currentSubtitle);
    m_playerPage->setPlaying(true);
    m_playerPage->setVolume(m_playerPage->volume());
    ensurePlayerControlsVisible();

    m_window->setWindowTitle(QStringLiteral("Emby PC — %1").arg(title.isEmpty() ? itemId : title));
    reportPlaybackStart();
    showStatus(tr("开始播放：%1").arg(playMethod), 2500);
}

QString PlayerController::sanitizedPlaybackUrl(const QString &urlString) const
{
    QUrl url(urlString);
    if (!url.isValid() || url.isEmpty())
        return {};
    QString result = url.scheme() + QStringLiteral("://") + url.host();
    if (url.port() > 0)
        result += QStringLiteral(":") + QString::number(url.port());
    result += url.path();
    return result;
}

void PlayerController::startPlaybackCandidate(int index)
{
    if (!m_player || index < 0 || index >= m_playbackCandidates.size())
        return;
    m_playbackCandidateIndex = index;
    m_currentPlaybackUrl = m_playbackCandidates.at(index);
    m_playbackFileLoaded = false;
    // 候选可能跨 CDN host；重新确认该 URL 的 header（跨源为空，需要时重建 mpv）。
    if (m_emby && !m_embyItemId.isEmpty()
        && !ensurePlayer(m_emby->streamHeaderLines(QUrl(m_currentPlaybackUrl)))) {
        handlePlaybackFailure(QStringLiteral("mpv header/reinitialize failed"));
        return;
    }
    m_hwdecFallbackStage = 0;
    qInfo() << "PlayerController::playback candidate start" << index + 1 << "/"
            << m_playbackCandidates.size() << "url"
            << sanitizedPlaybackUrl(m_currentPlaybackUrl);
    m_player->loadPlaylist({m_currentPlaybackUrl}, 0);
    if (m_playbackWatchdog) {
        // A-P1-21：首次起播给 15s 宽限（冷启动/大文件），重试候选 8s。
        m_playbackWatchdog->start(index == 0 ? 15000 : 8000);
    }
}

void PlayerController::tryNextPlaybackCandidate(const QString &reason)
{
    if (m_playbackFileLoaded)
        return;
    if (m_playbackWatchdog)
        m_playbackWatchdog->stop();
    if (m_playbackCandidateIndex + 1 < m_playbackCandidates.size()) {
        qWarning() << "PlayerController::playback candidate failed" << reason
                   << "retry next" << m_playbackCandidateIndex + 2 << "/"
                   << m_playbackCandidates.size();
        startPlaybackCandidate(m_playbackCandidateIndex + 1);
        return;
    }
    handlePlaybackFailure(reason);
}

void PlayerController::handlePlaybackTimeout()
{
    if (m_playbackFileLoaded)
        return;
    if (m_embyItemId.isEmpty() && !m_localPlayback)
        return;
    qWarning() << "PlayerController::playback watchdog no file-loaded item"
               << m_embyItemId << "local=" << m_localPlayback << "url"
               << sanitizedPlaybackUrl(m_currentPlaybackUrl);
    tryNextPlaybackCandidate(QStringLiteral("watchdog timeout without file-loaded"));
}

void PlayerController::handlePlaybackFailure(const QString &reason)
{
    if (m_playbackFileLoaded)
        return;
    if (m_playbackWatchdog)
        m_playbackWatchdog->stop();

    QString mpvError = m_player ? m_player->getPropertyString("error") : QString();
    static const QRegularExpression tokenPattern(
        QStringLiteral("(?i)((?:api_key|X-Emby-Token|Token|Authorization)\\s*[=:]\\s*)[^&,\\s]+"));
    mpvError.replace(tokenPattern, QStringLiteral("\\1<redacted>"));
    const QString httpStatus = m_player ? m_player->getPropertyString("http-status") : QString();
    QString code = QStringLiteral("PLAYBACK_FAILED");
    if (m_localPlayback)
        code = QStringLiteral("LOCAL_FILE_ERROR");
    else if (httpStatus == QLatin1String("403"))
        code = QStringLiteral("HTTP_403");
    else if (httpStatus == QLatin1String("404"))
        code = QStringLiteral("HTTP_404");
    else if (!httpStatus.isEmpty() && httpStatus.toInt() >= 500)
        code = QStringLiteral("HTTP_5XX");
    else if (reason.contains(QLatin1String("timeout"), Qt::CaseInsensitive))
        code = QStringLiteral("PLAYBACK_TIMEOUT");
    else if (m_playbackCandidates.isEmpty() && m_embyItemId.isEmpty())
        code = QStringLiteral("NO_MEDIA_SOURCE");

    QString detail = reason;
    if (!mpvError.isEmpty())
        detail += QStringLiteral("\nmpv: ") + mpvError;
    if (!httpStatus.isEmpty())
        detail += QStringLiteral("\nHTTP: ") + httpStatus;
    if (!m_currentPlaybackUrl.isEmpty())
        detail += QStringLiteral("\nURL: ") + sanitizedPlaybackUrl(m_currentPlaybackUrl);

    qWarning() << "PlayerController::playback FAILED code" << code << "reason" << reason
               << "item" << m_embyItemId << "mediaSourceId" << m_mediaSourceId
               << "method" << m_playMethod << "candidate"
               << m_playbackCandidateIndex + 1 << "/" << m_playbackCandidates.size()
               << "url" << sanitizedPlaybackUrl(m_currentPlaybackUrl)
               << "mpvError" << mpvError << "httpStatus" << httpStatus
               << "retryCount" << m_playbackRetryCount;

    if (m_playbackFailurePrompted)
        return;
    // A-P0-08：失败时先结束会话，避免服务器留下幽灵 Playing。
    if (m_reporting)
        reportPlaybackStopped(false);
    m_playbackFailurePrompted = true;
    m_lastPlaybackErrorCode = code;
    m_lastPlaybackErrorDetail = detail;
    showStatus(tr("播放失败（%1），已记录诊断，请导出 app.log/mpv.log").arg(code), 8000);
    if (qEnvironmentVariableIntValue("EMBY_PC_TEST_NO_DIALOG") > 0)
        return;
    showPlaybackErrorDialog(code, detail, true);
}

void PlayerController::showPlaybackErrorDialog(const QString &code, const QString &detail,
                                               bool retryable)
{
    // T64/B1-B：统一错误对话框，返回 Retry/Back/Export 动作。
    ui::PlaybackErrorDialog::Info info;
    info.code = code;
    info.title = tr("播放失败");
    info.detail = detail;
    info.diagnostics = redactDiagnosticText(diagnosticSummary());
    info.retryEnabled = retryable && m_playbackRetryCount < 2;
    const ui::PlaybackErrorDialog::Action action =
        ui::PlaybackErrorDialog::show(m_window, info);
    if (action == ui::PlaybackErrorDialog::Action::Retry) {
        if (m_playbackRetryCount >= 2) {
            showStatus(tr("已达到最大重试次数（2 次）"), 5000);
            return;
        }
        ++m_playbackRetryCount;
        m_playbackFailurePrompted = false;
        qInfo() << "playback retry requested count=" << m_playbackRetryCount
                << "code=" << m_lastPlaybackErrorCode;
        retryPlaybackAfterFailure();
    } else if (action == ui::PlaybackErrorDialog::Action::Back) {
        leavePlaybackAfterFailure();
    } else if (action == ui::PlaybackErrorDialog::Action::Export) {
        exportDiagnostics();
    } else {
        // 用户直接关闭对话框：与“返回”一致，避免停在失败播放页。
        leavePlaybackAfterFailure();
    }
}

void PlayerController::retryPlaybackAfterFailure()
{
    m_playbackFailurePrompted = false;
    if (m_localPlayback && !m_localFiles.isEmpty()) {
        const int retryCount = m_playbackRetryCount;
        playFiles(m_localFiles, m_localStartIndex);
        m_playbackRetryCount = retryCount;
        return;
    }
    if (!m_playbackCandidates.isEmpty()) {
        if (!m_reporting && !m_embyItemId.isEmpty())
            reportPlaybackStart();
        startPlaybackCandidate(0);
        return;
    }
    if (!m_embyItemId.isEmpty()) {
        playEmbyItem(m_embyItemId, m_embyTitle, -1, -1, 0);
        return;
    }
    showStatus(tr("没有可重试的播放源"), 4000);
}

void PlayerController::leavePlaybackAfterFailure()
{
    if (m_player && m_player->isReady())
        m_player->stop();
    if (m_reporting)
        reportPlaybackStopped(false);
    clearEmbySession();
    resetLocalPlaybackState();
    if (m_playerPage)
        m_playerPage->setPlaying(false);
    if (m_window->isFullScreen())
        QMetaObject::invokeMethod(m_window, "toggleFullscreen", Qt::DirectConnection);
    if (m_window->isPlayerMode())
        m_window->setPlayerMode(false);
    updatePlayerWindowChrome();
    scheduleHomeRefresh(300);
}

void PlayerController::resetLocalPlaybackState()
{
    m_localPlayback = false;
    m_localFiles.clear();
    m_localStartIndex = 0;
}

void PlayerController::showPlayerPage()
{
    if (!m_playerPage)
        return;

    // T17：播放时让主窗口进入 player mode（隐藏 sidebar/topbar/statusbar，
    // PlayerPage 铺满客户区），退出由 backRequested/stack currentChanged 恢复。
    if (!m_window->isPlayerMode())
        m_window->setPlayerMode(true);
    updatePlayerWindowChrome();

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
    qInfo() << "PlayerController::stepEmbyEpisode delta=" << delta
            << "item=" << m_embyItemId << "queue=" << m_episodeQueue.size()
            << "index=" << m_episodeIndex << "target=" << (m_episodeIndex + delta);
    if (m_episodeQueue.isEmpty() || m_episodeIndex < 0) {
        if (!m_embyItemId.isEmpty() && m_emby) {
            // T36 保守路径：队列尚未建好时允许点击，异步重试兄弟集列表。
            requestEpisodeSiblings(m_embyItemId);
            showStatus(tr("正在获取剧集列表…"), 2000);
            return;
        }
        if (m_playerPage)
            m_playerPage->setEpisodeNavigationEnabled(false, false);
        showStatus(tr("没有上一集/下一集"), 2000);
        return;
    }
    const int nextIndex = m_episodeIndex + delta;
    if (nextIndex < 0 || nextIndex >= m_episodeQueue.size()) {
        if (m_playerPage)
            m_playerPage->setEpisodeNavigationEnabled(
                m_episodeIndex > 0, m_episodeIndex + 1 < m_episodeQueue.size());
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
    qInfo() << "PlayerController::playEpisodeAt index=" << index
            << "queue=" << m_episodeQueue.size();
    if (m_playerPage)
        m_playerPage->setEpisodeNavigationEnabled(
            index > 0, index + 1 < m_episodeQueue.size());
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

void PlayerController::updatePlayerWindowChrome()
{
    // T40：应用不再创建 QMenuBar；播放/全屏只由 TitleBar/内容 chrome 控制。
}

void PlayerController::scheduleHomeRefresh(int delayMs)
{
    if (!m_window)
        return;
    QTimer::singleShot(qMax(0, delayMs), this, [this]() {
        if (!m_window || m_window->isPlayerMode())
            return;
        if (m_window->currentStackIndex() != static_cast<int>(ui::MainWindow::Page::Home))
            return;
        // 播放停止/返回首页后重新拉 Resume；navigateTo(Home) 会在首页重新 fetchHome。
        m_window->navigateTo(ui::MainWindow::Page::Home);
    });
}

bool PlayerController::isDuplicateKeyForward(const QString &name, int source)
{
    if (name.isEmpty())
        return true;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 只抑制“另一个来源”在极短时间内的同名按键；同一来源的自动重复要放行。
    if (name == m_lastForwardedKeyName && source != m_lastForwardedKeySource
        && now - m_lastForwardedKeyMs < 60) {
        return true;
    }
    m_lastForwardedKeyName = name;
    m_lastForwardedKeyMs = now;
    m_lastForwardedKeySource = source;
    return false;
}

void PlayerController::forwardKeyPressToMpv(const QString &name)
{
    if (!m_player || !m_player->isReady() || isDuplicateKeyForward(name, 0))
        return;
    m_player->command({QStringLiteral("keypress"), name});
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_INPUT"))
        qInfo() << "forward key to mpv (qt)" << name;
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
        QStringLiteral("options/load-scripts"), QStringLiteral("options/vo"),
        QStringLiteral("options/video-timing-offset"), QStringLiteral("video-params"),
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
            QStringLiteral("options/load-scripts"), QStringLiteral("options/vo"),
            QStringLiteral("options/video-timing-offset"), QStringLiteral("video-params"),
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

    // T20：海报缓存/来源统计（请求/失败/重试/缺 tag 条目数）。
    if (m_window) {
        const QString posterStats = m_window->posterStatsSummary();
        for (const QString &line : posterStats.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            lines.append(line);
    }

    // T24：首页继续观看数据层已到但 UI 空时，诊断要能区分「没收到回调」和
    // 「卡片创建了但布局/可见性异常」。
    if (m_window) {
        if (auto *home = m_window->findChild<ui::HomeView *>()) {
            const QString homeSummary = home->diagnosticSummary();
            for (const QString &line : homeSummary.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                lines.append(line);
        } else {
            lines.append(QStringLiteral("HomeView: <not found>"));
        }
    }

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
        // T63/A-P0-02：诊断包内日志先脱敏，避免旧版/verbose mpv.log 的 token 被带走。
        copyRedactedLog(dir + QStringLiteral("/app.log"),
                        bundleDir + QStringLiteral("/app.log"));
        copyRedactedLog(dir + QStringLiteral("/mpv.log"),
                        bundleDir + QStringLiteral("/mpv.log"));
        copyRedactedLog(path, bundleDir + QStringLiteral("/diagnostics.txt"));
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

void PlayerController::reportPlaybackStopped(bool waitForCompletion)
{
    if (!m_emby || m_embyItemId.isEmpty())
        return;

    const double seconds = m_lastKnownPositionSeconds > 0.0 ? m_lastKnownPositionSeconds
                                                            : m_player->position();
    emby::PlaybackProgressInfo info;
    info.itemId = m_embyItemId;
    info.mediaSourceId = m_mediaSourceId;
    info.playSessionId = m_playSessionId;
    info.positionTicks = ticksFromSeconds(seconds);
    info.playMethod = m_playMethod;
    info.eventName = QStringLiteral("Stopped");
    qInfo() << "playback stopped report: item" << m_embyItemId
            << "positionSeconds" << seconds;

    m_reporting = false;
    if (m_progressTimer)
        m_progressTimer->stop();
    m_lastKnownPositionSeconds = 0.0;

    // T63/A-P0-09：关闭窗口/退出应用时必须有界等待最后一次 Stopped 完成，
    // 否则 QNetworkReply 会在 EmbyClient 析构时被 abort，服务端续播位置丢失。
    QEventLoop waitLoop;
    QTimer waitTimer;
    waitTimer.setSingleShot(true);
    QObject::connect(&waitTimer, &QTimer::timeout, &waitLoop, &QEventLoop::quit);
    auto finished = QSharedPointer<bool>::create(false);
    QPointer<QEventLoop> waitLoopGuard(&waitLoop);
    QPointer<PlayerController> self(this);
    m_emby->reportPlaybackStopped(info, [self, finished, waitLoopGuard](const emby::ApiError &error) {
        *finished = true;
        if (self && error.isError())
            self->handleApiError(error);
        if (self)
            self->scheduleHomeRefresh(0);
        if (waitLoopGuard)
            waitLoopGuard->quit();
    });
    if (waitForCompletion && !*finished && QCoreApplication::instance()) {
        waitTimer.start(1500);
        waitLoop.exec();
        qInfo() << "playback stopped report wait finished=" << *finished
                << "elapsedMs=" << (1500 - waitTimer.remainingTime());
    }
}

void PlayerController::clearEmbySession()
{
    if (m_reporting)
        reportPlaybackStopped();
    // T65/A-P0-02：logout/切账号/本地播放统一清掉 mpv 旧 Authorization 头。
    if (m_player && m_player->isReady())
        m_player->clearHttpHeaderFields();
    m_progressTimer->stop();
    m_embyItemId.clear();
    m_embyTitle.clear();
    m_mediaSourceId.clear();
    m_playSessionId.clear();
    m_playMethod.clear();
    m_embyDurationTicks = 0;
    m_lastKnownPositionSeconds = 0.0;
    m_episodeQueue.clear();
    m_episodeCache.clear();
    ++m_episodeRequestSerial;
    m_episodeIndex = -1;
    if (m_playbackWatchdog)
        m_playbackWatchdog->stop();
    m_playbackCandidates.clear();
    m_playbackCandidateIndex = 0;
    m_currentPlaybackUrl.clear();
    m_playbackFileLoaded = false;
    m_playbackFailurePrompted = false;
    if (m_playerPage)
        m_playerPage->setEpisodeNavigationEnabled(false, false);
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
