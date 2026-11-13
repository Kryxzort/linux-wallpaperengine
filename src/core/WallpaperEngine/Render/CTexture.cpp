#include "CTexture.h"
#include "WallpaperEngine/Logging/Log.h"

#include <lz4.h>

#define STB_IMAGE_IMPLEMENTATION
#include "RenderContext.h"

#include <stb_image.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <system_error>
#include <thread>

using namespace WallpaperEngine::Render;

namespace {
DecodedTextureImage decodeImage (const MipmapSharedPtr& mipmap) {
    DecodedTextureImage image;
    image.width = mipmap->width;
    image.height = mipmap->height;
    int fileChannels;
    image.pixels.reset (stbi_load_from_memory (
	reinterpret_cast<unsigned char*> (mipmap->uncompressedData.get ()), mipmap->uncompressedSize, &image.width,
	&image.height, &fileChannels, 4
    ));
    return image;
}

/**
 * The threads deferred textures decode on: started as needed, at most half the cores and never more than 4 (a scene
 * loading next to a playing one leaves the rest of the machine alone). Never destroyed: a thread may still be
 * decoding when the process exits, and each job owns the mipmap it reads.
 */
class DecodePool {
public:
    static DecodePool& get () {
	static auto* pool = new DecodePool ();
	return *pool;
    }

    std::future<DecodedTextureImage> submit (std::packaged_task<DecodedTextureImage ()> job) {
	auto future = job.get_future ();
	{
	    std::lock_guard lock (this->m_mutex);
	    // more queued jobs than threads waiting for one: another thread, while there is room
	    if (static_cast<int> (this->m_jobs.size ()) >= this->m_idle && this->m_threads < this->m_maxThreads) {
		try {
		    std::thread (&DecodePool::run, this).detach ();
		    this->m_threads++;
		} catch (const std::system_error&) {
		    // no thread to be had: with none running, nothing would ever run the job
		}
	    }
	    if (this->m_threads == 0) {
		job ();
		return future;
	    }
	    this->m_jobs.push_back (std::move (job));
	}
	this->m_wake.notify_one ();
	return future;
    }

private:
    DecodePool () :
	m_maxThreads (std::clamp (static_cast<int> (std::thread::hardware_concurrency ()) / 2, 1, 4)) { }

    void run () {
	pthread_setname_np (pthread_self (), "wp-tex-decode");
	for (;;) {
	    std::packaged_task<DecodedTextureImage ()> job;
	    {
		std::unique_lock lock (this->m_mutex);
		this->m_idle++;
		this->m_wake.wait (lock, [this] { return !this->m_jobs.empty (); });
		this->m_idle--;
		job = std::move (this->m_jobs.front ());
		this->m_jobs.pop_front ();
	    }
	    job ();
	}
    }

    const int m_maxThreads;
    int m_threads = 0;
    int m_idle = 0;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::packaged_task<DecodedTextureImage ()>> m_jobs;
};
} // namespace

