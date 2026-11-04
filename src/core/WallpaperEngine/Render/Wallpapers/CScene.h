#pragma once

#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Objects/Puppet.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "linux-wallpaperengine/project.h"

#include <map>
#include <optional>
#include <random>

namespace WallpaperEngine::Scripting {
class ScriptEngine;
}
namespace WallpaperEngine::Render {
class Camera;
class CObject;
}

namespace WallpaperEngine::Render::Objects {
class CSound;
class CImage;
}

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::Data::Model;

class CScene final : public CWallpaper {
public:
    CScene (const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext, wp_mouse_input* mouseInput);

    ~CScene () override;

    [[nodiscard]] Scripting::ScriptEngine& getScriptEngine () const;
    [[nodiscard]] Camera& getCamera () const;

    [[nodiscard]] const Scene& getScene () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;

    // Time accessors used by dynamic text layers (CText + ScriptEngine).
    // Read from the application-wide g_Time/g_TimeLast globals that other
    // renderers already consume via extern (e.g. CParticle).
    [[nodiscard]] float getTime () const;
    [[nodiscard]] float getDeltaTime () const;
    [[nodiscard]] float getFps () const;

    /**
     * The size of the screen the scene is shown on, in pixels (the host's wp_project_hint_size; the scene's own size
     * until a host gives one): engine.screenResolution. A change after the scene is up sends the scripts
     * resizeScreen (size) (WE: "every time the wallpaper resizes because of a change to the current screen
     * resolution", never on creation)
     */
    [[nodiscard]] glm::vec2 getScreenSize () const;
    void setScreenSize (int width, int height);
    /**
     * The pointer in screen pixels (IInput.cursorScreenPosition, "the cursor on the screen in pixels"), x right, y
     * down: the host shows the scene covering the screen (scaled to cover, centred, the overflow cut). Inferred: the
     * cover mapping is liascape's, the y direction Windows' screen coordinates
     */
    [[nodiscard]] glm::vec2 getCursorScreenPosition () const;

    const glm::vec2* getMousePosition () const;
    const glm::vec2* getMousePositionLast () const;
    const glm::vec2* getMousePositionNormalized () const;
    /**
     * `g_PointerState` (WE uniform 0x71, GH 0x14010a0e0): xy 1 while the left button is down, z 1 on the frame it went
     * down, w 0
     */
    const glm::vec4* getPointerState () const;
    /** `g_ParallaxPosition`: the smoothed parallax position over the scene size, clamped to 0..1 (0.5 = centre) */
    const glm::vec2* getParallaxPosition () const;
    /**
     * The mouse parallax offset of an object, in scene units (Wallpaper Engine's world: x right, y up), or zero while
     * parallax is off. WE (GH 0x1401b9b50) translates each object's view by (root.origin − parallax position) ·
     * cameraparallaxamount · root.parallaxDepth, where root is the object's topmost ancestor
     */
    [[nodiscard]] glm::vec2 getParallaxOffset (const Object& object) const;
    /**
     * Whether an object is drawn: its own `visible` and, recursively, every ancestor's (WE's "effectively visible",
     * GH 0x1401b50e0: bit 0 of +0x120 AND the parent's at +0x180). A hidden group hides its whole subtree
     */
    [[nodiscard]] bool isEffectivelyVisible (const Object& object) const;

    /**
     * The passthrough (compose) layer that draws this object: its nearest ancestor that is a passthrough image. WE
     * renders a compose layer's subtree into the layer (GH 0x140218260, recursing through GH 0x1402181b0 up to a nested
     * compose layer, which draws its own), after its background copy or clear, and before the layer's effects run
     */
    [[nodiscard]] std::optional<int> composeOwnerOf (const Object& object) const;
    /** composeOwnerOf, when that layer was built and draws its subtree (else the object draws to the scene) */
    [[nodiscard]] std::optional<int> drawingOwnerOf (const Object& object) const;
    /** Whether a passthrough layer draws objects of its own (it has a descendant whose compose owner it is) */
    [[nodiscard]] bool composesChildren (const Object& image) const;
    /** The FBO an object draws its last pass into: its compose layer's group target, or the scene's */
    [[nodiscard]] std::shared_ptr<const CFBO> getTargetFor (const Object& object) const;
    /**
     * Whether an object's last draw leaves its target's alpha alone: the scene's, and a compose layer's that starts
     * from the background (opaque). A compose layer that starts transparent takes its subtree's alpha as coverage
     */
    [[nodiscard]] bool targetKeepsAlpha (const Object& object) const;
    /** A compose layer's group target (scene-sized, made on first use): its background (or transparent) + subtree */
    [[nodiscard]] std::shared_ptr<const CFBO> getComposeTarget (int id);
    /** Draws the objects a compose layer owns, in draw order, into `target` */
    void renderComposed (int ownerId, const CFBO& target);

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;

