#include <SDL.h>

#include <algorithm>
#include <cstdlib>

#include "CSound.h"

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
uint64_t soundSeed (const int objectId) {
    // WPENGINE_SOUND_SEED: the same picks and gaps on every run (tests); per object, so the order objects
    // are built in doesn't matter
    if (const char* env = std::getenv ("WPENGINE_SOUND_SEED"); env != nullptr && *env != '\0') {
	return std::strtoull (env, nullptr, 0) * 0x9E3779B97F4A7C15ull + static_cast<uint64_t> (objectId);
    }

    std::random_device device;
    return (static_cast<uint64_t> (device ()) << 32) ^ device ();
}
} // namespace

CSound::CSound (Wallpapers::CScene& scene, const Sound& sound) :
    CObject (scene, sound), m_sound (sound), m_random (soundSeed (sound.id)) {
    const auto& mode = sound.playbackmode;

    // absent = loop (WE's constructor default); an unknown name falls back to it too
    if (mode.has_value () && *mode == "random") {
	this->m_mode = Mode::Random;
    } else if (mode.has_value () && *mode == "single") {
	this->m_mode = Mode::Single;
    } else {
	this->m_mode = Mode::Loop;
    }

    this->m_audio = this->getContext ().getContext ().config.enableAudio && !sound.sounds.empty ();

    if (!this->m_audio) {
	return;
    }

    // one slot for the object's life, so the host sees a scene with sound even while this object is
    // held at volume 0 or between files. A startsilent object waits for a script: no slot until then
    if (!sound.startsilent) {
	this->m_slot = this->getScene ().getAudioContext ().addStream (nullptr);
    }

    if (sound.volume != nullptr && sound.volume->value != nullptr) {
	this->m_volumeListener = sound.volume->value->listen ([this] (const DynamicValue&, DynamicValue::UpdateSource) {
	    this->onVolumeChanged ();
	});
    }

    if (!sound.startsilent) {
	this->start ();
    }
}

CSound::~CSound () {
    if (this->m_volumeListener) {
	this->m_volumeListener ();
    }

    if (this->m_slot >= 0) {
	this->getScene ().getAudioContext ().removeStream (this->m_slot);
    }

    delete this->m_stream;
    this->m_stream = nullptr;
}

CSound::Mode CSound::getMode () const { return this->m_mode; }

float CSound::getGain () const {
    if (this->m_sound.volume == nullptr || this->m_sound.volume->value == nullptr) {
	return 1.0f;
    }

    // Wallpaper Engine hands the stream volume² (× the wallpaper's own level, the host's here); no clamp
    const float v = this->m_sound.volume->value->getFloat ();

    return v * v;
}

bool CSound::isPlaying () const { return this->m_stream != nullptr && !this->m_stream->isPlayedOut (); }

void CSound::swapStream (Audio::AudioStream* stream) {
    Audio::AudioStream* previous = this->m_stream;

    this->m_stream = stream;
    this->getScene ().getAudioContext ().replaceStream (this->m_slot, stream);
    // the driver no longer touches it
    delete previous;
}

void CSound::start () {
    if (!this->m_audio || this->m_slot < 0) {
	return;
    }

    this->m_neverStarted = false;

    const float gain = this->getGain ();

    // volume 0 (or the wallpaper muted): WE starts nothing; a later rise does (onVolumeChanged)
    if (!(gain > 0.0f)) {
	return;
    }

    // `random` starts only once its silence is over (a file that just played out starts its silence)
    if (this->m_mode == Mode::Random) {
	if (this->m_gap < 0.0 && this->m_stream != nullptr && this->m_stream->isPlayedOut ()) {
	    this->m_gap = this->m_nextGap;
	}

	if (this->m_gap > 0.0) {
	    return;
	}
    }

    const auto& files = this->m_sound.sounds;
    std::uniform_int_distribution<size_t> pick (0, files.size () - 1);
    const auto& file = files[pick (this->m_random)];
    Audio::AudioStream* stream = nullptr;

    try {
	stream = new Audio::AudioStream (this->getScene ().getAudioContext (), this->getAssetLocator ().read (file));
    } catch (const std::exception& e) {
	sLog.error ("Cannot play sound ", file, " of object ", this->getId (), ": ", e.what ());
	this->swapStream (nullptr);
	this->m_gap = -1.0;
	return;
    }

    // one file of a `loop` object loops natively; everything else plays once and is followed by the next start
    stream->setRepeat (this->m_mode == Mode::Loop && files.size () == 1);
    stream->setGain (gain);
    this->swapStream (stream);
    this->m_gap = -1.0;

    if (this->m_mode == Mode::Random) {
	const double lo = this->m_sound.mintime, hi = this->m_sound.maxtime;
	std::uniform_real_distribution<double> u (0.0, 1.0);

	// WE: U·(maxtime − mintime) + mintime, U in [0, 1)
	this->m_nextGap = u (this->m_random) * (hi - lo) + lo;
    }
}

void CSound::play () {
    if (!this->m_audio) {
	return;
    }

    if (this->m_slot < 0) {
	this->m_slot = this->getScene ().getAudioContext ().addStream (nullptr);
    }

    // a script's play() restarts a waiting `random` object at once
    this->m_gap = -1.0;
    this->start ();
}

void CSound::onVolumeChanged () {
    if (!this->m_audio || this->m_slot < 0) {
	return;
    }

    const float gain = this->getGain ();

    // instant, like WE (no ramp): the next pull mixes at the new level
    if (this->m_stream != nullptr) {
	this->m_stream->setGain (gain);
    }

    // a rise from 0 starts an object that isn't playing — never a `single` one, never one that hasn't
    // had its first start (startsilent)
    if (gain > 0.0f && !this->isPlaying () && this->m_mode != Mode::Single && !this->m_neverStarted) {
	this->start ();
    }
}

void CSound::tick (const double dt) {
    if (!this->m_audio || this->m_slot < 0 || this->m_neverStarted || this->m_mode == Mode::Single) {
	return;
    }

    // a muted object's schedule is frozen (it keeps playing silently)
    if (!(this->getGain () > 0.0f)) {
	return;
    }

    // a looping single file never ends
    if (this->m_stream == nullptr || !this->m_stream->isPlayedOut ()) {
	return;
    }

    if (this->m_mode == Mode::Loop) {
	// back to back: the next random pick as soon as the file played out
	this->start ();
	return;
    }

    // random: the silence is counted from the end of the file
    if (this->m_gap < 0.0) {
	this->m_gap = this->m_nextGap;
    }

    this->m_gap -= std::max (0.0, dt);

    if (this->m_gap <= 0.0) {
	this->m_gap = 0.0;
	this->start ();
    }
}

void CSound::render () { }
