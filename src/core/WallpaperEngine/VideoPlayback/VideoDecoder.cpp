#include "VideoDecoder.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <pthread.h>
#include <utility>

extern "C" {
#include <libavutil/pixdesc.h>
}

using namespace WallpaperEngine::VideoPlayback;

namespace {
/** decoded frames kept ahead of the player's clock: enough to cover a render that comes late, few enough that the
 * surfaces they hold stay a few MiB */
constexpr size_t kAhead = 3;
constexpr int kIoBuffer = 64 * 1024;

/**
 * The process's hardware devices, one per type, shared by every decoder: a scene's videos decode on one CUDA context
 * (the GPU's primary one: `primary_ctx`), not one each (each costs ~160 MiB of device memory on an RTX 3070 Ti).
 * Released with their last user, so a process without a video holds none. A type that failed to open isn't tried
 * again by the next video
 */
struct SharedDevice {
    AVBufferRef* ref = nullptr;
    int users = 0;
    bool failed = false;
};
std::mutex g_devicesMutex;
std::map<AVHWDeviceType, SharedDevice> g_devices;

AVBufferRef* acquireDevice (AVHWDeviceType type) {
    std::lock_guard lock (g_devicesMutex);
    SharedDevice& device = g_devices[type];
    if (device.ref == nullptr && !device.failed) {
	AVDictionary* options = nullptr;
	if (type == AV_HWDEVICE_TYPE_CUDA) {
	    av_dict_set (&options, "primary_ctx", "1", 0);
	}
	if (av_hwdevice_ctx_create (&device.ref, type, nullptr, options, 0) < 0) {
	    device.ref = nullptr;
	    device.failed = true;
	}
	av_dict_free (&options);
    }
    if (device.ref == nullptr) {
	return nullptr;
    }
    device.users++;
    return av_buffer_ref (device.ref);
}

void releaseDevice (AVHWDeviceType type) {
    std::lock_guard lock (g_devicesMutex);
    const auto it = g_devices.find (type);
    if (it == g_devices.end () || it->second.ref == nullptr) {
	return;
    }
    if (--it->second.users == 0) {
	av_buffer_unref (&it->second.ref);
	g_devices.erase (it);
    }
}

std::string error (int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror (code, buffer, sizeof buffer);
    return buffer;
}
} // namespace

HwdecChoice HwdecChoice::parse (const std::optional<std::string>& hwdec, const std::optional<std::string>& interop) {
    // the CUDA–GL interop is the one zero-copy path: allowed unless the host named another interop (or none)
    const bool cudaInterop = !interop.has_value () || *interop == "cuda" || *interop == "all" || *interop == "auto";
    HwdecChoice choice;
    auto add = [&choice] (AVHWDeviceType type, bool zeroCopy) {
	for (const auto& c : choice.candidates) {
	    if (c.type == type && c.zeroCopy == zeroCopy) {
		return;
	    }
	}
	choice.candidates.push_back ({ type, zeroCopy });
    };
    const std::string list = hwdec.value_or ("auto");
    size_t at = 0;
    while (at <= list.size ()) {
	const size_t comma = list.find (',', at);
	std::string name = list.substr (at, comma == std::string::npos ? std::string::npos : comma - at);
	at = comma == std::string::npos ? list.size () + 1 : comma + 1;
	name.erase (0, name.find_first_not_of (" \t"));
	name.erase (name.find_last_not_of (" \t") + 1);
	const bool copy = name.size () > 5 && name.ends_with ("-copy");
	const std::string base = copy ? name.substr (0, name.size () - 5) : name;
	if (name == "no") {
	    break;
	}
	if (base == "auto" || base == "auto-safe" || base == "auto-unsafe" || base == "yes" || name.starts_with ("auto-copy")) {
	    const bool anyCopy = copy || name.starts_with ("auto-copy");
	    add (AV_HWDEVICE_TYPE_CUDA, !anyCopy && cudaInterop);
	    add (AV_HWDEVICE_TYPE_VAAPI, false);
	    add (AV_HWDEVICE_TYPE_VULKAN, false);
	} else if (base == "nvdec" || base == "cuda") {
	    add (AV_HWDEVICE_TYPE_CUDA, !copy && cudaInterop);
	} else if (base == "vaapi") {
	    add (AV_HWDEVICE_TYPE_VAAPI, false);
	} else if (base == "vulkan") {
	    add (AV_HWDEVICE_TYPE_VULKAN, false);
	} else if (base == "vdpau") {
	    add (AV_HWDEVICE_TYPE_VDPAU, false);
	}
    }
    return choice;
}

