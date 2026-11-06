#include "mainwindow.h"

#include "authutils.h"
#include "detailview.h"
#include "embyclient.h"
#include "embythumbnailcache.h"
#include "homeview.h"
#include "icons.h"
#include "libraryview.h"
#include "logindialog.h"
#include "mocklibrary.h"
#include "playerpage.h"
#include "settingspage.h"
#include "sidebar.h"
#include "theme.h"
#include "tokens.h"
#include "thumbnailcache.h"
#include "titlebar.h"
#include "tokenstore.h"
#include "windowframe.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {
namespace {

constexpr int kWindowTitleBarHeight = 36;

TrackInfo makeTrack(const QString &id, const QString &language, const QString &title,
                    const QString &codec, bool selected)
{
    TrackInfo track;
    track.id = id;
    track.language = language;
    track.title = title;
    track.codec = codec;
    track.selected = selected;
    return track;
}

bool isUnauthorized(const emby::ApiError &error)
{
    return error.httpStatus == 401 || error.httpStatus == 403;
}

} // namespace

MainWindow::MainWindow(const Options &options, QWidget *parent)
    : QMainWindow(parent)
    , m_options(options)
{
    Theme::apply(*qApp);
    m_client = new emby::EmbyClient(this);
    m_client->setRequestTimeout(12000);
    if (m_options.tokenStore) {
        m_tokenStore = m_options.tokenStore;
    } else {
        m_tokenStore = new QSettingsTokenStore();
        m_ownsTokenStore = true;
    }

    m_cache = new EmbyThumbnailCache(this);
    m_cache->setClient(m_client);
    m_data = new LibraryData(m_client, this);

    // Options.requireAuthentication == false：demo / 截图 / UI 自测，直接进入 mock。
    m_mockMode = !m_options.requireAuthentication;
    m_mockLibrary = makeMockLibrary(200);
    for (int i = 0; i < m_mockLibrary.size(); ++i)
        m_mockIndex.insert(m_mockLibrary.at(i).id, i);

    setWindowTitle(QStringLiteral("Emby PC — 自研客户端"));
    setMinimumSize(800, 480);
    resize(1280, 760);

    buildUi();
    connectSignals();
    restoreWindowState();
    applyMockMode();
    applySettings();
    m_settings->setTokenBackendName(m_tokenStore->backendName());
    updateSessionUi();

    statusBar()->setSizeGripEnabled(false);
    statusBar()->clearMessage();

    if (m_options.requireAuthentication)
        QTimer::singleShot(0, this, &MainWindow::restoreSession);
}

MainWindow::~MainWindow()
{
    if (m_ownsTokenStore)
        delete m_tokenStore;
}

