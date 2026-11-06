#include "logindialog.h"

#include "authutils.h"
#include "embyclient.h"
#include "icons.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace ui {

LoginDialog::LoginDialog(emby::EmbyClient *client, QWidget *parent)
    : QDialog(parent)
    , m_client(client)
{
    setObjectName(QStringLiteral("LoginDialog"));
    setWindowTitle(QStringLiteral("登录 — Emby PC"));
    setModal(true);
    setMinimumWidth(620);
    buildUi();
}

void LoginDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(36, 32, 36, 28);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("连接到 Emby 服务器"), this);
    title->setObjectName(QStringLiteral("LoginTitle"));
    root->addWidget(title);

    auto *subtitle = new QLabel(QStringLiteral("输入服务器地址和账号"), this);
    subtitle->setObjectName(QStringLiteral("LoginSubtitle"));
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);
    root->addSpacing(6);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_serverEdit = new QLineEdit(this);
    m_serverEdit->setObjectName(QStringLiteral("ServerUrlEdit"));
    m_serverEdit->setProperty("role", QStringLiteral("formInput"));
    m_serverEdit->setPlaceholderText(QStringLiteral("http://192.168.1.10:8096"));
    m_serverEdit->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("服务器地址"), m_serverEdit);

    m_userEdit = new QLineEdit(this);
    m_userEdit->setObjectName(QStringLiteral("UsernameEdit"));
    m_userEdit->setProperty("role", QStringLiteral("formInput"));
    m_userEdit->setPlaceholderText(QStringLiteral("Emby 用户名"));
    form->addRow(QStringLiteral("用户名"), m_userEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setObjectName(QStringLiteral("PasswordEdit"));
    m_passwordEdit->setProperty("role", QStringLiteral("formInput"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("密码（无密码可留空）"));
    form->addRow(QStringLiteral("密码"), m_passwordEdit);

    root->addLayout(form);

    m_ignoreSslCheck = new QCheckBox(QStringLiteral("信任自签证书"), this);
    m_ignoreSslCheck->setObjectName(QStringLiteral("IgnoreSslCheck"));
    m_ignoreSslCheck->setToolTip(QStringLiteral("仅在服务器使用自签 HTTPS 证书时勾选"));
    m_ignoreSslCheck->setCursor(Qt::PointingHandCursor);
    root->addWidget(m_ignoreSslCheck);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("LoginStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumHeight(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusLabel->hide();
    root->addWidget(m_statusLabel);
    setStatus(QString(), QStringLiteral("info"));

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(10);
    m_testButton = new QPushButton(QStringLiteral("测试连接"), this);
    m_testButton->setObjectName(QStringLiteral("TestConnectionButton"));
    m_testButton->setProperty("role", QStringLiteral("secondary"));
    m_testButton->setCursor(Qt::PointingHandCursor);

    m_cancelButton = new QPushButton(QStringLiteral("取消"), this);
    m_cancelButton->setObjectName(QStringLiteral("LoginCancelButton"));
    m_cancelButton->setProperty("role", QStringLiteral("ghost"));
    m_cancelButton->setCursor(Qt::PointingHandCursor);

    m_loginButton = new QPushButton(icon(Icon::Play, QColor(0xFF, 0xFF, 0xFF), 16),
                                    QStringLiteral("登录"), this);
    m_loginButton->setObjectName(QStringLiteral("LoginSubmitButton"));
    m_loginButton->setProperty("role", QStringLiteral("primary"));
    m_loginButton->setCursor(Qt::PointingHandCursor);
    m_loginButton->setDefault(true);

    buttons->addWidget(m_testButton);
    buttons->addStretch(1);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_loginButton);
    root->addLayout(buttons);

    connect(m_testButton, &QPushButton::clicked, this, &LoginDialog::testConnection);
    connect(m_loginButton, &QPushButton::clicked, this, &LoginDialog::submit);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::submit);
    connect(m_serverEdit, &QLineEdit::returnPressed, this, &LoginDialog::submit);
    connect(m_userEdit, &QLineEdit::returnPressed, this, &LoginDialog::submit);
}

void LoginDialog::setInitialValues(const QString &serverUrl, const QString &userName,
                                   const QString &errorMessage)
{
    m_serverEdit->setText(serverUrl);
    m_userEdit->setText(userName);
    m_passwordEdit->clear();
    if (!errorMessage.isEmpty())
        setStatus(errorMessage, QStringLiteral("error"));
    if (serverUrl.isEmpty())
        m_serverEdit->setFocus();
    else if (userName.isEmpty())
        m_userEdit->setFocus();
    else
        m_passwordEdit->setFocus();
}

void LoginDialog::setStatus(const QString &text, const QString &state)
{
    const bool hasText = !text.trimmed().isEmpty();
    m_statusLabel->setText(text);
    m_statusLabel->setProperty("state", state);
    m_statusLabel->setVisible(hasText);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
    m_statusLabel->update();
}

void LoginDialog::setBusy(bool busy)
{
    m_serverEdit->setEnabled(!busy);
    m_userEdit->setEnabled(!busy);
    m_passwordEdit->setEnabled(!busy);
    m_ignoreSslCheck->setEnabled(!busy);
    m_testButton->setEnabled(!busy);
    m_loginButton->setEnabled(!busy);
}

