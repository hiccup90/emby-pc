#include "tokenstore.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace ui {
namespace {
constexpr const char *kProfileKey = "auth/profile";
}

QSettingsTokenStore::QSettingsTokenStore() = default;
QSettingsTokenStore::~QSettingsTokenStore() = default;

emby::ServerProfile QSettingsTokenStore::loadProfile() const
{
    const QByteArray data = m_settings.value(QLatin1String(kProfileKey)).toByteArray();
    if (data.isEmpty())
        return {};

    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return {};
    return emby::ServerProfile::fromJson(document.object());
}

void QSettingsTokenStore::saveProfile(const emby::ServerProfile &profile)
{
    const QByteArray data =
        QJsonDocument(profile.toJson()).toJson(QJsonDocument::Compact);
    m_settings.setValue(QLatin1String(kProfileKey), data);
    m_settings.sync();
}

void QSettingsTokenStore::clear()
{
    m_settings.remove(QLatin1String(kProfileKey));
    m_settings.sync();
}

QString QSettingsTokenStore::backendName() const
{
    return QStringLiteral("QSettings 临时存储（明文；Windows 后续改用 DPAPI）");
}

} // namespace ui