void MainWindow::buildUi()
{
    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("AppRoot"));
    auto *rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, root);
    m_splitter->setObjectName(QStringLiteral("MainSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(4);
    m_sidebar = new Sidebar(m_splitter);
    m_splitter->addWidget(m_sidebar);

    auto *column = new QWidget(m_splitter);
    column->setObjectName(QStringLiteral("MainColumn"));
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(0);

    // 顶栏
    m_topBar = new QWidget(column);
    m_topBar->setObjectName(QStringLiteral("TopBar"));
    m_topBar->setFixedHeight(tokens::TopBarHeight);
    auto *topLayout = new QHBoxLayout(m_topBar);
    topLayout->setContentsMargins(tokens::TopBarHorizontalPadding, 0,
                                  tokens::TopBarHorizontalPadding, 0);
    topLayout->setSpacing(14);

    topLayout->addStretch(1);
    m_search = new QLineEdit(m_topBar);
    m_search->setObjectName(QStringLiteral("SearchBox"));
    m_search->setPlaceholderText(QStringLiteral("搜索电影、剧集、类型…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedSize(380, 34);
    m_search->addAction(icon(Icon::Search, QColor(0x94, 0xA3, 0xB8), 18),
                        QLineEdit::LeadingPosition);
    topLayout->addWidget(m_search);
    topLayout->addStretch(1);

    m_sessionStatus = new QLabel(m_topBar);
    m_sessionStatus->setObjectName(QStringLiteral("TopStatus"));
    m_sessionStatus->setVisible(false);
    topLayout->addWidget(m_sessionStatus);

    m_avatar = new QPushButton(mockUserInitial(), m_topBar);
    m_avatar->setObjectName(QStringLiteral("TopAvatar"));
    m_avatar->setFixedSize(34, 34);
    m_avatar->setCursor(Qt::PointingHandCursor);
    m_avatar->setToolTip(QStringLiteral("当前用户：%1").arg(mockUserName()));
    topLayout->addWidget(m_avatar);

    columnLayout->addWidget(m_topBar);

    // 页面栈
    m_stack = new QStackedWidget(column);
    m_stack->setObjectName(QStringLiteral("ContentStack"));

    m_home = new HomeView(m_cache, m_stack);
    m_home->setObjectName(QStringLiteral("HomePage"));
    m_home->recentView()->setObjectName(QStringLiteral("HomeLibrary"));

    m_libraryView = new LibraryView(m_cache, m_stack);
    m_libraryView->setObjectName(QStringLiteral("LibraryContent"));
    m_libraryView->setTitle(QStringLiteral("媒体库"));

    m_settings = new SettingsPage(m_stack);
    m_detail = new DetailView(m_cache, m_stack);
    m_player = new PlayerPage(m_stack);

    m_stack->addWidget(m_home);         // StackHome
    m_stack->addWidget(m_libraryView);  // StackLibrary
    m_stack->addWidget(m_settings);     // StackSettings
    m_stack->addWidget(m_detail);       // StackDetail
    m_stack->addWidget(m_player);       // StackPlayer

    columnLayout->addWidget(m_stack, 1);
    m_splitter->addWidget(column);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    rootLayout->addWidget(m_splitter, 1);
    setCentralWidget(root);
    m_sidebarSaveTimer = new QTimer(this);
    m_sidebarSaveTimer->setSingleShot(true);
    m_sidebarSaveTimer->setInterval(220);
    connect(m_sidebarSaveTimer, &QTimer::timeout, this, &MainWindow::saveSidebarWidth);
    connect(m_splitter, &QSplitter::splitterMoved, this,
            [this](int, int) { m_sidebarSaveTimer->start(); });
    setupTitleBar();
    restoreSidebarWidth();
}

void MainWindow::setupTitleBar()
{
    m_titleBar = new TitleBar(this, this);
    m_titleBar->setObjectName(QStringLiteral("TitleBar"));
    connect(m_titleBar, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(m_titleBar, &TitleBar::maximizeToggleRequested, this, [this]() {
        if (m_windowFrame)
            m_windowFrame->toggleMaximize();
        else if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    connect(m_titleBar, &TitleBar::closeRequested, this, &QWidget::close);
    connect(m_titleBar, &TitleBar::maximizeButtonRectChanged, this,
            [this](const QRect &rect) {
                if (m_windowFrame)
                    m_windowFrame->setMaximizeButtonRect(rect);
            });

    m_windowFrame = WindowFrame::attach(this);
    if (m_windowFrame) {
        m_windowFrame->setTitleBarHeight(kWindowTitleBarHeight);
        m_windowFrame->setTitleBarWidget(m_titleBar);
        m_windowFrame->setMaximizeButtonRect(m_titleBar->maximizeButtonRect());
        connect(m_windowFrame, &WindowFrame::maximizedChanged, this,
                [this](bool maximized) { m_titleBar->setMaximized(maximized); });
    }

    // T30/WIN-01：UI 与原生层统一 800x480。
    setMinimumSize(800, 480);
    setContentsMargins(0, kWindowTitleBarHeight, 0, 0);
    syncTitleBarGeometry();
    m_titleBar->setMaximized(isMaximized());
    m_titleBar->show();
    m_titleBar->raise();
}

void MainWindow::syncTitleBarGeometry()
{
    if (!m_titleBar)
        return;
    m_titleBar->setGeometry(0, 0, width(), kWindowTitleBarHeight);
    m_titleBar->raise();
    if (m_windowFrame) {
        m_windowFrame->setTitleBarHeight(titleBarVisible() ? kWindowTitleBarHeight : 0);
        m_windowFrame->setMaximizeButtonRect(m_titleBar->maximizeButtonRect());
    }
}

bool MainWindow::titleBarVisible() const
{
    // T17 的 PlayerController 可能直接调用 showFullScreen()/showNormal()，
    // 因此除内部 m_fullscreen 外还要看窗口真实状态。
    return !m_playerMode && !m_fullscreen && !isFullScreen();
}

void MainWindow::updateWindowChrome()
{
    const bool chromeVisible = titleBarVisible();
    if (m_titleBar)
        m_titleBar->setVisible(chromeVisible);
    if (QMenuBar *menu = findChild<QMenuBar *>(QString(), Qt::FindDirectChildrenOnly))
        menu->setVisible(chromeVisible);
    if (m_splitter)
        m_splitter->setHandleWidth(chromeVisible ? 4 : 0);
    setContentsMargins(0, chromeVisible ? kWindowTitleBarHeight : 0, 0, 0);
    if (m_windowFrame)
        m_windowFrame->setTitleBarHeight(chromeVisible ? kWindowTitleBarHeight : 0);
    if (chromeVisible) {
        syncTitleBarGeometry();
        m_titleBar->raise();
    }
}

void MainWindow::restoreWindowState()
{
    QSettings settings;
    const QRect saved = settings.value(QStringLiteral("window/normalGeometry")).toRect();
    if (saved.isValid()) {
        QRect bounded = saved;
        QScreen *screen = QGuiApplication::screenAt(saved.center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect available = screen->availableGeometry();
            bounded.setWidth(qMin(bounded.width(), available.width()));
            bounded.setHeight(qMin(bounded.height(), available.height()));
            if (bounded.left() < available.left())
                bounded.moveLeft(available.left());
            if (bounded.top() < available.top())
                bounded.moveTop(available.top());
            if (bounded.right() > available.right())
                bounded.moveRight(available.right());
            if (bounded.bottom() > available.bottom())
                bounded.moveBottom(available.bottom());
            setGeometry(bounded);
        }
    }
    if (settings.value(QStringLiteral("window/maximized"), false).toBool())
        setWindowState(Qt::WindowMaximized);
    if (m_titleBar)
        m_titleBar->setMaximized(isMaximized());
}

void MainWindow::saveWindowState()
{
    QSettings settings;
    const QRect normal = isMaximized() ? normalGeometry() : geometry();
    if (normal.isValid())
        settings.setValue(QStringLiteral("window/normalGeometry"), normal);
    settings.setValue(QStringLiteral("window/maximized"), isMaximized());
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_titleBar && titleBarVisible())
        syncTitleBarGeometry();
}

void MainWindow::restoreSidebarWidth()
{
    if (!m_splitter || m_splitter->count() < 2)
        return;
    QSettings settings;
    const int width = qBound(tokens::SidebarMinWidth,
                             settings.value(QStringLiteral("ui/sidebarWidth"),
                                            tokens::SidebarDefaultWidth).toInt(),
                             tokens::SidebarMaxWidth);
    const int total = qMax(width, m_splitter->width());
    m_splitter->setSizes({width, qMax(1, total - width)});
    QTimer::singleShot(0, this, [this, width]() {
        if (!m_splitter || m_splitter->count() < 2)
            return;
        const int totalNow = qMax(width, m_splitter->width());
        m_splitter->setSizes({width, qMax(1, totalNow - width)});
    });
}

void MainWindow::saveSidebarWidth()
{
    if (m_sidebarSaveTimer)
        m_sidebarSaveTimer->stop();
    if (!m_sidebar)
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("ui/sidebarWidth"), m_sidebar->width());
}

void MainWindow::applySettings()
{
    if (m_data)
        m_data->clearHomeSectionsCache();
    QSettings settings;
    if (m_player) {
        const int delay = qBound(500, settings.value(QStringLiteral("player/autoHideDelayMs"), 2500).toInt(),
                                 10000);
        m_player->setOverlayHideDelay(delay);
        if (settings.value(QStringLiteral("player/rememberVolume"), true).toBool()) {
            const int volume = qBound(0, settings.value(QStringLiteral("player/volume"), 80).toInt(),
                                      100);
            if (m_player->volume() != volume)
                m_player->setVolume(volume);
        }
    }
    if (m_cache) {
        const int limitMb = qBound(64, settings.value(QStringLiteral("cache/imageLimitMB"), 1024).toInt(),
                                   8192);
        m_cache->setDiskCacheMaxBytes(qint64(limitMb) * 1024 * 1024);
    }
}

void MainWindow::clearImageCache()
{
    if (m_cache)
        m_cache->clear();
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
             + QStringLiteral("/images"));
    const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::NoSort);
    for (const QFileInfo &file : files)
        QFile::remove(file.absoluteFilePath());
    if (m_settings)
        m_settings->refreshCacheUsage();
    statusBar()->showMessage(QStringLiteral("海报缓存已清理"), 2500);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateWindowChrome();
        if (m_titleBar)
            m_titleBar->setMaximized(isMaximized());
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveWindowState();
    saveSidebarWidth();
    QMainWindow::closeEvent(event);
}

void MainWindow::connectSignals()
{
    connect(m_sidebar, &Sidebar::sectionSelected, this, [this](int index) {
        const int libraryCount = m_sidebar->sections().size();
        if (index <= 0) {
            navigateTo(Page::Home);
        } else if (index <= libraryCount) {
            const int fullIndex = index - 1 < m_sidebarSectionIndexes.size()
                                      ? m_sidebarSectionIndexes.at(index - 1)
                                      : -1;
            if (fullIndex >= 0)
                selectSection(fullIndex);
        } else {
            navigateTo(Page::Settings);
        }
    });

    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(320);
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::applySearchFilter);
    connect(m_search, &QLineEdit::textChanged, this, [this]() { m_searchTimer->start(); });
    connect(m_search, &QLineEdit::returnPressed, this, &MainWindow::applySearchFilter);

    // 首页
    connect(m_home, &HomeView::itemActivated, this, [this](const QString &itemId) {
        openDetail(itemId);
    });
    connect(m_home, &HomeView::playRequested, this,
            [this](const QString &itemId) { playItem(itemId, -1, -1); });
    connect(m_home, &HomeView::sectionActivated, this,
            [this](int kind, const QString &libraryId) {
                if (kind == static_cast<int>(HomeSection::Kind::Recent)) {
                    showRecentPage();
                    return;
                }
                if (kind != static_cast<int>(HomeSection::Kind::Library))
                    return;
                for (int i = 0; i < m_sections.size(); ++i) {
                    if (m_sections.at(i).id == libraryId) {
                        selectSection(i);
                        return;
                    }
                }
            });
    connect(m_home, &HomeView::sortChanged, this, [this](int mode) {
        if (m_mockMode) {
            m_home->recentView()->setSortMode(mode);
        } else {
            // 真实首页最近添加固定按 DateCreated 排序，切到媒体库页应用排序。
            Q_UNUSED(mode);
        }
    });

    // 媒体库
    connect(m_libraryView, &LibraryView::itemActivated, this, [this](const QString &itemId) {
        openDetail(itemId);
    });
    connect(m_libraryView, &LibraryView::playRequested, this,
            [this](const QString &itemId) { playItem(itemId, -1, -1); });
    connect(m_libraryView, &LibraryView::loadMoreRequested, this, [this]() {
        if (m_mockMode || m_libraryLoading || !m_libraryHasMore)
            return;
        m_libraryLoading = true;
        m_libraryView->setLoadingMore(true);
        loadLibraryPage();
    });
    connect(m_libraryView, &LibraryView::sortChanged, this, [this](int) {
        if (!m_mockMode)
            reloadLibrary();
    });

    connect(m_detail, &DetailView::backRequested, this, [this]() {
        m_stack->setCurrentIndex(m_lastContentIndex);
    });
    connect(m_detail, &DetailView::playRequested, this,
            [this](const QString &itemId, int season, int episode) {
                playItem(itemId, season, episode);
            });
    connect(m_detail, &DetailView::episodeActivated, this,
            [this](const QString &itemId, int season, int episode) {
                playItem(itemId, season, episode);
            });

    connect(m_player, &PlayerPage::backRequested, this, [this]() {
        // 播放器模式下“返回”先退出接管，恢复进入前的页面与侧边栏/顶栏。
        if (m_playerMode) {
            if (m_fullscreen)
                toggleFullscreen();
            setPlayerMode(false);
            return;
        }
        if (!m_currentItem.id.isEmpty())
            openDetail(m_currentItem.id);
        else
            m_stack->setCurrentIndex(
                qBound(static_cast<int>(StackHome), m_lastContentIndex, static_cast<int>(StackSettings)));
    });
    connect(m_player, &PlayerPage::playPauseRequested, this, [this]() {
        m_player->setPlaying(!m_player->isPlaying());
    });
    connect(m_player, &PlayerPage::previousEpisodeRequested, this, [this]() {
        // mock 数据由 UI 自己的 episode 列表切换；真实播放由 PlayerController 的
        // Emby episode queue 处理，避免同一信号双触发。
        if (m_mockMode)
            stepEpisode(-1);
    });
    connect(m_player, &PlayerPage::nextEpisodeRequested, this, [this]() {
        if (m_mockMode)
            stepEpisode(1);
    });
    connect(m_player, &PlayerPage::seekRequested, this, [this](qint64 position) {
        m_player->setPosition(position, m_currentDurationMs);
    });
    connect(m_player, &PlayerPage::fullscreenRequested, this, &MainWindow::toggleFullscreen);
    connect(m_player, &PlayerPage::statsRequested, this, [this]() {
        statusBar()->showMessage(QStringLiteral("播放统计面板待接入（mpv stats.lua）"), 2500);
    });
    connect(m_player, &PlayerPage::volumeChanged, this, [this](int volume) {
        statusBar()->showMessage(QStringLiteral("音量 %1%").arg(volume), 1200);
        QSettings settings;
        if (settings.value(QStringLiteral("player/rememberVolume"), true).toBool())
            settings.setValue(QStringLiteral("player/volume"), volume);
    });
    connect(m_player, &PlayerPage::subtitleTrackSelected, this, [this](const QString &trackId) {
        for (TrackInfo &track : m_subtitleTracks)
            track.selected = (track.id == trackId);
        m_player->setSubtitleTracks(m_subtitleTracks);
        statusBar()->showMessage(QStringLiteral("已选择字幕轨道：%1").arg(trackId), 1800);
    });
    connect(m_player, &PlayerPage::audioTrackSelected, this, [this](const QString &trackId) {
        for (TrackInfo &track : m_audioTracks)
            track.selected = (track.id == trackId);
        m_player->setAudioTracks(m_audioTracks);
        statusBar()->showMessage(QStringLiteral("已选择音轨：%1").arg(trackId), 1800);
    });

    connect(m_settings, &SettingsPage::loginRequested, this, &MainWindow::requestLogin);
    connect(m_settings, &SettingsPage::logoutRequested, this, &MainWindow::logout);
    connect(m_settings, &SettingsPage::mockModeChanged, this, &MainWindow::setMockMode);
    connect(m_settings, &SettingsPage::settingsChanged, this, &MainWindow::applySettings);
    connect(m_settings, &SettingsPage::libraryDisplaySettingsChanged, this, [this]() {
        if (m_data)
            m_data->clearHomeSectionsCache();
        rebuildSidebar();
        if (m_mockMode)
            loadMockHome();
        else
            loadRealHome();
    });
    connect(m_settings, &SettingsPage::clearImageCacheRequested, this,
            &MainWindow::clearImageCache);
    connect(m_settings, &SettingsPage::exportDiagnosticsRequested, this, [this]() {
        // T17 的导出逻辑在 app 层；UI 层复用其 QAction（action 由 PlayerController 挂到帮助菜单）。
        if (auto *action = findChild<QAction *>(QStringLiteral("ExportDiagnosticsAction")))
            action->trigger();
        else
            statusBar()->showMessage(QStringLiteral("诊断导出功能尚未就绪"), 2500);
    });

    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, &MainWindow::handleEscape);
    auto *fullscreenKey = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fullscreenKey, &QShortcut::activated, this, &MainWindow::toggleFullscreen);
}

