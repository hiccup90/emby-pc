#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace emby {
struct ServerProfile;
struct ServerInfo;
}

namespace ui {

// 设置页（T28）：账户、播放、存储/日志、关于。
// QSettings 键供 T29 播放侧读取：player/subtitleLanguage、player/autoPlayNext、
// player/rememberVolume、player/volume、player/autoHideDelayMs、cache/imageLimitMB。
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

    // T28/T29：播放侧回填信息。
    void setHardwareDecoderText(const QString &text);
    void setLibmpvVersionText(const QString &text);

    // T28：设置项读取 API（T29 播放侧也读 QSettings；这里便于打开设置页时使用）。
    QString subtitleLanguage() const;
    bool autoPlayNext() const;
    bool rememberVolume() const;
    int autoHideDelayMs() const;
    int cacheImageLimitMB() const;

    // 重新扫描 CacheLocation/images 占用。
    void refreshCacheUsage();

protected:
    void showEvent(QShowEvent *event) override;

signals:
    void loginRequested();
    void logoutRequested();
    void mockModeChanged(bool enabled);
    void settingsChanged();
    void clearImageCacheRequested();
    void exportDiagnosticsRequested();
    void copyDiagnosticsRequested();

private:
    void buildUi();
    void loadSettings();
    void saveControlValues();
    void openLogDirectory();
    void copyDiagnosticsToClipboard();

    QLabel *m_accountHint = nullptr;
    QLabel *m_connectionValue = nullptr;
    QLabel *m_serverValue = nullptr;
    QLabel *m_userValue = nullptr;
    QLabel *m_versionValue = nullptr;
    QLabel *m_tokenValue = nullptr;
    QPushButton *m_loginButton = nullptr;
    QPushButton *m_logoutButton = nullptr;
    QCheckBox *m_mockCheck = nullptr;
    QLabel *m_dataSourceValue = nullptr;

    QComboBox *m_subtitleCombo = nullptr;
    QCheckBox *m_autoPlayNextCheck = nullptr;
    QCheckBox *m_rememberVolumeCheck = nullptr;
    QComboBox *m_autoHideCombo = nullptr;
    QLabel *m_hardwareDecoderValue = nullptr;

    QLabel *m_cacheUsageValue = nullptr;
    QSpinBox *m_cacheLimitSpin = nullptr;
    QPushButton *m_clearCacheButton = nullptr;
    QPushButton *m_openLogButton = nullptr;
    QPushButton *m_exportDiagnosticsButton = nullptr;

    QLabel *m_appVersionValue = nullptr;
    QLabel *m_qtVersionValue = nullptr;
    QLabel *m_buildTimeValue = nullptr;
    QLabel *m_libmpvVersionValue = nullptr;
    QPushButton *m_copyDiagnosticsButton = nullptr;

    bool m_loading = false;
};

} // namespace ui
