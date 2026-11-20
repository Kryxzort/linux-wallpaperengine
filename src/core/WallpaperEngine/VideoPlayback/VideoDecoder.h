#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

namespace WallpaperEngine::VideoPlayback {
/**
 * The hardware a video may decode on, in the host's order (wp_context_set_video_hwdec takes mpv's `hwdec` names and
 * `gpu-hwdec-interop`): `nvdec`/`cuda` decode on CUDA and hand frames to GL through the CUDA–GL interop when the
 * interop is `cuda` (or unset, or `all`); `nvdec-copy`, `vaapi[-copy]`, `vulkan[-copy]`, `vdpau[-copy]` decode there and
 * copy each frame to memory; `auto` is CUDA, VAAPI then Vulkan; `no` (or a list that runs out) is software.
 */
struct HwdecChoice {
    struct Candidate {
	AVHWDeviceType type;
	bool zeroCopy;
    };
    std::vector<Candidate> candidates;

    static HwdecChoice parse (const std::optional<std::string>& hwdec, const std::optional<std::string>& interop);
};

/**
 * One video's demuxer and decoder on a thread of its own ("wp-video"), a few frames ahead of the player: every frame
 * carries its time on the video's own clock, which runs on across loops (a loop's frames follow the last one's at
 * the video's length, so looping is gapless). The player takes the newest frame due on its clock; a seek restarts the
 * clock at the target. Every hardware device is shared by the process's decoders (HwDevices): one CUDA device on the
 * GPU's primary context, whatever the number of videos.
 */
class VideoDecoder {
public:
    /** decodes `size` bytes at `data` (kept alive by the caller for the decoder's life) or `file` */
    VideoDecoder (const uint8_t* data, size_t size, HwdecChoice choice);
    VideoDecoder (std::filesystem::path file, HwdecChoice choice);
    ~VideoDecoder ();

    VideoDecoder (const VideoDecoder&) = delete;
    VideoDecoder& operator= (const VideoDecoder&) = delete;

    /**
     * The newest decoded frame due at `time` (the caller owns it; the older ones are freed), or, while `first`, the
     * first frame whatever its time. Null when none is due. `frameTime` gets its time
     */
    AVFrame* take (double time, bool first, double* frameTime);
    /** waits up to `bound` for the first frame (or a failure); true when one is there */
    bool waitFirst (std::chrono::milliseconds bound);
    /** restarts decoding at `seconds` (the clock's new time) */
    void seek (double seconds);
    void setLoop (bool loop);
    /** hardware frames reach the player copied to memory from now on (the interop failed) */
    void setTransfer ();

    /** the video's length in seconds: the container's, then the frames' own once a loop ended; 0 until known */
    [[nodiscard]] double duration () const;
    /** a video that doesn't loop decoded its last frame and the player took it */
    [[nodiscard]] bool ended () const;
    /** the thread gave up (nothing can open or decode the video) */
    [[nodiscard]] bool failed () const { return this->m_failed.load (); }
    /** the newest frame the thread has made, in time on the video's clock (seek targets count); -1 before any */
    [[nodiscard]] double newest () const;
    /** the decoder's hardware device (to copy CUDA frames with), null when software */
    [[nodiscard]] AVBufferRef* device () const { return this->m_device; }
    /** what the thread had to report (opened with what, errors), for the caller's log: the log isn't thread-safe */
    std::vector<std::string> messages ();

private:
    void start ();
    void run ();
    bool open ();
    bool openCodec (const HwdecChoice::Candidate* candidate);
    /** decodes until a frame is queued, the end, a stop or a seek; false at the end of the input */
    bool step ();
    /** queues a decoded frame (false: discarded, before a seek's target) */
    bool deliver (AVFrame* frame);
    void doSeek (double seconds);
    void say (std::string message);
    static AVPixelFormat pickFormat (AVCodecContext* ctx, const AVPixelFormat* formats);

    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_position = 0;
    std::filesystem::path m_file;
    HwdecChoice m_choice;

    AVFormatContext* m_format = nullptr;
    AVIOContext* m_io = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVPacket* m_packet = nullptr;
    int m_stream = -1;
    /** the device in use (shared: HwDevices) and its type, AV_HWDEVICE_TYPE_NONE for software */
    AVBufferRef* m_device = nullptr;
    AVHWDeviceType m_deviceType = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat m_hwFormat = AV_PIX_FMT_NONE;
    std::atomic<bool> m_transfer = false;
    /** the stream's first timestamp and time base: frame times count from it */
    int64_t m_startPts = 0;
    AVRational m_timeBase { 1, 1 };
    double m_frameDuration = 1.0 / 30.0;

    /** the decode thread's own state: the clock offset of this loop, a seek's target, the last frame's time */
    double m_loopOffset = 0.0;
    std::optional<double> m_discardBefore;
    double m_lastTime = -1.0;
    AVFrame* m_lastDiscarded = nullptr;
    bool m_packetPending = false;
    /** time spent opening (or waiting for another decoder to open) hardware devices, for the open's log line */
    std::chrono::steady_clock::duration m_deviceTime {};
    int m_framesMade = 0;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_firstReady;
    std::deque<std::pair<AVFrame*, double>> m_frames;
    std::optional<double> m_seekTo;
    /** the player's clock at its last take: frames up to it are due */
    double m_wanted = 0.0;
    bool m_stop = false;
    bool m_loop = true;
    bool m_eof = false;
    bool m_gotFirst = false;
    double m_newest = -1.0;
    double m_containerDuration = 0.0;
    double m_frameLength = 0.0;
    std::vector<std::string> m_messages;
    std::atomic<bool> m_failed = false;
    std::thread m_thread;
};
} // namespace WallpaperEngine::VideoPlayback
