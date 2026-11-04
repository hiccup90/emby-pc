#pragma once

#include "embytypes.h"
#include "librarydata.h"
#include "uitypes.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QWidget;

namespace emby {
class EmbyClient;
}

namespace ui {

class DetailView;
class EmbyThumbnailCache;
class HomeView;
class LibraryData;
class LibraryView;
class LoginDialog;
class PlayerPage;
class SettingsPage;
class Sidebar;
class TokenStore;

// MainWindow 的启动选项（放在类外，避免嵌套类的默认成员初始化在默认实参里触发 GCC 限制）。
struct MainWindowOptions
{
    // true：启动时检查 TokenStore；无有效会话则弹出登录框。
    // demo/截图/单元测试可传 false 直接进入 mock 界面。
    bool requireAuthentication = true;
    // 外部 TokenStore（不转移所有权）；为空时内部创建 QSettingsTokenStore。
    TokenStore *tokenStore = nullptr;
};

// 应用主窗口壳：侧边栏（首页 + 真实媒体库列表 + 设置）+ 顶栏搜索 + QStackedWidget。
// 页面顺序：首页 / 媒体库 / 设置 / 详情 / 播放。
// M2-A 起支持登录会话；M2-B 起用 app/src/emby 的真实数据替换 mock（设置页可切换）。
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    using Options = MainWindowOptions;

    enum class Page {
        Home = 0,
        Library = 1,
        // 兼容旧调用：电影/剧集都会打开同一个可复用媒体库页，但先按类型定位 section。
        Movies = 2,
        Series = 3,
        Settings = 4,
    };

    explicit MainWindow(const Options &options = Options(), QWidget *parent = nullptr);
    ~MainWindow() override;

    void navigateTo(Page page);
    bool openDetail(const QString &itemId);
    void openPlayerDemo(const QString &itemId, int seasonNumber = -1, int episodeNumber = -1);
    void setSearchText(const QString &text);

    // 打开登录框（设置页“登录…”也走这里）。
    void requestLogin();

    bool hasSession() const { return m_sessionActive && m_sessionProfile.isAuthenticated(); }
    const emby::ServerProfile &sessionProfile() const { return m_sessionProfile; }
    const emby::ServerInfo &serverInfo() const { return m_serverInfo; }

    // T8 播放链路使用：登录成功后拿到底层 EmbyClient（token/设备信息已就绪）。
    emby::EmbyClient *embyClient() const { return m_client; }

    // 当前是否使用本地演示数据（mock）。设置页可切换。
    bool isMockMode() const { return m_mockMode; }
    void setMockMode(bool enabled);

    QStringList libraryIds() const;
    QStringList sectionNames() const;
    // 当前可见的完整 item（真实数据包含简介/季集等字段），供播放链路和自测使用。
    QList<MediaItem> visibleItems() const;
    int currentStackIndex() const;
    int currentSectionIndex() const { return m_currentSection; }

signals:
    // true = 登录成功/启动恢复成功；false = 退出登录或会话失效。
    void loginStateChanged(bool loggedIn);
    // 真实数据加载状态：首屏媒体库/首页加载完成（成功或失败）。
    void dataLoaded(bool ok, const QString &error);
    void mockModeChanged(bool enabled);

private:
    enum StackPage {
        StackHome = 0,
        StackLibrary,
        StackSettings,
        StackDetail,
        StackPlayer,
    };

    void buildUi();
    void connectSignals();

    // 数据源
    void applyMockMode();
    void rebuildSidebar();
    void refreshServerCard();
    void loadMockHome();
    void loadMockSection(int sectionIndex);
    void loadRealViews();
    void loadRealHome();
    void selectSection(int sectionIndex);
    void reloadLibrary();
    void loadLibraryPage();
    void applySearchFilter();

    // 详情 / 播放
    void showDetailFor(const MediaItem &item);
    void playItem(const QString &itemId, int seasonNumber, int episodeNumber);
    void stepEpisode(int delta);
    void toggleFullscreen();
    void handleEscape();
    const MediaItem *findMockItem(const QString &itemId) const;
    bool knownItem(const QString &itemId, MediaItem *out) const;
    bool listedItem(const QString &itemId, MediaItem *out) const;
    void rememberItems(const QList<MediaItem> &items);

    // 会话
    void restoreSession();
    void showLoginDialog(const QString &message = QString());
    void onLoggedIn(const emby::ServerProfile &profile, const emby::ServerInfo &info);
    void logout();
    void updateSessionUi();

    Options m_options;
    EmbyThumbnailCache *m_cache = nullptr;
    emby::EmbyClient *m_client = nullptr;
    LibraryData *m_data = nullptr;
    TokenStore *m_tokenStore = nullptr;
    bool m_ownsTokenStore = false;
    QPointer<LoginDialog> m_loginDialog;
    emby::ServerProfile m_sessionProfile;
    emby::ServerInfo m_serverInfo;
    bool m_sessionActive = false;
    QString m_lastServerUrl;
    QString m_lastUserName;

    Sidebar *m_sidebar = nullptr;
    QWidget *m_topBar = nullptr;
    QLabel *m_sessionStatus = nullptr;
    QPushButton *m_avatar = nullptr;
    QLineEdit *m_search = nullptr;
    QTimer *m_searchTimer = nullptr;
    QStackedWidget *m_stack = nullptr;
    HomeView *m_home = nullptr;
    LibraryView *m_libraryView = nullptr;
    SettingsPage *m_settings = nullptr;
    DetailView *m_detail = nullptr;
    PlayerPage *m_player = nullptr;

    bool m_mockMode = true;
    QList<MediaItem> m_mockLibrary;
    QHash<QString, int> m_mockIndex;
    QHash<QString, MediaItem> m_knownItems;

    QList<LibrarySection> m_sections;
    int m_currentSection = -1;
    int m_libraryStart = 0;
    int m_libraryGeneration = 0;
    int m_homeGeneration = 0;
    int m_detailGeneration = 0;
    bool m_libraryLoading = false;
    bool m_libraryHasMore = false;

    MediaItem m_currentItem;
    int m_currentSeason = -1;
    int m_currentEpisode = -1;
    int m_currentDurationMs = 0;
    int m_lastContentIndex = StackHome;
    int m_fullscreenReturnIndex = StackHome;
    bool m_fullscreen = false;

    QList<TrackInfo> m_subtitleTracks;
    QList<TrackInfo> m_audioTracks;
};

} // namespace ui
