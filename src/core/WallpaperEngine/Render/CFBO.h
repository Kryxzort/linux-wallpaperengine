#pragma once

#include <string>

#include "TextureProvider.h"

using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render {
class CFBO final : public TextureProvider {
public:
    CFBO (
	std::string name, const TextureFormat format, const uint32_t flags, const float scale, uint32_t realWidth,
	uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight
    );
    ~CFBO () override;

    [[nodiscard]] const std::string& getName () const;
    [[nodiscard]] const float& getScale () const;
    [[nodiscard]] TextureFormat getFormat () const override;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] GLuint getFramebuffer () const;
    [[nodiscard]] GLuint getDepthbuffer () const;
    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] uint32_t getSpritesheetCols () const override;
    [[nodiscard]] uint32_t getSpritesheetRows () const override;
    [[nodiscard]] uint32_t getSpritesheetFrames () const override;
    [[nodiscard]] float getSpritesheetDuration () const override;

    /**
     * Gives the FBO a new size in place (its texture's storage is reallocated and cleared to transparent): passes keep
     * their pointer to it, and to its resolution, so they follow. A text layer's targets change size with its text
     */
    void resize (uint32_t realWidth, uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight);
    /**
     * Gives the FBO a depth buffer (made once; it follows resize): a 3D scene's target, which its models depth test
     * against. Cleared to the far plane
     */
    void attachDepthBuffer () const;
    /**
     * Turns this FBO into a shadow atlas: its texture becomes a 24-bit depth texture (the framebuffer's depth
     * attachment, no colour) that samples as a comparison (sampler2DShadow, GL_LEQUAL, filtered: WE's
     * SamplerComparisonState, inferred), cleared to 1. A resize keeps it one
     */
    void makeShadowAtlas ();
    [[nodiscard]] bool isShadowAtlas () const;

    void incrementUsageCount () const override;
    void decrementUsageCount () const override;
    void update () const override;
    bool isReady () const override;

private:
    GLuint m_framebuffer = GL_NONE;
    mutable GLuint m_depthbuffer = GL_NONE;
    GLuint m_texture = GL_NONE;
    bool m_shadowAtlas = false;
    glm::vec4 m_resolution = {};
    float m_scale = 0;
    std::string m_name = "";
    TextureFormat m_format = TextureFormat_UNKNOWN;
    uint32_t m_flags = TextureFlags_NoFlags;
    /** Placeholder for frames, FBOs only have ONE */
    std::vector<FrameSharedPtr> m_frames = {};
};
} // namespace WallpaperEngine::Render
