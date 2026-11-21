#include "GLPlayer.h"

#include "CudaGL.h"

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <climits>
#include <cmath>

// hwcontext_cuda.h wants cuda.h for two handle types; no build dependency ships cuda.h
#ifndef CUDA_VERSION
#define CUDA_VERSION 10000
typedef struct CUctx_st* CUcontext;
typedef struct CUstream_st* CUstream;
#endif

extern "C" {
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

using namespace WallpaperEngine::VideoPlayback;

namespace {
/** a frame's display width: its own, stretched by a non-square sample aspect as mpv's dwidth (INT_MAX at most) */
int displayWidth (const AVFrame* frame) {
    const AVRational sar = frame->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0 || sar.num == sar.den) {
	return frame->width;
    }
    const double width = std::round (frame->width * av_q2d (sar));
    return width >= static_cast<double> (INT_MAX) ? INT_MAX : static_cast<int> (width);
}

/**
 * How long a player's first render waits for its first frame (decoding starts with the scene's build, so it is
 * usually there): a scene's first picture then shows its videos instead of their empty textures
 */
constexpr std::chrono::milliseconds kFirstFrameWait { 500 };
/** a decoder this far behind the clock (a rate it can't keep up with, a stalled render) jumps to the clock instead */
constexpr double kMaxLag = 1.0;

constexpr const char* kVertex = R"(#version 330 core
out vec2 v_uv;
void main () {
    vec2 p = vec2 ((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// texture row 0 is the image's top row, as LWE's other textures (no flip)
constexpr const char* kFragment = R"(#version 330 core
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_y;
uniform sampler2D u_u;
uniform sampler2D u_v;
uniform sampler2D u_a;
uniform vec4 u_my;
uniform vec4 u_mu;
uniform vec4 u_mv;
uniform vec4 u_ma;
uniform float u_scale;
uniform vec2 u_chromaShift;
uniform mat4 u_matrix;
uniform int u_rgba;
uniform int u_alpha;
void main () {
    if (u_rgba != 0) {
        o_color = texture (u_y, v_uv);
        return;
    }
    vec2 c = v_uv + u_chromaShift;
    vec4 yuv = vec4 (dot (texture (u_y, v_uv), u_my), dot (texture (u_u, c), u_mu), dot (texture (u_v, c), u_mv), 1.0);
    yuv.xyz *= u_scale;
    vec3 rgb = (u_matrix * yuv).rgb;
    float a = u_alpha != 0 ? dot (texture (u_a, v_uv), u_ma) * u_scale : 1.0;
    o_color = vec4 (clamp (rgb, 0.0, 1.0), a);
}
)";

/** the GL state a frame's upload and conversion touch, put back as LWE left it */
struct SavedState {
    GLint program, vao, activeTexture, drawFbo, readFbo, unpackBuffer, unpackAlignment, unpackRowLength;
    GLint viewport[4];
    GLint textures[4];
    GLint samplers[4];
    GLboolean colorMask[4];
    GLboolean blend, depth, scissor, cull, stencil, srgb;

    SavedState () {
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vao);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &activeTexture);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &readFbo);
	glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
	glGetIntegerv (GL_UNPACK_ALIGNMENT, &unpackAlignment);
	glGetIntegerv (GL_UNPACK_ROW_LENGTH, &unpackRowLength);
	glGetIntegerv (GL_VIEWPORT, viewport);
	glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
	for (int i = 0; i < 4; i++) {
	    glActiveTexture (GL_TEXTURE0 + i);
	    glGetIntegerv (GL_TEXTURE_BINDING_2D, &textures[i]);
	    glGetIntegerv (GL_SAMPLER_BINDING, &samplers[i]);
	}
	blend = glIsEnabled (GL_BLEND);
	depth = glIsEnabled (GL_DEPTH_TEST);
	scissor = glIsEnabled (GL_SCISSOR_TEST);
	cull = glIsEnabled (GL_CULL_FACE);
	stencil = glIsEnabled (GL_STENCIL_TEST);
	srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    }

    ~SavedState () {
	auto set = [] (GLenum cap, GLboolean on) { on ? glEnable (cap) : glDisable (cap); };
	set (GL_BLEND, blend);
	set (GL_DEPTH_TEST, depth);
	set (GL_SCISSOR_TEST, scissor);
	set (GL_CULL_FACE, cull);
	set (GL_STENCIL_TEST, stencil);
	set (GL_FRAMEBUFFER_SRGB, srgb);
	for (int i = 3; i >= 0; i--) {
	    glActiveTexture (GL_TEXTURE0 + i);
	    glBindTexture (GL_TEXTURE_2D, textures[i]);
	    glBindSampler (i, samplers[i]);
	}
	glActiveTexture (activeTexture);
	glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
	glViewport (viewport[0], viewport[1], viewport[2], viewport[3]);
	glPixelStorei (GL_UNPACK_ALIGNMENT, unpackAlignment);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, unpackRowLength);
	glBindBuffer (GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, drawFbo);
	glBindFramebuffer (GL_READ_FRAMEBUFFER, readFbo);
	glBindVertexArray (vao);
	glUseProgram (program);
    }
};

