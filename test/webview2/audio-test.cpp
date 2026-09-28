#include "AudioMixerBridge.hpp"

#include <obs.hpp>
#include <util/platform.h>

#include <QCoreApplication>
#include <QJsonDocument>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <thread>

using namespace OBSWeb::AudioMixerDetail;

namespace {
void RegisterAudioFixture()
{
	obs_source_info info{};
	info.id = "webview2-audio-fixture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = [](void *) { return "WebView2 audio fixture"; };
	info.create = [](obs_data_t *, obs_source_t *source) -> void * { return source; };
	info.destroy = [](void *) {};
	obs_register_source(&info);
}

void OutputBlock(obs_source_t *source, float amplitude)
{
	std::array<float, 1024> samples;
	samples.fill(amplitude);
	obs_source_audio audio{};
	audio.data[0] = reinterpret_cast<const uint8_t *>(samples.data());
	audio.data[1] = reinterpret_cast<const uint8_t *>(samples.data());
	audio.frames = static_cast<uint32_t>(samples.size());
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.samples_per_sec = 48000;
	audio.timestamp = os_gettime_ns();
	obs_source_output_audio(source, &audio);
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	// Headless libobs emits graphics-context debug messages during source
	// destruction. Keep warnings/errors visible and omit routine debug noise.
	base_set_log_handler([](int level, const char *format, va_list args, void *) {
		if (level <= LOG_WARNING) {
			vfprintf(stderr, format, args);
			fputc('\n', stderr);
		}
	}, nullptr);
	int checks = 0;
	int failures = 0;
	auto check = [&](bool pass, const char *label) {
		++checks;
		if (!pass) {
			++failures;
			std::cerr << "FAIL: " << label << '\n';
		}
	};
	QString error;
	const QString uuid = QStringLiteral("test-source-uuid");
	for (const double value : {0.0, 0.25, 1.0}) {
		const auto request = ParseCommand("audio.volume", {{"uuid", uuid}, {"value", value}}, error);
		check(request && request->command == Command::Volume && request->uuid == uuid && request->value == value &&
			      error.isEmpty(),
		      "valid normalized fader values survive validation");
	}
	for (const auto value : {QJsonValue(-0.01), QJsonValue(1.01), QJsonValue("0.5"), QJsonValue(true),
				QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Undefined),
				QJsonValue(std::numeric_limits<double>::infinity())}) {
		check(!ParseCommand("audio.volume", {{"uuid", uuid}, {"value", value}}, error) && !error.isEmpty(),
		      "invalid volume is rejected without Qt coercion or clamping");
	}
	for (const bool value : {false, true}) {
		const auto request = ParseCommand("audio.mute", {{"uuid", uuid}, {"value", value}}, error);
		check(request && request->command == Command::Mute && request->value == double(value),
		      "mute accepts both boolean states");
	}
	for (const auto value : {QJsonValue(0), QJsonValue(1), QJsonValue("true"), QJsonValue(QJsonValue::Null)})
		check(!ParseCommand("audio.mute", {{"uuid", uuid}, {"value", value}}, error),
		      "mute rejects non-boolean values");
	for (const int value : {0, 2}) {
		const auto request = ParseCommand("audio.monitor", {{"uuid", uuid}, {"value", value}}, error);
		check(request && request->command == Command::Monitor && request->value == value,
		      "OBS33 monitoring off and on are accepted");
	}
	for (const auto value : {QJsonValue(-1), QJsonValue(1), QJsonValue(3), QJsonValue(0.5), QJsonValue(true),
				QJsonValue("2")})
		check(!ParseCommand("audio.monitor", {{"uuid", uuid}, {"value", value}}, error),
		      "unsupported or malformed monitoring mode is rejected");
	for (const auto id : {QJsonValue(""), QJsonValue("  "), QJsonValue(42), QJsonValue(QJsonValue::Null),
			     QJsonValue(QString(257, 'x')), QJsonValue(QString::fromUtf8("source\0suffix", 13))})
		check(!ParseCommand("audio.mute", {{"uuid", id}, {"value", true}}, error),
		      "missing or unsafe source identity is rejected");
	check(!ParseCommand("audio.unknown", {{"uuid", uuid}, {"value", true}}, error),
	      "unknown audio command is rejected");

