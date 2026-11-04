#include "CFBO.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render;

namespace {
/**
 * The GL storage of an FBO of WE's texture format `format` (the .tex enum; WE's effect FBO `format` names map onto it,
 * FBOProvider::formatFromName). WE creates its render targets in that format (GH 0x1401d7460): rg88 and r8 have no
 * blue/alpha (they sample as 0 and 1), the f formats are half floats (HDR scenes keep values above 1). An RGB format
 * has no alpha: RGB16F isn't required to be colour-renderable, so it is RGBA16F with alpha sampled as 1
 */
struct GLStorage {
    GLint internalFormat;
    GLenum format;
    GLenum type;
    bool opaqueAlpha;
};

GLStorage storageFor (const TextureFormat format) {
    switch (format) {
	case TextureFormat_RG88:
	    return { GL_RG8, GL_RG, GL_UNSIGNED_BYTE, false };
	case TextureFormat_R8:
	    return { GL_R8, GL_RED, GL_UNSIGNED_BYTE, false };
	case TextureFormat_RG1616f:
	    return { GL_RG16F, GL_RG, GL_HALF_FLOAT, false };
	case TextureFormat_R16f:
	    return { GL_R16F, GL_RED, GL_HALF_FLOAT, false };
	case TextureFormat_RGBA16161616f:
	    return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false };
	case TextureFormat_RGB161616f:
	    return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, true };
	case TextureFormat_RGBa1010102:
	    return { GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, false };
	default:
	    // ARGB8888, RGB888 (LWE's scene and layer targets keep their alpha: compose layers and blend modes read it)
	    return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false };
    }
}
} // namespace

CFBO::CFBO (
    std::string name, const TextureFormat format, const uint32_t flags, const float scale, uint32_t realWidth,
    uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight
) : m_scale (scale), m_name (std::move (name)), m_format (format), m_flags (flags) {
    // create an empty texture that'll be free'd so the FBO is transparent
    constexpr GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    // create the main framebuffer
    glGenFramebuffers (1, &this->m_framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    // create the main texture
    glGenTextures (1, &this->m_texture);
    // bind the new texture to set settings on it
    glBindTexture (GL_TEXTURE_2D, this->m_texture);
    // give OpenGL an empty image
    const GLStorage storage = storageFor (format);
    glTexImage2D (
	GL_TEXTURE_2D, 0, storage.internalFormat, textureWidth, textureHeight, 0, storage.format, storage.type, nullptr
    );
    if (storage.opaqueAlpha) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    // label stuff for debugging
#if !NDEBUG
    if (GLAD_GL_VERSION_4_3) {
	glObjectLabel (GL_TEXTURE, this->m_texture, -1, this->m_name.c_str ());
    }
#endif /* DEBUG */
    // set filtering parameters, otherwise the texture is not rendered
    if (flags & TextureFlags_ClampUVs) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (flags & TextureFlags_ClampUVsBorder) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (flags & TextureFlags_NoInterpolation) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);

    // set the texture as the colour attachmend #0
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_texture, 0);
    // finally set the list of draw buffers
    glDrawBuffers (1, drawBuffers);

    // ensure first framebuffer is okay
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.exception ("Framebuffers are not properly set");
    }

    // Layer framebuffers must start transparent. The scene clear color is often opaque,
    // and using it here makes empty layer areas render as solid rectangles.
    GLfloat previousClearColor[4] = {};
    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);

    this->m_resolution = { textureWidth, textureHeight, realWidth, realHeight };

    // create the textureframe entries
    const auto frame = std::make_shared<Frame> ();

    frame->frameNumber = 0;
    frame->frametime = 0;
    frame->height1 = textureHeight;
    frame->height2 = realHeight;
    frame->width1 = textureWidth;
    frame->width2 = realWidth;
    frame->x = 0;
    frame->y = 0;

    this->m_frames.push_back (frame);
}