GLuint compile (GLenum type, const char* source) {
    const GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);
    GLint ok = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
	char log[1024] = {};
	glGetShaderInfoLog (shader, sizeof log, nullptr, log);
	sLog.error ("video: the frame conversion shader won't compile: ", log);
	glDeleteShader (shader);
	return 0;
    }
    return shader;
}

/** rows of the matrix taking (Y, Cb, Cr, 1) as stored (normalised to their bit depth) to RGB */
std::array<float, 16> yuvToRgb (const AVFrame* frame, int depth, bool fullRange) {
    double kr = 0.2126, kb = 0.0722;
    switch (frame->colorspace) {
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_FCC: kr = 0.299, kb = 0.114; break;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL: kr = 0.2627, kb = 0.0593; break;
	case AVCOL_SPC_SMPTE240M: kr = 0.212, kb = 0.087; break;
	case AVCOL_SPC_BT709: break;
	default:
	    // unspecified: what mpv assumes, BT.601 below HD
	    if (frame->width < 1280 && frame->height <= 576) {
		kr = 0.299, kb = 0.114;
	    }
	    break;
    }
    const double kg = 1.0 - kr - kb;
    const double max = (1 << depth) - 1;
    const double unit = 1 << (depth - 8);
    // Y' = ay·v + by, C = ac·v + bc, from the stored value v in [0, 1]
    double ay, by, ac, bc;
    if (fullRange) {
	ay = 1.0, by = 0.0;
	ac = 1.0, bc = -(1 << (depth - 1)) / max;
    } else {
	ay = max / (219.0 * unit), by = -16.0 / 219.0;
	ac = max / (224.0 * unit), bc = -128.0 / 224.0;
    }
    const double rv = 2.0 * (1.0 - kr), gu = 2.0 * kb * (1.0 - kb) / kg, gv = 2.0 * kr * (1.0 - kr) / kg,
		 bu = 2.0 * (1.0 - kb);
    return {
	static_cast<float> (ay), 0.0f, static_cast<float> (rv * ac), static_cast<float> (by + rv * bc),
	static_cast<float> (ay), static_cast<float> (-gu * ac), static_cast<float> (-gv * ac),
	static_cast<float> (by - (gu + gv) * bc),
	static_cast<float> (ay), static_cast<float> (bu * ac), 0.0f, static_cast<float> (by + bu * bc),
	0.0f, 0.0f, 0.0f, 1.0f,
    };
}
} // namespace