CTexture::CTexture (RenderContext& context, TextureUniquePtr header, const bool deferUpload) :
    Helpers::ContextAware (context), m_header (std::move (header)) {
    // ensure the header is parsed
    this->setupResolution ();
    const GLint internalFormat = this->setupInternalFormat ();

    // videos are a bit special, they only have one framebuffer, one mipmap
    if (this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video) {
	if (this->m_header->images.empty () || this->m_header->images.begin ()->second.empty ()) {
	    sLog.exception ("Cannot load video texture, no mipmaps found");
	}

	// generate the texture and set it up to be used by the player
	this->m_textureID = new GLuint[1];
	glGenTextures (1, this->m_textureID);
	this->setupOpenGLParameters (0);

	const auto mipmap = *this->m_header->images.begin ()->second.begin ();

	this->m_player = std::make_unique<GLPlayer> (
	    this->getContext (), this->m_textureID[0],
	    std::make_unique<MemoryStreamProtocol> (mipmap->uncompressedData.get (), mipmap->uncompressedSize),
	    this->m_header->textureWidth, this->m_header->textureHeight
	);
	// setup texture video player
	this->m_player->setMuted ();
	this->m_player->setVolume (0.0f);
	this->m_player->setUntimed ();
	// texture is ready, nothing else to do
	return;
    }

    // allocate texture ids list
    this->m_textureID = new GLuint[this->m_header->imageCount];
    // ask opengl for the correct amount of textures and framebuffers
    glGenTextures (this->m_header->imageCount, this->m_textureID);

    for (const auto& [index, mipmaps] : this->m_header->images) {
	this->setupOpenGLParameters (index);

	int level = 0;

	for (const auto& mipmap : mipmaps) {
	    if (this->m_header->freeImageFormat != FIF_UNKNOWN && deferUpload) {
		this->m_pendingUploads.push_back (
		    { index, level,
		      DecodePool::get ().submit (std::packaged_task<DecodedTextureImage ()> ([mipmap] {
			  return decodeImage (mipmap);
		      })) }
		);
		level++;
		continue;
	    }

	    DecodedTextureImage decoded;
	    const void* dataptr = mipmap->uncompressedData.get ();
	    int width = mipmap->width;
	    int height = mipmap->height;
	    const uint32_t bufferSize = mipmap->uncompressedSize;
	    GLenum textureFormat = GL_RGBA;

	    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
		decoded = decodeImage (mipmap);
		dataptr = decoded.pixels.get ();
		width = decoded.width;
		height = decoded.height;
	    } else {
		if (this->m_header->format == TextureFormat_R8) {
		    // red textures are 1-byte-per-pixel, so it's alignment has to be set manually
		    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
		    textureFormat = GL_RED;
		} else if (this->m_header->format == TextureFormat_RG88) {
		    textureFormat = GL_RG;
		}
	    }

	    switch (internalFormat) {
		case GL_RGBA8:
		case GL_RG8:
		case GL_R8:
		    glTexImage2D (
			GL_TEXTURE_2D, level, internalFormat, width, height, 0, textureFormat, GL_UNSIGNED_BYTE, dataptr
		    );
		    break;
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		    glCompressedTexImage2D (
			GL_TEXTURE_2D, level, internalFormat, width, height, 0, bufferSize, dataptr
		    );
		    break;
		default:
		    sLog.exception ("Cannot load texture, unknown format", this->m_header->format);
	    }

	    level++;
	}
    }
}

void CTexture::finishUpload () const {
    if (this->m_pendingUploads.empty ()) {
	return;
    }

    // this may run in the middle of a frame (getTextureID): upload from client memory and put the bindings back
    GLint boundTexture = 0, unpackBuffer = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &boundTexture);
    glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    if (unpackBuffer != 0) {
	glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    }

    // JPEG / PNG / … rows are RGBA8, so whatever GL_UNPACK_ALIGNMENT another texture left reads them the same
    for (auto& pending : this->m_pendingUploads) {
	const DecodedTextureImage image = pending.decoded.get ();
	glBindTexture (GL_TEXTURE_2D, this->m_textureID[pending.image]);
	glTexImage2D (
	    GL_TEXTURE_2D, pending.level, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
	    image.pixels.get ()
	);
    }
    this->m_pendingUploads.clear ();

    if (unpackBuffer != 0) {
	glBindBuffer (GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    }
    glBindTexture (GL_TEXTURE_2D, boundTexture);
}

CTexture::~CTexture () {
    // first release the player to prevent using null references
    this->m_player.reset ();

    if (this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video) {
	glDeleteTextures (1, this->m_textureID);
    } else {
	glDeleteTextures (this->m_header->imageCount, this->m_textureID);
    }

    delete[] this->m_textureID;
}