void MainWindow::navigateTo(Page page)
{
    if (m_playerMode)
        setPlayerMode(false);
    if (page == Page::Home) {
        m_sidebar->setCurrentIndex(0);
        m_stack->setCurrentIndex(StackHome);
        m_lastContentIndex = StackHome;
        if (m_mockMode) {
            if (m_home->recentView()->itemCount() == 0)
                loadMockHome();
        } else if (hasSession()) {
            loadRealHome();
        }
        return;
    }

    if (page == Page::Settings) {
        m_sidebar->setCurrentIndex(m_sidebar->sections().size() + 1);
        m_stack->setCurrentIndex(StackSettings);
        m_lastContentIndex = StackSettings;
        return;
    }

    // Page::Library / Movies / Series（后两者是同一页的兼容别名）。
    if (!m_sections.isEmpty()) {
        int wanted = 0;
        if (page == Page::Movies) {
            for (int i = 0; i < m_sections.size(); ++i)
                if (m_sections.at(i).collectionType == QLatin1String("movies")) {
                    wanted = i;
                    break;
                }
        } else if (page == Page::Series) {
            for (int i = 0; i < m_sections.size(); ++i)
                if (m_sections.at(i).collectionType == QLatin1String("tvshows")) {
                    wanted = i;
                    break;
                }
        }
        selectSection(wanted);
    }
}

