#include "TwitchTokenStore.hpp"

#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QUuid>
#include <Windows.h>
#include <wincrypt.h>

namespace twitch {

QByteArray protectCredentials(const Credentials &credentials)
{
	if (credentials.accessToken.isEmpty() || credentials.refreshToken.isEmpty()) return {};
	QJsonObject object{{"version", 1}, {"client_id", credentials.clientId}, {"access", credentials.accessToken},
		{"refresh", credentials.refreshToken}, {"login", credentials.login}, {"user_id", credentials.userId},
		{"expires", credentials.expiresAt}};
	auto plain = QJsonDocument(object).toJson(QJsonDocument::Compact);
	DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(plain.data())}, output{};
	if (!CryptProtectData(&input, L"OBS Twitch device authorization", nullptr, nullptr, nullptr,
			      CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		SecureZeroMemory(plain.data(), plain.size());
		return {};
	}
	QByteArray encrypted(reinterpret_cast<const char *>(output.pbData), output.cbData);
	LocalFree(output.pbData);
	SecureZeroMemory(plain.data(), plain.size());
	return encrypted;
}

std::optional<Credentials> unprotectCredentials(const QByteArray &encrypted)
{
	if (encrypted.isEmpty() || encrypted.size() > 65536) return std::nullopt;
	DATA_BLOB input{DWORD(encrypted.size()), reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()))}, output{};
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return std::nullopt;
	auto document = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char *>(output.pbData), output.cbData));
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	auto json = document.object();
	if (json["version"].toInt() != 1) return std::nullopt;
	Credentials result{json["client_id"].toString(), json["access"].toString(), json["refresh"].toString(),
		json["login"].toString(), json["user_id"].toString(), json["expires"].toInteger()};
	if (result.clientId.isEmpty() || result.accessToken.isEmpty() || result.refreshToken.isEmpty()) return std::nullopt;
	return result;
}

QString SessionStore::newId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString SessionStore::path(const QString &id) const
{
	const QUuid uuid(id);
	if (directory.isEmpty() || uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id) return {};
	return QDir(directory).filePath(id + ".dpapi");
}

std::optional<Credentials> SessionStore::read(const QString &id) const
{
	const auto filename = path(id);
	if (filename.isEmpty()) return std::nullopt;
	QFile file(filename);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return std::nullopt;
	return unprotectCredentials(file.readAll());
}

bool SessionStore::write(const QString &id, const Credentials &credentials) const
{
	const auto filename = path(id);
	if (filename.isEmpty()) return false;
	const auto encrypted = protectCredentials(credentials);
	if (encrypted.isEmpty() || !QDir().mkpath(directory)) return false;
	QSaveFile file(filename);
	return file.open(QIODevice::WriteOnly) && file.write(encrypted) == encrypted.size() && file.commit();
}

bool SessionStore::remove(const QString &id) const
{
	const auto filename = path(id);
	return !filename.isEmpty() && (!QFile::exists(filename) || QFile::remove(filename));
}

} // namespace twitch