    /** An object's own matrix in WE's world: T (origin) · Rz · Ry · Rx · S (GH 0x1401b5170 / 0x14020af40) */
    [[nodiscard]] static glm::mat4 localMatrixOf (const Object& object);
    /** An object's matrix in WE's world with its parents' (a 3D scene's models, image layers and lights) */
    [[nodiscard]] glm::mat4 worldMatrixOf (const Object& object) const;
    /** Placed in a 3D scene's world (a model, a particle system, an image that isn't full-screen) */
    [[nodiscard]] bool drawsInWorld (const CObject& object) const;

    /**
     * A 3D scene's lights this frame, packed as the generated PerformLighting_V1 reads them (GH 0x1401bf270): the
     * point and spot lights (`count`), then the tubes (`tubes`) in origin … extra; the projections of the lights that
     * cast shadows or project a cookie; the directional lights
     */
    /** The shadow quality (LIGHTS_SHADOW_MAPPING_QUALITY): WE's "shadows" setting's default, medium (GH 0x14013f3f0) */
    static constexpr int ShadowQuality = 2;
    struct SceneLights {
	static constexpr int Max = 16;
	static constexpr int MaxProjections = 16;
	static constexpr int MaxDirectional = 3;
	int count = 0;
	int tubes = 0;
	glm::vec4 origin[Max] = {};
	glm::vec4 color[Max] = {};
	glm::vec4 direction[Max] = {};
	glm::vec4 extra[Max] = {};
	int projections = 0;
	glm::mat4 projection[MaxProjections] = {};
	glm::vec4 projectionTransform[MaxProjections] = {};
	glm::vec4 pointProjection[MaxProjections] = {};
	int directional = 0;
	glm::vec4 directionalColor[MaxDirectional] = {};
	glm::vec4 directionalDirection[MaxDirectional] = {};
	/** the shadow atlas's 1 / size and size (g_Texture6Texel: SHADOW_ATLAS_TEXEL) */
	glm::vec4 atlasTexel = glm::vec4 (0.5f, 0.5f, 2.0f, 2.0f);
    };
    [[nodiscard]] const SceneLights& getLights () const;
    /**
     * One viewport of the shadow pass: its rect in the atlas (pixels, x y w h) and the clip matrix that draws a
     * caster there, so that the lit materials' header (CalculateProjectedCoords / …Point, common_pbr_2.h) finds it
     */
    struct ShadowView {
	glm::vec4 rect;
	glm::mat4 clip;
    };
    /** This frame's shadow viewports (empty when no light casts a shadow) */
    [[nodiscard]] const std::vector<ShadowView>& getShadowViews () const;
    /** A light in the scene has castshadow: its models build their shadow casters and sample the atlas */
    [[nodiscard]] bool castsShadows () const;
    /** The cookie of the scene's spot lights with usecookie (the last one's: WE binds one, scene+0x1310), or null */
    [[nodiscard]] std::shared_ptr<const TextureProvider> getLightCookie () const;
    /** How many directional lights the scene has (LIGHTS_DIRECTIONAL, at most 3) */
    [[nodiscard]] int directionalLightCount () const;
    /** The shadow atlas (a depth texture, grown as the shadow lights need) */
    [[nodiscard]] std::shared_ptr<const CFBO> getShadowAtlas () const;
    /** The scene's one random source for particles (WE: scene+0x1f08, GH 0x140223090), shared by every system */
    [[nodiscard]] std::mt19937& getParticleRng ();
    [[nodiscard]] const CObject* getObject (int id) const;
    /** The layers scripts index (thisScene.getLayer (n), enumerateLayers): the scene's objects in their order */
    [[nodiscard]] const std::vector<CObject*>& getLayers () const;
    /**
     * ILayer.setParent: `object`'s parent becomes `parent` (none: a root layer), at its puppet `attachment` if one is
     * named; its local transform stays as it is (the script adjusts it to keep the world one)
     */
    void setParentOf (CObject& object, CObject* parent, const std::optional<std::string>& attachment);
    /**
     * IScene.createLayer: builds a layer from a scene object's JSON (`config`, as scene.json writes one; its `id` is
     * given here), last in the layer list and drawn last (before the bloom). nullptr when it can't be built
     */
    CObject* createLayer (const std::string& config);

