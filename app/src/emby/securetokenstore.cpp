#include "securetokenstore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace emby {

namespace {

constexpr const char *kLegacySettingsKey = "auth/profile";
constexpr const char *kSecureSettingsKey = "auth/profile.dpapi.v1";
constexpr const char *kSecureFileMagic = "EMBYSEC1";
constexpr int kSecureNonceSize = 16;
constexpr int kSecureChecksumSize = 32;

QByteArray machineIdentity()
{
    QByteArray identity = QSysInfo::machineUniqueId();
    if (identity.isEmpty())
        identity = QSysInfo::machineHostName().toUtf8();
    identity += '|';
    identity += QStandardPaths::writableLocation(QStandardPaths::HomeLocation).toUtf8();
    return identity;
}

QByteArray keyStream(const QByteArray &nonce, int size)
{
    const QByteArray seed =
        QCryptographicHash::hash(QByteArrayLiteral("EmbyPC.SecureProfile.v1|")
                                     + machineIdentity(),
                                 QCryptographicHash::Sha256);
    QByteArray stream;
    stream.reserve(size + 32);
    for (int block = 0; stream.size() < size; ++block) {
        QByteArray counter;
        counter.setNum(block);
        stream += QCryptographicHash::hash(seed + nonce + counter, QCryptographicHash::Sha256);
    }
    return stream;
}

QByteArray randomNonce()
{
    QByteArray nonce;
    nonce.reserve(kSecureNonceSize);
    for (int i = 0; i < kSecureNonceSize; i += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        nonce.append(static_cast<char>((value >> 24) & 0xff));
        nonce.append(static_cast<char>((value >> 16) & 0xff));
        nonce.append(static_cast<char>((value >> 8) & 0xff));
        nonce.append(static_cast<char>(value & 0xff));
    }
    return nonce;
}

QByteArray xorWithStream(const QByteArray &data, const QByteArray &nonce)
{
    const QByteArray stream = keyStream(nonce, data.size());
    QByteArray result = data;
    for (int i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(result.at(i) ^ stream.at(i));
    return result;
}

#ifdef Q_OS_WIN

QByteArray dpapiProtect(const QByteArray &plain)
{
    if (plain.isEmpty())
        return {};
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());
    static const char entropyText[] = "EmbyPC.SecureProfile.v1";
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(entropyText));
    entropy.cbData = static_cast<DWORD>(sizeof(entropyText) - 1);
    DATA_BLOB out{};
    const BOOL ok = ::CryptProtectData(&in, L"Emby PC secure profile", &entropy, nullptr,
                                       nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok || !out.pbData)
        return {};
    const QByteArray result(reinterpret_cast<const char *>(out.pbData),
                            static_cast<int>(out.cbData));
    ::LocalFree(out.pbData);
    return result;
}

QByteArray dpapiUnprotect(const QByteArray &cipher)
{
    if (cipher.isEmpty())
        return {};
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));
    in.cbData = static_cast<DWORD>(cipher.size());
    static const char entropyText[] = "EmbyPC.SecureProfile.v1";
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(entropyText));
    entropy.cbData = static_cast<DWORD>(sizeof(entropyText) - 1);
    DATA_BLOB out{};
    const BOOL ok = ::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                         CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok || !out.pbData)
        return {};
    const QByteArray result(reinterpret_cast<const char *>(out.pbData),
                            static_cast<int>(out.cbData));
    ::LocalFree(out.pbData);
    return result;
}

#endif  // Q_OS_WIN

void removeLegacyKey()
{
    QSettings settings;
    if (settings.contains(QLatin1String(kLegacySettingsKey))) {
        settings.remove(QLatin1String(kLegacySettingsKey));
        settings.sync();
    }
}

}  // namespace

SecureProfileStore::SecureProfileStore(const QString &storageDirectory)
    : m_storageDirectory(storageDirectory.trimmed())
{
}

QString SecureProfileStore::storageDirectory() const
{
    if (!m_storageDirectory.isEmpty())
        return m_storageDirectory;
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.emby-pc");
    return base + QStringLiteral("/secure");
}

QString SecureProfileStore::secureFilePath() const
{
    return storageDirectory() + QStringLiteral("/profile.dat");
}

QString SecureProfileStore::storagePath() const
{
#ifdef Q_OS_WIN
    return QStringLiteral("QSettings:") + QLatin1String(kSecureSettingsKey);
#else
    return secureFilePath();
#endif
}

bool SecureProfileStore::hasStoredProfile() const
{
#ifdef Q_OS_WIN
    QSettings settings;
    if (!settings.value(QLatin1String(kSecureSettingsKey)).toString().isEmpty())
        return true;
#else
    if (QFileInfo::exists(secureFilePath()))
        return true;
#endif
    QSettings legacy;
    return !legacy.value(QLatin1String(kLegacySettingsKey)).toByteArray().isEmpty();
}

QString SecureProfileStore::backendName() const
{
#ifdef Q_OS_WIN
    return QStringLiteral("Windows DPAPI（当前用户）");
#else
    return QStringLiteral("Linux 文件回退（混淆，权限 0600）");
#endif
}

