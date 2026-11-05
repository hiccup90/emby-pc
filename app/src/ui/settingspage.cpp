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
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("SettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget();
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(34, 24, 34, 40);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("设置"), content);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto *subtitle = new QLabel(QStringLiteral("账户、播放、存储与关于"), content);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    layout->addWidget(title);
    layout->addWidget(subtitle);

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
                QStringLiteral("libmpv · --wid 子窗口（方案 A）"),
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
    auto *mockHint = new QLabel(QStringLiteral("使用演示数据（本地 Mock）"), mockRow);
    mockHint->setObjectName(QStringLiteral("SettingHint"));
    m_mockCheck = new QCheckBox(mockRow);
    m_mockCheck->setObjectName(QStringLiteral("SettingsMockModeCheck"));
    m_mockCheck->setCursor(Qt::PointingHandCursor);
    m_mockCheck->setToolTip(
        QStringLiteral("勾选后界面显示本地演示数据；取消勾选使用已登录 Emby 服务器的真实媒体库。"));
    mockRowLayout->addWidget(mockName);
    mockRowLayout->addStretch(1);
    mockRowLayout->addWidget(mockHint);
    mockRowLayout->addSpacing(8);
    mockRowLayout->addWidget(m_mockCheck);
    uiLayout->addWidget(mockRow);
    m_dataSourceValue = addValueRow(uiLayout, QStringLiteral("数据来源"),
                                    QStringLiteral("—"),
                                    QStringLiteral("SettingsDataSourceValue"), ui);
    layout->addWidget(ui);

    // 4) 存储与日志
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
    root->addWidget(scroll);

    connect(m_loginButton, &QPushButton::clicked, this, &SettingsPage::loginRequested);
    connect(m_logoutButton, &QPushButton::clicked, this, &SettingsPage::logoutRequested);
    connect(m_mockCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_dataSourceValue) {
            m_dataSourceValue->setText(checked ? QStringLiteral("Mock（本地演示数据）")
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

    const QString product = serverInfo.productName.isEmpty()
                                ? (profile.serverType == emby::ServerType::Jellyfin
                                       ? QStringLiteral("Jellyfin")
                                       : QStringLiteral("Emby"))
                                : serverInfo.productName;
    m_accountHint->setText(QStringLiteral("已连接 %1（%2）。Token 仅保存在本机。")
                               .arg(profile.userName.isEmpty() ? QStringLiteral("当前用户")
                                                               : profile.userName,
                                    product));
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
    m_accountHint->setText(QStringLiteral("未登录：点击“登录…”连接 Emby 服务器。"));
    m_loginButton->setText(QStringLiteral("登录…"));
    m_logoutButton->setEnabled(false);
}

void SettingsPage::setTokenBackendName(const QString &backendName)
{
    m_tokenValue->setText(backendName);
}

void SettingsPage::setMockMode(bool enabled)
{
    if (m_mockCheck) {
        const QSignalBlocker blocker(m_mockCheck);
        m_mockCheck->setChecked(enabled);
    }
    if (m_dataSourceValue) {
        m_dataSourceValue->setText(enabled ? QStringLiteral("Mock（本地演示数据）")
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
