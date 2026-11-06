#pragma once
#include <map>
#include <optional>

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
    /** A `perspective` layer of an orthographic scene: drawn through Camera::getPerspective2DViewProjection */
    [[nodiscard]] bool isPerspectiveLayer () const;

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
     * A puppet attachment's matrix in this layer at the current pose, in the layer's own pixels (y up, before the
     * layer's scale and angle): the bone's posed world · the MDAT matrix, whole (GH 0x14024b530, the puppet's
     * vtable +0x78: local ← (bone · MDAT) · local; the bone is the animator's +0x30 array, the posed bone world in the
     * model's space: GH 0x140243600 composes it parent-first, the skinning product goes to +0x90). False when there's
     * none by that name (G09)
     */
    [[nodiscard]] bool attachmentMatrix (const std::string& name, glm::mat4& matrix);
    /** The puppet's attachment names, in the model's order (none without a puppet) */
    [[nodiscard]] std::vector<std::string> attachmentNames () const;
    /** IImageLayer.alignment from a script: where the quad sits on the origin, from the next frame */
    void setAlignment (const std::string& alignment);

    /** One puppet animation layer as it plays (SceneScript's IAnimationLayer) */
    struct PuppetLayerState {
	Puppet::Clock clock;
	bool blendIn = false;
	/** its settings: rate, visible, blend, the animation (the scene's, or one a script created) */
	const ImageAnimationLayer* layer = nullptr;
	/** the scripts' handle on it (stays while layers before it go) */
	int key = 0;
	std::string name;
	/** pause () / stop () hold the clock */
	bool playing = true;
	/** playSingleAnimation: plays once, then the layer is removed */
	bool single = false;
	/** its clock went once (the next step's events start after it, not at frame 0) */
	bool started = false;
    };
    /** the puppet's animation layers in order (none without a skinned puppet) */
    [[nodiscard]] std::vector<PuppetLayerState>& getPuppetLayers () { return this->m_puppetLayers; }
    [[nodiscard]] PuppetLayerState* findPuppetLayer (int key);
    /** the animation a layer plays, nullptr if the puppet has none of that id */
    [[nodiscard]] const Puppet::Animation* puppetAnimationOf (const PuppetLayerState& state) const;
    /**
     * createAnimationLayer / playSingleAnimation: a new layer, last, playing the puppet animation named `name` (or of
     * that id), rate/blend 1, visible. Its key, or -1 when the puppet has no such animation
     */
    int createPuppetLayer (const std::string& name, int id, bool single);
    bool destroyPuppetLayer (int key);
    /** The puppet's bones (none without a puppet) */
    [[nodiscard]] const std::vector<Puppet::Bone>& getPuppetBones () const { return this->m_puppet.bones; }
    /** The posed bones now: local (the pose with the overrides) and world (in the mesh's space: centred, y up) */
    void puppetBonePose (std::vector<glm::mat4>& local, std::vector<glm::mat4>& world);
    /** A script's local matrix for a bone, over what the animations pose (nullopt: back to the animations) */
    void setBoneOverride (int bone, const std::optional<glm::mat4>& local);
    /** The puppet's blend shapes (MDMP targets, every mesh's in order): their names */
    [[nodiscard]] std::vector<std::string> blendShapeNames () const;
    /** A blend shape's weight now (the animations' mix, or the script's), 0 when there's no such shape */
    [[nodiscard]] float blendShapeWeight (int index);
    /** A script's weight for a blend shape, over the animations' (nullopt: back to them) */
    void setBlendShapeWeight (int index, const std::optional<float>& weight);
    /** a script changed an animation layer (its frame, its blend): the pose follows now */
    void puppetPoseChanged () { this->m_puppetPoseDirty = true; }

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
    /**
     * A shape (ImageData::shape): its quad's corners in the scene, this frame — the first effect's point0..point3
     * (GH 0x1402861b0; the unit square without them) on the layer's square, as two triangles (0 2 1, 0 3 2)
     */
    void updateShapeBuffers ();
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
    /** a shape's quad: scene positions and its corners as texture coordinates (6 vertices) */
    GLuint m_shapeScenePosition = GL_NONE;
    GLuint m_shapeTexCoord = GL_NONE;
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
    /** the IK targets' bind keys (Puppet::targetBindPose) */
    std::vector<Puppet::Key> m_puppetTargetBind = {};
    std::vector<glm::mat4> m_puppetBindWorldInverse = {};
    std::vector<glm::mat4> m_puppetWorld = {};
    std::vector<PuppetLayerState> m_puppetLayers = {};
    /** animation layers scripts created (createAnimationLayer, playSingleAnimation) */
    std::vector<std::unique_ptr<ImageAnimationLayer>> m_createdAnimationLayers = {};
    int m_nextAnimationLayerKey = 0;
    /** the pose mixed last (local, per bone), before the bone overrides */
    std::vector<Puppet::Key> m_puppetPose = {};
    /** bone poses a script set (setLocalBoneTransform …): the bone's local matrix, over the animations */
    std::map<int, glm::mat4> m_boneOverrides = {};
    float m_puppetPoseTime = -1.0f;
    /** a script changed a layer or a bone: pose again this frame (with no time passing) */
    bool m_puppetPoseDirty = false;
    bool m_puppetSkinned = false;
    /** a mesh has blend shapes (MDMP): posed even without animations */
    bool m_puppetMorphed = false;
    /** the animations carry bone alphas or the targets alphas: the first pass gets VERTEXCOLOR and a_Color */
    bool m_puppetVertexAlpha = false;
    /** per vertex of m_puppetVertices, its mesh */
    std::vector<uint16_t> m_puppetVertexMesh = {};
    /** the blend shapes' (mesh, target), in blendShapeNames' order */
    std::vector<std::pair<size_t, size_t>> m_puppetShapes = {};
    /** per mesh, per target: the weights mixed last (with the script's) */
    std::vector<std::vector<float>> m_puppetShapeWeights = {};
    /** blend-shape weights a script set, by blendShapeNames' index */
    std::map<int, float> m_shapeOverrides = {};
    /** per vertex: (1, 1, 1, bone alpha · morph alpha) when m_puppetVertexAlpha */
    std::vector<GLfloat> m_puppetColors = {};
    GLuint m_puppetColor = GL_NONE;
    glm::vec2 m_puppetSize = glm::vec2 (0.0f);

    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionPass = {};
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelViewProjectionPassInverse = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};

    glm::mat4 m_modelMatrix = {};
    /** g_LayerModelMatrix (CPass::setLayerModelMatrix) */
    glm::mat4 m_layerModelMatrix = glm::mat4 (1.0f);
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
    /** the `command: swap` passes after the last draw (the fluid simulation's ping-pong): run after it, so the draw
     *  before them is the one that reaches the scene */
    std::vector<Effects::CPass*> m_trailingSwaps = {};
    /** the last draw that puts a fogged layer into a 3D scene (effectpassthrough_4 with FOG_COMPUTED), or null */
    Effects::CPass* m_fogPass = nullptr;
    /** Per built effect, its `visible` when m_activePasses was set up */
    std::vector<bool> m_effectVisibility = {};
    int m_visibleEffects = 0;
    std::vector<MaterialPassUniquePtr> m_virtualPassess = {};

    /**
     * A simulation effect's clock (cursorripple): the passes that carry its state from frame to frame (they read one
     * of the effect's FBOs before this frame writes it, and never the layer's picture) step at the frame rate the host
     * means to draw at (wp_project_set_frame_rate: WE's fps setting), not once per frame actually drawn. Their shaders
     * count per frame — a force per frame from the pointer's move that frame, scaled by the frame's time; a decay of at
     * least 1/255 per frame in an rgba8888 buffer — so in WE the water's strength depends on its fps, and a frame that
     * came late (a throttled scene) pushed several frames' worth of force at once. A late frame runs several steps, an
     * early one none; the passes that draw the effect's picture run every frame. No rate set: one step per frame
     */
    struct SimulationClock {
	/** the host's renderTime the clock last saw (a frame drawn twice at one time steps once) */
	float seenTime = -1.0f;
	/** time towards the next step */
	float phase = 0.0f;
	/** steps this frame */
	int steps = 0;
	/** where the pointer was at the last step, and the frame's, for the steps between */
	glm::vec2 from = {};
	glm::vec2 to = {};
	/** CScene::getPointerJumps when last seen: a jump moves `from` to where the pointer landed */
	uint64_t jumps = 0;
	/** a press seen on a frame without a step (WE's z marks the press on the frame that reads it) */
	bool pressed = false;
	/** the stepping passes' g_Frametime, g_PointerPosition, g_PointerPositionLast, g_PointerState for the step */
	float frametime = 0.0f;
	glm::vec2 pointer = {};
	glm::vec2 pointerLast = {};
	glm::vec4 pointerState = {};
    };
    SimulationClock m_simulation;
    /** The m_passes on m_simulation's clock */
    std::vector<const Effects::CPass*> m_simulationPasses = {};
    /** Brings m_simulation to this frame: how many steps the stepping passes run */
    void advanceSimulationClock ();
    /** Sets the stepping passes' uniforms for step `step` (0-based) of this frame's m_simulation.steps */
    void enterSimulationStep (int step);
    [[nodiscard]] bool isSimulationPass (const Effects::CPass* pass) const;

    glm::vec4 m_pos = {};
    glm::vec3 m_sceneCenter = {};
    glm::vec3 m_scenePivot = {};
    glm::vec2 m_size = {};
    /** setLocalOffset's offset; unset: placed by `alignment` */
    std::optional<glm::vec2> m_localOffset = std::nullopt;
    /** The FBOs sized after the layer, with their scale (the composite pair: 1), for resizeTargets */
    struct SizedTarget {
	std::shared_ptr<CFBO> fbo;
	float scale = 1.0f;
	uint32_t fit = 0;
	uint32_t width = 0;
	uint32_t height = 0;
    };
    std::vector<SizedTarget> m_sizedTargets = {};

    bool m_initialized = false;
    /** (color.rgb, alpha) for g_Color4, refreshed by getColor4 () — every render () */
    mutable glm::vec4 m_color4 = glm::vec4 (1.0f);
    /** g_Color: the colour, × brightness in an HDR scene (GH 0x1402321d0), refreshed by getColor4 () */
    mutable glm::vec3 m_drawColor = glm::vec3 (1.0f);

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
