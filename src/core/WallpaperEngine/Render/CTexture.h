#pragma once

#include "Helpers/ContextAware.h"
#include "TextureProvider.h"
#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <glad/glad.h>
#include <cstdlib>
#include <future>
#include <glm/vec4.hpp>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <vector>

namespace WallpaperEngine::Render {
class RenderContext;
using namespace WallpaperEngine::Data::Assets;
using namespace WallpaperEngine::VideoPlayback::MPV;
/** A JPEG / PNG / … mipmap decoded to RGBA8 by stb_image (null: it refused the file), freed as stbi_image_free does */
struct DecodedTextureImage {
    std::unique_ptr<unsigned char, void (*) (void*)> pixels { nullptr, std::free };
    int width = 0;
    int height = 0;
};
/**
 * A normal texture file in WallpaperEngine's format
 */
class CTexture final : public TextureProvider, public Helpers::ContextAware {
public:
    /**
     * @param deferUpload decode a JPEG / PNG / … texture on a pool thread and upload it in finishUpload instead of
     * here (the texture names and parameters are still set up here)
     */
    explicit CTexture (RenderContext& context, TextureUniquePtr header, bool deferUpload = false);
    ~CTexture () override;

    /**
     * Uploads the mipmaps still decoding (waits for them), leaving the GL bindings as they were; a no-op once done.
     * getTextureID calls it, so nothing can sample a texture before its pixels are in
     */
    void finishUpload () const;

    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] TextureFormat getFormat () const override;
    /** a .tex that holds a JPEG / PNG / … (FreeImage): uploaded as RGBA8 whatever its format field says */
    [[nodiscard]] bool isFreeImage () const;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] uint32_t getSpritesheetCols () const override;
    [[nodiscard]] uint32_t getSpritesheetRows () const override;
    [[nodiscard]] uint32_t getSpritesheetFrames () const override;
    [[nodiscard]] float getSpritesheetDuration () const override;

    /**
     * Increments the usage count of the texture
     *
     * Directly controls playback for video CTextures, only started when at least one thing is using it
     * Initializes mpv if needed and starts playback
     */
    void incrementUsageCount () const override;
    /**
     * Decrements the usage count of the texture
     *
     * Directly controls playback for video CTextures, only stopped when nothing is using it
     * De-initializes mpv if needed
     */
    void decrementUsageCount () const override;
    /**
     * Some textures need to be updated
     */
    void update () const override;
    bool isReady () const override;
    [[nodiscard]] GLPlayer* getVideoPlayer () const override { return this->m_player.get (); }

private:
    /**
     * @return The texture header
     */
    [[nodiscard]] const Texture& getHeader () const;

    /**
     * Calculate's texture's resolution vec4
     */
    void setupResolution ();
    /**
     * Determines the texture's internal storage format
     */
    GLint setupInternalFormat () const;
    /**
     * Prepares openGL parameters for loading texture data
     */
    void setupOpenGLParameters (uint32_t textureID) const;

    /** The texture header */
    TextureUniquePtr m_header;
    /** OpenGL's texture ID */
    GLuint* m_textureID = nullptr;
    /** Resolution vector of the texture */
    glm::vec4 m_resolution {};
    /** The video player in use */
    GLPlayerUniquePtr m_player;

    struct PendingUpload {
	uint32_t image;
	GLint level;
	std::future<DecodedTextureImage> decoded;
    };
    /** Deferred mipmaps, in the order the constructor would have uploaded them */
    mutable std::vector<PendingUpload> m_pendingUploads;
};
} // namespace WallpaperEngine::Assets