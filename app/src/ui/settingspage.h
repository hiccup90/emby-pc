#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

namespace emby {
struct ServerProfile;
struct ServerInfo;
}

namespace ui {

// 设置页：账户（当前服务器/用户/版本 + 登录/退出登录）+ 播放/界面/关于占位。
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

    void setSession(const emby::ServerProfile &profile, const emby::ServerInfo &serverInfo);
    void clearSession();
    void setTokenBackendName(const QString &backendName);

    // M2-B：在演示数据（本地 mock）与真实 Emby 媒体库之间切换。
    void setMockMode(bool enabled);
    bool mockMode() const;

signals:
    void loginRequested();
    void logoutRequested();
    void mockModeChanged(bool enabled);

private:
    void buildUi();

    QLabel *m_accountHint = nullptr;
    QLabel *m_serverValue = nullptr;
    QLabel *m_userValue = nullptr;
    QLabel *m_versionValue = nullptr;
    QLabel *m_tokenValue = nullptr;
    QPushButton *m_loginButton = nullptr;
    QPushButton *m_logoutButton = nullptr;
    QCheckBox *m_mockCheck = nullptr;
    QLabel *m_dataSourceValue = nullptr;
};

} // namespace ui
