#include <algorithm>
#include <string>
#include "GLPlayer.h"

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Logging/Log.h"

#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>

using namespace WallpaperEngine::VideoPlayback::MPV;

namespace {
// mpv_observe_property's reply_userdata for the display size
constexpr uint64_t kObserveWidth = 1;
constexpr uint64_t kObserveHeight = 2;
} // namespace

void* get_proc_address (void* ctx, const char* name) {
    const auto gl_proc_address = static_cast<GLPlayer*> (ctx)->getContext ().getContext ().gl_proc_address;

    return gl_proc_address->get_proc_address (gl_proc_address->user_parameter, name);
}

GLPlayer::GLPlayer (
    RenderContext& context, GLuint outputTexture, const std::filesystem::path& file, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_outputTexture (outputTexture), m_width (baseWidth), m_height (baseHeight) {
    this->m_fbo = fbo;
    this->m_doWeOwnFramebuffer = this->m_fbo == GL_NONE;

    this->prepareGL ();
    this->setSource (file);
}

GLPlayer::GLPlayer (
    RenderContext& context, const GLuint outputTexture, MemoryStreamProtocolUniquePtr stream, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_outputTexture (outputTexture), m_width (baseWidth), m_height (baseHeight) {
    this->m_fbo = fbo;
    this->m_doWeOwnFramebuffer = this->m_fbo == GL_NONE;

    this->prepareGL ();
    this->setSource (std::move (stream));
}

GLPlayer::~GLPlayer () {
    // clean up any kept resources
    this->stop ();

    // only clean up framebuffer if we own it
    if (this->m_doWeOwnFramebuffer) {
	// free gl resources too
	glDeleteFramebuffers (1, &this->m_fbo);
    }
}

void GLPlayer::incrementUsageCount () {
    this->m_usageCount++;

    if (this->m_usageCount == 1) {
	this->play ();
    }
}

void GLPlayer::decrementUsageCount () {
    if (this->m_usageCount == 0) {
	sLog.exception ("GLPlayer usage count would underflow");
    }

    this->m_usageCount--;

    if (this->m_usageCount == 0) {
	this->stop ();
    }
}

void GLPlayer::setUntimed () {
    if (this->m_handle) {
	sLog.exception ("Cannot set untimed mode after playback has started");
    }

    this->m_untimed = true;
}

void GLPlayer::clearUntimed () {
    if (this->m_handle) {
	sLog.exception ("Cannot set untimed mode after playback has started");
    }

    this->m_untimed = false;
}

void GLPlayer::setMuted () {
    this->m_muted = true;

    if (this->m_handle) {
	mpv_set_property_string (this->m_handle, "mute", "yes");
    }
}

void GLPlayer::clearMuted () {
    this->m_muted = false;

    if (this->m_handle) {
	mpv_set_property_string (this->m_handle, "mute", "no");
	// a player started muted has no audio track (init); sound needs one
	mpv_set_property_string (this->m_handle, "aid", "auto");
    }
}

void GLPlayer::setVolume (double volume) {
    this->m_volume = volume;

    if (this->m_handle) {
	mpv_set_property (this->m_handle, "volume", MPV_FORMAT_DOUBLE, &this->m_volume);
    }
}

void GLPlayer::setPaused () {
    this->m_paused = true;

    if (this->m_handle) {
	mpv_set_property_string (this->m_handle, "pause", "yes");
    }
}

void GLPlayer::clearPaused () {
    this->m_paused = false;

    if (this->m_handle) {
	mpv_set_property_string (this->m_handle, "pause", "no");
    }
}

double GLPlayer::getDuration () const {
    double value = 0.0;
    if (this->m_handle == nullptr || mpv_get_property (this->m_handle, "duration", MPV_FORMAT_DOUBLE, &value) < 0) {
	return 0.0;
    }
    return value;
}

double GLPlayer::getTime () const {
    // a seek not done yet (mpv hasn't loaded the video, or is still seeking this frame): where it will be
    if (this->m_pendingSeek.has_value ()) {
	return *this->m_pendingSeek;
    }
    if (this->m_seekTarget.has_value ()) {
	return *this->m_seekTarget;
    }
    double value = 0.0;
    if (this->m_handle == nullptr || mpv_get_property (this->m_handle, "time-pos", MPV_FORMAT_DOUBLE, &value) < 0) {
	return 0.0;
    }
    return value;
}

void GLPlayer::seek (double seconds) {
    seconds = std::max (seconds, 0.0);
    double position = 0.0;
    // mpv seeks only a loaded video: until then the seek waits (render () does it)
    if (this->m_handle == nullptr || mpv_get_property (this->m_handle, "time-pos", MPV_FORMAT_DOUBLE, &position) < 0) {
	this->m_pendingSeek = seconds;
	return;
    }
    this->m_pendingSeek = std::nullopt;
    this->m_seekTarget = seconds;
    const std::string at = std::to_string (seconds);
    const char* command[] = { "seek", at.c_str (), "absolute+exact", nullptr };
    mpv_command (this->m_handle, command);
}

void GLPlayer::setRate (double rate) {
    // mpv's speed range
    this->m_rate = std::clamp (rate, 0.01, 100.0);
    if (this->m_handle) {
	mpv_set_property (this->m_handle, "speed", MPV_FORMAT_DOUBLE, &this->m_rate);
    }
}

void GLPlayer::setLoop (bool loop) {
    this->m_loop = loop;
    if (this->m_handle) {
	mpv_set_property_string (this->m_handle, "loop", loop ? "inf" : "no");
    }
}

bool GLPlayer::reachedEnd () const {
    int flag = 0;
    if (this->m_handle == nullptr || mpv_get_property (this->m_handle, "eof-reached", MPV_FORMAT_FLAG, &flag) < 0) {
	return false;
    }
    return flag != 0;
}

void GLPlayer::render () const {
    // rendering should only happen if the texture is in use
    if (this->m_handle == nullptr) {
	return;
    }
    // a seek issued last frame is mpv's to report now
    const_cast<GLPlayer*> (this)->m_seekTarget = std::nullopt;
    // a script's seek from before mpv had the video loaded
    if (this->m_pendingSeek.has_value ()) {
	const_cast<GLPlayer*> (this)->seek (*this->m_pendingSeek);
    }

    // read all the events available. The frame size comes from observed properties, never mpv_get_property: a get
    // waits for mpv's core thread, which can be busy for seconds (an audio output that won't open), and this is the
    // render thread
    while (true) {
	const mpv_event* event = mpv_wait_event (this->m_handle, 0);

	if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
	    break;
	}

	if (event->event_id != MPV_EVENT_PROPERTY_CHANGE) {
	    continue;
	}

	const auto* property = static_cast<const mpv_event_property*> (event->data);

	if (property->format != MPV_FORMAT_INT64) {
	    continue;
	}

	const int64_t value = *static_cast<const int64_t*> (property->data);

	if (event->reply_userdata == kObserveWidth) {
	    this->m_videoWidth = value;
	} else if (event->reply_userdata == kObserveHeight) {
	    this->m_videoHeight = value;
	}
    }

    if (this->m_videoWidth > 0 && this->m_videoHeight > 0
	&& (this->m_videoWidth != this->m_width || this->m_videoHeight != this->m_height)) {
	this->m_width = this->m_videoWidth;
	this->m_height = this->m_videoHeight;
	// reconfigure the texture
	glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, this->m_width, this->m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }

    // render the next
    glViewport (0, 0, this->m_width, this->m_height);

    mpv_opengl_fbo fbo { static_cast<int> (this->m_fbo), static_cast<int> (this->m_width),
			 static_cast<int> (this->m_height), GL_RGBA8 };

    // no need to flip as it'll be handled by the wallpaper rendering code
    int flip_y = 0;
    // The host loop is paced by the compositor's frame callbacks (vsync). Do not
    // let libmpv block until the frame's target time: with several outputs each
    // owning a player on one thread, that serialises every screen onto the
    // video's frame grid and judders 24fps content on 60Hz outputs.
    int block_for_target_time = 0;

    mpv_render_param params[] = { { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
				  { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
				  { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time },
				  { MPV_RENDER_PARAM_INVALID, nullptr } };

    mpv_render_context_render (this->m_renderContext, params);
}

int GLPlayer::getWidth () const { return this->m_width; }
int GLPlayer::getHeight () const { return this->m_height; }

void GLPlayer::prepareGL () {
    if (!this->m_doWeOwnFramebuffer || this->m_fbo != GL_NONE) {
	return;
    }

    glGenFramebuffers (1, &this->m_fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_fbo);
    glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
    // reset texture's contents
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, this->m_width, this->m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    constexpr GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_outputTexture, 0);
    glDrawBuffers (1, drawBuffers);

    // ensure first framebuffer is okay
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("Framebuffers are not properly set");
    }

    // clear the framebuffer
    glClear (GL_COLOR_BUFFER_BIT);
}

void GLPlayer::init () {
    this->m_handle = mpv_create ();

    if (this->m_handle == nullptr) {
	sLog.exception ("Cannot create mpv context for video texture");
    }

    // setup mpv options for playback
    mpv_set_option_string (this->m_handle, "terminal", "yes");
#if NDEBUG
    mpv_set_option_string (this->m_handle, "msg-level", "all=status,statusline=no");
#else
    mpv_set_option_string (this->m_handle, "msg-level", "all=v");
#endif
    mpv_set_option_string (this->m_handle, "input-cursor", "no");
    mpv_set_option_string (this->m_handle, "cursor-autohide", "no");
    mpv_set_option_string (this->m_handle, "config", "no");
    mpv_set_option_string (this->m_handle, "fbo-format", "rgba8");
    mpv_set_option_string (this->m_handle, "vo", "libmpv");
    mpv_set_option_string (this->m_handle, "profile", "fast");
    mpv_set_option_string (this->m_handle, "untimed", this->m_untimed ? "yes" : "no");
    // a muted player (every video texture) selects no audio track, so it never opens an audio output: one that can't
    // connect (no PipeWire) can hold mpv's core thread for 20-30 s in its cleanup, and a muted one makes no sound
    // anyway. clearMuted selects one
    mpv_set_option_string (this->m_handle, "aid", this->m_muted ? "no" : "auto");

    if (mpv_initialize (this->m_handle) < 0) {
	sLog.exception ("Could not initialize mpv context");
    }

    // ensure video is muted and plays in a loop
    mpv_set_property_string (this->m_handle, "hwdec", "auto");
    mpv_set_property_string (this->m_handle, "loop", this->m_loop ? "inf" : "no");
    // at the end of a video that doesn't loop, stay on its last frame (IVideoTexture's ended callback)
    mpv_set_property_string (this->m_handle, "keep-open", "yes");
    mpv_set_property (this->m_handle, "speed", MPV_FORMAT_DOUBLE, &this->m_rate);
    mpv_set_property (this->m_handle, "volume", MPV_FORMAT_DOUBLE, &this->m_volume);

    mpv_observe_property (this->m_handle, kObserveWidth, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property (this->m_handle, kObserveHeight, "dheight", MPV_FORMAT_INT64);

    // initialize gl context for mpv
    mpv_opengl_init_params gl_init_params { get_proc_address, this };
    mpv_render_param params[] { { MPV_RENDER_PARAM_API_TYPE, const_cast<char*> (MPV_RENDER_API_TYPE_OPENGL) },
				{ MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
				{ MPV_RENDER_PARAM_INVALID, nullptr } };

    if (mpv_render_context_create (&this->m_renderContext, this->m_handle, params) < 0) {
	sLog.exception ("Failed to initialize MPV's GL context");
    }

    // mute the video if required
    mpv_set_property_string (this->m_handle, "mute", this->m_muted ? "yes" : "no");
    // ensure play/pause status is respected too
    mpv_set_property_string (this->m_handle, "pause", this->m_paused ? "yes" : "no");
}

void GLPlayer::setSource (const std::filesystem::path& file) { this->m_file = file; }

void GLPlayer::setSource (MemoryStreamProtocolUniquePtr source) { this->m_stream = std::move (source); }

void GLPlayer::play () {
    if (this->m_handle != nullptr) {
	sLog.exception ("Cannot play the same GLPlayer twice");
    }

    if (!this->m_file.has_value () && !this->m_stream.has_value ()) {
	sLog.exception ("Cannot play a GLPlayer without a source");
    }

    this->init ();

    if (this->m_file.has_value ()) {
	// build the path to the video file
	const char* command[] = { "loadfile", this->m_file.value ().c_str (), nullptr };

	if (mpv_command (this->m_handle, command) < 0) {
	    sLog.exception ("Cannot load video to play");
	}
    } else if (this->m_stream) {
	this->m_stream.value ()->registerReadCallback (this->m_handle);

	// start playing the video
	const char* command[] = { "loadfile", "buffer://", nullptr };

	if (mpv_command (this->m_handle, command) < 0) {
	    sLog.exception ("Cannot load video texture to play");
	}
    }
}

void GLPlayer::stop () {
    // clean up mpv and get it ready to start again at some point
    if (this->m_renderContext) {
	mpv_render_context_free (this->m_renderContext);
	this->m_renderContext = nullptr;
    }

    if (this->m_handle) {
	mpv_terminate_destroy (this->m_handle);
	this->m_handle = nullptr;
    }
}