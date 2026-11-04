#pragma once

#include "embytypes.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace emby {
class EmbyClient;
}

namespace ui {

// 登录 / 服务器连接对话框。
// 流程：测试连接 -> fetchPublicSystemInfo；登录 -> fetchPublicSystemInfo 后 authenticate。
// 成功后发出 loginSucceeded(profile)，由 MainWindow 负责 TokenStore 持久化。
// 密码只在内存中传给 EmbyClient，不写日志、不拼 URL。
class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(emby::EmbyClient *client, QWidget *parent = nullptr);

    void setInitialValues(const QString &serverUrl, const QString &userName,
                          const QString &errorMessage = QString());

    const emby::ServerInfo &serverInfo() const { return m_serverInfo; }

signals:
    void loginSucceeded(const emby::ServerProfile &profile);

private:
    void buildUi();
    void testConnection();
    void submit();
    void setBusy(bool busy);
    void setStatus(const QString &text, const QString &state);
    void applyServerProfile(const QString &normalizedUrl);
    void applyServerType(const emby::ServerInfo &info);

    emby::EmbyClient *m_client = nullptr;
    QLineEdit *m_serverEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_ignoreSslCheck = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_testButton = nullptr;
    QPushButton *m_loginButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    emby::ServerInfo m_serverInfo;
};

} // namespace ui