VideoDecoder::VideoDecoder (const uint8_t* data, size_t size, HwdecChoice choice) :
    m_data (data), m_size (size), m_choice (std::move (choice)) {
    this->start ();
}

VideoDecoder::VideoDecoder (std::filesystem::path file, HwdecChoice choice, std::string formats) :
    m_file (std::move (file)), m_formats (std::move (formats)), m_choice (std::move (choice)) {
    this->start ();
}

void VideoDecoder::start () {
    this->m_thread = std::thread ([this] { this->run (); });
}

VideoDecoder::~VideoDecoder () {
    {
	std::lock_guard lock (this->m_mutex);
	this->m_stop = true;
    }
    this->m_wake.notify_all ();
    if (this->m_thread.joinable ()) {
	this->m_thread.join ();
    }
    for (auto& [frame, time] : this->m_frames) {
	av_frame_free (&frame);
    }
    av_frame_free (&this->m_lastDiscarded);
    av_packet_free (&this->m_packet);
    avcodec_free_context (&this->m_codec);
    avformat_close_input (&this->m_format);
    if (this->m_io != nullptr) {
	av_freep (&this->m_io->buffer);
	avio_context_free (&this->m_io);
    }
    if (this->m_device != nullptr) {
	av_buffer_unref (&this->m_device);
	releaseDevice (this->m_deviceType);
    }
}

void VideoDecoder::say (std::string message) {
    std::lock_guard lock (this->m_mutex);
    this->m_messages.push_back (std::move (message));
}

std::vector<std::string> VideoDecoder::messages () {
    std::lock_guard lock (this->m_mutex);
    return std::exchange (this->m_messages, {});
}

AVPixelFormat VideoDecoder::pickFormat (AVCodecContext* ctx, const AVPixelFormat* formats) {
    const auto* self = static_cast<const VideoDecoder*> (ctx->opaque);
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++) {
	if (*f == self->m_hwFormat) {
	    return *f;
	}
    }
    // the hardware can't take this stream (its profile, its size): software, on the same decoder
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++) {
	const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get (*f);
	if (desc != nullptr && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
	    return *f;
	}
    }
    return AV_PIX_FMT_NONE;
}