    /**
     * IScene.createModelData (F4): geometry a script built, as an MDL model's meshes (the loader's Puppet::Model).
     * A model layer names it `__modeldata/<id>` (createLayer ({ model: modelData })). `version` counts applyData /
     * replaceData; `rebuilt` the last replaceData (shapes or materials may have changed)
     */
    struct ScriptModel {
	Objects::Puppet::Model model;
	/** per shape its vertexFormat (IModelData.POSITION, NORMAL, TANGENT_SIGNED, UV, COLOR in that order) */
	std::vector<std::vector<std::string>> formats;
	uint32_t version = 0;
	uint32_t rebuilt = 0;
    };
    int createScriptModel (Objects::Puppet::Model model);
    [[nodiscard]] ScriptModel* scriptModel (int id);
    bool destroyScriptModel (int id);
    /** IScene.destroyLayer: the layer leaves the layer list and the draw order (it is freed with the scene) */
    bool destroyLayer (CObject* layer);
    /** Whether a layer is still in the scene (not destroyed: engine.isObjectValid) */
    [[nodiscard]] bool isLayerAlive (const CObject* layer) const;
    /** IScene.sortLayer: moves a layer to `index` in the layer list, and in the draw order with it */
    bool sortLayer (CObject* layer, int index);
    /** The scene's scripts call `getEffect`: every effect is built, a script may show it (SceneScript IEffect.visible) */
    [[nodiscard]] bool scriptsReachEffects () const;
    /**
     * WE renders this scene HDR: `general.hdr`, with WE's post-processing at Ultra (renderer flag 0x2000, set from the
     * "postprocessing" setting "ultra", GH 0x14013f860). LWE has no quality setting: on unless WPENGINE_HDR=0.
     * Its targets are half float, its bloom is the HDR chain
     */
    [[nodiscard]] bool rendersHDR () const;

protected:
    void renderFrame () override;
    void updateMouse ();
    /** Hit-tests the cursor against the layers with cursor handlers and sends their events (SceneScript) */
    void updateCursorEvents ();
    /** Advances every keyframe animation by this frame's dt and writes their values (G01/G02) */
    void advanceAnimations ();
    /** The camera update (WE GH 0x1401b8af0): shake, the active camera object's zoom, general.zoom, the parallax */
    void updateCamera ();
    /**
     * Camera paths (GH 0x1401b8af0): the frame's sample of `camera.paths`, advanced by dt; false without paths.
     * Sets m_fadeAlpha (camerafade, GH 0x1401afbf0)
     */
    bool samplePath (float dt, SceneData::Camera::PathKey& out);
    /** camerafade's overlay: fade.json's colour (the `schemecolor` property · 0.7) at m_fadeAlpha over the scene */
    void renderFade ();
    /** Packs the 3D scene's lights for this frame (getLights), and lays out the shadow atlas (getShadowViews) */
    void updateLights ();
    /** WE's shadow pass (GH 0x1401c4f30): the casters into each shadow viewport of the atlas */
    void renderShadows ();
    /** The order objects draw in this frame: the render order, a 3D scene's world objects sorted by `transparentsorting` */
    const std::vector<CObject*>& drawOrder ();