void LoginDialog::applyServerProfile(const QString &normalizedUrl)
{
    emby::ServerProfile profile = m_client->profile();
    profile.url = normalizedUrl;
    profile.accessToken.clear();
    profile.userId.clear();
    profile.userName.clear();
    profile.serverId.clear();
    profile.serverType = emby::ServerType::Emby;
    profile.ignoreSslErrors = m_ignoreSslCheck->isChecked();
    if (profile.deviceId.isEmpty())
        profile.deviceId = emby::ServerProfile::generateDeviceId();
    if (profile.deviceName.isEmpty())
        profile.deviceName = QStringLiteral("emby-pc");
    m_client->setProfile(profile);
}

void LoginDialog::applyServerType(const emby::ServerInfo &info)
{
    emby::ServerProfile profile = m_client->profile();
    profile.serverType = info.isJellyfin() ? emby::ServerType::Jellyfin
                                          : emby::ServerType::Emby;
    if (!info.serverId.isEmpty())
        profile.serverId = info.serverId;
    if (!info.serverName.isEmpty())
        profile.name = info.serverName;
    m_client->setProfile(profile);
}

void LoginDialog::testConnection()
{
    if (!m_client)
        return;

    QString error;
    const QString url = normalizeServerUrl(m_serverEdit->text(), &error);
    if (url.isEmpty()) {
        setStatus(error, QStringLiteral("error"));
        m_serverEdit->setFocus();
        return;
    }

    m_serverInfo = {};
    applyServerProfile(url);
    setBusy(true);
    setStatus(QStringLiteral("正在连接服务器…"), QStringLiteral("busy"));

    QPointer<LoginDialog> self(this);
    m_client->fetchPublicSystemInfo([self](const emby::ServerInfo &info, const emby::ApiError &apiError) {
        if (!self)
            return;
        if (apiError.isError()) {
            self->setBusy(false);
            self->setStatus(friendlyApiError(apiError), QStringLiteral("error"));
            return;
        }
        self->m_serverInfo = info;
        self->applyServerType(info);
        const QString product = info.productName.isEmpty()
                                    ? (info.isJellyfin() ? QStringLiteral("Jellyfin")
                                                         : QStringLiteral("Emby"))
                                    : info.productName;
        self->setStatus(QStringLiteral("连接成功：%1 %2（%3）")
                            .arg(info.serverName, info.version, product),
                        QStringLiteral("success"));
        self->setBusy(false);
    });
}

void LoginDialog::submit()
{
    if (!m_client)
        return;

    QString error;
    const QString url = normalizeServerUrl(m_serverEdit->text(), &error);
    if (url.isEmpty()) {
        setStatus(error, QStringLiteral("error"));
        m_serverEdit->setFocus();
        return;
    }
    const QString username = m_userEdit->text().trimmed();
    if (username.isEmpty()) {
        setStatus(QStringLiteral("请输入用户名。"), QStringLiteral("error"));
        m_userEdit->setFocus();
        return;
    }

    m_serverInfo = {};
    applyServerProfile(url);
    const QString password = m_passwordEdit->text();
    setBusy(true);
    setStatus(QStringLiteral("正在连接服务器…"), QStringLiteral("busy"));

    QPointer<LoginDialog> self(this);
    m_client->fetchPublicSystemInfo(
        [self, username, password](const emby::ServerInfo &info, const emby::ApiError &apiError) {
            if (!self)
                return;
            if (apiError.isError()) {
                self->setBusy(false);
                self->setStatus(friendlyApiError(apiError), QStringLiteral("error"));
                return;
            }
            self->m_serverInfo = info;
            self->applyServerType(info);
            self->setStatus(QStringLiteral("服务器可用，正在验证账号…"), QStringLiteral("busy"));

            self->m_client->authenticate(
                username, password,
                [self](const emby::AuthResult &auth, const emby::ApiError &authError) {
                    if (!self)
                        return;
                    if (authError.isError()) {
                        self->setBusy(false);
                        self->setStatus(friendlyApiError(authError), QStringLiteral("error"));
                        return;
                    }
                    Q_UNUSED(auth);

                    emby::ServerProfile profile = self->m_client->profile();
                    if (!self->m_serverInfo.serverName.isEmpty())
                        profile.name = self->m_serverInfo.serverName;
                    if (!self->m_serverInfo.serverId.isEmpty())
                        profile.serverId = self->m_serverInfo.serverId;
                    profile.url = self->m_client->profile().baseUrl();
                    profile.ignoreSslErrors = self->m_ignoreSslCheck->isChecked();
                    profile.serverType = self->m_serverInfo.isJellyfin()
                                             ? emby::ServerType::Jellyfin
                                             : emby::ServerType::Emby;
                    self->m_client->setProfile(profile);

                    self->setBusy(false);
                    self->setStatus(QStringLiteral("登录成功。"), QStringLiteral("success"));
                    emit self->loginSucceeded(profile);
                    self->accept();
                });
        });
}

} // namespace ui