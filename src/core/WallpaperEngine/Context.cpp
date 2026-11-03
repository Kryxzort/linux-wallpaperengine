#include "Context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

#include "Audio/Drivers/HostAudioDriver.h"
#include "Audio/Drivers/SDLAudioDriver.h"
#include "WallpaperEngine/Audio/AudioPlayingDetector.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"
#include "WallpaperEngine/Audio/PlaybackRecorder.h"

WallpaperEngine::Context::Context (
    const Configuration& config, wp_time_counter* time_counter, wp_gl_proc_address* gl_proc_addresses,
    wp_audio_input_mix* audio_input_mix
) :
    texture_cache (std::make_unique<TextureCache> ()), config (config), gl_proc_address (gl_proc_addresses),
    time_counter (time_counter), audio_input_mix (audio_input_mix), isRunning (true), renderTime (0.0f),
    renderTimeLast (0.0f), frameTime (0.0f), shaderTime (0.0f), daytime (0.0f) {
    // a host audio output replaces SDL entirely: no SDL audio subsystem, no device
    std::unique_ptr<Audio::Drivers::AudioDriver> driver;

    if (config.audioOutput.has_value ()) {
	driver = std::make_unique<Audio::Drivers::HostAudioDriver> (
	    *this, config.audioOutput->sample_rate, config.audioOutput->channels
	);
    } else {
	driver = std::make_unique<Audio::Drivers::SDLAudioDriver> (*this);
    }

    this->audio = std::make_unique<Audio::AudioContext> (
	std::move (driver), std::make_unique<Audio::PlaybackRecorder> (*this),
	std::make_unique<Audio::AudioPlayingDetector> (*this)
    );
}

void WallpaperEngine::Context::updateTime () {
    this->renderTimeLast = this->renderTime;
    this->renderTime = this->time_counter->get_time (this->time_counter->user_parameter);
    this->frameTime = std::max (0.0f, this->renderTime - this->renderTimeLast);
    this->shaderTime = this->renderTime >= 0.0f ? std::fmod (this->renderTime, 432000.0f) : this->renderTime;

    // the local time of day (WE's FUN_14013f370: GetLocalTime → h/24 + m/1440 + s/86400 + ms/86400000)
    const auto now = std::chrono::system_clock::now ();
    const std::time_t seconds = std::chrono::system_clock::to_time_t (now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch ()).count () % 1000;
    std::tm local {};

    if (localtime_r (&seconds, &local) != nullptr) {
	this->daytime = static_cast<float> (
	    local.tm_hour / 24.0 + local.tm_min / 1440.0 + local.tm_sec / 86400.0 + static_cast<double> (ms) / 86400000.0
	);
    }
    this->audio->getDetector ().update ();
    this->audio->getRecorder ().update ();
}