void CTexture::setupResolution () {
    if (this->isAnimated ()) {
	this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->gifWidth,
			       this->m_header->gifHeight };
    } else {
	if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	    // wpengine-texture format always has one mipmap
	    // get first image size
	    const auto element = this->m_header->images.find (0)->second.begin ();

	    // set the texture resolution
	    this->m_resolution
		= { (*element)->width, (*element)->height, this->m_header->width, this->m_header->height };
	} else {
	    // set the texture resolution
	    this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->width,
				   this->m_header->height };
	}
    }
}

GLint CTexture::setupInternalFormat () const {
    if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	return GL_RGBA8;
    }

    // detect the image format and hand it to openGL to be used
    switch (this->m_header->format) {
	case TextureFormat_DXT5:
	    return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	case TextureFormat_DXT3:
	    return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	case TextureFormat_DXT1:
	    return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case TextureFormat_ARGB8888:
	    return GL_RGBA8;
	case TextureFormat_R8:
	    return GL_R8;
	case TextureFormat_RG88:
	    return GL_RG8;
	default:
	    sLog.exception ("Cannot determine texture format");
    }
}

void CTexture::setupOpenGLParameters (const uint32_t textureID) const {
    // TODO: LABEL ELEMENTS TOO
    // bind the texture to assign information to it
    glBindTexture (GL_TEXTURE_2D, this->m_textureID[textureID]);

    // set mipmap levels
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, this->m_header->images[textureID].size () - 1);

    // setup texture wrapping and filtering
    if (this->m_header->flags & TextureFlags_ClampUVs) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (this->m_header->flags & TextureFlags_NoInterpolation) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

GLuint CTexture::getTextureID (const uint32_t imageIndex) const {
    this->finishUpload ();

    // ensure we do not go out of bounds
    if (imageIndex >= this->m_header->imageCount) {
	return this->m_textureID[0];
    }

    return this->m_textureID[imageIndex];
}

uint32_t CTexture::getTextureWidth (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->getHeader ().textureWidth;
    }

    return (*this->m_header->images[imageIndex].begin ())->width;
}

uint32_t CTexture::getTextureHeight (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->getHeader ().textureHeight;
    }

    return (*this->m_header->images[imageIndex].begin ())->height;
}

uint32_t CTexture::getRealWidth () const {
    return this->isAnimated () ? this->getHeader ().gifWidth : this->getHeader ().width;
}

uint32_t CTexture::getRealHeight () const {
    return this->isAnimated () ? this->getHeader ().gifHeight : this->getHeader ().height;
}

TextureFormat CTexture::getFormat () const { return this->getHeader ().format; }

bool CTexture::isFreeImage () const { return this->getHeader ().freeImageFormat != FIF_UNKNOWN; }

uint32_t CTexture::getFlags () const { return this->getHeader ().flags; }

const Texture& CTexture::getHeader () const { return *this->m_header; }

const glm::vec4* CTexture::getResolution () const { return &this->m_resolution; }

const std::vector<FrameSharedPtr>& CTexture::getFrames () const { return this->getHeader ().frames; }

bool CTexture::isAnimated () const { return this->getHeader ().isAnimated (); }

uint32_t CTexture::getSpritesheetCols () const { return this->getHeader ().spritesheetCols; }

uint32_t CTexture::getSpritesheetRows () const { return this->getHeader ().spritesheetRows; }

uint32_t CTexture::getSpritesheetFrames () const { return this->getHeader ().spritesheetFrames; }

float CTexture::getSpritesheetDuration () const { return this->getHeader ().spritesheetDuration; }

void CTexture::incrementUsageCount () const {
    if (this->m_player) {
	this->m_player->incrementUsageCount ();
    }
}

void CTexture::decrementUsageCount () const {
    if (this->m_player) {
	this->m_player->decrementUsageCount ();
    }
}

void CTexture::update () const {
    if (this->m_player) {
	this->m_player->render ();
    }
}

// CTextures are always ready to be rendered at all times
bool CTexture::isReady () const { return true; }