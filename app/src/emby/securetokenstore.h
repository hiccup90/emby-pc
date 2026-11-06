#pragma once

// Secure on-disk storage for emby::ServerProfile (token) and a small set of
// text-redaction helpers used by app.log/crash diagnostics.
//
// Windows: DPAPI CryptProtectData/CryptUnprotectData (current user).
// Other platforms: no libsecret dependency is available in this build, so the
// fallback obfuscates the JSON and writes a 0600 file.  The fallback is NOT a
// substitute for a system keyring; it only avoids plaintext-at-rest.

#include "embytypes.h"

#include <QByteArray>
#include <QString>

namespace emby {

class SecureProfileStore
{
public:
    explicit SecureProfileStore(const QString &storageDirectory = QString());

    /// Reads the secure profile. If no secure value exists but the legacy
    /// QSettings key "auth/profile" (written by older builds) does, the legacy
    /// value is migrated, the secure value is written, and the legacy key is
    /// removed. Returns a default ServerProfile on any error.
    ServerProfile loadProfile() const;

    /// Encrypts/obfuscates profile JSON. Never stores a password (ServerProfile
    /// has no password field). Returns false if the platform storage failed.
    bool saveProfile(const ServerProfile &profile) const;

    /// Removes both the secure value and the legacy plaintext key.
    void clear();

    bool hasStoredProfile() const;
    QString backendName() const;

    /// Linux test/debug: path of the secure blob. Windows: the QSettings key.
    QString storagePath() const;

private:
    QString storageDirectory() const;
    QString secureFilePath() const;
    QByteArray encodeForStorage(const QByteArray &plain) const;
    QByteArray decodeFromStorage(const QByteArray &payload) const;
    bool writeSecurePayload(const QByteArray &plain) const;
    QByteArray readSecurePayload() const;
    ServerProfile loadLegacyProfile() const;
    void removeLegacyProfile() const;

    QString m_storageDirectory;
};

/// Redacts common credential forms from a log/diagnostic line:
/// Authorization headers, Bearer tokens, api_key/access_token/Token= query or
/// JSON fields, and Password/Pw fields. The original text is returned when no
/// pattern matches. This function never needs the actual token value.
QString redactSensitiveText(const QString &text);

}  // namespace emby