void MainWindow::applyMockMode()
{
    if (m_mockMode) {
        QList<LibrarySection> sections;
        LibrarySection movies;
        movies.id = QStringLiteral("mock-movies");
        movies.name = QStringLiteral("电影");
        movies.collectionType = QStringLiteral("movies");
        sections.append(movies);
        LibrarySection series;
        series.id = QStringLiteral("mock-series");
        series.name = QStringLiteral("剧集");
        series.collectionType = QStringLiteral("tvshows");
        sections.append(series);
        m_sections = sections;
        m_currentSection = -1;
        loadMockHome();
        if (m_stack->currentIndex() == StackLibrary)
            selectSection(0);
    } else if (hasSession()) {
        loadRealViews();
        if (m_stack->currentIndex() == StackHome)
            loadRealHome();
        else if (m_stack->currentIndex() == StackLibrary && m_currentSection >= 0)
            reloadLibrary();
    } else {
        m_sections.clear();
        m_currentSection = -1;
        m_home->setLoading(false);
        m_home->setResumeItems({});
        m_home->setRecentItems({});
        m_home->recentView()->setLocalFilteringEnabled(true);
        m_home->recentView()->setTotalCount(0);
        m_home->setError(QStringLiteral("未登录：请先在设置页登录 Emby 服务器。"));
        m_libraryView->clearItems();
        m_libraryView->setError(QStringLiteral("未登录：请先登录 Emby 服务器。"));
    }
    rebuildSidebar();
    refreshServerCard();
    m_settings->setMockMode(m_mockMode);
    updateSessionUi();
    statusBar()->showMessage(m_mockMode ? QStringLiteral("已切换数据源")
                                        : QStringLiteral("已连接 Emby 媒体库"),
                             2200);
}

void MainWindow::rebuildSidebar()
{
    QSettings settings;
    const QStringList hidden =
        settings.value(QStringLiteral("sidebar/hiddenLibraries")).toStringList();
    m_sidebarSectionIndexes.clear();
    QList<SidebarSection> sidebarSections;
    sidebarSections.reserve(m_sections.size());
    for (int i = 0; i < m_sections.size(); ++i) {
        const LibrarySection &section = m_sections.at(i);
        if (hidden.contains(section.id))
            continue;
        SidebarSection entry;
        entry.id = section.id;
        entry.name = section.name;
        entry.collectionType = section.collectionType;
        sidebarSections.append(entry);
        m_sidebarSectionIndexes.append(i);
    }
    m_sidebar->setSections(sidebarSections);
    if (m_currentSection >= 0 && !m_sidebarSectionIndexes.contains(m_currentSection))
        m_currentSection = m_sidebarSectionIndexes.isEmpty() ? -1 : m_sidebarSectionIndexes.first();
    if (m_stack->currentIndex() == StackHome)
        m_sidebar->setCurrentIndex(0);
    else if (m_stack->currentIndex() == StackSettings)
        m_sidebar->setCurrentIndex(m_sidebar->sections().size() + 1);
    else if (m_currentSection >= 0) {
        const int sidebarIndex = m_sidebarSectionIndexes.indexOf(m_currentSection);
        m_sidebar->setCurrentIndex(sidebarIndex >= 0 ? sidebarIndex + 1 : 0);
    }
    if (m_settings)
        m_settings->setLibrarySections(m_sections);
}

void MainWindow::refreshServerCard()
{
    const bool connected = hasSession() && !m_mockMode;
    const QString serverName = connected ? (m_serverInfo.serverName.isEmpty()
                                                ? m_sessionProfile.baseUrl()
                                                : m_serverInfo.serverName)
                                         : QString();
    const QString userName = connected ? m_sessionProfile.userName : QString();
    m_sidebar->setServerInfo(serverName, userName, !connected);
}

void MainWindow::loadMockHome()
{
    m_home->recentView()->setLocalFilteringEnabled(true);
    m_home->recentView()->setLocalSortingEnabled(true);
    m_home->recentView()->setTitle(QStringLiteral("最近添加"));
    m_home->recentView()->setFilterText(QString());

    QList<MediaItem> resume;
    for (const MediaItem &item : m_mockLibrary) {
        if (item.progress > 0.0 && item.progress < 0.99 && resume.size() < 10)
            resume.append(item);
    }
    m_home->setResumeItems(resume);
    m_home->setRecentItems(m_mockLibrary);
    m_home->recentView()->setTotalCount(m_mockLibrary.size());
    m_home->recentView()->setHasMore(false);

    // T40：mock 也走多 section（继续观看横向，其余固定预览），保证演示/截图一致。
    QList<HomeSection> sections;
    HomeSection resumeSection;
    resumeSection.kind = HomeSection::Kind::Resume;
    resumeSection.title = QStringLiteral("继续观看");
    resumeSection.items = resume;
    sections.append(resumeSection);
    HomeSection recentSection;
    recentSection.kind = HomeSection::Kind::Recent;
    recentSection.title = QStringLiteral("最近添加");
    recentSection.items = m_mockLibrary.mid(0, 18);
    recentSection.totalCount = m_mockLibrary.size();
    sections.append(recentSection);
    for (const LibrarySection &library : m_sections) {
        HomeSection section;
        section.kind = HomeSection::Kind::Library;
        section.libraryId = library.id;
        section.title = library.name;
        for (const MediaItem &item : m_mockLibrary) {
            if (library.collectionType == QLatin1String("movies") && item.isSeries())
                continue;
            if (library.collectionType == QLatin1String("tvshows") && !item.isSeries())
                continue;
            section.items.append(item);
            if (section.items.size() >= 18)
                break;
        }
        section.totalCount = section.items.size();
        sections.append(section);
    }
    m_home->setHomeSections(sections);
    m_home->setLoading(false);
}

void MainWindow::loadMockSection(int sectionIndex)
{
    if (sectionIndex < 0 || sectionIndex >= m_sections.size())
        return;
    const LibrarySection &section = m_sections.at(sectionIndex);
    QList<MediaItem> items;
    for (const MediaItem &item : m_mockLibrary) {
        if (section.collectionType == QLatin1String("movies") && !item.isSeries())
            items.append(item);
        else if (section.collectionType == QLatin1String("tvshows") && item.isSeries())
            items.append(item);
        else if (section.collectionType != QLatin1String("movies")
                 && section.collectionType != QLatin1String("tvshows"))
            items.append(item);
    }
    m_libraryView->setLocalFilteringEnabled(true);
    m_libraryView->setLocalSortingEnabled(true);
    m_libraryView->setTitle(section.name);
    m_libraryView->setFilterText(m_search ? m_search->text() : QString());
    m_libraryView->setItems(items);
    m_libraryView->setHasMore(false);
    m_libraryView->setTotalCount(items.size());
}

