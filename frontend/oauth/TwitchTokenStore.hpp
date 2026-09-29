#pragma once

#include "TwitchDeviceFlow.hpp"
#include <optional>

namespace twitch {
QByteArray protectCredentials(const Credentials &credentials);
std::optional<Credentials> unprotectCredentials(const QByteArray &encrypted);

// Profiles hold an opaque reference, so a copied profile sees rotated tokens too.
class SessionStore {
public:
	explicit SessionStore(QString directory) : directory(std::move(directory)) {}
	static QString newId();
	std::optional<Credentials> read(const QString &id) const;
	bool write(const QString &id, const Credentials &credentials) const;
	bool remove(const QString &id) const;

private:
	QString path(const QString &id) const;
	QString directory;
};
} // namespace twitch
