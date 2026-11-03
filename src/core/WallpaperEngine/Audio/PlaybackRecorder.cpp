#include "PlaybackRecorder.h"
#include "WallpaperEngine/Context.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// Every constant here is wallpaper64.exe 2.8.485's (see the header and the gap audit's "B8 result"):
// the capture object's defaults are set by its constructor GH 0x1400f61d0 (this = 0x1405275f8)
namespace {
// FFT points per 44.1 kHz: 64 · 30 (obj+0xec = 30.0, the band constant 64.0 at 0x1404d4474)
constexpr float kBands = 64.0f;
constexpr float kSizeFactor = 30.0f;
// bins analysed: 64 · 10 (obj+0xf0 = 10.0); the fresh share of the window is 1 − 10/30
constexpr float kBinFactor = 10.0f;
// the band mapping's exponent (obj+0xe4 = 0.25) and the bin weight's constant (obj+0xe8 = 0x3f004189)
constexpr float kExponent = 0.25f;
const float kWeight = [] {
    const unsigned bits = 0x3f004189u;
    float f;
    std::memcpy (&f, &bits, sizeof f);
    return f;
}();
// a sample s goes in as (127 + 127 s, 1 / (127 + 127 s)); the window's unfilled tail is (127, 1/127)
constexpr float kSampleScale = 127.0f;
// obj+0xc = audioinputvolume · 0.02 (WE's app setting, default 50: config.json "audioinputvolume" : 50)
constexpr float kInputVolume = 50.0f * 0.02f;
// no input for this long (WE: no WASAPI packet for > 1000 ms) zeroes the analysis
constexpr float kSilenceAfter = 1.0f;

bool smallFactors (int n) {
    for (const int p : { 2, 3, 5 }) {
	while (n % p == 0) {
	    n /= p;
	}
    }
    return n == 1;
}
} // namespace