void MainWindow::loadRealViews()
{
    if (!m_data || !hasSession())
        return;
    m_data->fetchViews([this](const QList<LibrarySection> &sections, const QString &error) {
        // 退出登录/切回 mock 后，迟到的响应不能再填充真实媒体库。
        if (!hasSession() || m_mockMode)
            return;
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("加载媒体库列表失败：%1").arg(error), 5000);
            emit dataLoaded(false, error);
            return;
        }
        m_sections = sections;
        rebuildSidebar();
        // 默认选中第一个媒体库，供首页“搜索/浏览”与后续切换使用。
        if (!m_sections.isEmpty()) {
            if (m_currentSection < 0)
                m_currentSection = 0;
            m_currentSection = qBound(0, m_currentSection, m_sections.size() - 1);
        } else {
            m_currentSection = -1;
        }
    });
}

void MainWindow::loadRealHome()
{
    if (!m_data || !hasSession()) {
        m_home->setError(QStringLiteral("未登录：请先在设置页登录 Emby 服务器。"));
        return;
    }
    const int generation = ++m_homeGeneration;
    m_home->setLoading(true, QStringLiteral("正在加载首页…"));
    m_home->recentView()->setLocalFilteringEnabled(false);
    m_home->recentView()->setLocalSortingEnabled(false);
    m_home->recentView()->setTitle(QStringLiteral("最近添加"));

    HomeQuery query;
    query.resumeLimit = 12;
    query.recentLimit = 24;
    query.perLibraryLimit = 18;
    QPointer<MainWindow> self(this);
    m_data->fetchHomeSections(
        query, [self, generation](const QList<HomeSection> &sections, const QString &error) {
            // 测试/退出时 MainWindow 可能先于网络回调销毁，QPointer 防止悬空。
            if (!self || generation != self->m_homeGeneration)
                return;
            qInfo() << "MainWindow::loadRealHome callback: generation" << generation
                    << "currentGeneration" << self->m_homeGeneration
                    << "sections" << sections.size()
                    << "error" << error
                    << "homeVisible" << self->m_home->isVisible()
                    << "stackIndex" << self->m_stack->currentIndex();
            if (!error.isEmpty() && sections.isEmpty()) {
                self->m_home->setError(error);
                emit self->dataLoaded(false, error);
                return;
            }
            self->m_home->setHomeSections(sections);
            self->m_home->setLoading(false);
            for (const HomeSection &section : sections)
                self->rememberItems(section.items);
            emit self->dataLoaded(true, QString());
            self->statusBar()->showMessage(QStringLiteral("已连接媒体库"), 3000);
        });
}

void MainWindow::selectSection(int sectionIndex)
{
    if (sectionIndex < 0 || sectionIndex >= m_sections.size())
        return;
    m_currentSection = sectionIndex;
    const int sidebarIndex = m_sidebarSectionIndexes.indexOf(sectionIndex);
    m_sidebar->setCurrentIndex(sidebarIndex >= 0 ? sidebarIndex + 1 : 0);
    m_stack->setCurrentIndex(StackLibrary);
    m_lastContentIndex = StackLibrary;

    if (m_mockMode) {
        loadMockSection(sectionIndex);
        return;
    }
    reloadLibrary();
}

void MainWindow::showRecentPage()
{
    if (!m_home)
        return;
    const QList<MediaItem> items = m_home->sectionItems(HomeSection::Kind::Recent);
    if (items.isEmpty())
        return;
    m_stack->setCurrentIndex(StackLibrary);
    m_lastContentIndex = StackLibrary;
    m_currentSection = -1;
    if (m_sidebar)
        m_sidebar->setCurrentIndex(0);
    m_libraryView->setLocalFilteringEnabled(true);
    m_libraryView->setLocalSortingEnabled(true);
    m_libraryView->setTitle(QStringLiteral("最近添加"));
    m_libraryView->setFilterText(m_search ? m_search->text() : QString());
    m_libraryView->setItems(items);
    m_libraryView->setHasMore(false);
    m_libraryView->setTotalCount(items.size());
    statusBar()->showMessage(QStringLiteral("最近添加 · %1 项").arg(items.size()), 2500);
}

void MainWindow::reloadLibrary()
{
    if (!m_data || !hasSession()) {
        m_libraryView->setError(QStringLiteral("未登录：请先登录 Emby 服务器。"));
        return;
    }
    if (m_currentSection < 0)
        return;

    ++m_libraryGeneration;
    m_libraryStart = 0;
    m_libraryHasMore = false;
    m_libraryLoading = true;
    m_libraryView->setLocalFilteringEnabled(false);
    m_libraryView->setLocalSortingEnabled(false);
    m_libraryView->clearItems();
    m_libraryView->setInitialLoading(true,
                                    QStringLiteral("正在加载 %1…").arg(m_sections.at(m_currentSection).name));
    loadLibraryPage();
}

void MainWindow::loadLibraryPage()
{
    if (!m_data || !hasSession() || m_currentSection < 0) {
        m_libraryLoading = false;
        return;
    }

    const int generation = m_libraryGeneration;
    const int start = m_libraryStart;
    const LibrarySection section = m_sections.at(m_currentSection);
    const int sortMode = m_libraryView->sortMode();
    const QString searchTerm = m_search ? m_search->text().trimmed() : QString();

    m_data->fetchItems(section, start, 60, sortMode, searchTerm,
                       [this, generation, start](const QList<MediaItem> &items, int total,
                                                 bool hasMore, const QString &error) {
                           if (generation != m_libraryGeneration)
                               return;
                           m_libraryLoading = false;
                           if (!error.isEmpty()) {
                               if (start == 0) {
                                   m_libraryView->setError(error);
                                   emit dataLoaded(false, error);
                               } else {
                                   m_libraryView->setLoadingMore(false);
                                   statusBar()->showMessage(
                                       QStringLiteral("加载更多失败：%1").arg(error), 4000);
                               }
                               return;
                           }

                           rememberItems(items);
                           if (start == 0) {
                               m_libraryView->setItems(items);
                               m_libraryView->setTotalCount(total);
                               m_libraryView->setHasMore(hasMore);
                           } else {
                               m_libraryView->appendItems(items, hasMore);
                               m_libraryView->setTotalCount(total);
                           }
                           m_libraryStart = start + items.size();
                           m_libraryHasMore = hasMore;
                           if (start == 0)
                               emit dataLoaded(true, QString());
                       });
}

