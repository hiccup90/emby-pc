#pragma once

#include "embytypes.h"

#include <QSettings>
#include <QString>

namespace ui {

// Token 持久化接口。UI 层只依赖这个抽象，方便后续在 Windows 上替换为
// DPAPI（CryptProtectData/CryptUnprotectData）实现：
//   * 当前 QSettingsTokenStore 是临时实现：JSON 明文写入 QSettings；
//   * 不打印 token、不把 token 拼进 URL；
//   * Windows DPAPI 版本只需实现 loadProfile / saveProfile / clear 即可。
class TokenStore
{
public:
    virtual ~TokenStore() = default;

    // 没有保存过或数据损坏时返回默认构造的 ServerProfile（isValid() == false）。
    virtual emby::ServerProfile loadProfile() const = 0;
    virtual void saveProfile(const emby::ServerProfile &profile) = 0;
    virtual void clear() = 0;

    // 供设置页展示当前后端（例如提醒用户当前是明文临时存储）。
    virtual QString backendName() const = 0;
};

// 临时实现：QSettings 明文存储（Windows 后续替换为 DPAPI）。
class QSettingsTokenStore : public TokenStore
{
public:
    QSettingsTokenStore();
    ~QSettingsTokenStore() override;

    emby::ServerProfile loadProfile() const override;
    void saveProfile(const emby::ServerProfile &profile) override;
    void clear() override;
    QString backendName() const override;

private:
    mutable QSettings m_settings;
};

} // namespace ui
