#include "settingspage.h"

#include "embytypes.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace ui {
namespace {

QLabel *addValueRow(QVBoxLayout *parentLayout, const QString &title, const QString &value,
                    const QString &objectName, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    auto *name = new QLabel(title, row);
    name->setObjectName(QStringLiteral("SettingName"));
    auto *val = new QLabel(value, row);
    val->setObjectName(objectName);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(name);
    layout->addStretch(1);
    layout->addWidget(val);
    parentLayout->addWidget(row);
    return val;
}

void addControlRow(QVBoxLayout *parentLayout, const QString &title, QWidget *control,
                   QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    auto *name = new QLabel(title, row);
    name->setObjectName(QStringLiteral("SettingName"));
    layout->addWidget(name);
    layout->addStretch(1);
    layout->addWidget(control);
    parentLayout->addWidget(row);
}

QGroupBox *makeGroup(const QString &title, QWidget *parent)
{
    auto *group = new QGroupBox(title, parent);
    group->setObjectName(QStringLiteral("SettingsGroup"));
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(18, 16, 18, 12);
    layout->setSpacing(2);
    return group;
}

QString formatBytes(qint64 bytes)
{
    if (bytes >= qint64(1024) * 1024)
        return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

QString imageCacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/images");
}

} // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SettingsPage"));
    buildUi();
    loadSettings();
    clearSession();
    refreshCacheUsage();
}