bool VideoDecoder::open () {
    const auto started = std::chrono::steady_clock::now ();
    this->m_format = avformat_alloc_context ();
    if (this->m_format == nullptr) {
	return false;
    }
    if (this->m_data != nullptr) {
	auto* buffer = static_cast<unsigned char*> (av_malloc (kIoBuffer));
	this->m_io = avio_alloc_context (
	    buffer, kIoBuffer, 0, this,
	    [] (void* opaque, uint8_t* out, int wanted) -> int {
		auto* self = static_cast<VideoDecoder*> (opaque);
		const size_t left = self->m_size - self->m_position;
		if (left == 0) {
		    return AVERROR_EOF;
		}
		const size_t n = std::min (left, static_cast<size_t> (wanted));
		memcpy (out, self->m_data + self->m_position, n);
		self->m_position += n;
		return static_cast<int> (n);
	    },
	    nullptr,
	    [] (void* opaque, int64_t offset, int whence) -> int64_t {
		auto* self = static_cast<VideoDecoder*> (opaque);
		if (whence == AVSEEK_SIZE) {
		    return static_cast<int64_t> (self->m_size);
		}
		int64_t to = offset;
		switch (whence & ~AVSEEK_FORCE) {
		    case SEEK_SET: break;
		    case SEEK_CUR: to += static_cast<int64_t> (self->m_position); break;
		    case SEEK_END: to += static_cast<int64_t> (self->m_size); break;
		    default: return -1;
		}
		if (to < 0 || to > static_cast<int64_t> (self->m_size)) {
		    return -1;
		}
		self->m_position = static_cast<size_t> (to);
		return to;
	    }
	);
	if (buffer == nullptr || this->m_io == nullptr) {
	    return false;
	}
	this->m_format->pb = this->m_io;
	this->m_format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    // FFmpeg picks a demuxer by the bytes, whatever the name: a host's file of someone else's opens only as what it
    // claims to be, never as a playlist that reads other files
    AVDictionary* options = nullptr;
    if (this->m_data == nullptr && !this->m_formats.empty ()) {
	av_dict_set (&options, "format_whitelist", this->m_formats.c_str (), 0);
	av_dict_set (&options, "protocol_whitelist", "file", 0);
    }
    const int opened =
	avformat_open_input (&this->m_format, this->m_data ? nullptr : this->m_file.c_str (), nullptr, &options);
    av_dict_free (&options);
    if (opened < 0) {
	this->say ("video: cannot open the video: " + error (opened));
	return false;
    }
    this->m_stream = av_find_best_stream (this->m_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    // an MP4's header names the codec and size; probing (which decodes frames) only when it doesn't
    if (this->m_stream < 0 || this->m_format->streams[this->m_stream]->codecpar->width <= 0) {
	avformat_find_stream_info (this->m_format, nullptr);
	this->m_stream = av_find_best_stream (this->m_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    if (this->m_stream < 0) {
	this->say ("video: no video stream");
	return false;
    }
    for (unsigned i = 0; i < this->m_format->nb_streams; i++) {
	if (static_cast<int> (i) != this->m_stream) {
	    this->m_format->streams[i]->discard = AVDISCARD_ALL;
	}
    }
    const AVStream* stream = this->m_format->streams[this->m_stream];
    this->m_timeBase = stream->time_base;
    this->m_startPts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    const AVRational rate = av_guess_frame_rate (this->m_format, const_cast<AVStream*> (stream), nullptr);
    if (rate.num > 0 && rate.den > 0) {
	this->m_frameDuration = av_q2d (av_inv_q (rate));
    }
    {
	std::lock_guard lock (this->m_mutex);
	if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
	    this->m_containerDuration = static_cast<double> (stream->duration) * av_q2d (this->m_timeBase);
	} else if (this->m_format->duration != AV_NOPTS_VALUE && this->m_format->duration > 0) {
	    this->m_containerDuration = static_cast<double> (this->m_format->duration) / AV_TIME_BASE;
	}
    }

    for (const auto& candidate : this->m_choice.candidates) {
	if (this->openCodec (&candidate)) {
	    break;
	}
    }
    if (this->m_codec == nullptr && !this->openCodec (nullptr)) {
	return false;
    }
    const char* name = avcodec_get_name (stream->codecpar->codec_id);
    const auto ms = [] (std::chrono::steady_clock::duration d) {
	return std::to_string (std::chrono::duration_cast<std::chrono::milliseconds> (d).count ());
    };
    this->say (
	std::string ("video: ") + name + " " + std::to_string (stream->codecpar->width) + "x"
	+ std::to_string (stream->codecpar->height) + " decodes on "
	+ (this->m_device ? av_hwdevice_get_type_name (this->m_deviceType) : "the CPU")
	+ (this->m_device ? (this->m_transfer ? " (frames copied to memory)" : " (frames to GL through the interop)") : "")
	+ ", open in " + ms (std::chrono::steady_clock::now () - started) + " ms (its device " + ms (this->m_deviceTime)
	+ " ms)"
    );
    this->m_packet = av_packet_alloc ();
    return this->m_packet != nullptr;
}

bool VideoDecoder::openCodec (const HwdecChoice::Candidate* candidate) {
    const AVCodecParameters* parameters = this->m_format->streams[this->m_stream]->codecpar;
    const AVCodec* codec = nullptr;
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    if (candidate == nullptr) {
	codec = avcodec_find_decoder (parameters->codec_id);
	if (codec == nullptr) {
	    this->say (std::string ("video: no decoder for ") + avcodec_get_name (parameters->codec_id));
	    return false;
	}
    } else {
	// the first decoder of the codec that can decode on this hardware: the preferred one may have none (libdav1d is
	// AV1's, FFmpeg's own `av1` has the hwaccels)
	void* at = nullptr;
	while (hwFormat == AV_PIX_FMT_NONE && (codec = av_codec_iterate (&at)) != nullptr) {
	    if (codec->id != parameters->codec_id || !av_codec_is_decoder (codec)) {
		continue;
	    }
	    for (int i = 0;; i++) {
		const AVCodecHWConfig* config = avcodec_get_hw_config (codec, i);
		if (config == nullptr) {
		    break;
		}
		if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == candidate->type) {
		    hwFormat = config->pix_fmt;
		    break;
		}
	    }
	}
	// no decoder of it has such hardware: the device isn't even opened
	if (hwFormat == AV_PIX_FMT_NONE) {
	    return false;
	}
    }
    AVCodecContext* ctx = avcodec_alloc_context3 (codec);
    if (ctx == nullptr || avcodec_parameters_to_context (ctx, parameters) < 0) {
	avcodec_free_context (&ctx);
	return false;
    }
    ctx->pkt_timebase = this->m_timeBase;
    AVBufferRef* device = nullptr;
    if (candidate != nullptr) {
	const auto asked = std::chrono::steady_clock::now ();
	device = acquireDevice (candidate->type);
	this->m_deviceTime += std::chrono::steady_clock::now () - asked;
	if (device == nullptr) {
	    avcodec_free_context (&ctx);
	    return false;
	}
	ctx->hw_device_ctx = av_buffer_ref (device);
	ctx->opaque = this;
	ctx->get_format = &VideoDecoder::pickFormat;
	this->m_hwFormat = hwFormat;
	// the hardware does the work: frame threads would only add frames of delay and surfaces
	ctx->thread_count = 1;
    } else {
	ctx->thread_count = 0;
    }
    if (const int rc = avcodec_open2 (ctx, codec, nullptr); rc < 0) {
	this->say (std::string ("video: ") + codec->name + " won't open"
		   + (candidate ? std::string (" on ") + av_hwdevice_get_type_name (candidate->type) : "") + ": " + error (rc));
	avcodec_free_context (&ctx);
	if (device != nullptr) {
	    av_buffer_unref (&device);
	    releaseDevice (candidate->type);
	}
	this->m_hwFormat = AV_PIX_FMT_NONE;
	return false;
    }
    this->m_codec = ctx;
    if (candidate != nullptr) {
	this->m_device = device;
	this->m_deviceType = candidate->type;
	this->m_transfer = !(candidate->zeroCopy && candidate->type == AV_HWDEVICE_TYPE_CUDA);
    }
    return true;
}

void VideoDecoder::run () {
    pthread_setname_np (pthread_self (), "wp-video");
    if (!this->open ()) {
	std::lock_guard lock (this->m_mutex);
	this->m_failed = true;
	this->m_firstReady.notify_all ();
	return;
    }
    int framesThisLoop = 0;
    while (true) {
	{
	    std::unique_lock lock (this->m_mutex);
	    this->m_wake.wait (lock, [this] {
		if (this->m_stop || this->m_seekTo.has_value ()) {
		    return true;
		}
		if (this->m_eof) {
		    return false;
		}
		// frames still to come on the player's clock (a due one counts once: a newer one replaces it)
		size_t ahead = 0;
		for (const auto& [frame, time] : this->m_frames) {
		    ahead += time > this->m_wanted ? 1 : 0;
		}
		return ahead < kAhead;
	    });
	    if (this->m_stop) {
		return;
	    }
	    if (this->m_seekTo.has_value ()) {
		const double to = *this->m_seekTo;
		this->m_seekTo.reset ();
		for (auto& [frame, time] : this->m_frames) {
		    av_frame_free (&frame);
		}
		this->m_frames.clear ();
		this->m_eof = false;
		this->m_newest = to;
		lock.unlock ();
		this->doSeek (to);
		framesThisLoop = 0;
		continue;
	    }
	}
	const int before = this->m_framesMade;
	if (this->step ()) {
	    framesThisLoop += this->m_framesMade - before;
	    continue;
	}
	framesThisLoop += this->m_framesMade - before;
	// the end of the input: every frame of this pass is out
	if (framesThisLoop == 0) {
	    this->say ("video: no frame decodes");
	    std::lock_guard lock (this->m_mutex);
	    this->m_failed = true;
	    this->m_eof = true;
	    this->m_firstReady.notify_all ();
	    continue;
	}
	bool loop;
	{
	    std::lock_guard lock (this->m_mutex);
	    loop = this->m_loop;
	    if (this->m_frameLength <= 0.0 && this->m_lastTime >= 0.0) {
		this->m_frameLength = this->m_lastTime - this->m_loopOffset + this->m_frameDuration;
	    }
	    if (!loop) {
		this->m_eof = true;
	    }
	}
	if (loop) {
	    // the next pass's frames follow this one's last at the video's length: no gap, no repeat
	    this->m_loopOffset += this->m_frameLength;
	    av_seek_frame (this->m_format, this->m_stream, this->m_startPts, AVSEEK_FLAG_BACKWARD);
	    avcodec_flush_buffers (this->m_codec);
	    framesThisLoop = 0;
	}
    }
}

bool VideoDecoder::step () {
    AVFrame* frame = av_frame_alloc ();
    bool queued = false;
    bool ended = false;
    while (!queued && !ended) {
	const int received = avcodec_receive_frame (this->m_codec, frame);
	if (received == 0) {
	    queued = this->deliver (frame);
	    av_frame_unref (frame);
	    continue;
	}
	if (received == AVERROR_EOF) {
	    ended = true;
	    break;
	}
	if (received != AVERROR (EAGAIN)) {
	    // a frame that failed to decode: the next packet may still
	    continue;
	}
	if (!this->m_packetPending) {
	    const int read = av_read_frame (this->m_format, this->m_packet);
	    if (read < 0) {
		// the end of the input: drain what the decoder holds
		avcodec_send_packet (this->m_codec, nullptr);
		continue;
	    }
	    if (this->m_packet->stream_index != this->m_stream) {
		av_packet_unref (this->m_packet);
		continue;
	    }
	}
	const int sent = avcodec_send_packet (this->m_codec, this->m_packet);
	this->m_packetPending = sent == AVERROR (EAGAIN);
	if (!this->m_packetPending) {
	    av_packet_unref (this->m_packet);
	}
	{
	    std::lock_guard lock (this->m_mutex);
	    if (this->m_stop || this->m_seekTo.has_value ()) {
		break;
	    }
	}
    }
    av_frame_free (&frame);
    if (ended && this->m_discardBefore.has_value () && this->m_lastDiscarded != nullptr) {
	// a seek past the last frame shows the last frame
	this->m_discardBefore.reset ();
	AVFrame* last = std::exchange (this->m_lastDiscarded, nullptr);
	this->deliver (last);
	av_frame_free (&last);
    }
    return !ended;
}

bool VideoDecoder::deliver (AVFrame* frame) {
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
	pts = frame->pts;
    }
    double time = pts == AV_NOPTS_VALUE
		    ? this->m_lastTime + this->m_frameDuration
		    : static_cast<double> (pts - this->m_startPts) * av_q2d (this->m_timeBase) + this->m_loopOffset;
    if (this->m_discardBefore.has_value () && time + this->m_frameDuration / 2 < *this->m_discardBefore) {
	// before a seek's target: decoded for the frames that follow, kept only in case nothing does
	if (this->m_lastDiscarded == nullptr) {
	    this->m_lastDiscarded = av_frame_alloc ();
	}
	av_frame_unref (this->m_lastDiscarded);
	av_frame_ref (this->m_lastDiscarded, frame);
	this->m_lastTime = time;
	return false;
    }
    this->m_discardBefore.reset ();
    av_frame_free (&this->m_lastDiscarded);

    AVFrame* out = av_frame_alloc ();
    if (frame->format == this->m_hwFormat && this->m_transfer.load ()) {
	if (const int rc = av_hwframe_transfer_data (out, frame, 0); rc < 0) {
	    this->say ("video: a frame won't copy from the GPU: " + error (rc));
	    av_frame_free (&out);
	    return false;
	}
	av_frame_copy_props (out, frame);
    } else {
	av_frame_ref (out, frame);
    }
    this->m_lastTime = time;
    this->m_framesMade++;
    std::lock_guard lock (this->m_mutex);
    // a frame already due replaces the due one before it: only the newest due frame is ever shown
    if (!this->m_frames.empty () && this->m_frames.back ().second <= this->m_wanted && time <= this->m_wanted) {
	av_frame_free (&this->m_frames.back ().first);
	this->m_frames.pop_back ();
    }
    this->m_frames.emplace_back (out, time);
    this->m_newest = time;
    if (!this->m_gotFirst) {
	this->m_gotFirst = true;
	this->m_firstReady.notify_all ();
    }
    return true;
}

void VideoDecoder::doSeek (double seconds) {
    const auto ts = this->m_startPts + static_cast<int64_t> (seconds / av_q2d (this->m_timeBase));
    av_seek_frame (this->m_format, this->m_stream, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers (this->m_codec);
    if (this->m_packetPending) {
	av_packet_unref (this->m_packet);
	this->m_packetPending = false;
    }
    this->m_loopOffset = 0.0;
    this->m_lastTime = seconds - this->m_frameDuration;
    this->m_discardBefore = seconds;
}

AVFrame* VideoDecoder::take (double time, bool first, double* frameTime) {
    std::lock_guard lock (this->m_mutex);
    this->m_wanted = time;
    AVFrame* best = nullptr;
    while (!this->m_frames.empty () && (this->m_frames.front ().second <= time + 1e-6 || (first && best == nullptr))) {
	av_frame_free (&best);
	best = this->m_frames.front ().first;
	*frameTime = this->m_frames.front ().second;
	this->m_frames.pop_front ();
    }
    this->m_wake.notify_one ();
    return best;
}

bool VideoDecoder::waitFirst (std::chrono::milliseconds bound) {
    std::unique_lock lock (this->m_mutex);
    this->m_firstReady.wait_for (lock, bound, [this] { return this->m_gotFirst || this->m_failed.load (); });
    return this->m_gotFirst;
}

void VideoDecoder::seek (double seconds) {
    {
	std::lock_guard lock (this->m_mutex);
	this->m_seekTo = seconds;
	this->m_wanted = seconds;
	this->m_newest = seconds;
    }
    this->m_wake.notify_all ();
}

void VideoDecoder::setLoop (bool loop) {
    std::lock_guard lock (this->m_mutex);
    this->m_loop = loop;
}

void VideoDecoder::setTransfer () { this->m_transfer = true; }

double VideoDecoder::duration () const {
    std::lock_guard lock (this->m_mutex);
    return this->m_frameLength > 0.0 ? this->m_frameLength : this->m_containerDuration;
}

bool VideoDecoder::ended () const {
    std::lock_guard lock (this->m_mutex);
    return this->m_eof && this->m_frames.empty ();
}

double VideoDecoder::newest () const {
    std::lock_guard lock (this->m_mutex);
    return this->m_newest;
}
