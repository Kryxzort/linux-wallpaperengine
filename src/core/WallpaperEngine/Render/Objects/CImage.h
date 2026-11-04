#pragma once

#include <optional>

#include "CRenderable.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <glm/vec3.hpp>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;
namespace WallpaperEngine::Render::Objects::Effects {
class CMaterial;
class CPass;
} // namespace WallpaperEngine::Render::Objects::Effects

namespace WallpaperEngine::Render::Objects {
class CImage : public CRenderable, public ScriptableObject {
    friend CObject;

public:
    CImage (Wallpapers::CScene& scene, const Image& image);
    ~CImage () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const Image& getImage () const;
    /** A passthrough (compose) layer built with a group target: it draws its subtree into it (CScene::renderComposed) */
    [[nodiscard]] bool drawsSubtree () const;
    [[nodiscard]] glm::vec2 getSize () const;

    /**
     * Whether a point of the scene (WE's world: x right, y up, scene pixels) is on this layer as it was last drawn:
     * its quad with its parents' transform, rotation and mouse parallax (the camera's zoom is not undone).
     * `local` gets the point in the layer, 0..size (CursorEvent.localPosition), y up
     */
    [[nodiscard]] bool hitTest (const glm::vec2& world, glm::vec2& local) const;

    [[nodiscard]] GLuint getSceneSpacePosition () const;
    [[nodiscard]] GLuint getCopySpacePosition () const;
    [[nodiscard]] GLuint getPassSpacePosition () const;
    [[nodiscard]] GLuint getTexCoordCopy () const;
    [[nodiscard]] GLuint getTexCoordPass () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

    /**
     * Performs a ping-pong on the available framebuffers to be able to continue rendering things to them
     *
     * @param drawTo The framebuffer to use
     * @param asInput The last texture used as output (if needed)
     */
    void pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput);

protected:
    /**
     * Gives the layer a new size: its composite FBOs and its effects' FBOs (at their scale) are resized in place, so the
     * passes built for them keep working. A text layer's size follows its text (CText)
     */
    void resizeTargets (const glm::vec2& size);
    /**
     * Places the quad's centre at this offset from the origin, in the layer's own pixels (WE's y up; scaled and turned
     * with the layer) instead of by `alignment`: a text layer's offset comes from its laid-out block (GH 0x14027c600)
     */
    void setLocalOffset (const glm::vec2& offset);

    /**
     * Fills the group target: the background (copybackground, or `copyBackground`; else transparent), then the
     * objects this layer owns
     */
    void renderComposeTarget (bool copyBackground);
    void setupPasses ();
    /** The passes that run (the effects' while visible), their blending and targets set up for that chain */
    void updateActivePasses ();
    /** An effect's `visible` differs from the one the active chain was set up for */
    [[nodiscard]] bool effectVisibilityChanged () const;
    /** Visible now, or a property or script can make it visible (also a layer script's getEffect ()): it is built */
    [[nodiscard]] bool effectCanBeVisible (const ImageEffect& effect) const;

    void updateScreenSpacePosition ();

private:
    bool loadPuppetMesh (const glm::vec2& size);
    void updatePuppetPositionBuffer (const glm::vec2& size);
    void setupPuppetGeometryCallback (Effects::CPass* pass) const;
    ResolvedTransform updateGeometryBuffers ();
    [[nodiscard]] glm::vec2 resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const;
    void updateScenePosition (
	const glm::vec3& origin, const glm::vec2& size, const glm::vec3& scale, float sceneWidth, float sceneHeight
    );
    void uploadGeometryBuffers (const glm::vec2& size);
    [[nodiscard]] bool shouldRenderFinalPass (bool isLastPass) const;
    bool configurePassTarget (
	Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo,
	const std::shared_ptr<const TextureProvider>& asInput, std::shared_ptr<const TextureProvider>& effectInput,
	bool& inTargetEffectSequence
    );

    GLuint m_sceneSpacePosition;
    GLuint m_copySpacePosition;
    GLuint m_passSpacePosition;
    GLuint m_texcoordCopy;
    GLuint m_texcoordPass;
    GLuint m_puppetSpacePosition = GL_NONE;
    GLuint m_puppetTexCoord = GL_NONE;
    GLuint m_puppetIndices = GL_NONE;
    GLsizei m_puppetIndexCount = 0;
    bool m_hasPuppetMesh = false;
    std::vector<GLfloat> m_puppetRawPositions = {};

    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionPass = {};
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelViewProjectionPassInverse = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};

    glm::mat4 m_modelMatrix = {};
    glm::mat4 m_viewProjectionMatrix = {};

    /**
     * A passthrough layer's group target: the background behind it (copybackground, WE's default) or transparent, then
     * the objects it owns; its base pass reads this instead of _rt_FullFrameBuffer. Null for every other layer
     */
    std::shared_ptr<const CFBO> m_composeTarget = nullptr;
    std::shared_ptr<const CFBO> m_mainFBO = nullptr;
    std::shared_ptr<const CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    const Image& m_image;

    std::vector<Effects::CPass*> m_passes = {};
    /** The effect each of m_passes belongs to, nullptr for a pass that always runs */
    std::vector<const ImageEffect*> m_passEffects = {};
    /** The blending each of m_passes was built with */
    std::vector<BlendingMode> m_passBlending = {};
    /** The m_passes that run, in order */
    std::vector<Effects::CPass*> m_activePasses = {};
    /** Per built effect, its `visible` when m_activePasses was set up */
    std::vector<bool> m_effectVisibility = {};
    int m_visibleEffects = 0;
    std::vector<MaterialPassUniquePtr> m_virtualPassess = {};

    glm::vec4 m_pos = {};
    glm::vec3 m_sceneCenter = {};
    glm::vec3 m_scenePivot = {};
    glm::vec2 m_size = {};
    /** setLocalOffset's offset; unset: placed by `alignment` */
    std::optional<glm::vec2> m_localOffset = std::nullopt;
    /** The FBOs sized after the layer, with their scale (the composite pair: 1), for resizeTargets */
    std::vector<std::pair<std::shared_ptr<CFBO>, float>> m_sizedTargets = {};

    bool m_initialized = false;
    /** (color.rgb, alpha) for g_Color4, refreshed by getColor4 () — every render () */
    mutable glm::vec4 m_color4 = glm::vec4 (1.0f);

    struct {
	struct {
	    MaterialUniquePtr material;
	    ImageEffectPassOverrideUniquePtr override;
	} colorBlending;
	std::vector<MaterialUniquePtr> compatibilityMaterials = {};
	std::vector<ImageEffectPassOverrideUniquePtr> compatibilityOverrides = {};
    } m_materials;
};
} // namespace WallpaperEngine::Render::Objects