void MainWindow::applySearchFilter()
{
    if (!m_search)
        return;
    const QString text = m_search->text();
    if (m_mockMode) {
        m_home->recentView()->setFilterText(text);
        m_libraryView->setFilterText(text);
        return;
    }

    // 真实数据走服务器 SearchTerm：切到媒体库页重新请求，避免把已加载的 60 条本地过滤。
    if (!hasSession())
        return;
    const bool onLibrary = m_stack->currentIndex() == StackLibrary && m_currentSection >= 0;
    const bool onHome = m_stack->currentIndex() == StackHome;
    if (!onLibrary && !onHome)
        return;
    if (onHome) {
        if (!text.trimmed().isEmpty() && !m_sections.isEmpty()) {
            selectSection(0);
        } else {
            loadRealHome();
        }
        return;
    }
    reloadLibrary();
}

void MainWindow::setMockMode(bool enabled)
{
    if (m_mockMode == enabled) {
        m_settings->setMockMode(enabled);
        return;
    }
    if (!enabled && !hasSession()) {
        // 没有真实会话：保持 mock，勾选框回退，并拉起登录框。
        m_settings->setMockMode(true);
        requestLogin();
        return;
    }

    m_mockMode = enabled;
    m_cache->clear();
    m_libraryGeneration++;
    m_homeGeneration++;
    m_detailGeneration++;
    m_libraryLoading = false;
    m_currentSection = -1;
    if (m_search)
        m_search->clear();
    applyMockMode();
    emit mockModeChanged(enabled);
}

bool MainWindow::openDetail(const QString &itemId)
{
    if (m_mockMode) {
        const MediaItem *item = findMockItem(itemId);
        if (!item)
            return false;
        showDetailFor(*item);
        return true;
    }

    if (!hasSession())
        return false;

    MediaItem known;
    const bool fromKnown = knownItem(itemId, &known);
    const bool fromList = !fromKnown && listedItem(itemId, &known);
    const bool hasKnown = fromKnown || fromList;
    if (hasKnown)
        showDetailFor(known);

    const int generation = ++m_detailGeneration;
    m_data->fetchDetail(itemId, [this, generation, itemId](const MediaItem &item,
                                                          const QString &error) {
        if (generation != m_detailGeneration)
            return;
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("加载详情失败：%1").arg(error), 5000);
            return;
        }
        rememberItems({item});
        showDetailFor(item);
    });
    return hasKnown || !itemId.isEmpty();
}

void MainWindow::showDetailFor(const MediaItem &item)
{
    if (m_playerMode)
        setPlayerMode(false);
    m_knownItems.insert(item.id, item);
    m_currentItem = item;
    m_detail->setItem(item);
    m_stack->setCurrentIndex(StackDetail);
}

void MainWindow::openPlayerDemo(const QString &itemId, int seasonNumber, int episodeNumber)
{
    playItem(itemId, seasonNumber, episodeNumber);
}

void MainWindow::playItem(const QString &itemId, int seasonNumber, int episodeNumber)
{
    MediaItem item;
    if (!knownItem(itemId, &item) && !listedItem(itemId, &item))
        return;

    // 剧集如果还没有季/集数据（列表页直接点播放），先补详情再进入播放页。
    if (!m_mockMode && item.isSeries() && item.seasons.isEmpty()) {
        if (!openDetail(itemId))
            return;
        statusBar()->showMessage(QStringLiteral("正在加载剧集信息，请稍候…"), 2000);
        return;
    }

    m_currentItem = item;
    m_currentSeason = -1;
    m_currentEpisode = -1;
    int durationMinutes = item.runtimeMinutes;
    QString subtitle;

    if (item.isSeries() && !item.seasons.isEmpty()) {
        int season = seasonNumber;
        if (season < 0)
            season = item.seasons.first().number;
        const Season *seasonPtr = nullptr;
        for (const Season &candidate : item.seasons) {
            if (candidate.number == season) {
                seasonPtr = &candidate;
                break;
            }
        }
        if (!seasonPtr)
            seasonPtr = &item.seasons.first();

        int episode = episodeNumber;
        if (episode < 0 && !seasonPtr->episodes.isEmpty())
            episode = seasonPtr->episodes.first().number;

        m_currentSeason = seasonPtr->number;
        m_currentEpisode = episode;
        QString episodeTitle;
        for (const Episode &candidate : seasonPtr->episodes) {
            if (candidate.number == episode) {
                episodeTitle = candidate.title;
                durationMinutes = candidate.runtimeMinutes;
                break;
            }
        }
        subtitle = QStringLiteral("S%1:E%2 · %3 · %4 分钟")
                       .arg(m_currentSeason, 2, 10, QLatin1Char('0'))
                       .arg(m_currentEpisode, 2, 10, QLatin1Char('0'))
                       .arg(episodeTitle)
                       .arg(durationMinutes);
    } else {
        subtitle = QStringLiteral("%1 · %2 · %3 分钟")
                       .arg(item.year)
                       .arg(item.genreText())
                       .arg(durationMinutes);
    }

    m_currentDurationMs = durationMinutes * 60 * 1000;
    m_player->setEpisodeInfo(item.title, subtitle);
    m_player->setPosition(0, m_currentDurationMs);
    m_player->setPlaying(true);
    m_player->setFullscreen(m_fullscreen);

    m_subtitleTracks = {
        makeTrack(QStringLiteral("sub-chi"), QStringLiteral("chi"), QStringLiteral("中文（简体）"),
                  QStringLiteral("ass"), true),
        makeTrack(QStringLiteral("sub-eng"), QStringLiteral("eng"), QStringLiteral("English"),
                  QStringLiteral("ass"), false),
        makeTrack(QStringLiteral("sub-off"), QString(), QStringLiteral("关闭字幕"), QString(), false),
    };
    m_audioTracks = {
        makeTrack(QStringLiteral("aud-chi"), QStringLiteral("chi"), QStringLiteral("中文 · 5.1"),
                  QStringLiteral("eac3"), true),
        makeTrack(QStringLiteral("aud-eng"), QStringLiteral("eng"), QStringLiteral("English · 5.1"),
                  QStringLiteral("eac3"), false),
    };
    m_player->setSubtitleTracks(m_subtitleTracks);
    m_player->setAudioTracks(m_audioTracks);
    updateEpisodeNavigation();

    setPlayerMode(true);
}

void MainWindow::updateEpisodeNavigation()
{
    if (!m_player)
        return;
    bool hasPrevious = false;
    bool hasNext = false;
    if (m_currentItem.isSeries()) {
        QList<Episode> flat;
        int currentIndex = -1;
        for (const Season &season : m_currentItem.seasons) {
            for (const Episode &episode : season.episodes) {
                if (season.number == m_currentSeason && episode.number == m_currentEpisode)
                    currentIndex = flat.size();
                flat.append(episode);
            }
        }
        hasPrevious = currentIndex > 0;
        hasNext = currentIndex >= 0 && currentIndex + 1 < flat.size();
    }
    m_player->setEpisodeNavigationEnabled(hasPrevious, hasNext);
}

