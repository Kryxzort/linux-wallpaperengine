#pragma once

#include <cstddef>
#include <vector>

#include "kiss_fft.h"
#include "linux-wallpaperengine/context.h"

namespace WallpaperEngine {
class Context;
}

namespace WallpaperEngine::Audio {
/**
 * The audio spectrum Wallpaper Engine gives audio-responsive wallpapers (shader uniforms g_AudioSpectrum*,
 * SceneScript's engine.registerAudioBuffers, particle emitters with audioprocessingmode), computed the way
 * wallpaper64.exe 2.8.485 does (docs: liascape docs/player/spikes/we-lwe-gap-audit.md, "B8 result"):
 *
 * - the producer (WE's capture thread GH 0x140103630): f32 frames of what the system plays, per channel a
 *   complex FFT of N = max(rate/44100, 1)·1920 points whose first 2N/3 are fresh samples, the bins 1..639
 *   reduced to 64 bands (pow ((i−1)/639, 0.25)·64, at most one band further per bin, max per band) of
 *   sqrt (w·|X|²), w = 0.501 − 0.499·cos (π (i−1)/639), scaled by 0.001·640/(N/2);
 * - the consumer (the wallpaper frame tick GH 0x140141520), every rendered frame: an auto-gain per group of
 *   8 bands, a dt-based low-pass and a rise/fall-limited follow, then the average channel, 32 and 16 bands
 *   (max of pairs).
 */
class PlaybackRecorder final {
public:
    explicit PlaybackRecorder (Context& context);
    ~PlaybackRecorder ();

    PlaybackRecorder (const PlaybackRecorder&) = delete;
    PlaybackRecorder& operator= (const PlaybackRecorder&) = delete;

    /** host input: `count` interleaved f32 frames of `channels` channels at `rate` Hz (the system's output) */
    void push (const float* frames, size_t count, int channels, int rate);

    /** once per rendered frame: the frame's spectrum from the latest analysis, over the frame's dt */
    void update ();

    /** whether any audio input was ever given (nothing runs before that) */
    [[nodiscard]] bool active () const { return m_active; }

    enum Channel { Left = 0, Right = 1, Average = 2 };

    /** the current bands of `resolution` (16, 32 or 64) for `channel`, or nullptr */
    [[nodiscard]] const float* bands (int resolution, int channel) const;

    float audio16[3][16] = {};
    float audio32[3][32] = {};
    float audio64[3][64] = {};

private:
    void configure (int rate);
    void analyse ();
    void transform (const kiss_fft_cpx* in, kiss_fft_cpx* out);
    void releasePlans ();

    Context& m_context;

    // producer: the analysis WE's capture thread runs
    int m_rate = 0;
    int m_channels = 0;
    int m_size = 0; /** N, the FFT points */
    int m_hop = 0; /** fresh frames per analysis (2N/3) */
    int m_fill = 0;
    std::vector<kiss_fft_cpx> m_input[2];
    std::vector<kiss_fft_cpx> m_output;
    std::vector<int> m_bandOf;
    std::vector<float> m_weight;
    // a size with a factor above 5 runs as a Bluestein convolution of a power of two (as WE's FFT does)
    kiss_fft_cfg m_direct = nullptr;
    kiss_fft_cfg m_forward = nullptr;
    kiss_fft_cfg m_inverse = nullptr;
    int m_convSize = 0;
    std::vector<kiss_fft_cpx> m_chirp;
    std::vector<kiss_fft_cpx> m_kernel;
    std::vector<kiss_fft_cpx> m_work;
    std::vector<kiss_fft_cpx> m_work2;
    /** the latest analysis: 64 bands left, then 64 right (WE's 128-float hand-over buffer) */
    float m_raw[128] = {};
    float m_sinceInput = 0.0f;
    bool m_active = false;

    // consumer: the frame tick's state
    float m_envelope[16] = {};
    float m_lowPass[128] = {};
    float m_follow[128] = {};
};
} // namespace WallpaperEngine::Audio