QByteArray SecureProfileStore::encodeForStorage(const QByteArray &plain) const
{
    const QByteArray nonce = randomNonce();
    const QByteArray checksum = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);
    QByteArray payload;
    payload.reserve(8 + nonce.size() + checksum.size() + plain.size());
    payload.append(kSecureFileMagic, 8);
    payload.append(nonce);
    payload.append(checksum);
    payload.append(xorWithStream(plain, nonce));
    return payload;
}

QByteArray SecureProfileStore::decodeFromStorage(const QByteArray &payload) const
{
    const int minimum = 8 + kSecureNonceSize + kSecureChecksumSize;
    if (payload.size() < minimum)
        return {};
    if (payload.left(8) != QByteArray(kSecureFileMagic, 8))
        return {};
    const QByteArray nonce = payload.mid(8, kSecureNonceSize);
    const QByteArray expectedChecksum = payload.mid(8 + kSecureNonceSize, kSecureChecksumSize);
    const QByteArray plain =
        xorWithStream(payload.mid(minimum), nonce);
    if (QCryptographicHash::hash(plain, QCryptographicHash::Sha256) != expectedChecksum)
        return {};
    return plain;
}

bool SecureProfileStore::writeSecurePayload(const QByteArray &plain) const
{
#ifdef Q_OS_WIN
    const QByteArray cipher = dpapiProtect(plain);
    if (cipher.isEmpty())
        return false;
    QSettings settings;
    settings.setValue(QLatin1String(kSecureSettingsKey),
                      QString::fromLatin1(cipher.toBase64()));
    settings.sync();
    return settings.status() == QSettings::NoError;
#else
    if (!QDir().mkpath(storageDirectory()))
        return false;
    QFile::setPermissions(storageDirectory(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner
                              | QFileDevice::ExeOwner);
    const QByteArray payload = encodeForStorage(plain);
    QSaveFile file(secureFilePath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(payload) != payload.size() || !file.commit())
        return false;
    QFile::setPermissions(secureFilePath(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
#endif
}

QByteArray SecureProfileStore::readSecurePayload() const
{
#ifdef Q_OS_WIN
    QSettings settings;
    const QString encoded = settings.value(QLatin1String(kSecureSettingsKey)).toString();
    if (encoded.isEmpty())
        return {};
    const QByteArray cipher = QByteArray::fromBase64(encoded.toLatin1());
    return dpapiUnprotect(cipher);
#else
    QFile file(secureFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return decodeFromStorage(file.readAll());
#endif
}

ServerProfile SecureProfileStore::loadLegacyProfile() const
{
    QSettings settings;
    const QByteArray data = settings.value(QLatin1String(kLegacySettingsKey)).toByteArray();
    if (data.isEmpty())
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return {};
    return ServerProfile::fromJson(document.object());
}

void SecureProfileStore::removeLegacyProfile() const
{
    removeLegacyKey();
}

ServerProfile SecureProfileStore::loadProfile() const
{
    const QByteArray plain = readSecurePayload();
    if (!plain.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(plain);
        if (document.isObject())
            return ServerProfile::fromJson(document.object());
    }

    // Migration path from the pre-DPAPI build: the old ui::QSettingsTokenStore
    // wrote the plaintext JSON under "auth/profile". Read it once, write the
    // new secure value, then remove the legacy plaintext.
    ServerProfile legacy = loadLegacyProfile();
    if (legacy.isAuthenticated()) {
        saveProfile(legacy);
        return legacy;
    }
    return {};
}

bool SecureProfileStore::saveProfile(const ServerProfile &profile) const
{
    const QByteArray plain =
        QJsonDocument(profile.toJson()).toJson(QJsonDocument::Compact);
    if (!writeSecurePayload(plain))
        return false;
    removeLegacyProfile();
    return true;
}

void SecureProfileStore::clear()
{
#ifdef Q_OS_WIN
    QSettings settings;
    settings.remove(QLatin1String(kSecureSettingsKey));
    settings.remove(QLatin1String(kLegacySettingsKey));
    settings.sync();
#else
    QFile::remove(secureFilePath());
    removeLegacyKey();
#endif
}

QString redactSensitiveText(const QString &text)
{
    QString result = text;

    // Authorization: ... (also catches mpv's escaped log form).
    static const QRegularExpression authorization(
        QStringLiteral("(?i)(Authorization\\s*:\\s*)[^\\r\\n]+"));
    result.replace(authorization, QStringLiteral("\\1<redacted>"));

    static const QRegularExpression bearer(
        QStringLiteral("(?i)(Bearer\\s+)[A-Za-z0-9._~+/=-]+"));
    result.replace(bearer, QStringLiteral("\\1<redacted>"));

    // Token=, Token: , "AccessToken": "...", api_key=..., X-Emby-Token: ...
    // The value deliberately stops at common query/JSON separators so normal
    // surrounding text survives.
    static const QRegularExpression tokenFields(
        QStringLiteral("(?i)([\\\"']?(?:api_key|access_token|accessToken|"
                       "apiKey|X-Emby-Token|X-MediaBrowser-Token|Token|Password|Pw)"
                       "[\\\"']?\\s*[=:]\\s*)[^&,\\s;]+"));
    result.replace(tokenFields, QStringLiteral("\\1<redacted>"));

    return result;
}

}  // namespace emby
