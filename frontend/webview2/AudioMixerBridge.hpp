#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <memory>
#include <optional>

struct obs_source;

namespace OBSWeb {

namespace AudioMixerDetail {
enum class Command { Volume, Mute, Monitor, Key, Wheel };

struct Request {
	Command command;
	QString uuid;
	double value;
	QString key;
	int modifiers = 0;
};

std::optional<Request> ParseCommand(const QString &command, const QJsonObject &args, QString &error);

// Owns an OBS audio callback without retaining the audio source. The callback
// never touches Qt widgets, and destruction waits for callbacks to finish.
class LevelMeter final {
public:
	explicit LevelMeter(obs_source *source);
	~LevelMeter();
	LevelMeter(const LevelMeter &) = delete;
	LevelMeter &operator=(const LevelMeter &) = delete;

	QJsonArray peaks() const;
	int channelCount() const;
	void setTruePeak(bool enabled);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
} // namespace AudioMixerDetail

class AudioMixerBridge final : public QObject {
public:
	// Pass the native main window as parent, so the bridge sees only its real
	// AudioMixer controls. All three methods are called on this object's thread.
	explicit AudioMixerBridge(QObject *parent);
	~AudioMixerBridge() override;

	QJsonArray snapshot();
	QJsonObject levels();
	bool execute(const QString &command, const QJsonObject &args, QString &error);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace OBSWeb
