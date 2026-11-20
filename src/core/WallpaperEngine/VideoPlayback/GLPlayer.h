#pragma once

#include "VideoDecoder.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <glad/glad.h>
#include <memory>
#include <optional>
#include <string>

struct SwsContext;

namespace WallpaperEngine::VideoPlayback {
/**
 * A video drawn into a GL texture, decoded by FFmpeg (VideoDecoder) on the host's hardware and played on the scene's
 * clock: the context's render time, so it follows the host's speed and pauses and starts where the scene first draws
 * it. Silent: a video texture's sound is never decoded. Each new frame is converted from its YUV planes into the
 * texture by one draw; a CUDA frame reaches those planes through the CUDA–GL interop without leaving the GPU.
 */
class GLPlayer : public Helpers::ContextAware {
public:
    /** plays `file` */
    GLPlayer (
	RenderContext& context, GLuint outputTexture, const std::filesystem::path& file, int64_t baseWidth,
	int64_t baseHeight, GLuint fbo = GL_NONE
    );
    /** plays the `size` bytes at `data`, which the caller keeps alive for the player's life */
    GLPlayer (
	RenderContext& context, GLuint outputTexture, const uint8_t* data, size_t size, int64_t baseWidth,
	int64_t baseHeight, GLuint fbo = GL_NONE
    );
    ~GLPlayer () override;

    /** playback runs while at least one thing uses the player (decoding starts at the first) */
    void incrementUsageCount ();
    /** stops and frees the decoder when nothing uses the player any more */
    void decrementUsageCount ();
    /**
     * Starts decoding before anything uses the player, so its first frame is ready when it first draws: a scene's
     * video textures start as its build creates them, well before the layers that draw them are set up
     */
    void prefetch ();

    void setPaused ();
    void clearPaused ();

    /** advances the clock to the context's render time and draws the frame due then, if it's a new one */
    void render ();

    // SceneScript's IVideoTexture (a layer's video): the player's state and controls. Before decoding starts they keep
    // the state it starts with
    [[nodiscard]] bool isPaused () const { return this->m_paused; }
    /** the video's length in seconds (0 until the decoder knows it) */
    [[nodiscard]] double getDuration () const;
    /** where the video is, in seconds */
    [[nodiscard]] double getTime () const;
    void seek (double seconds);
    [[nodiscard]] double getRate () const { return this->m_rate; }
    void setRate (double rate);
    [[nodiscard]] bool getLoop () const { return this->m_loop; }
    void setLoop (bool loop);
    /** a video that doesn't loop showed its last frame (cleared by a seek) */
    [[nodiscard]] bool reachedEnd () const;

    int getWidth () const;
    int getHeight () const;

private:
    /** how a frame's pixel format maps onto plane textures and the conversion's inputs */
    struct Layout {
	AVPixelFormat format = AV_PIX_FMT_NONE;
	int width = 0;
	int height = 0;
	int planes = 0;
	struct Plane {
	    GLint internal;
	    GLenum format;
	    GLenum type;
	    int width;
	    int height;
	    int bytesPerPixel;
	};
	std::array<Plane, 4> plane {};
	/** the plane and channel of Y, Cb, Cr and alpha (plane -1: none) */
	std::array<int, 4> compPlane { -1, -1, -1, -1 };
	std::array<int, 4> compChannel {};
	/** RGBA from swscale in plane 0 (a format the planes can't take) */
	bool rgba = false;
	/** a sample's stored value / its full scale, from the texture's normalised value */
	float scale = 1.0f;
	int depth = 8;
	int log2ChromaW = 0;
	int log2ChromaH = 0;

	bool operator== (const Layout& o) const {
	    return format == o.format && width == o.width && height == o.height;
	}
    };

    void prepareGL ();
    void play ();
    void startDecoder ();
    void stop ();
    /** shows `frame` (any format), true when it reached the texture */
    bool show (AVFrame* frame);
    bool layoutFor (AVPixelFormat format, int width, int height, Layout& layout) const;
    /** (re)allocates the plane textures for `layout` (and their CUDA registrations when `cuda`) */
    bool preparePlanes (const Layout& layout, bool cuda);
    bool copyCuda (AVFrame* frame);
    bool upload (AVFrame* frame);
    void convert (const AVFrame* frame);
    void releaseCuda ();
    void sizeOutput (int width, int height);

    bool m_doWeOwnFramebuffer;
    GLuint m_outputTexture;
    GLuint m_fbo = GL_NONE;
    int64_t m_width;
    int64_t m_height;

    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    std::optional<std::filesystem::path> m_file;
    std::unique_ptr<VideoDecoder> m_decoder;
    uint32_t m_usageCount = 0;

    bool m_paused = false;
    double m_rate = 1.0;
    bool m_loop = true;
    /** the video's clock (seconds, running on across loops), its last render time, a seek before decoding started */
    double m_clock = 0.0;
    std::optional<double> m_lastRenderTime;
    std::optional<double> m_pendingSeek;
    bool m_shownAny = false;
    bool m_waitedFirst = false;
    int m_rendersWithout = 0;
    std::chrono::steady_clock::time_point m_playedAt;
    std::optional<std::chrono::steady_clock::time_point> m_firstRender;

    Layout m_layout;
    std::array<GLuint, 4> m_planes {};
    bool m_planesForCuda = false;
    std::array<void*, 4> m_cudaResources {};
    bool m_cudaFailed = false;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    SwsContext* m_sws = nullptr;
    std::vector<uint8_t> m_rgba;
};

using GLPlayerUniquePtr = std::unique_ptr<GLPlayer>;
using GLPlayerSharedPtr = std::shared_ptr<GLPlayer>;
} // namespace WallpaperEngine::VideoPlayback