namespace WallpaperEngine::Audio {
PlaybackRecorder::PlaybackRecorder (Context& context) : m_context (context) {}

PlaybackRecorder::~PlaybackRecorder () { this->releasePlans (); }

void PlaybackRecorder::releasePlans () {
    for (kiss_fft_cfg* cfg : { &m_direct, &m_forward, &m_inverse }) {
	if (*cfg != nullptr) {
	    kiss_fft_free (*cfg);
	    *cfg = nullptr;
	}
    }
}

void PlaybackRecorder::configure (int rate) {
    this->releasePlans ();
    this->m_rate = rate;
    // FUN_1401027f0: N = (int) (max (rate / 44100, 1) · 64 · 30), n = (int) (64 · 10)
    const float scale = std::max (static_cast<float> (rate) / 44100.0f, 1.0f);
    this->m_size = static_cast<int> (scale * kBands * kSizeFactor);
    const int bins = static_cast<int> (kBinFactor * kBands);
    // the thread's fill target: (int) (N − (10 / 30) · N), in float as WE computes it (1279 at 44.1 kHz)
    this->m_hop = static_cast<int> (
	static_cast<float> (this->m_size) - (kBinFactor / kSizeFactor) * static_cast<float> (this->m_size)
    );
    this->m_fill = 0;
    for (auto& in : this->m_input) {
	in.assign (this->m_size, { kSampleScale, 1.0f / kSampleScale });
    }
    this->m_output.assign (this->m_size, { 0.0f, 0.0f });

    // per bin 1..n-1: its band (pow mapping, never more than one band past the previous bin's) and weight
    this->m_bandOf.assign (bins, 0);
    this->m_weight.assign (bins, 0.0f);
    int band = 0;
    const float last = static_cast<float> (bins - 1);
    for (int i = 1; i < bins; i++) {
	const float x = static_cast<float> (i - 1) / last;
	int mapped = static_cast<int> (std::pow (x, kExponent) * kBands) & 63;
	band = std::min (band + 1, mapped);
	this->m_bandOf [i] = band;
	this->m_weight [i] = kWeight - std::cos (x * static_cast<float> (M_PI)) * (1.0f - kWeight);
    }

    if (smallFactors (this->m_size)) {
	this->m_direct = kiss_fft_alloc (this->m_size, 0, nullptr, nullptr);
	return;
    }

    // Bluestein: X_k = w_k · Σ (x_j w_j) conj (w_{k−j}), w_k = e^{−iπk²/N}, as a power-of-two convolution of
    // M = 2^bitlen (2N − 1) points (WE's FUN_14017a970 sizes it the same way)
    int m = 1;
    while (m < 2 * this->m_size - 1) {
	m <<= 1;
    }
    this->m_convSize = m;
    this->m_forward = kiss_fft_alloc (m, 0, nullptr, nullptr);
    this->m_inverse = kiss_fft_alloc (m, 1, nullptr, nullptr);
    this->m_chirp.resize (this->m_size);
    const long long twoN = 2LL * this->m_size;
    for (int k = 0; k < this->m_size; k++) {
	const long long kk = (static_cast<long long> (k) * k) % twoN;
	const double angle = M_PI * static_cast<double> (kk) / this->m_size;
	this->m_chirp [k] = { static_cast<float> (std::cos (angle)), static_cast<float> (-std::sin (angle)) };
    }
    std::vector<kiss_fft_cpx> b (m, { 0.0f, 0.0f });
    b [0] = { this->m_chirp [0].r, -this->m_chirp [0].i };
    for (int k = 1; k < this->m_size; k++) {
	b [k] = b [m - k] = { this->m_chirp [k].r, -this->m_chirp [k].i };
    }
    this->m_kernel.resize (m);
    kiss_fft (this->m_forward, b.data (), this->m_kernel.data ());
    const float inv = 1.0f / static_cast<float> (m);
    for (auto& c : this->m_kernel) {
	c.r *= inv;
	c.i *= inv;
    }
    this->m_work.resize (m);
    this->m_work2.resize (m);
}

void PlaybackRecorder::transform (const kiss_fft_cpx* in, kiss_fft_cpx* out) {
    if (this->m_direct != nullptr) {
	kiss_fft (this->m_direct, in, out);
	return;
    }
    const int n = this->m_size;
    const int m = this->m_convSize;
    for (int k = 0; k < n; k++) {
	const auto& w = this->m_chirp [k];
	this->m_work [k] = { in [k].r * w.r - in [k].i * w.i, in [k].r * w.i + in [k].i * w.r };
    }
    std::fill (this->m_work.begin () + n, this->m_work.end (), kiss_fft_cpx { 0.0f, 0.0f });
    kiss_fft (this->m_forward, this->m_work.data (), this->m_work2.data ());
    for (int k = 0; k < m; k++) {
	const auto a = this->m_work2 [k];
	const auto& b = this->m_kernel [k];
	this->m_work2 [k] = { a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r };
    }
    kiss_fft (this->m_inverse, this->m_work2.data (), this->m_work.data ());
    for (int k = 0; k < n; k++) {
	const auto& w = this->m_chirp [k];
	const auto& c = this->m_work [k];
	out [k] = { c.r * w.r - c.i * w.i, c.r * w.i + c.i * w.r };
    }
}

void PlaybackRecorder::push (const float* frames, size_t count, int channels, int rate) {
    if (frames == nullptr || count == 0 || channels <= 0 || rate <= 0) {
	return;
    }
    if (rate != this->m_rate) {
	this->configure (rate);
    }
    this->m_channels = channels;
    this->m_active = true;
    this->m_sinceInput = 0.0f;

    // a long backlog (the host stalled): only the newest two windows matter
    const size_t keep = static_cast<size_t> (this->m_hop) * 2;
    if (count > keep) {
	frames += (count - keep) * channels;
	count = keep;
    }

    for (size_t f = 0; f < count; f++) {
	const float* frame = frames + f * channels;
	for (int c = 0; c < std::min (channels, 2); c++) {
	    // WE's packet copy: re = s · 127 + 127, im = 1 / re (all.asm 0x140103b60..0x140103bcb)
	    const float v = frame [c] * kSampleScale + kSampleScale;
	    this->m_input [c][this->m_fill] = { v, 1.0f / v };
	}
	if (++this->m_fill >= this->m_hop) {
	    this->analyse ();
	    this->m_fill = 0;
	}
    }
}

void PlaybackRecorder::analyse () {
    const int channels = std::min (this->m_channels, 2);
    const int bins = static_cast<int> (this->m_bandOf.size ());
    float bands [2][64] = {};

    for (int c = 0; c < channels; c++) {
	auto& in = this->m_input [c];
	// the window's tail past the fresh frames keeps the buffer's initial value
	std::fill (in.begin () + this->m_hop, in.end (), kiss_fft_cpx { kSampleScale, 1.0f / kSampleScale });
	this->transform (in.data (), this->m_output.data ());
	for (int i = 1; i < bins; i++) {
	    const auto& x = this->m_output [i];
	    float power = x.r * x.r + x.i * x.i;
	    if (!std::isfinite (power)) {
		power = 0.0f;
	    }
	    const float magnitude = std::sqrt (std::max (0.0f, this->m_weight [i] * power));
	    float& band = bands [c][this->m_bandOf [i]];
	    band = std::max (band, magnitude);
	}
    }

    // × audioinputvolume · 0.02 · 0.001 · n / (N · 0.5)
    const float scale
	= kInputVolume * 0.001f * (static_cast<float> (bins) / (static_cast<float> (this->m_size) * 0.5f));
    for (int b = 0; b < 64; b++) {
	this->m_raw [b] = bands [0][b] * scale;
	// a mono device's right half is its left one
	this->m_raw [64 + b] = bands [channels > 1 ? 1 : 0][b] * scale;
    }
}

const float* PlaybackRecorder::bands (int resolution, int channel) const {
    if (channel < 0 || channel > 2) {
	return nullptr;
    }
    switch (resolution) {
	case 16:
	    return this->audio16 [channel];
	case 32:
	    return this->audio32 [channel];
	case 64:
	    return this->audio64 [channel];
	default:
	    return nullptr;
    }
}

void PlaybackRecorder::update () {
    // the host frontend's u8 mono mix (WP_AUDIO_INPUT_FRAME_SIZE samples, centre 128), taken as 44.1 kHz
    if (const auto* mix = this->m_context.audio_input_mix;
	mix != nullptr && mix->is_frame_ready != nullptr && mix->get_frame != nullptr
	&& mix->is_frame_ready (mix->user_parameter)) {
	const unsigned char* buffer = mix->get_frame (mix->user_parameter);
	if (buffer != nullptr) {
	    float samples [WP_AUDIO_INPUT_FRAME_SIZE];
	    for (int i = 0; i < WP_AUDIO_INPUT_FRAME_SIZE; i++) {
		samples [i] = (buffer [i] - 128) / 128.0f;
	    }
	    this->push (samples, WP_AUDIO_INPUT_FRAME_SIZE, 1, 44100);
	}
    }

    // a scene that never got audio costs nothing here
    if (!this->m_active) {
	return;
    }

    const float dt = std::max (0.0f, this->m_context.frameTime);
    this->m_sinceInput += dt;
    if (this->m_sinceInput > kSilenceAfter) {
	std::fill (std::begin (this->m_raw), std::end (this->m_raw), 0.0f);
    }

    // The frame tick (GH 0x140141520, f_141520.c 765-2000). The 128 bands are 16 groups of 8 (left 0-7,
    // right 8-15); a group's peak, floored at 0.333 of the loudest group's
    float peak [16];
    float loudest = 0.0f;
    for (int g = 0; g < 16; g++) {
	peak [g] = 0.0f;
	for (int i = 0; i < 8; i++) {
	    peak [g] = std::max (peak [g], this->m_raw [g * 8 + i]);
	}
	loudest = std::max (loudest, peak [g]);
    }
    const bool silent = loudest < 1e-4f;
    const float floor = loudest * 0.333f;
    // an envelope that sank to nothing restarts at 1 when sound comes back
    if (this->m_envelope [0] <= 1e-4f && !silent) {
	std::fill (std::begin (this->m_envelope), std::end (this->m_envelope), 1.0f);
    }
    // each group's envelope follows its peak at 1/s up, 0.5/s down (snapping within 1e-4)
    const float step = std::min (dt, 1.0f);
    float gain [16];
    for (int g = 0; g < 16; g++) {
	const float target = std::max (peak [g], floor);
	const float d = target - this->m_envelope [g];
	if (std::fabs (d) <= 1e-4f) {
	    this->m_envelope [g] = target;
	} else {
	    this->m_envelope [g] += std::min (std::fabs (d), step) * (d > 0.0f ? 1.0f : -0.5f);
	}
	gain [g] = 1.0f / std::max (this->m_envelope [g], 0.001f);
    }

    float out [128];
    if (silent) {
	std::fill (std::begin (out), std::end (out), 0.0f);
    } else {
	// a low-pass at 20/s, then a follow limited to ±40/s
	const float a = std::min (dt * 20.0f, 1.0f);
	const float rise = std::min (dt * 40.0f, 1.0f);
	const float fall = std::max (dt * -40.0f, -1.0f);
	for (int i = 0; i < 128; i++) {
	    this->m_lowPass [i] += (this->m_raw [i] * gain [i / 8] - this->m_lowPass [i]) * a;
	    const float d = this->m_lowPass [i] - this->m_follow [i];
	    this->m_follow [i] += d > 0.0f ? std::min (rise, d) : std::max (fall, d);
	    out [i] = this->m_follow [i];
	}
    }

    // left, right, their average; 32 and 16 bands are the max of pairs
    for (int b = 0; b < 64; b++) {
	this->audio64 [Left][b] = out [b];
	this->audio64 [Right][b] = out [64 + b];
	this->audio64 [Average][b] = (out [b] + out [64 + b]) * 0.5f;
    }
    for (int c = 0; c < 3; c++) {
	for (int b = 0; b < 32; b++) {
	    this->audio32 [c][b] = std::max (this->audio64 [c][2 * b], this->audio64 [c][2 * b + 1]);
	}
	for (int b = 0; b < 16; b++) {
	    this->audio16 [c][b] = std::max (this->audio32 [c][2 * b], this->audio32 [c][2 * b + 1]);
	}
    }
}
} // namespace WallpaperEngine::Audio