void SettingsPage::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // T45：qEmby 式左侧分类导航 + 右侧分组内容。
    auto *nav = new QWidget(this);
    nav->setObjectName(QStringLiteral("SettingsSidebar"));
    nav->setFixedWidth(232);
    auto *navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(18, 24, 18, 20);
    navLayout->setSpacing(6);
    auto *navTitle = new QLabel(QStringLiteral("设置"), nav);
    navTitle->setObjectName(QStringLiteral("SettingsNavTitle"));
    navLayout->addWidget(navTitle);
    navLayout->addSpacing(18);
    auto makeNavButton = [nav, navLayout](const QString &title) {
        auto *button = new QPushButton(title, nav);
        button->setObjectName(QStringLiteral("SettingsNavButton"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        navLayout->addWidget(button);
        return button;
    };
    auto *serverNav = makeNavButton(QStringLiteral("服务器"));
    auto *playbackNav = makeNavButton(QStringLiteral("播放"));
    auto *libraryNav = makeNavButton(QStringLiteral("媒体库"));
    auto *storageNav = makeNavButton(QStringLiteral("存储"));
    auto *aboutNav = makeNavButton(QStringLiteral("关于"));
    navLayout->addStretch(1);
    auto *navHint = new QLabel(QStringLiteral("设置保存在本机"), nav);
    navHint->setObjectName(QStringLiteral("SettingsNavHint"));
    navLayout->addWidget(navHint);
    root->addWidget(nav);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("SettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll, 1);

    auto *content = new QWidget();
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(32, 24, 32, 40);
    layout->setSpacing(20);

    auto *title = new QLabel(QStringLiteral("设置"), content);
    title->setObjectName(QStringLiteral("PageTitle"));
    layout->addWidget(title);

    // 1) 服务器与账号
    auto *account = makeGroup(QStringLiteral("服务器与账号"), content);
    auto *accountLayout = qobject_cast<QVBoxLayout *>(account->layout());
    m_accountHint = new QLabel(account);
    m_accountHint->setObjectName(QStringLiteral("SettingsAccountHint"));
    m_accountHint->setWordWrap(true);
    accountLayout->addWidget(m_accountHint);
    accountLayout->addSpacing(4);
    m_connectionValue = addValueRow(accountLayout, QStringLiteral("连接状态"),
                                    QStringLiteral("未连接"),
                                    QStringLiteral("SettingsConnectionValue"), account);
    m_serverValue = addValueRow(accountLayout, QStringLiteral("服务器"), QStringLiteral("—"),
                                QStringLiteral("SettingsServerValue"), account);
    m_userValue = addValueRow(accountLayout, QStringLiteral("用户"), QStringLiteral("—"),
                              QStringLiteral("SettingsUserValue"), account);
    m_versionValue = addValueRow(accountLayout, QStringLiteral("服务器版本"), QStringLiteral("—"),
                                 QStringLiteral("SettingsVersionValue"), account);
    m_tokenValue = addValueRow(accountLayout, QStringLiteral("Token 存储"), QString(),
                               QStringLiteral("SettingsTokenValue"), account);

    auto *accountButtons = new QHBoxLayout();
    accountButtons->setContentsMargins(0, 10, 0, 0);
    accountButtons->setSpacing(10);
    m_loginButton = new QPushButton(QStringLiteral("登录…"), account);
    m_loginButton->setObjectName(QStringLiteral("SettingsLoginButton"));
    m_loginButton->setProperty("role", QStringLiteral("primary"));
    m_loginButton->setCursor(Qt::PointingHandCursor);
    m_logoutButton = new QPushButton(QStringLiteral("退出登录"), account);
    m_logoutButton->setObjectName(QStringLiteral("SettingsLogoutButton"));
    m_logoutButton->setProperty("role", QStringLiteral("secondary"));
    m_logoutButton->setCursor(Qt::PointingHandCursor);
    accountButtons->addWidget(m_loginButton);
    accountButtons->addWidget(m_logoutButton);
    accountButtons->addStretch(1);
    accountLayout->addLayout(accountButtons);
    layout->addWidget(account);

    // 2) 播放
    auto *playback = makeGroup(QStringLiteral("播放"), content);
    auto *playbackLayout = qobject_cast<QVBoxLayout *>(playback->layout());

    m_subtitleCombo = new QComboBox(playback);
    m_subtitleCombo->setObjectName(QStringLiteral("SettingsSubtitleLanguageCombo"));
    m_subtitleCombo->addItem(QStringLiteral("简中优先"), QStringLiteral("chi,zho,zh"));
    m_subtitleCombo->addItem(QStringLiteral("繁中优先"), QStringLiteral("zho,chi,zh"));
    m_subtitleCombo->addItem(QStringLiteral("英文优先"), QStringLiteral("en"));
    m_subtitleCombo->addItem(QStringLiteral("不自动开启"), QStringLiteral("none"));
    addControlRow(playbackLayout, QStringLiteral("默认字幕语言"), m_subtitleCombo, playback);

    m_autoPlayNextCheck = new QCheckBox(QStringLiteral("自动连播"), playback);
    m_autoPlayNextCheck->setObjectName(QStringLiteral("SettingsAutoPlayNextCheck"));
    m_autoPlayNextCheck->setCursor(Qt::PointingHandCursor);
    addControlRow(playbackLayout, QStringLiteral("剧集播放"), m_autoPlayNextCheck, playback);

    m_autoResumeCheck = new QCheckBox(QStringLiteral("自动续播"), playback);
    m_autoResumeCheck->setObjectName(QStringLiteral("SettingsAutoResumeCheck"));
    m_autoResumeCheck->setCursor(Qt::PointingHandCursor);
    m_autoResumeCheck->setToolTip(QStringLiteral("从上次观看位置继续；已看完 95% 的条目自动从头播放"));
    addControlRow(playbackLayout, QStringLiteral("续播"), m_autoResumeCheck, playback);

    m_rememberVolumeCheck = new QCheckBox(QStringLiteral("记住音量"), playback);
    m_rememberVolumeCheck->setObjectName(QStringLiteral("SettingsRememberVolumeCheck"));
    m_rememberVolumeCheck->setCursor(Qt::PointingHandCursor);
    addControlRow(playbackLayout, QStringLiteral("音量"), m_rememberVolumeCheck, playback);

    m_autoHideCombo = new QComboBox(playback);
    m_autoHideCombo->setObjectName(QStringLiteral("SettingsAutoHideDelayCombo"));
    m_autoHideCombo->addItem(QStringLiteral("1.5 秒"), 1500);
    m_autoHideCombo->addItem(QStringLiteral("2.5 秒"), 2500);
    m_autoHideCombo->addItem(QStringLiteral("3 秒"), 3000);
    addControlRow(playbackLayout, QStringLiteral("控制条自动隐藏"), m_autoHideCombo, playback);

    m_hardwareDecoderValue = addValueRow(playbackLayout, QStringLiteral("硬件解码"),
                                         QStringLiteral("未初始化"),
                                         QStringLiteral("SettingsHardwareDecoderValue"), playback);
    addValueRow(playbackLayout, QStringLiteral("视频渲染"),
                QStringLiteral("libmpv · D3D11 纹理（render API）"),
                QStringLiteral("SettingsVideoRendererValue"), playback);
    addValueRow(playbackLayout, QStringLiteral("HDR / DV"),
                QStringLiteral("tone-map 到 SDR"), QStringLiteral("SettingsHdrValue"),
                playback);
    layout->addWidget(playback);

    // 3) 界面 / 数据源
    auto *ui = makeGroup(QStringLiteral("界面"), content);
    auto *uiLayout = qobject_cast<QVBoxLayout *>(ui->layout());
    addValueRow(uiLayout, QStringLiteral("主题"), QStringLiteral("深色（固定）"),
                QStringLiteral("SettingsThemeValue"), ui);
    addValueRow(uiLayout, QStringLiteral("语言"), QStringLiteral("简体中文"),
                QStringLiteral("SettingsLanguageValue"), ui);
    addValueRow(uiLayout, QStringLiteral("海报尺寸"), QStringLiteral("自适应网格"),
                QStringLiteral("SettingsPosterSizeValue"), ui);

    auto *mockRow = new QWidget(ui);
    auto *mockRowLayout = new QHBoxLayout(mockRow);
    mockRowLayout->setContentsMargins(0, 6, 0, 6);
    auto *mockName = new QLabel(QStringLiteral("数据源"), mockRow);
    mockName->setObjectName(QStringLiteral("SettingName"));
    m_mockCheck = new QCheckBox(mockRow);
    m_mockCheck->setObjectName(QStringLiteral("SettingsMockModeCheck"));
    m_mockCheck->setCursor(Qt::PointingHandCursor);
    m_mockCheck->setToolTip(
        QStringLiteral("勾选后界面显示本地演示数据；取消勾选使用已登录 Emby 服务器的真实媒体库。"));
    mockRowLayout->addWidget(mockName);
    mockRowLayout->addStretch(1);
    mockRowLayout->addWidget(m_mockCheck);
    uiLayout->addWidget(mockRow);
    m_dataSourceValue = addValueRow(uiLayout, QStringLiteral("数据来源"),
                                    QStringLiteral("—"),
                                    QStringLiteral("SettingsDataSourceValue"), ui);
    layout->addWidget(ui);

    // 4) 媒体库显示与首页顺序（T38）
    m_libraryGroup = makeGroup(QStringLiteral("媒体库"), content);
    m_libraryGroup->setObjectName(QStringLiteral("SettingsLibraryGroup"));
    auto *libraryLayout = qobject_cast<QVBoxLayout *>(m_libraryGroup->layout());
    m_libraryRowsLayout = new QVBoxLayout();
    m_libraryRowsLayout->setContentsMargins(0, 6, 0, 0);
    m_libraryRowsLayout->setSpacing(6);
    libraryLayout->addLayout(m_libraryRowsLayout);
    m_libraryEmpty = new QLabel(QStringLiteral("登录后显示服务器媒体库"), m_libraryGroup);
    m_libraryEmpty->setObjectName(QStringLiteral("EmptyState"));
    libraryLayout->addWidget(m_libraryEmpty);
    layout->addWidget(m_libraryGroup);
    m_libraryGroup->setVisible(false);

    // 5) 存储与日志
    auto *storage = makeGroup(QStringLiteral("存储与日志"), content);
    auto *storageLayout = qobject_cast<QVBoxLayout *>(storage->layout());
    m_cacheUsageValue = addValueRow(storageLayout, QStringLiteral("海报缓存占用"),
                                    QStringLiteral("0 B"),
                                    QStringLiteral("SettingsCacheUsageValue"), storage);

    auto *cacheButtons = new QHBoxLayout();
    cacheButtons->setContentsMargins(0, 8, 0, 0);
    cacheButtons->setSpacing(10);
    m_clearCacheButton = new QPushButton(QStringLiteral("清理海报缓存"), storage);
    m_clearCacheButton->setObjectName(QStringLiteral("SettingsClearCacheButton"));
    m_clearCacheButton->setProperty("role", QStringLiteral("secondary"));
    m_clearCacheButton->setCursor(Qt::PointingHandCursor);
    m_openLogButton = new QPushButton(QStringLiteral("打开日志目录"), storage);
    m_openLogButton->setObjectName(QStringLiteral("SettingsOpenLogDirButton"));
    m_openLogButton->setProperty("role", QStringLiteral("secondary"));
    m_openLogButton->setCursor(Qt::PointingHandCursor);
    auto *openCrashButton = new QPushButton(QStringLiteral("崩溃目录"), storage);
    openCrashButton->setObjectName(QStringLiteral("SettingsOpenCrashDirButton"));
    openCrashButton->setProperty("role", QStringLiteral("secondary"));
    openCrashButton->setCursor(Qt::PointingHandCursor);
    openCrashButton->setToolTip(QStringLiteral("打开崩溃转储目录（crash-*.dmp/.txt）"));
    m_exportDiagnosticsButton = new QPushButton(QStringLiteral("导出诊断信息…"), storage);
    m_exportDiagnosticsButton->setObjectName(QStringLiteral("SettingsExportDiagnosticsButton"));
    m_exportDiagnosticsButton->setProperty("role", QStringLiteral("secondary"));
    m_exportDiagnosticsButton->setCursor(Qt::PointingHandCursor);
    cacheButtons->addWidget(m_clearCacheButton);
    cacheButtons->addWidget(m_openLogButton);
    cacheButtons->addWidget(openCrashButton);
    cacheButtons->addWidget(m_exportDiagnosticsButton);
    cacheButtons->addStretch(1);
    storageLayout->addLayout(cacheButtons);

    m_cacheLimitSpin = new QSpinBox(storage);
    m_cacheLimitSpin->setObjectName(QStringLiteral("SettingsCacheLimitSpin"));
    m_cacheLimitSpin->setRange(64, 8192);
    m_cacheLimitSpin->setSingleStep(128);
    m_cacheLimitSpin->setSuffix(QStringLiteral(" MB"));
    m_cacheLimitSpin->setValue(1024);
    addControlRow(storageLayout, QStringLiteral("缓存上限"), m_cacheLimitSpin, storage);
    layout->addWidget(storage);

    // 5) 关于
    auto *about = makeGroup(QStringLiteral("关于"), content);
    auto *aboutLayout = qobject_cast<QVBoxLayout *>(about->layout());
    m_appVersionValue = addValueRow(aboutLayout, QStringLiteral("应用版本"),
                                   QCoreApplication::applicationVersion().isEmpty()
                                       ? QStringLiteral("0.2.0")
                                       : QCoreApplication::applicationVersion(),
                                   QStringLiteral("SettingsAppVersionValue"), about);
    m_qtVersionValue = addValueRow(aboutLayout, QStringLiteral("Qt 版本"),
                                   QString::fromLatin1(qVersion()),
                                   QStringLiteral("SettingsQtVersionValue"), about);
    m_buildTimeValue = addValueRow(aboutLayout, QStringLiteral("构建时间"),
                                   QStringLiteral("%1 %2")
                                       .arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)),
                                   QStringLiteral("SettingsBuildTimeValue"), about);
    m_libmpvVersionValue = addValueRow(aboutLayout, QStringLiteral("libmpv 版本"),
                                       QStringLiteral("未初始化"),
                                       QStringLiteral("SettingsLibmpvVersionValue"), about);
    m_copyDiagnosticsButton = new QPushButton(QStringLiteral("复制诊断信息"), about);
    m_copyDiagnosticsButton->setObjectName(QStringLiteral("SettingsCopyDiagnosticsButton"));
    m_copyDiagnosticsButton->setProperty("role", QStringLiteral("secondary"));
    m_copyDiagnosticsButton->setCursor(Qt::PointingHandCursor);
    aboutLayout->addWidget(m_copyDiagnosticsButton);
    layout->addWidget(about);
    layout->addStretch(1);

    scroll->setWidget(content);

    // 左侧分类只切换右侧分组可见性；控件实例/信号保持不变，QSettings 键不受影响。
    const QList<QPushButton *> navButtons = {serverNav, playbackNav, libraryNav, storageNav,
                                             aboutNav};
    const auto activateCategory = [=, this](QPushButton *active, const QList<QWidget *> &groups) {
        for (QPushButton *button : navButtons)
            button->setChecked(button == active);
        const QList<QWidget *> allGroups = {account, playback, ui, m_libraryGroup, storage, about};
        for (QWidget *group : allGroups)
            group->setVisible(groups.contains(group));
        m_activeCategory = (active == libraryNav) ? QStringLiteral("library")
                            : (active == playbackNav) ? QStringLiteral("playback")
                            : (active == storageNav) ? QStringLiteral("storage")
                            : (active == aboutNav) ? QStringLiteral("about")
                                                   : QStringLiteral("server");
        title->setText(active->text());
    };
    connect(serverNav, &QPushButton::clicked, this,
            [=, this]() { activateCategory(serverNav, {account, ui}); });
    connect(playbackNav, &QPushButton::clicked, this,
            [=, this]() { activateCategory(playbackNav, {playback}); });
    connect(libraryNav, &QPushButton::clicked, this,
            [=, this]() { activateCategory(libraryNav, {m_libraryGroup}); });
    connect(storageNav, &QPushButton::clicked, this,
            [=, this]() { activateCategory(storageNav, {storage}); });
    connect(aboutNav, &QPushButton::clicked, this,
            [=, this]() { activateCategory(aboutNav, {about}); });
    activateCategory(serverNav, {account, ui});

    connect(m_loginButton, &QPushButton::clicked, this, &SettingsPage::loginRequested);
    connect(m_logoutButton, &QPushButton::clicked, this, &SettingsPage::logoutRequested);
    connect(m_mockCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_dataSourceValue) {
            m_dataSourceValue->setText(checked ? QStringLiteral("本地演示数据")
                                               : QStringLiteral("Emby 服务器（真实媒体库）"));
        }
        emit mockModeChanged(checked);
    });
    connect(m_clearCacheButton, &QPushButton::clicked, this,
            &SettingsPage::clearImageCacheRequested);
    connect(m_openLogButton, &QPushButton::clicked, this, &SettingsPage::openLogDirectory);
    connect(openCrashButton, &QPushButton::clicked, this, []() {
        const QString base = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
        if (base.isEmpty())
            return;
        const QString directory = base + QStringLiteral("/crashes");
        QDir().mkpath(directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    connect(m_exportDiagnosticsButton, &QPushButton::clicked, this,
            &SettingsPage::exportDiagnosticsRequested);
    connect(m_copyDiagnosticsButton, &QPushButton::clicked, this,
            &SettingsPage::copyDiagnosticsToClipboard);

    const auto changed = [this]() {
        if (m_loading)
            return;
        saveControlValues();
        emit settingsChanged();
    };
    connect(m_subtitleCombo, &QComboBox::currentIndexChanged, this, changed);
    connect(m_autoPlayNextCheck, &QCheckBox::toggled, this, changed);
    connect(m_autoResumeCheck, &QCheckBox::toggled, this, changed);
    connect(m_rememberVolumeCheck, &QCheckBox::toggled, this, changed);
    connect(m_autoHideCombo, &QComboBox::currentIndexChanged, this, changed);
    connect(m_cacheLimitSpin, &QSpinBox::valueChanged, this, changed);

    setMockMode(true);
}

void SettingsPage::loadSettings()
{
    m_loading = true;
    QSettings settings;

    const QString subtitle = settings.value(QStringLiteral("player/subtitleLanguage"),
                                            QStringLiteral("chi,zho,zh")).toString();
    const int subtitleIndex = m_subtitleCombo->findData(subtitle);
    m_subtitleCombo->setCurrentIndex(subtitleIndex >= 0 ? subtitleIndex : 0);

    m_autoPlayNextCheck->setChecked(
        settings.value(QStringLiteral("player/autoPlayNext"), true).toBool());
    m_autoResumeCheck->setChecked(
        settings.value(QStringLiteral("player/autoResume"), true).toBool());
    m_rememberVolumeCheck->setChecked(
        settings.value(QStringLiteral("player/rememberVolume"), true).toBool());

    const int delay = settings.value(QStringLiteral("player/autoHideDelayMs"), 2500).toInt();
    const int delayIndex = m_autoHideCombo->findData(delay);
    m_autoHideCombo->setCurrentIndex(delayIndex >= 0 ? delayIndex : 1);

    const int limit = qBound(64, settings.value(QStringLiteral("cache/imageLimitMB"), 1024).toInt(),
                             8192);
    m_cacheLimitSpin->setValue(limit);

    // 把默认值落盘，T29 播放侧无需再复制一份默认逻辑。
    if (!settings.contains(QStringLiteral("player/subtitleLanguage")))
        settings.setValue(QStringLiteral("player/subtitleLanguage"), QStringLiteral("chi,zho,zh"));
    if (!settings.contains(QStringLiteral("player/autoPlayNext")))
        settings.setValue(QStringLiteral("player/autoPlayNext"), true);
    if (!settings.contains(QStringLiteral("player/autoResume")))
        settings.setValue(QStringLiteral("player/autoResume"), true);
    if (!settings.contains(QStringLiteral("player/rememberVolume")))
        settings.setValue(QStringLiteral("player/rememberVolume"), true);
    if (!settings.contains(QStringLiteral("player/volume")))
        settings.setValue(QStringLiteral("player/volume"), 80);
    if (!settings.contains(QStringLiteral("player/autoHideDelayMs")))
        settings.setValue(QStringLiteral("player/autoHideDelayMs"), 2500);
    if (!settings.contains(QStringLiteral("cache/imageLimitMB")))
        settings.setValue(QStringLiteral("cache/imageLimitMB"), 1024);
    m_loading = false;
}

void SettingsPage::saveControlValues()
{
    QSettings settings;
    settings.setValue(QStringLiteral("player/subtitleLanguage"), subtitleLanguage());
    settings.setValue(QStringLiteral("player/autoPlayNext"), autoPlayNext());
    settings.setValue(QStringLiteral("player/autoResume"), autoResume());
    settings.setValue(QStringLiteral("player/rememberVolume"), rememberVolume());
    settings.setValue(QStringLiteral("player/autoHideDelayMs"), autoHideDelayMs());
    settings.setValue(QStringLiteral("cache/imageLimitMB"), cacheImageLimitMB());
}

void SettingsPage::setSession(const emby::ServerProfile &profile, const emby::ServerInfo &serverInfo)
{
    const QString serverUrl = profile.baseUrl();
    m_serverValue->setText(serverUrl.isEmpty() ? QStringLiteral("—") : serverUrl);
    m_userValue->setText(profile.userName.isEmpty() ? QStringLiteral("—") : profile.userName);
    m_connectionValue->setText(QStringLiteral("已连接"));
    m_connectionValue->setProperty("state", QStringLiteral("connected"));
    m_connectionValue->style()->unpolish(m_connectionValue);
    m_connectionValue->style()->polish(m_connectionValue);

    QString version;
    if (!serverInfo.serverName.isEmpty())
        version = serverInfo.serverName;
    if (!serverInfo.version.isEmpty()) {
        if (!version.isEmpty())
            version += QStringLiteral(" · ");
        version += QStringLiteral("v%1").arg(serverInfo.version);
    }
    m_versionValue->setText(version.isEmpty() ? QStringLiteral("—") : version);

    m_accountHint->setText(QStringLiteral("已连接 %1")
                               .arg(profile.userName.isEmpty() ? QStringLiteral("当前用户")
                                                               : profile.userName));
    m_loginButton->setText(QStringLiteral("重新登录…"));
    m_logoutButton->setEnabled(true);
}

void SettingsPage::clearSession()
{
    m_serverValue->setText(QStringLiteral("—"));
    m_userValue->setText(QStringLiteral("—"));
    m_versionValue->setText(QStringLiteral("—"));
    m_connectionValue->setText(QStringLiteral("未连接"));
    m_connectionValue->setProperty("state", QStringLiteral("disconnected"));
    m_connectionValue->style()->unpolish(m_connectionValue);
    m_connectionValue->style()->polish(m_connectionValue);
    m_accountHint->setText(QStringLiteral("未登录"));
    m_loginButton->setText(QStringLiteral("登录…"));
    m_logoutButton->setEnabled(false);
}

void SettingsPage::setTokenBackendName(const QString &backendName)
{
    // T45：设置页只显示存储类型，完整实现细节保留在 tooltip。
    m_tokenValue->setText(backendName.contains(QStringLiteral("DPAPI"))
                              ? QStringLiteral("DPAPI")
                              : QStringLiteral("QSettings"));
    m_tokenValue->setToolTip(backendName);
}

void SettingsPage::setMockMode(bool enabled)
{
    if (m_mockCheck) {
        const QSignalBlocker blocker(m_mockCheck);
        m_mockCheck->setChecked(enabled);
    }
    if (m_dataSourceValue) {
        m_dataSourceValue->setText(enabled ? QStringLiteral("本地演示数据")
                                           : QStringLiteral("Emby 服务器（真实媒体库）"));
    }
}

bool SettingsPage::mockMode() const
{
    return m_mockCheck ? m_mockCheck->isChecked() : true;
}

void SettingsPage::setHardwareDecoderText(const QString &text)
{
    if (m_hardwareDecoderValue)
        m_hardwareDecoderValue->setText(text.isEmpty() ? QStringLiteral("未初始化") : text);
}

void SettingsPage::setLibmpvVersionText(const QString &text)
{
    if (m_libmpvVersionValue)
        m_libmpvVersionValue->setText(text.isEmpty() ? QStringLiteral("未初始化") : text);
}

QString SettingsPage::subtitleLanguage() const
{
    return m_subtitleCombo ? m_subtitleCombo->currentData().toString() : QString();
}

bool SettingsPage::autoPlayNext() const
{
    return m_autoPlayNextCheck && m_autoPlayNextCheck->isChecked();
}

bool SettingsPage::autoResume() const
{
    return m_autoResumeCheck && m_autoResumeCheck->isChecked();
}

bool SettingsPage::rememberVolume() const
{
    return m_rememberVolumeCheck && m_rememberVolumeCheck->isChecked();
}

int SettingsPage::autoHideDelayMs() const
{
    return m_autoHideCombo ? m_autoHideCombo->currentData().toInt() : 2500;
}

int SettingsPage::cacheImageLimitMB() const
{
    return m_cacheLimitSpin ? m_cacheLimitSpin->value() : 1024;
}

void SettingsPage::setLibrarySections(const QList<LibrarySection> &sections)
{
    m_librarySections = sections;
    m_libraryOrder.clear();
    QSettings settings;
    const QStringList configured = settings.value(QStringLiteral("home/sectionOrder")).toStringList();
    for (const QString &token : configured) {
        if (!token.startsWith(QStringLiteral("lib:")))
            continue;
        const QString id = token.mid(4);
        bool exists = false;
        for (const LibrarySection &section : m_librarySections) {
            if (section.id == id) {
                exists = true;
                break;
            }
        }
        if (exists && !m_libraryOrder.contains(id))
            m_libraryOrder.append(id);
    }
    for (const LibrarySection &section : m_librarySections) {
        if (!m_libraryOrder.contains(section.id))
            m_libraryOrder.append(section.id);
    }
    rebuildLibraryRows();
}

void SettingsPage::rebuildLibraryRows()
{
    if (!m_libraryRowsLayout)
        return;
    while (QLayoutItem *item = m_libraryRowsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_libraryRows.clear();
    if (!m_libraryGroup || !m_libraryEmpty)
        return;
    m_libraryGroup->setVisible(!m_librarySections.isEmpty()
                               && m_activeCategory == QLatin1String("library"));
    m_libraryEmpty->setVisible(m_librarySections.isEmpty());
    if (m_librarySections.isEmpty())
        return;

    QSettings settings;
    const QStringList hiddenSidebar =
        settings.value(QStringLiteral("sidebar/hiddenLibraries")).toStringList();
    const QStringList hiddenHome =
        settings.value(QStringLiteral("home/hiddenLibraries")).toStringList();

    for (const QString &id : m_libraryOrder) {
        const LibrarySection *section = nullptr;
        for (const LibrarySection &candidate : m_librarySections) {
            if (candidate.id == id) {
                section = &candidate;
                break;
            }
        }
        if (!section)
            continue;

        LibraryRow row;
        row.id = section->id;
        row.name = section->name;
        row.row = new QWidget(m_libraryGroup);
        row.row->setObjectName(QStringLiteral("SettingsLibraryRow"));
        row.row->setProperty("libraryId", row.id);
        // T64：800×480 下内容区只有约 260px；媒体库行分两行，避免固定控件横向溢出。
        auto *layout = new QVBoxLayout(row.row);
        layout->setContentsMargins(0, 2, 0, 2);
        layout->setSpacing(4);
        auto *titleLine = new QHBoxLayout();
        titleLine->setSpacing(8);
        auto *name = new QLabel(row.name, row.row);
        name->setObjectName(QStringLiteral("SettingName"));
        name->setMinimumWidth(80);
        titleLine->addWidget(name);
        titleLine->addStretch(1);

        auto *up = new QPushButton(QStringLiteral("↑"), row.row);
        up->setObjectName(QStringLiteral("SettingsLibraryUpButton"));
        up->setProperty("role", QStringLiteral("secondary"));
        up->setFixedSize(28, 26);
        up->setToolTip(QStringLiteral("上移首页顺序"));
        auto *down = new QPushButton(QStringLiteral("↓"), row.row);
        down->setObjectName(QStringLiteral("SettingsLibraryDownButton"));
        down->setProperty("role", QStringLiteral("secondary"));
        down->setFixedSize(28, 26);
        down->setToolTip(QStringLiteral("下移首页顺序"));
        titleLine->addWidget(up);
        titleLine->addWidget(down);
        layout->addLayout(titleLine);

        auto *checksLine = new QHBoxLayout();
        checksLine->setSpacing(8);
        row.sidebarVisible = new QCheckBox(QStringLiteral("侧边栏显示"), row.row);
        row.sidebarVisible->setObjectName(QStringLiteral("SettingsSidebarVisibleCheck"));
        row.sidebarVisible->setChecked(!hiddenSidebar.contains(row.id));
        row.homeVisible = new QCheckBox(QStringLiteral("首页显示"), row.row);
        row.homeVisible->setObjectName(QStringLiteral("SettingsHomeVisibleCheck"));
        row.homeVisible->setChecked(!hiddenHome.contains(row.id));
        row.homeVisible->setEnabled(row.sidebarVisible->isChecked());
        row.homeVisible->setToolTip(row.sidebarVisible->isChecked()
                                        ? QStringLiteral("在首页显示该媒体库最新条目")
                                        : QStringLiteral("需先勾选侧边栏显示"));
        checksLine->addWidget(row.sidebarVisible);
        checksLine->addWidget(row.homeVisible);
        checksLine->addStretch(1);
        layout->addLayout(checksLine);

        const QString rowId = row.id;
        connect(row.sidebarVisible, &QCheckBox::toggled, this, [this, rowId](bool checked) {
            for (const LibraryRow &candidate : m_libraryRows) {
                if (candidate.id != rowId || !candidate.homeVisible)
                    continue;
                candidate.homeVisible->setEnabled(checked);
                candidate.homeVisible->setToolTip(
                    checked ? QStringLiteral("在首页显示该媒体库最新条目")
                            : QStringLiteral("需先勾选侧边栏显示"));
            }
            saveLibrarySettings();
        });
        connect(row.homeVisible, &QCheckBox::toggled, this, [this]() { saveLibrarySettings(); });
        connect(up, &QPushButton::clicked, this, [this, rowId]() { moveLibrary(rowId, -1); });
        connect(down, &QPushButton::clicked, this, [this, rowId]() { moveLibrary(rowId, 1); });

        m_libraryRowsLayout->addWidget(row.row);
        m_libraryRows.append(row);
    }
}

void SettingsPage::moveLibrary(const QString &id, int delta)
{
    const int index = m_libraryOrder.indexOf(id);
    const int target = index + delta;
    if (index < 0 || target < 0 || target >= m_libraryOrder.size())
        return;
    m_libraryOrder.move(index, target);
    rebuildLibraryRows();
    saveLibrarySettings();
}

void SettingsPage::saveLibrarySettings()
{
    QSettings settings;
    QStringList sidebarHidden;
    QStringList homeHidden;
    for (const LibraryRow &row : m_libraryRows) {
        if (row.sidebarVisible && !row.sidebarVisible->isChecked())
            sidebarHidden.append(row.id);
        if (row.homeVisible && !row.homeVisible->isChecked())
            homeHidden.append(row.id);
    }
    settings.setValue(QStringLiteral("sidebar/hiddenLibraries"), sidebarHidden);
    settings.setValue(QStringLiteral("home/hiddenLibraries"), homeHidden);

    // 保留 resume/recent 的位置，只重排媒体库行。
    const QStringList oldOrder =
        settings.value(QStringLiteral("home/sectionOrder")).toStringList();
    QStringList order;
    for (const QString &token : oldOrder) {
        if ((token == QLatin1String("resume") || token == QLatin1String("recent"))
            && !order.contains(token)) {
            order.append(token);
        }
    }
    if (!order.contains(QStringLiteral("resume")))
        order.prepend(QStringLiteral("resume"));
    if (!order.contains(QStringLiteral("recent")))
        order.insert(qMin(1, order.size()), QStringLiteral("recent"));
    for (const QString &id : m_libraryOrder)
        order.append(QStringLiteral("lib:") + id);
    settings.setValue(QStringLiteral("home/sectionOrder"), order);
    emit libraryDisplaySettingsChanged();
}

void SettingsPage::refreshCacheUsage()
{
    QDir dir(imageCacheDirectory());
    qint64 bytes = 0;
    int files = 0;
    if (dir.exists()) {
        const QFileInfoList entries = dir.entryInfoList(QDir::Files, QDir::NoSort);
        for (const QFileInfo &entry : entries) {
            bytes += entry.size();
            ++files;
        }
    }
    if (m_cacheUsageValue) {
        m_cacheUsageValue->setText(QStringLiteral("%1（%2 个文件）")
                                       .arg(formatBytes(bytes))
                                       .arg(files));
    }
}

void SettingsPage::openLogDirectory()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}

void SettingsPage::copyDiagnosticsToClipboard()
{
    QStringList lines;
    lines << QStringLiteral("Emby PC diagnostics (UI)");
    lines << QStringLiteral("app: %1").arg(m_appVersionValue->text());
    lines << QStringLiteral("qt: %1").arg(m_qtVersionValue->text());
    lines << QStringLiteral("build: %1").arg(m_buildTimeValue->text());
    lines << QStringLiteral("libmpv: %1").arg(m_libmpvVersionValue->text());
    lines << QStringLiteral("hardware decoder: %1").arg(m_hardwareDecoderValue->text());
    lines << QStringLiteral("cache usage: %1").arg(m_cacheUsageValue->text());
    lines << QStringLiteral("subtitle language: %1").arg(subtitleLanguage());
    lines << QStringLiteral("auto play next: %1").arg(autoPlayNext() ? 1 : 0);
    lines << QStringLiteral("auto resume: %1").arg(autoResume() ? 1 : 0);
    lines << QStringLiteral("remember volume: %1").arg(rememberVolume() ? 1 : 0);
    lines << QStringLiteral("auto hide delay ms: %1").arg(autoHideDelayMs());
    lines << QStringLiteral("image cache limit MB: %1").arg(cacheImageLimitMB());
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(lines.join(QLatin1Char('\n')));
    emit copyDiagnosticsRequested();
}

void SettingsPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshCacheUsage();
}

} // namespace ui