void MainWindow::stepEpisode(int delta)
{
    qInfo() << "MainWindow::stepEpisode delta=" << delta
            << "item=" << m_currentItem.id
            << "season=" << m_currentSeason << "episode=" << m_currentEpisode
            << "mockMode=" << m_mockMode;
    if (!m_currentItem.isSeries() || m_currentSeason < 0 || m_currentEpisode < 0)
        return;

    QList<Episode> flat;
    for (const Season &season : m_currentItem.seasons)
        for (const Episode &episode : season.episodes)
            flat.append(episode);

    int currentIndex = -1;
    int index = 0;
    for (const Season &season : m_currentItem.seasons) {
        for (const Episode &episode : season.episodes) {
            if (season.number == m_currentSeason && episode.number == m_currentEpisode)
                currentIndex = index;
            ++index;
        }
    }
    if (currentIndex < 0)
        return;
    const int nextIndex = currentIndex + delta;
    if (nextIndex < 0 || nextIndex >= flat.size())
        return;

    int count = 0;
    for (const Season &season : m_currentItem.seasons) {
        for (const Episode &episode : season.episodes) {
            if (count == nextIndex) {
                playItem(m_currentItem.id, season.number, episode.number);
                updateEpisodeNavigation();
                return;
            }
            ++count;
        }
    }
}

void MainWindow::setPlayerMode(bool enabled)
{
    if (!m_player)
        return;

    if (enabled) {
        if (!m_playerMode) {
            m_playerReturnIndex = m_stack->currentIndex();
            m_playerMode = true;
        }
        m_sidebar->hide();
        m_topBar->hide();
        statusBar()->hide();
        updateWindowChrome();
        m_stack->setCurrentIndex(StackPlayer);
        m_player->setPlayerModeActive(true);
        emit playerModeChanged(true);
        return;
    }

    if (!m_playerMode)
        return;
    if (m_fullscreen) {
        // T17 可能在全屏播放中直接结束播放：先退出全屏，才能恢复窗口 chrome。
        showNormal();
        m_fullscreen = false;
        m_player->setFullscreen(false);
    }
    m_playerMode = false;
    m_player->setPlayerModeActive(false);
    m_sidebar->show();
    m_topBar->show();
    statusBar()->show();
    updateWindowChrome();
    const int returnIndex = qBound(static_cast<int>(StackHome), m_playerReturnIndex,
                                   static_cast<int>(StackPlayer));
    m_stack->setCurrentIndex(returnIndex);
    m_player->setFullscreen(m_fullscreen);
    emit playerModeChanged(false);
}

void MainWindow::toggleFullscreen()
{
    qInfo() << "MainWindow::toggleFullscreen enter isFullScreen=" << isFullScreen()
            << "m_fullscreen=" << m_fullscreen << "m_playerMode=" << m_playerMode
            << "geometry=" << geometry();
    if (m_fullscreen) {
        showNormal();
        m_fullscreen = false;
        m_player->setFullscreen(false);
        if (!m_playerMode) {
            m_player->setPlayerModeActive(false);
            m_sidebar->show();
            m_topBar->show();
            statusBar()->show();
            updateWindowChrome();
            m_stack->setCurrentIndex(m_fullscreenReturnIndex);
        }
        qInfo() << "MainWindow::toggleFullscreen leave-fullscreen isFullScreen=" << isFullScreen()
                << "m_fullscreen=" << m_fullscreen << "geometry=" << geometry();
        return;
    }

    m_fullscreenReturnIndex = m_stack->currentIndex();
    m_fullscreen = true;
    m_sidebar->hide();
    m_topBar->hide();
    statusBar()->hide();
    updateWindowChrome();
    m_stack->setCurrentIndex(StackPlayer);
    m_player->setPlayerModeActive(true);
    m_player->setFullscreen(true);
    showFullScreen();
    qInfo() << "MainWindow::toggleFullscreen enter-fullscreen isFullScreen=" << isFullScreen()
            << "m_fullscreen=" << m_fullscreen << "geometry=" << geometry();
}

void MainWindow::handleEscape()
{
    if (m_fullscreen) {
        toggleFullscreen();
        return;
    }
    if (m_playerMode) {
        setPlayerMode(false);
        return;
    }
    if (m_stack->currentIndex() == StackPlayer) {
        if (!m_currentItem.id.isEmpty())
            openDetail(m_currentItem.id);
        else
            m_stack->setCurrentIndex(
                qBound(static_cast<int>(StackHome), m_lastContentIndex, static_cast<int>(StackSettings)));
    } else if (m_stack->currentIndex() == StackDetail) {
        m_stack->setCurrentIndex(m_lastContentIndex);
    }
}

void MainWindow::requestLogin()
{
    showLoginDialog();
}

void MainWindow::restoreSession()
{
    const emby::ServerProfile stored = m_tokenStore->loadProfile();
    if (!stored.isAuthenticated()) {
        showLoginDialog();
        return;
    }

    m_sessionProfile = stored;
    m_client->setProfile(stored);
    statusBar()->showMessage(QStringLiteral("正在恢复上次的登录状态…"), 2000);

    QPointer<MainWindow> self(this);
    m_client->fetchPublicSystemInfo([self, stored](const emby::ServerInfo &info,
                                                   const emby::ApiError &error) {
        if (!self)
            return;
        if (error.isError()) {
            self->showLoginDialog(
                QStringLiteral("无法连接上次的服务器：%1\n请检查服务器是否在线，或修改服务器地址后重试。")
                    .arg(friendlyApiError(error)));
            return;
        }
        self->m_serverInfo = info;
        self->m_client->fetchViews(
            [self, stored](const emby::ItemQueryResult &, const emby::ApiError &viewError) {
                if (!self)
                    return;
                if (viewError.isError()) {
                    if (isUnauthorized(viewError)) {
                        self->m_tokenStore->clear();
                        self->m_sessionProfile = emby::ServerProfile();
                        self->m_sessionActive = false;
                        self->m_client->setProfile(emby::ServerProfile());
                        self->m_settings->clearSession();
                        self->updateSessionUi();
                        self->showLoginDialog(QStringLiteral("登录状态已失效，请重新登录。"));
                    } else {
                        self->showLoginDialog(
                            QStringLiteral("验证登录状态失败：%1").arg(friendlyApiError(viewError)));
                    }
                    return;
                }
                self->onLoggedIn(stored, self->m_serverInfo);
            });
    });
}

void MainWindow::showLoginDialog(const QString &message)
{
    if (!m_client)
        return;
    if (m_loginDialog) {
        m_loginDialog->show();
        m_loginDialog->raise();
        m_loginDialog->activateWindow();
        return;
    }

    const QString initialUrl = m_sessionProfile.url.isEmpty() ? m_lastServerUrl
                                                              : m_sessionProfile.url;
    const QString initialUser = m_sessionProfile.userName.isEmpty() ? m_lastUserName
                                                                   : m_sessionProfile.userName;
    auto *dialog = new LoginDialog(m_client, this);
    dialog->setInitialValues(initialUrl, initialUser, message);
    connect(dialog, &LoginDialog::loginSucceeded, this,
            [this, dialog](const emby::ServerProfile &profile) {
                onLoggedIn(profile, dialog->serverInfo());
            });
    connect(dialog, &QDialog::finished, this, [this, dialog](int) {
        if (m_loginDialog == dialog)
            m_loginDialog = nullptr;
        dialog->deleteLater();
    });
    m_loginDialog = dialog;
    dialog->open();
}

