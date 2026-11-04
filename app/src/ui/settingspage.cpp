#include "settingspage.h"

#include "embytypes.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ui {
namespace {

QLabel *addRow(QVBoxLayout *parentLayout, const QString &title, const QString &value,
                   QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    auto *name = new QLabel(title, row);
    name->setObjectName(QStringLiteral("SettingName"));
    auto *val = new QLabel(value, row);
    val->setObjectName(QStringLiteral("SettingValue"));
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(name);
    layout->addStretch(1);
    layout->addWidget(val);
    parentLayout->addWidget(row);
    return val;
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

} // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SettingsPage"));
    buildUi();
    clearSession();
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
    auto *subtitle = new QLabel(QStringLiteral("账户、播放与界面设置"), content);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    layout->addWidget(title);
    layout->addWidget(subtitle);

    // 账户
    auto *account = makeGroup(QStringLiteral("账户"), content);
    auto *accountLayout = qobject_cast<QVBoxLayout *>(account->layout());
    m_accountHint = new QLabel(account);
    m_accountHint->setObjectName(QStringLiteral("SettingsAccountHint"));
    m_accountHint->setWordWrap(true);
    accountLayout->addWidget(m_accountHint);
    accountLayout->addSpacing(4);
    m_serverValue = addRow(accountLayout, QStringLiteral("服务器"), QStringLiteral("—"), account);
    m_serverValue->setObjectName(QStringLiteral("SettingsServerValue"));
    m_userValue = addRow(accountLayout, QStringLiteral("用户"), QStringLiteral("—"), account);
    m_userValue->setObjectName(QStringLiteral("SettingsUserValue"));
    m_versionValue = addRow(accountLayout, QStringLiteral("服务器版本"), QStringLiteral("—"), account);
    m_versionValue->setObjectName(QStringLiteral("SettingsVersionValue"));
    m_tokenValue = addRow(accountLayout, QStringLiteral("Token 存储"), QString(), account);
    m_tokenValue->setObjectName(QStringLiteral("SettingsTokenValue"));

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

    // 播放
    auto *playback = makeGroup(QStringLiteral("播放"), content);
    auto *playbackLayout = qobject_cast<QVBoxLayout *>(playback->layout());
    addRow(playbackLayout, QStringLiteral("视频渲染"),
           QStringLiteral("libmpv · --wid 子窗口（方案 A）"), playback);
    addRow(playbackLayout, QStringLiteral("硬件解码"), QStringLiteral("d3d11va（待接入）"), playback);
    addRow(playbackLayout, QStringLiteral("HDR / DV"), QStringLiteral("tone-map 到 SDR"), playback);
    addRow(playbackLayout, QStringLiteral("字幕"), QStringLiteral("ASS 特效字幕 · 自动加载外挂"),
           playback);
    layout->addWidget(playback);

    // 界面
    auto *ui = makeGroup(QStringLiteral("界面"), content);
    auto *uiLayout = qobject_cast<QVBoxLayout *>(ui->layout());
    addRow(uiLayout, QStringLiteral("主题"), QStringLiteral("深色（固定）"), ui);
    addRow(uiLayout, QStringLiteral("语言"), QStringLiteral("简体中文"), ui);
    addRow(uiLayout, QStringLiteral("海报尺寸"), QStringLiteral("自适应网格"), ui);

    // 数据源开关：演示数据（本地 mock）<-> 已登录 Emby 服务器的真实媒体库。
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
    layout->addWidget(ui);

    // 关于
    auto *about = makeGroup(QStringLiteral("关于"), content);
    auto *aboutLayout = qobject_cast<QVBoxLayout *>(about->layout());
    addRow(aboutLayout, QStringLiteral("版本"), QStringLiteral("0.2.0 · M2-B 真实数据"), about);
    m_dataSourceValue = addRow(aboutLayout, QStringLiteral("数据来源"), QStringLiteral("—"), about);
    m_dataSourceValue->setObjectName(QStringLiteral("SettingsDataSourceValue"));
    addRow(aboutLayout, QStringLiteral("构建"), QStringLiteral("Qt 6 Widgets · CMake"), about);
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
    setMockMode(true);
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

void SettingsPage::setSession(const emby::ServerProfile &profile, const emby::ServerInfo &serverInfo)
{
    const QString serverUrl = profile.baseUrl();
    m_serverValue->setText(serverUrl.isEmpty() ? QStringLiteral("—") : serverUrl);
    m_userValue->setText(profile.userName.isEmpty() ? QStringLiteral("—") : profile.userName);

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
    m_loginButton->setText(QStringLiteral("切换账号…"));
    m_logoutButton->setEnabled(true);
}

void SettingsPage::clearSession()
{
    m_serverValue->setText(QStringLiteral("—"));
    m_userValue->setText(QStringLiteral("—"));
    m_versionValue->setText(QStringLiteral("—"));
    m_accountHint->setText(QStringLiteral("未登录：点击“登录…”连接 Emby 服务器。"));
    m_loginButton->setText(QStringLiteral("登录…"));
    m_logoutButton->setEnabled(false);
}

void SettingsPage::setTokenBackendName(const QString &backendName)
{
    m_tokenValue->setText(backendName);
}

} // namespace ui