	if (!obs_startup("en-US", nullptr, nullptr)) {
		check(false, "isolated libobs starts");
		return 1;
	}
	obs_audio_info audioInfo{};
	audioInfo.samples_per_sec = 48000;
	audioInfo.speakers = SPEAKERS_STEREO;
	check(obs_reset_audio(&audioInfo), "isolated libobs audio initializes");
	RegisterAudioFixture();
	{
		OBSSourceAutoRelease source = obs_source_create("webview2-audio-fixture", "Meter fixture", nullptr, nullptr);
		check(source != nullptr, "real audio fixture source is created");
		LevelMeter meter(source);
		check(meter.peaks().isEmpty(), "meter has no fabricated samples before audio arrives");
		check(meter.channelCount() == 2, "idle stereo source keeps the native default meter channel count");
		for (int i = 0; i < 8; ++i)
			OutputBlock(source, 0.5f);
		const auto peaks = meter.peaks();
		check(meter.channelCount() == peaks.size(), "meter channel geometry matches the real PCM callback");
		check(peaks.size() == 2 && std::abs(peaks[0].toDouble() + 6.0206) < 0.02 &&
			      std::abs(peaks[1].toDouble() + 6.0206) < 0.02,
		      "real stereo half-amplitude PCM produces minus six dB channel peaks");
		obs_source_set_volume(source, 0.5f);
		for (int i = 0; i < 8; ++i)
			OutputBlock(source, 0.5f);
		const auto quieter = meter.peaks();
		check(quieter.size() == 2 && std::abs(quieter[0].toDouble() + 12.0412) < 0.02,
		      "native source fader affects reported meter peaks");
		std::this_thread::sleep_for(std::chrono::milliseconds(550));
		const auto idle = meter.peaks();
		check(idle.size() == 2 && idle[0].toDouble() <= -100.0,
		      "stopped audio expires rather than leaving a frozen loud meter");
		obs_source_remove(source);
		check(meter.peaks().isEmpty(), "removed source cannot continue reporting levels");
	}
	{
		OBSSourceAutoRelease source = obs_source_create("webview2-audio-fixture", "Lifetime fixture", nullptr, nullptr);
		OBSWeakSource weak = OBSGetWeakRef(source);
		auto meter = std::make_unique<LevelMeter>(source);
		for (int i = 0; i < 8; ++i)
			OutputBlock(source, 0.25f);
		source = nullptr;
		obs_wait_for_destroy_queue();
		check(!OBSGetStrongRef(weak), "meter does not keep a deleted source alive");
		check(meter->peaks().isEmpty(), "source destruction invalidates meter samples");
		meter.reset();
	}
	{
		OBSSourceAutoRelease source = obs_source_create("webview2-audio-fixture", "Thread fixture", nullptr, nullptr);
		std::atomic<bool> stop{false};
		std::thread producer([&]() {
			while (!stop.load(std::memory_order_relaxed)) {
				OutputBlock(source, 0.25f);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
		bool finiteSamples = true;
		int sampledMeters = 0;
		for (int i = 0; i < 80; ++i) {
			LevelMeter meter(source);
			meter.setTruePeak((i % 2) == 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			const auto peaks = meter.peaks();
			sampledMeters += !peaks.isEmpty();
			for (const auto value : peaks)
				finiteSamples &= value.isDouble() && std::isfinite(value.toDouble());
		}
		stop = true;
		producer.join();
		check(finiteSamples && sampledMeters > 0,
		      "meter creation and callback teardown are safe during concurrent audio");
	}
	obs_shutdown();
	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}
