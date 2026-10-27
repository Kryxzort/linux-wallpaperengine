#pragma once

#include <cstddef>
#include <map>
#include <mutex>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"

namespace WallpaperEngine::Audio::Drivers {
/**
 * Decode state of one stream played through the host driver
 */
struct HostAudioBuffer {
    AudioStream* stream = nullptr;
    /** decoded F32 interleaved samples; same capacity as the SDL driver's per-stream buffer */
    alignas (float) uint8_t data[(192000 * 3) / 2] = { 0 };
    /** valid samples in data */
    size_t size = 0;
    /** next sample to mix */
    size_t index = 0;
};

/**
 * Audio driver for embedders that own audio output themselves (wp_config_set_audio_output).
 *
 * Opens no device: the host pulls F32 interleaved PCM at the configured rate and channel count with
 * wp_context_audio_pull, from whatever thread drives its own output (a real-time callback included:
 * a pull never waits for decoding — what is not decoded yet is silence). Every stream is mixed at
 * unity gain; volume, ramps, mute and pause are the host's (not pulling = paused: the decode queues
 * fill and the read threads wait). Because the host decides how many frames it pulls per second it
 * can also play a scene's sound faster or slower, e.g. through a time-stretch stage of its own.
 */
class HostAudioDriver final : public AudioDriver {
public:
    HostAudioDriver (Context& applicationContext, int sampleRate, int channels);
    ~HostAudioDriver () override;

    /** @inheritdoc */
    int addStream (AudioStream* stream) override;
    /** @inheritdoc */
    void removeStream (int streamId) override;

    /** @inheritdoc */
    [[nodiscard]] AVSampleFormat getFormat () const override;
    /** @inheritdoc */
    [[nodiscard]] int getSampleRate () const override;
    /** @inheritdoc */
    [[nodiscard]] int getChannels () const override;

    /**
     * Mixes the next frames of every stream into out (frames * channels floats), silence where
     * nothing is decoded yet
     *
     * @return frames written (always frames, 0 for a null buffer)
     */
    size_t pull (float* out, size_t frames);

    /**
     * @return The number of streams currently registered
     */
    [[nodiscard]] int streamCount ();

private:
    int m_sampleRate;
    int m_channels;
    int m_lastStreamID = 0;
    std::mutex m_streamsMutex;
    std::map<int, HostAudioBuffer*> m_streams {};
};
} // namespace WallpaperEngine::Audio::Drivers
