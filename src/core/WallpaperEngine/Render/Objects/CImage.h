#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "CRenderable.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "Puppet.h"

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
    /** In a 3D scene's world (drawn with its world matrix and the perspective camera), not over the screen */
    [[nodiscard]] bool isWorldPlaced () const;

    /**
     * Whether a point of the scene (WE's world: x right, y up, scene pixels) is on this layer as it was last drawn:
     * its quad with its parents' transform, rotation and mouse parallax (the camera's zoom is not undone).
     * `local` gets the point in the layer, 0..size (CursorEvent.localPosition), y up
     */
    [[nodiscard]] bool hitTest (const glm::vec2& world, glm::vec2& local) const;
    /**
     * WPENGINE_HEALTH: where the layer's quad lands this frame — its canvas bbox (scene px, x right, y down, from the
     * quad's corners through the screen MVP), the part of it on the canvas, alpha, and whether the transform is finite
     */
    [[nodiscard]] nlohmann::json healthLayer () const;

    [[nodiscard]] GLuint getSceneSpacePosition () const;
    [[nodiscard]] GLuint getCopySpacePosition () const;
    [[nodiscard]] GLuint getPassSpacePosition () const;
    [[nodiscard]] GLuint getTexCoordCopy () const;
    [[nodiscard]] GLuint getTexCoordPass () const;
    /** Every pass built for the image: its base pass, then its effects' passes in order (shown or not) */
    [[nodiscard]] const std::vector<Effects::CPass*>& getPasses () const;

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

    /**
     * A puppet attachment's place in this layer at the current pose: its offset from the layer's origin in the
     * layer's own pixels (y up, before the layer's scale and angle) and its z angle. False when there's none by that
     * name (G09; the attachment is the bone's posed world · the MDAT matrix)
     */
    [[nodiscard]] bool attachmentTransform (const std::string& name, glm::vec2& offset, float& angle);
    /** The puppet's attachment names, in the model's order (none without a puppet) */
    [[nodiscard]] std::vector<std::string> attachmentNames () const;
    /** IImageLayer.alignment from a script: where the quad sits on the origin, from the next frame */
    void setAlignment (const std::string& alignment);

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
    /** A 3D scene's layer: its quad in its own space and its world matrix through the perspective camera */
    void updateWorldPosition ();
    /** The view-projection of a layer over the screen: the camera's, a 3D scene's 2D one */
    [[nodiscard]] glm::mat4 screenViewProjection () const;

private:
    bool loadPuppetMesh (const glm::vec2& size);
    void updatePuppetPositionBuffer (const glm::vec2& size);
    /** Poses the puppet for the scene's current time (once per time): advances the layers' clocks, mixes them and
     *  skins the mesh */
    void updatePuppetPose ();
    /** autosize: the canvas grown (symmetrically) to hold the mesh in every frame of every animation */
    [[nodiscard]] glm::vec2 puppetCanvasSize (const glm::vec2& size) const;
    /** `toScene`: the pass draws to the scene (no effects), with the mesh in the layer's scene rectangle */
    void setupPuppetGeometryCallback (Effects::CPass* pass, bool toScene) const;
    void updatePuppetScenePositionBuffer ();
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
    GLuint m_puppetScenePosition = GL_NONE;
    GLuint m_puppetTexCoord = GL_NONE;
    GLuint m_puppetIndices = GL_NONE;
    GLsizei m_puppetIndexCount = 0;
    bool m_hasPuppetMesh = false;
    /** the puppet's vertices (every mesh's, in order), as posed last; y up, centred */
    std::vector<GLfloat> m_puppetRawPositions = {};
    std::vector<Puppet::Vertex> m_puppetVertices = {};
    Puppet::Model m_puppet = {};
    std::vector<Puppet::Key> m_puppetBind = {};
    std::vector<glm::mat4> m_puppetBindWorldInverse = {};
    std::vector<glm::mat4> m_puppetWorld = {};
    struct PuppetLayerState {
	Puppet::Clock clock;
	bool blendIn = false;
    };
    std::vector<PuppetLayerState> m_puppetLayers = {};
    float m_puppetPoseTime = -1.0f;
    bool m_puppetSkinned = false;
    glm::vec2 m_puppetSize = glm::vec2 (0.0f);

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