void MainWindow::onLoggedIn(const emby::ServerProfile &profile, const emby::ServerInfo &info)
{
    if (!profile.isAuthenticated())
        return;
    m_sessionProfile = profile;
    m_serverInfo = info;
    m_sessionActive = true;
    m_lastServerUrl = profile.baseUrl();
    m_lastUserName = profile.userName;
    m_client->setProfile(profile);
    if (m_data)
        m_data->clearHomeSectionsCache();
    m_tokenStore->saveProfile(profile);
    m_settings->setSession(profile, info);
    m_cache->clear();
    if (m_data)
        m_data->resetPosterStats();

    // 登录成功即使用真实媒体库；用户随后仍可在设置页手动切回 mock。
    m_mockMode = false;
    m_settings->setMockMode(false);
    m_currentSection = -1;
    ++m_homeGeneration;
    ++m_libraryGeneration;
    updateSessionUi();
    rebuildSidebar();
    refreshServerCard();
    loadRealViews();
    if (m_stack->currentIndex() == StackLibrary)
        selectSection(0);
    else
        loadRealHome();

    emit loginStateChanged(true);
    statusBar()->showMessage(QStringLiteral("已登录：%1 @ %2")
                                 .arg(profile.userName, profile.baseUrl()),
                             3000);
}

void MainWindow::logout()
{
    if (m_playerMode)
        setPlayerMode(false);
    m_tokenStore->clear();
    m_sessionProfile = emby::ServerProfile();
    m_serverInfo = {};
    m_sessionActive = false;
    m_client->setProfile(emby::ServerProfile());
    if (m_data)
        m_data->clearHomeSectionsCache();
    m_client->abortAll();
    m_cache->clear();
    if (m_data)
        m_data->resetPosterStats();
    m_knownItems.clear();
    m_sections.clear();
    m_currentSection = -1;
    ++m_homeGeneration;
    ++m_libraryGeneration;
    ++m_detailGeneration;
    m_libraryLoading = false;
    m_settings->clearSession();
    m_home->setResumeItems({});
    m_home->setRecentItems({});
    m_home->setError(QStringLiteral("未登录：请先登录 Emby 服务器。"));
    m_libraryView->clearItems();
    m_libraryView->setError(QStringLiteral("未登录：请先登录 Emby 服务器。"));
    rebuildSidebar();
    refreshServerCard();
    updateSessionUi();
    emit loginStateChanged(false);
    statusBar()->showMessage(QStringLiteral("已退出登录"), 2000);
    showLoginDialog(QStringLiteral("已退出登录，请重新连接服务器。"));
}

void MainWindow::updateSessionUi()
{
    if (!m_sessionStatus)
        return;
    if (hasSession() && !m_mockMode) {
        m_sessionStatus->setText(QStringLiteral("已登录 · %1").arg(
            m_sessionProfile.userName.isEmpty() ? m_sessionProfile.baseUrl()
                                                : m_sessionProfile.userName));
        m_sessionStatus->setToolTip(m_sessionProfile.baseUrl());
        if (m_avatar) {
            m_avatar->setText(m_sessionProfile.userName.left(1).toUpper());
            m_avatar->setToolTip(QStringLiteral("当前用户：%1").arg(m_sessionProfile.userName));
        }
    } else if (hasSession() && m_mockMode) {
        m_sessionStatus->setText(QString());
        m_sessionStatus->setToolTip(QString());
        if (m_avatar)
            m_avatar->setText(m_sessionProfile.userName.left(1).toUpper());
    } else {
        m_sessionStatus->setText(QString());
        m_sessionStatus->setToolTip(QString());
        if (m_avatar) {
            m_avatar->setText(mockUserInitial());
            m_avatar->setToolTip(QStringLiteral("当前用户：%1").arg(mockUserName()));
        }
    }
}

void MainWindow::setSearchText(const QString &text)
{
    m_search->setText(text);
    applySearchFilter();
}

QStringList MainWindow::libraryIds() const
{
    if (m_mockMode) {
        QStringList ids;
        ids.reserve(m_mockLibrary.size());
        for (const MediaItem &item : m_mockLibrary)
            ids.append(item.id);
        return ids;
    }
    QStringList ids;
    if (m_stack && m_stack->currentIndex() == StackLibrary)
        ids = m_libraryView->visibleItemIds();
    else if (m_home)
        ids = m_home->visibleHomeItemIds();
    return ids;
}

QStringList MainWindow::sectionNames() const
{
    QStringList names;
    for (const LibrarySection &section : m_sections)
        names.append(section.name);
    return names;
}

QString MainWindow::posterStatsSummary() const
{
    QStringList lines;
    if (m_cache)
        lines.append(m_cache->stats().summary());
    if (m_data)
        lines.append(m_data->posterStatsSummary());
    return lines.join(QLatin1Char('\n'));
}

QList<MediaItem> MainWindow::visibleItems() const
{
    if (m_mockMode) {
        const QList<MediaItem> all = m_mockLibrary;
        if (m_stack && m_stack->currentIndex() == StackLibrary) {
            QList<MediaItem> filtered;
            for (const MediaItem &item : all) {
                if (m_currentSection >= 0 && m_currentSection < m_sections.size()) {
                    const QString type = m_sections.at(m_currentSection).collectionType;
                    if (type == QLatin1String("movies") && item.isSeries())
                        continue;
                    if (type == QLatin1String("tvshows") && !item.isSeries())
                        continue;
                }
                filtered.append(item);
            }
            return filtered;
        }
        return all;
    }
    if (m_stack && m_stack->currentIndex() == StackLibrary)
        return m_libraryView->visibleItems();
    return m_home ? m_home->visibleHomeItems() : QList<MediaItem>();
}

int MainWindow::currentStackIndex() const
{
    return m_stack ? m_stack->currentIndex() : -1;
}

const MediaItem *MainWindow::findMockItem(const QString &itemId) const
{
    const auto it = m_mockIndex.constFind(itemId);
    if (it == m_mockIndex.constEnd())
        return nullptr;
    return &m_mockLibrary.at(it.value());
}

bool MainWindow::knownItem(const QString &itemId, MediaItem *out) const
{
    if (m_mockMode) {
        const MediaItem *item = findMockItem(itemId);
        if (!item)
            return false;
        if (out)
            *out = *item;
        return true;
    }
    const auto it = m_knownItems.constFind(itemId);
    if (it == m_knownItems.constEnd())
        return false;
    if (out)
        *out = it.value();
    return true;
}

// 真实模式下列表页里已经有服务器返回的完整 MediaItem（Fields 已请求），
// 直接用它，避免再发一次 /Items/{id} 造成详情页等待。
bool MainWindow::listedItem(const QString &itemId, MediaItem *out) const
{
    if (m_home && m_home->recentView()->itemById(itemId, out))
        return true;
    if (m_libraryView && m_libraryView->itemById(itemId, out))
        return true;
    return false;
}

void MainWindow::rememberItems(const QList<MediaItem> &items)
{
    for (const MediaItem &item : items)
        m_knownItems.insert(item.id, item);
}

} // namespace ui