void CFBO::resize (uint32_t realWidth, uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight) {
    if (this->m_resolution == glm::vec4 (textureWidth, textureHeight, realWidth, realHeight)) {
	return;
    }

    GLint previousFramebuffer = 0;
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

    glBindTexture (GL_TEXTURE_2D, this->m_texture);
    const GLStorage storage = storageFor (this->m_format);
    glTexImage2D (
	GL_TEXTURE_2D, 0, storage.internalFormat, textureWidth, textureHeight, 0, storage.format, storage.type, nullptr
    );

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    GLfloat previousClearColor[4] = {};
    glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    glBindFramebuffer (GL_FRAMEBUFFER, previousFramebuffer);

    this->m_resolution = { textureWidth, textureHeight, realWidth, realHeight };

    if (this->m_depthbuffer != GL_NONE) {
	glBindRenderbuffer (GL_RENDERBUFFER, this->m_depthbuffer);
	glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, textureWidth, textureHeight);
	glBindRenderbuffer (GL_RENDERBUFFER, 0);
    }

    for (const auto& frame : this->m_frames) {
	frame->width1 = textureWidth;
	frame->height1 = textureHeight;
	frame->width2 = realWidth;
	frame->height2 = realHeight;
    }
}

void CFBO::attachDepthBuffer () const {
    if (this->m_depthbuffer != GL_NONE) {
	return;
    }

    GLint previousFramebuffer = 0;
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

    glGenRenderbuffers (1, &this->m_depthbuffer);
    glBindRenderbuffer (GL_RENDERBUFFER, this->m_depthbuffer);
    glRenderbufferStorage (
	GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, static_cast<GLsizei> (this->m_resolution.x),
	static_cast<GLsizei> (this->m_resolution.y)
    );
    glBindRenderbuffer (GL_RENDERBUFFER, 0);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, this->m_depthbuffer);

    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	sLog.error ("The depth buffer of ", this->m_name, " left the framebuffer incomplete, drawing without depth");
	glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
	glDeleteRenderbuffers (1, &this->m_depthbuffer);
	this->m_depthbuffer = GL_NONE;
    } else {
	glDepthMask (GL_TRUE);
	glClearDepth (1.0);
	glClear (GL_DEPTH_BUFFER_BIT);
    }

    glBindFramebuffer (GL_FRAMEBUFFER, previousFramebuffer);
}

CFBO::~CFBO () {
    if (this->m_depthbuffer != GL_NONE) {
	glDeleteRenderbuffers (1, &this->m_depthbuffer);
    }
    // free opengl texture and framebuffer
    glDeleteTextures (1, &this->m_texture);
    glDeleteFramebuffers (1, &this->m_framebuffer);
}

const std::string& CFBO::getName () const { return this->m_name; }

const float& CFBO::getScale () const { return this->m_scale; }

TextureFormat CFBO::getFormat () const { return this->m_format; }

uint32_t CFBO::getFlags () const { return this->m_flags; }

GLuint CFBO::getFramebuffer () const { return this->m_framebuffer; }

GLuint CFBO::getDepthbuffer () const { return this->m_depthbuffer; }

GLuint CFBO::getTextureID (uint32_t imageIndex) const { return this->m_texture; }

uint32_t CFBO::getTextureWidth (uint32_t imageIndex) const { return this->m_resolution.x; }

uint32_t CFBO::getTextureHeight (uint32_t imageIndex) const { return this->m_resolution.y; }

uint32_t CFBO::getRealWidth () const { return this->m_resolution.z; }

uint32_t CFBO::getRealHeight () const { return this->m_resolution.w; }

const std::vector<FrameSharedPtr>& CFBO::getFrames () const { return this->m_frames; }

const glm::vec4* CFBO::getResolution () const { return &this->m_resolution; }

bool CFBO::isAnimated () const { return false; }

uint32_t CFBO::getSpritesheetCols () const {
    return 0; // FBOs don't have spritesheets
}

uint32_t CFBO::getSpritesheetRows () const {
    return 0; // FBOs don't have spritesheets
}

uint32_t CFBO::getSpritesheetFrames () const {
    return 0; // FBOs don't have spritesheets
}

float CFBO::getSpritesheetDuration () const {
    return 0.0f; // FBOs don't have spritesheets
}

void CFBO::incrementUsageCount () const { }
void CFBO::decrementUsageCount () const { }
void CFBO::update () const { }
// FBOs are always ready
bool CFBO::isReady () const { return true; }