    friend class CWallpaper;

private:
    /** the host's screen size (0: none given) */
    glm::ivec2 m_screenSize = { 0, 0 };
    Render::CObject* createObject (const Object& object);
    Render::CObject* dispatchObjectType (const Object& object);
    void addObjectToRenderOrder (const Object& object);
    /** Builds the bloom image: WE's LDR chain, or the HDR chain of `m_hdrLevels` levels when rendersHDR */
    void setupBloom (const Scene& scene, uint32_t sceneWidth, uint32_t sceneHeight);
    /** WPENGINE_HEALTH: the scene and one `object` record per scene object (built or not), after the build */
    void reportHealthBuild () const;
    /** WPENGINE_HEALTH: one `layers` record per drawable object (visibility, alpha, canvas bbox, finite transform) */
    void reportHealthLayers (int frame) const;
    /** Points the HDR bloom image's passes at their inputs (tap offsets, strength, knee, tint, scatter) */
    void connectHDRBloom (Objects::CImage& image);
    /** The HDR chain's live inputs (GH 0x1401b42b0), from general's bloomhdr* values */
    void updateHDRBloom ();

    std::unique_ptr<Scripting::ScriptEngine> m_scriptEngine;
    std::unique_ptr<Camera> m_camera;
    ObjectUniquePtr m_bloomObjectData;
    CObject* m_bloomObject = nullptr;
    std::map<int, CObject*> m_objects = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    std::vector<CObject*> m_layers = {};
    /** the data of the layers scripts created (createLayer), owned here as the scene owns its own */
    std::vector<ObjectUniquePtr> m_createdObjects = {};
    /** the next id a created layer gets (above every id in the scene) */
    int m_nextCreatedId = 0;
    std::map<int, ScriptModel> m_scriptModels = {};
    int m_nextScriptModel = 1;
    /** layers a script destroyed: out of the lists, freed with the scene */
    std::vector<CObject*> m_destroyed = {};
    /** drawOrder's sorted copy of the render order */
    std::vector<CObject*> m_sortedOrder = {};
    SceneLights m_lights = {};
    std::vector<ShadowView> m_shadowViews = {};
    bool m_castsShadows = false;
    int m_directionalCount = 0;
    std::shared_ptr<const TextureProvider> m_lightCookie = nullptr;
    std::shared_ptr<CFBO> m_shadowAtlas = nullptr;
    /** the sound objects, ticked on the host's clock every frame (they don't render) */
    std::vector<Objects::CSound*> m_sounds = {};
    std::vector<DynamicValue*> m_scriptedValues = {};
    std::mt19937 m_particleRng;
    /** frames rendered while WPENGINE_HEALTH is on (layers are reported on the 1st and the 31st) */
    int m_healthFrames = 0;
    /** composeOwnerOf, by object id (built on first use from the scene data) */
    mutable std::map<int, std::optional<int>> m_composeOwners = {};
    mutable bool m_composeOwnersBuilt = false;
    std::map<int, std::shared_ptr<CFBO>> m_composeTargets = {};
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
    glm::vec4 m_pointerState = {};
    bool m_pointerWasDown = false;
    glm::vec2 m_mousePositionNormalized = {};
    /** the smoothed parallax position (WE scene+0x300) in scene units, y up; starts at the centre */
    glm::vec2 m_parallaxPosition = {};
    /** the camera paths' playback (scene +0xe4 path, +0xe8 key, +0xec time) */
    size_t m_pathIndex = 0;
    size_t m_pathKey = 0;
    float m_pathTime = 0.0f;
    float m_fadeAlpha = 0.0f;
    GLuint m_fadeProgram = 0;
    glm::vec2 m_parallaxPositionNormalized = { 0.5f, 0.5f };
    std::shared_ptr<const CFBO> _rt_4FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_8FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_Bloom = nullptr;
    std::shared_ptr<const CFBO> _rt_shadowAtlas = nullptr;
    bool m_hdr = false;
    /** The HDR chain's targets, _rt_2FrameBuffer … (scene size / (2 << level)), and how many levels run */
    std::vector<std::shared_ptr<const CFBO>> m_hdrTargets = {};
    int m_hdrLevels = 0;
    /** The scene size the tap offsets are relative to (WE's renderer +0x84/+0x88) */
    glm::vec2 m_hdrSize = {};
    /** Per HDR pass (downsamples, upsamples), its g_RenderVar0: the four taps' offsets */
    std::vector<std::unique_ptr<glm::vec4>> m_hdrRenderVars = {};
    float m_hdrStrength = 0.0f;
    glm::vec4 m_hdrBlend = {};
    glm::vec3 m_hdrTint = {};
    float m_hdrScatter = 0.0f;
};
} // namespace WallpaperEngine::Render::Wallpaper