GLPlayer::GLPlayer (
    RenderContext& context, GLuint outputTexture, const std::filesystem::path& file, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_doWeOwnFramebuffer (fbo == GL_NONE), m_outputTexture (outputTexture), m_fbo (fbo),
    m_width (baseWidth), m_height (baseHeight), m_file (file) {
    this->prepareGL ();
}

GLPlayer::GLPlayer (
    RenderContext& context, const GLuint outputTexture, const uint8_t* data, const size_t size, const int64_t baseWidth,
    const int64_t baseHeight, const GLuint fbo
) : ContextAware (context), m_doWeOwnFramebuffer (fbo == GL_NONE), m_outputTexture (outputTexture), m_fbo (fbo),
    m_width (baseWidth), m_height (baseHeight), m_data (data), m_size (size) {
    this->prepareGL ();
}

GLPlayer::~GLPlayer () {
    this->stop ();
    for (GLuint& plane : this->m_planes) {
	if (plane != 0) {
	    glDeleteTextures (1, &plane);
	}
    }
    if (this->m_program != 0) {
	glDeleteProgram (this->m_program);
    }
    if (this->m_vao != 0) {
	glDeleteVertexArrays (1, &this->m_vao);
    }
    sws_freeContext (this->m_sws);
    if (this->m_doWeOwnFramebuffer) {
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

void GLPlayer::setPaused () { this->m_paused = true; }

void GLPlayer::clearPaused () { this->m_paused = false; }

double GLPlayer::getDuration () const { return this->m_decoder ? this->m_decoder->duration () : 0.0; }

double GLPlayer::getTime () const {
    if (this->m_pendingSeek.has_value ()) {
	return *this->m_pendingSeek;
    }
    const double length = this->getDuration ();
    if (length <= 0.0) {
	return this->m_clock;
    }
    return this->m_loop ? std::fmod (this->m_clock, length) : std::min (this->m_clock, length);
}

void GLPlayer::seek (double seconds) {
    seconds = std::max (seconds, 0.0);
    if (!this->m_decoder) {
	this->m_pendingSeek = seconds;
	return;
    }
    const double length = this->m_decoder->duration ();
    if (length > 0.0) {
	seconds = this->m_loop ? std::fmod (seconds, length) : std::min (seconds, length);
    }
    this->m_clock = seconds;
    this->m_decoder->seek (seconds);
}

void GLPlayer::setRate (double rate) {
    // the range IVideoTexture had with mpv
    this->m_rate = std::clamp (rate, 0.01, 100.0);
}

void GLPlayer::setLoop (bool loop) {
    this->m_loop = loop;
    if (this->m_decoder) {
	this->m_decoder->setLoop (loop);
    }
}

bool GLPlayer::reachedEnd () const { return this->m_decoder && !this->m_loop && this->m_decoder->ended (); }

int GLPlayer::getWidth () const { return static_cast<int> (this->m_width); }
int GLPlayer::getHeight () const { return static_cast<int> (this->m_height); }

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

void GLPlayer::prefetch () {
    if (!this->m_decoder) {
	this->startDecoder ();
    }
}

void GLPlayer::startDecoder () {
    if (!this->m_file.has_value () && this->m_data == nullptr) {
	sLog.exception ("Cannot play a GLPlayer without a source");
    }
    const auto& context = this->getContext ().getContext ();
    HwdecChoice choice = HwdecChoice::parse (context.video_hwdec, context.video_hwdec_interop);
    this->m_decoder = this->m_file.has_value ()
			? std::make_unique<VideoDecoder> (*this->m_file, std::move (choice))
			: std::make_unique<VideoDecoder> (this->m_data, this->m_size, std::move (choice));
    this->m_decoder->setLoop (this->m_loop);
    this->m_playedAt = std::chrono::steady_clock::now ();
}

void GLPlayer::play () {
    // a prefetched decoder is already at the video's start
    if (!this->m_decoder) {
	this->startDecoder ();
    }
    this->m_lastRenderTime.reset ();
    this->m_shownAny = false;
    this->m_waitedFirst = false;
    this->m_rendersWithout = 0;
    this->m_firstRender.reset ();
    this->m_clock = 0.0;
    if (this->m_pendingSeek.has_value ()) {
	this->m_clock = *this->m_pendingSeek;
	this->m_decoder->seek (*this->m_pendingSeek);
	this->m_pendingSeek.reset ();
    }
}

void GLPlayer::stop () {
    if (!this->m_decoder) {
	return;
    }
    // the interop's registrations go before the decoder, whose device holds their CUDA context
    this->releaseCuda ();
    this->m_decoder.reset ();
}

void GLPlayer::releaseCuda () {
    const CudaGL* cuda = CudaGL::get ();
    AVBufferRef* device = this->m_decoder ? this->m_decoder->device () : nullptr;
    const bool any = std::any_of (this->m_cudaResources.begin (), this->m_cudaResources.end (), [] (void* r) { return r != nullptr; });
    if (!any) {
	return;
    }
    if (cuda == nullptr || device == nullptr) {
	this->m_cudaResources.fill (nullptr);
	return;
    }
    const auto* hw = static_cast<const AVCUDADeviceContext*> (reinterpret_cast<AVHWDeviceContext*> (device->data)->hwctx);
    cuda->ctxPushCurrent (hw->cuda_ctx);
    for (void*& resource : this->m_cudaResources) {
	if (resource != nullptr) {
	    cuda->graphicsUnregisterResource (resource);
	    resource = nullptr;
	}
    }
    CudaGL::CUcontext popped = nullptr;
    cuda->ctxPopCurrent (&popped);
    this->m_planesForCuda = false;
    this->m_layout = {};
}

void GLPlayer::render () {
    if (!this->m_decoder) {
	return;
    }
    if (!this->m_firstRender.has_value ()) {
	this->m_firstRender = std::chrono::steady_clock::now ();
    }
    const double now = this->getContext ().getContext ().renderTime;
    const double elapsed = this->m_lastRenderTime.has_value () ? std::max (0.0, now - *this->m_lastRenderTime) : 0.0;
    this->m_lastRenderTime = now;
    if (!this->m_paused) {
	this->m_clock += elapsed * this->m_rate;
    }
    const double length = this->m_decoder->duration ();
    if (!this->m_loop && length > 0.0) {
	this->m_clock = std::min (this->m_clock, length);
    }
    if (!this->m_shownAny && !this->m_waitedFirst) {
	this->m_waitedFirst = true;
	this->m_decoder->waitFirst (kFirstFrameWait);
    }
    for (const std::string& message : this->m_decoder->messages ()) {
	sLog.error (message);
    }

    double frameTime = 0.0;
    AVFrame* frame = this->m_decoder->take (this->m_clock, !this->m_shownAny, &frameTime);
    if (frame != nullptr) {
	const bool shown = this->show (frame);
	if (shown && !this->m_shownAny) {
	    this->m_shownAny = true;
	    const auto at = std::chrono::steady_clock::now ();
	    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (at - this->m_playedAt);
	    const auto late = std::chrono::duration_cast<std::chrono::milliseconds> (at - *this->m_firstRender);
	    sLog.error (
		"video: first frame ", this->m_width, "x", this->m_height, " ",
		av_get_pix_fmt_name (static_cast<AVPixelFormat> (frame->format)),
		frame->format == AV_PIX_FMT_CUDA && !this->m_cudaFailed ? " through the CUDA-GL interop" : " uploaded",
		" ", ms.count (), " ms after its decoder started, ", this->m_rendersWithout, " renders (", late.count (), " ms) without it"
	    );
	}
	av_frame_free (&frame);
    } else if (!this->m_shownAny) {
	this->m_rendersWithout++;
    }

    if (this->m_shownAny && !this->m_paused && !this->m_decoder->failed ()) {
	const double newest = this->m_decoder->newest ();
	const bool atEnd = !this->m_loop && length > 0.0 && this->m_clock >= length - 1e-3;
	if (!atEnd && newest >= 0.0 && newest < this->m_clock - kMaxLag) {
	    this->seek (this->m_clock);
	}
    }
}

void GLPlayer::setFrameCaps (const int maxSide, const std::uint64_t maxPixels) {
    this->m_maxSide = maxSide;
    this->m_maxPixels = maxPixels;
}

bool GLPlayer::show (AVFrame* frame) {
    if (this->m_maxSide > 0) {
	const int displayed = displayWidth (frame);
	const auto over = [this] (int width, int height) {
	    return width > this->m_maxSide || height > this->m_maxSide
		|| static_cast<std::uint64_t> (width) * static_cast<std::uint64_t> (height) > this->m_maxPixels;
	};
	if (over (frame->width, frame->height) || over (displayed, frame->height)) {
	    if (!this->m_saidOverCaps) {
		this->m_saidOverCaps = true;
		sLog.error (
		    "video: a ", frame->width, "x", frame->height, " frame (", displayed,
		    " wide as displayed) is over ", this->m_maxSide, " px a side or ", this->m_maxPixels,
		    " px: not shown"
		);
	    }
	    return false;
	}
    }
    const SavedState saved;
    AVFrame* copied = nullptr;
    const AVFrame* source = frame;
    if (frame->hw_frames_ctx != nullptr) {
	if (frame->format == AV_PIX_FMT_CUDA && !this->m_cudaFailed) {
	    if (this->copyCuda (frame)) {
		this->convert (frame);
		return true;
	    }
	    this->m_cudaFailed = true;
	    this->m_decoder->setTransfer ();
	    this->releaseCuda ();
	    sLog.error ("video: the CUDA-GL interop failed; frames are copied through memory");
	}
	copied = av_frame_alloc ();
	if (av_hwframe_transfer_data (copied, frame, 0) < 0) {
	    av_frame_free (&copied);
	    return false;
	}
	av_frame_copy_props (copied, frame);
	source = copied;
    }
    const bool uploaded = this->upload (const_cast<AVFrame*> (source));
    if (uploaded) {
	this->convert (source);
    }
    av_frame_free (&copied);
    return uploaded;
}

bool GLPlayer::layoutFor (AVPixelFormat format, int width, int height, Layout& layout) const {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get (format);
    if (desc == nullptr) {
	return false;
    }
    layout = {};
    layout.format = format;
    layout.width = width;
    layout.height = height;
    constexpr uint64_t kOther
	= AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL
	| AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_FLOAT;
    bool planar = !(desc->flags & kOther) && (desc->flags & AV_PIX_FMT_FLAG_PLANAR) && desc->nb_components >= 3;
    const int depth = desc->comp[0].depth;
    const int bytes = depth > 8 ? 2 : 1;
    std::array<int, 4> channels {};
    for (int c = 0; planar && c < desc->nb_components; c++) {
	const AVComponentDescriptor& comp = desc->comp[c];
	planar = comp.depth == depth && depth <= 16 && comp.shift == desc->comp[0].shift && comp.step % bytes == 0
	      && comp.offset % bytes == 0 && comp.step / bytes <= 2 && comp.offset / bytes < 2 && comp.plane < 4;
	if (planar) {
	    channels[comp.plane] = std::max (channels[comp.plane], comp.step / bytes);
	    layout.compPlane[c] = comp.plane;
	    layout.compChannel[c] = comp.offset / bytes;
	}
    }
    if (!planar) {
	// anything else is converted to RGBA by swscale first
	layout.rgba = true;
	layout.planes = 1;
	layout.plane[0] = { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height, 4 };
	layout.compPlane = { 0, -1, -1, -1 };
	return true;
    }
    layout.planes = av_pix_fmt_count_planes (format);
    layout.depth = depth;
    layout.log2ChromaW = desc->log2_chroma_w;
    layout.log2ChromaH = desc->log2_chroma_h;
    layout.scale = static_cast<float> (
	((1 << (8 * bytes)) - 1) / (static_cast<double> (1 << desc->comp[0].shift) * ((1 << depth) - 1))
    );
    for (int p = 0; p < layout.planes; p++) {
	const bool chroma = p == layout.compPlane[1] || p == layout.compPlane[2];
	const int w = chroma ? AV_CEIL_RSHIFT (width, desc->log2_chroma_w) : width;
	const int h = chroma ? AV_CEIL_RSHIFT (height, desc->log2_chroma_h) : height;
	const bool two = channels[p] == 2;
	layout.plane[p] = { two ? (bytes == 1 ? GL_RG8 : GL_RG16) : (bytes == 1 ? GL_R8 : GL_R16),
			    two ? GLenum (GL_RG) : GLenum (GL_RED), bytes == 1 ? GLenum (GL_UNSIGNED_BYTE) : GLenum (GL_UNSIGNED_SHORT),
			    w, h, channels[p] * bytes };
    }
    return true;
}

bool GLPlayer::preparePlanes (const Layout& layout, bool cuda) {
    if (this->m_layout == layout && this->m_planesForCuda == cuda && this->m_planes[0] != 0) {
	return true;
    }
    this->releaseCuda ();
    for (int p = 0; p < layout.planes; p++) {
	if (this->m_planes[p] == 0) {
	    glGenTextures (1, &this->m_planes[p]);
	}
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, this->m_planes[p]);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const auto& plane = layout.plane[p];
	glTexImage2D (GL_TEXTURE_2D, 0, plane.internal, plane.width, plane.height, 0, plane.format, plane.type, nullptr);
    }
    if (cuda) {
	const CudaGL* calls = CudaGL::get ();
	for (int p = 0; p < layout.planes; p++) {
	    if (calls->graphicsGLRegisterImage (
		    &this->m_cudaResources[p], this->m_planes[p], GL_TEXTURE_2D, CudaGL::kRegisterWriteDiscard
		)
		!= 0) {
		this->m_cudaResources[p] = nullptr;
		return false;
	    }
	}
    }
    this->m_layout = layout;
    this->m_planesForCuda = cuda;
    return true;
}

bool GLPlayer::copyCuda (AVFrame* frame) {
    const CudaGL* cuda = CudaGL::get ();
    if (cuda == nullptr || frame->hw_frames_ctx == nullptr) {
	return false;
    }
    const auto* frames = reinterpret_cast<const AVHWFramesContext*> (frame->hw_frames_ctx->data);
    Layout layout;
    if (!this->layoutFor (frames->sw_format, frame->width, frame->height, layout) || layout.rgba) {
	return false;
    }
    const auto* hw = static_cast<const AVCUDADeviceContext*> (frames->device_ctx->hwctx);
    if (cuda->ctxPushCurrent (hw->cuda_ctx) != 0) {
	return false;
    }
    bool ok = this->preparePlanes (layout, true);
    if (ok) {
	auto* resources = reinterpret_cast<CudaGL::CUgraphicsResource*> (this->m_cudaResources.data ());
	ok = cuda->graphicsMapResources (layout.planes, resources, hw->stream) == 0;
	if (ok) {
	    for (int p = 0; ok && p < layout.planes; p++) {
		CudaGL::CUarray array = nullptr;
		ok = cuda->graphicsSubResourceGetMappedArray (&array, resources[p], 0, 0) == 0;
		CudaGL::Memcpy2D copy {};
		copy.srcMemoryType = CudaGL::kMemoryDevice;
		copy.srcDevice = reinterpret_cast<CudaGL::CUdeviceptr> (frame->data[p]);
		copy.srcPitch = frame->linesize[p];
		copy.dstMemoryType = CudaGL::kMemoryArray;
		copy.dstArray = array;
		copy.WidthInBytes = static_cast<size_t> (layout.plane[p].width) * layout.plane[p].bytesPerPixel;
		copy.Height = layout.plane[p].height;
		ok = ok && cuda->memcpy2DAsync (&copy, hw->stream) == 0;
	    }
	    // the unmap orders the copies before any GL work that follows
	    ok = cuda->graphicsUnmapResources (layout.planes, resources, hw->stream) == 0 && ok;
	}
    }
    CudaGL::CUcontext popped = nullptr;
    cuda->ctxPopCurrent (&popped);
    return ok;
}

bool GLPlayer::upload (AVFrame* frame) {
    Layout layout;
    if (!this->layoutFor (static_cast<AVPixelFormat> (frame->format), frame->width, frame->height, layout)) {
	return false;
    }
    const uint8_t* rgba = nullptr;
    if (layout.rgba) {
	this->m_sws = sws_getCachedContext (
	    this->m_sws, frame->width, frame->height, static_cast<AVPixelFormat> (frame->format), frame->width,
	    frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr
	);
	if (this->m_sws == nullptr) {
	    return false;
	}
	this->m_rgba.resize (static_cast<size_t> (frame->width) * frame->height * 4);
	uint8_t* planes[4] = { this->m_rgba.data () };
	int strides[4] = { frame->width * 4 };
	sws_scale (this->m_sws, frame->data, frame->linesize, 0, frame->height, planes, strides);
	rgba = this->m_rgba.data ();
    }
    if (!this->preparePlanes (layout, false)) {
	return false;
    }
    glActiveTexture (GL_TEXTURE0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    for (int p = 0; p < layout.planes; p++) {
	const auto& plane = layout.plane[p];
	const uint8_t* data = rgba ? rgba : frame->data[p];
	const int stride = rgba ? frame->width * 4 : frame->linesize[p];
	glBindTexture (GL_TEXTURE_2D, this->m_planes[p]);
	if (stride > 0 && stride % plane.bytesPerPixel == 0) {
	    glPixelStorei (GL_UNPACK_ROW_LENGTH, stride / plane.bytesPerPixel);
	    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, plane.width, plane.height, plane.format, plane.type, data);
	} else {
	    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	    for (int row = 0; row < plane.height; row++) {
		glTexSubImage2D (
		    GL_TEXTURE_2D, 0, 0, row, plane.width, 1, plane.format, plane.type, data + static_cast<ptrdiff_t> (row) * stride
		);
	    }
	}
    }
    return true;
}

void GLPlayer::sizeOutput (int width, int height) {
    if (width <= 0 || height <= 0 || (width == this->m_width && height == this->m_height)) {
	return;
    }
    this->m_width = width;
    this->m_height = height;
    glBindTexture (GL_TEXTURE_2D, this->m_outputTexture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

void GLPlayer::convert (const AVFrame* frame) {
    const Layout& layout = this->m_layout;
    glActiveTexture (GL_TEXTURE0);
    this->sizeOutput (displayWidth (frame), frame->height);

    if (this->m_program == 0) {
	const GLuint vertex = compile (GL_VERTEX_SHADER, kVertex);
	const GLuint fragment = compile (GL_FRAGMENT_SHADER, kFragment);
	if (vertex == 0 || fragment == 0) {
	    return;
	}
	this->m_program = glCreateProgram ();
	glAttachShader (this->m_program, vertex);
	glAttachShader (this->m_program, fragment);
	glLinkProgram (this->m_program);
	glDeleteShader (vertex);
	glDeleteShader (fragment);
	glGenVertexArrays (1, &this->m_vao);
    }

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_fbo);
    glViewport (0, 0, static_cast<GLsizei> (this->m_width), static_cast<GLsizei> (this->m_height));
    glDisable (GL_BLEND);
    glDisable (GL_DEPTH_TEST);
    glDisable (GL_SCISSOR_TEST);
    glDisable (GL_CULL_FACE);
    glDisable (GL_STENCIL_TEST);
    glDisable (GL_FRAMEBUFFER_SRGB);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram (this->m_program);
    glBindVertexArray (this->m_vao);

    const GLuint program = this->m_program;
    const char* samplers[4] = { "u_y", "u_u", "u_v", "u_a" };
    const char* masks[4] = { "u_my", "u_mu", "u_mv", "u_ma" };
    for (int c = 0; c < 4; c++) {
	const int plane = layout.compPlane[c] >= 0 ? layout.compPlane[c] : 0;
	glActiveTexture (GL_TEXTURE0 + c);
	glBindTexture (GL_TEXTURE_2D, this->m_planes[plane]);
	glBindSampler (c, 0);
	glUniform1i (glGetUniformLocation (program, samplers[c]), c);
	float mask[4] = {};
	mask[layout.compChannel[c]] = 1.0f;
	glUniform4fv (glGetUniformLocation (program, masks[c]), 1, mask);
    }
    glUniform1i (glGetUniformLocation (program, "u_rgba"), layout.rgba ? 1 : 0);
    glUniform1i (glGetUniformLocation (program, "u_alpha"), layout.compPlane[3] >= 0 ? 1 : 0);
    glUniform1f (glGetUniformLocation (program, "u_scale"), layout.scale);
    if (!layout.rgba) {
	const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG || layout.format == AV_PIX_FMT_YUVJ420P
			    || layout.format == AV_PIX_FMT_YUVJ422P || layout.format == AV_PIX_FMT_YUVJ444P;
	const auto matrix = yuvToRgb (frame, layout.depth, fullRange);
	glUniformMatrix4fv (glGetUniformLocation (program, "u_matrix"), 1, GL_TRUE, matrix.data ());
	// chroma sited on the left luma sample (MPEG-2 / H.264's default, and mpv's for an unspecified one) sits half a
	// luma pixel left of where a half-size texture's texel centres put it
	const bool left = frame->chroma_location == AVCHROMA_LOC_UNSPECIFIED || frame->chroma_location == AVCHROMA_LOC_LEFT
		       || frame->chroma_location == AVCHROMA_LOC_TOPLEFT;
	const bool top = frame->chroma_location == AVCHROMA_LOC_TOPLEFT || frame->chroma_location == AVCHROMA_LOC_TOP;
	const float shiftX = left && layout.log2ChromaW == 1 ? 0.5f / static_cast<float> (layout.width) : 0.0f;
	const float shiftY = top && layout.log2ChromaH == 1 ? 0.5f / static_cast<float> (layout.height) : 0.0f;
	glUniform2f (glGetUniformLocation (program, "u_chromaShift"), shiftX, shiftY);
    }
    glDrawArrays (GL_TRIANGLES, 0, 3);
}
