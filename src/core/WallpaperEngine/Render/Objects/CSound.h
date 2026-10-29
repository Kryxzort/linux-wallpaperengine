#pragma once

#include <functional>
#include <random>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CObject.h"

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * A sound object, played the way Wallpaper Engine plays it (WE 2.8.485, decompiled):
 *
 * - every start picks ONE of the object's files uniformly at random (repeats allowed) and plays it at
 *   volume² (the object's `volume`, bound to a user property or not, never clamped: > 1 boosts);
 * - `loop` (also an absent or unknown playbackmode): one file loops natively; several play back to
 *   back, each start a new random pick;
 * - `random`: a file, then a uniform [mintime, maxtime] s of silence (on the host's clock), then a new pick;
 * - `single`: one random file, once;
 * - `startsilent`: not started at load, only by a script's play();
 * - a volume change applies at once (no ramp); a volume rising from 0 starts a not-playing, not-`single`
 *   object; a playing object at volume 0 keeps running silently and its schedule freezes.
 *
 * The object keeps one slot in the audio driver for its whole life; each start swaps a fresh stream of
 * the picked file into it. Picks come from a per-object RNG, seeded from WPENGINE_SOUND_SEED when set
 * (deterministic tests), else from std::random_device.
 */
class CSound final : public CObject {
public:
    enum class Mode { Loop, Random, Single };

    CSound (Wallpapers::CScene& scene, const Sound& sound);
    ~CSound () override;

    void render () override;

    /**
     * Advances the object's schedule (the next file, the silence of `random`) by dt seconds of the
     * host's clock; called once per rendered frame
     *
     * @param dt Seconds since the previous frame (< 0, a clock that went back, counts as 0)
     */
    void tick (double dt);

    /**
     * A script's play(): starts the object even if `startsilent` (SceneScript has no sound methods
     * yet; the entry point exists for when it does)
     */
    void play ();

    /** @return The playback mode in use */
    [[nodiscard]] Mode getMode () const;
    /** @return volume², the amplitude the streams play at */
    [[nodiscard]] float getGain () const;
    /** @return If a file of this object is playing (audible or not) */
    [[nodiscard]] bool isPlaying () const;

private:
    /** WE's start/advance routine: stop, pick, play, schedule (nothing at volume 0) */
    void start ();
    /** the volume's listener: the gain now, and a rise from 0 restarts a stopped non-`single` object */
    void onVolumeChanged ();
    /** swaps `stream` into the driver slot and deletes the previous one */
    void swapStream (Audio::AudioStream* stream);

    const Sound& m_sound;
    Mode m_mode = Mode::Loop;
    bool m_audio = false;

    /** the driver slot (-1: no audio) */
    int m_slot = -1;
    /** the stream in the slot (nullptr between files / before the first start) */
    Audio::AudioStream* m_stream = nullptr;

    /** a start was never attempted (WE's "stopped" bit): volume changes don't start the object */
    bool m_neverStarted = true;
    /** `random`: seconds of silence still to wait once the current file played out (< 0: none pending) */
    double m_gap = -1.0;
    /** `random`: the gap drawn for the file playing now */
    double m_nextGap = 0.0;

    std::mt19937_64 m_random;
    std::function<void ()> m_volumeListener;
};
} // namespace WallpaperEngine::Render::Objects
