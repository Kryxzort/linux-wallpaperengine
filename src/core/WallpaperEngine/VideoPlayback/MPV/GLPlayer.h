#pragma once

#include "MemoryStreamProtocol.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include <filesystem>
#include <glad/glad.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <optional>
#include <string>

namespace WallpaperEngine::VideoPlayback::MPV {
class GLPlayer : public Helpers::ContextAware {
    friend struct MemoryStreamProtocol;

public:
    GLPlayer (
	RenderContext& context, GLuint outputTexture, const std::filesystem::path& file, int64_t baseWidth,
	int64_t baseHeight, GLuint fbo = GL_NONE
    );
    GLPlayer (
	RenderContext& context, GLuint outputTexture, MemoryStreamProtocolUniquePtr stream, int64_t baseWidth,
	int64_t baseHeight, GLuint fbo = GL_NONE
    );
    ~GLPlayer () override;

    /**
     * Increments the usage count of the player
     *
     * Directly controls playback, only started when at least one thing is using it
     * Initializes mpv if needed and starts playback
     */
    void incrementUsageCount ();
    /**
     * Decrements the usage count of the player
     *
     * Directly controls playback, only stopped when nothing is using it
     * De-initializes mpv if needed
     */
    void decrementUsageCount ();

    void setUntimed ();
    void clearUntimed ();
    void setMuted ();
    void clearMuted ();
    void setVolume (double volume);
    void setPaused ();
    void clearPaused ();

    void render () const;

    // SceneScript's IVideoTexture (a layer's video): the player's state and controls. Before mpv runs (nothing
    // draws the video yet) they keep the state it starts with
    [[nodiscard]] bool isPaused () const { return this->m_paused; }
    /** the video's length in seconds (0 until mpv knows it) */
    [[nodiscard]] double getDuration () const;
    /** where the video is, in seconds */
    [[nodiscard]] double getTime () const;
    void seek (double seconds);
    [[nodiscard]] double getRate () const { return this->m_rate; }
    void setRate (double rate);
    [[nodiscard]] bool getLoop () const { return this->m_loop; }
    void setLoop (bool loop);
    /** mpv reached the end of a video that doesn't loop (cleared by a seek) */
    [[nodiscard]] bool reachedEnd () const;

    int getWidth () const;
    int getHeight () const;

private:
    void prepareGL ();
    void init ();
    void play ();
    void setSource (MemoryStreamProtocolUniquePtr source);
    void setSource (const std::filesystem::path& file);
    void stop ();

protected:
    bool m_doWeOwnFramebuffer;
    GLuint m_outputTexture;
    GLuint m_fbo = GL_NONE;
    mutable int64_t m_width;
    mutable int64_t m_height;
    /** mpv's display size as its property-change events report it (render () reads no property: see there) */
    mutable int64_t m_videoWidth = 0;
    mutable int64_t m_videoHeight = 0;
    mpv_handle* m_handle = nullptr;
    mpv_render_context* m_renderContext = nullptr;
    double m_volume = 0.0f;
    bool m_muted = false;
    bool m_untimed = false;
    bool m_paused = false;
    double m_rate = 1.0;
    bool m_loop = true;
    /** a seek asked for before mpv ran */
    std::optional<double> m_pendingSeek = std::nullopt;
    /** a seek issued this frame: getTime reports it until mpv does (the next render) */
    std::optional<double> m_seekTarget = std::nullopt;
    std::optional<std::filesystem::path> m_file;
    std::optional<MemoryStreamProtocolUniquePtr> m_stream;
    uint32_t m_usageCount = 0;
};

using GLPlayerUniquePtr = std::unique_ptr<GLPlayer>;
using GLPlayerSharedPtr = std::shared_ptr<GLPlayer>;
};
