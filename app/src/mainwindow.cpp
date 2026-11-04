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
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
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
        if (m_pendingSeekSeconds > 0.0) {
            m_player->seekAbsolute(m_pendingSeekSeconds);
            m_pendingSeekSeconds = 0.0;
        }
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
    connect(m_player, &MpvPlayer::toggleFullscreenRequested, this, [this]() {
        setFullscreen(!m_window->isFullScreen());
    });
    connect(m_player, &MpvPlayer::exitFullscreenRequested, this, [this]() {
        setFullscreen(false);
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
                m_player->setPropertyString("aid", trackId);
                showStatus(tr("已选择音轨 %1").arg(trackId));
                reportPlaybackProgress(QStringLiteral("AudioTrackChange"));
                QTimer::singleShot(150, this, &PlayerController::refreshTracks);
            });
    // 从播放页返回时恢复 Qt 控制条（它只在真实播放时隐藏），并上报停止。
    connect(m_playerPage, &ui::PlayerPage::backRequested, this, [this]() {
        if (!m_embyItemId.isEmpty())
            clearEmbySession();
        if (auto *bar = m_playerPage->findChild<QWidget *>(QStringLiteral("PlayerControlBar")))
            bar->show();
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
        });
    }

    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            if (m_reporting)
                reportPlaybackStopped();
        });
    }

    connectUiPlaybackSignals();
}

PlayerController::~PlayerController() = default;

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

    if (auto *controlBar = m_playerPage->findChild<QWidget *>(QStringLiteral("PlayerControlBar")))
        controlBar->hide();

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
    if (auto *controlBar = m_playerPage->findChild<QWidget *>(QStringLiteral("PlayerControlBar")))
        controlBar->hide();

    m_window->setWindowTitle(QStringLiteral("Emby PC — %1").arg(title.isEmpty() ? itemId : title));
    reportPlaybackStart();
    showStatus(tr("开始播放：%1").arg(playMethod), 2500);
}

void PlayerController::showPlayerPage()
{
    if (!m_playerPage)
        return;
    QStackedWidget *stack = qobject_cast<QStackedWidget *>(m_playerPage->parentWidget());
    if (!stack)
        stack = m_window->findChild<QStackedWidget *>(QStringLiteral("ContentStack"));
    if (stack)
        stack->setCurrentWidget(m_playerPage);
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
    QList<ui::TrackInfo> audio;
    bool subtitleSelected = false;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject track = value.toObject();
        const QString type = track.value(QStringLiteral("type")).toString();
        const int id = track.value(QStringLiteral("id")).toInt();
        if (id <= 0)
            continue;
        ui::TrackInfo info;
        info.id = QString::number(id);
        info.language = track.value(QStringLiteral("lang")).toString();
        info.codec = track.value(QStringLiteral("codec")).toString();
        info.title = displayTrackTitle(track, id);
        info.selected = track.value(QStringLiteral("selected")).toBool();
        if (type == QLatin1String("sub")) {
            subtitles.append(info);
            subtitleSelected = subtitleSelected || info.selected;
        } else if (type == QLatin1String("audio")) {
            audio.append(info);
        }
    }

    ui::TrackInfo off;
    off.id = QStringLiteral("no");
    off.title = tr("关闭字幕");
    off.selected = !subtitleSelected;
    subtitles.prepend(off);

    m_playerPage->setSubtitleTracks(subtitles);
    m_playerPage->setAudioTracks(audio);
}

void PlayerController::setFullscreen(bool fullscreen)
{
    if (!m_window)
        return;
    if (fullscreen)
        m_window->showFullScreen();
    else
        m_window->showNormal();
    m_playerPage->setFullscreen(fullscreen);
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
