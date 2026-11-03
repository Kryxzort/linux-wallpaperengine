#pragma once

#include "WallpaperEngine/Render/Camera.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "linux-wallpaperengine/project.h"

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

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;
    /** The scene's one random source for particles (WE: scene+0x1f08, GH 0x140223090), shared by every system */
    [[nodiscard]] std::mt19937& getParticleRng ();
    [[nodiscard]] const CObject* getObject (int id) const;
    /** The layers scripts index (thisScene.getLayer (n), enumerateLayers): the scene's objects in their order */
    [[nodiscard]] const std::vector<CObject*>& getLayers () const;
    /** IScene.sortLayer: moves a layer to `index` in the layer list, and in the draw order with it */
    bool sortLayer (CObject* layer, int index);
    /** The scene's scripts call `getEffect`: every effect is built, a script may show it (SceneScript IEffect.visible) */
    [[nodiscard]] bool scriptsReachEffects () const;

protected:
    void renderFrame () override;
    void updateMouse ();
    /** Hit-tests the cursor against the layers with cursor handlers and sends their events (SceneScript) */
    void updateCursorEvents ();
    /** Advances every keyframe animation by this frame's dt and writes their values (G01/G02) */
    void advanceAnimations ();
    /** The camera update (WE GH 0x1401b8af0): shake, the active camera object's zoom, general.zoom, the parallax */
    void updateCamera ();

    friend class CWallpaper;

private:
    Render::CObject* createObject (const Object& object);
    Render::CObject* dispatchObjectType (const Object& object);
    void addObjectToRenderOrder (const Object& object);

    std::unique_ptr<Scripting::ScriptEngine> m_scriptEngine;
    std::unique_ptr<Camera> m_camera;
    ObjectUniquePtr m_bloomObjectData;
    CObject* m_bloomObject = nullptr;
    std::map<int, CObject*> m_objects = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    std::vector<CObject*> m_layers = {};
    /** the sound objects, ticked on the host's clock every frame (they don't render) */
    std::vector<Objects::CSound*> m_sounds = {};
    std::vector<DynamicValue*> m_scriptedValues = {};
    std::mt19937 m_particleRng;
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
    glm::vec4 m_pointerState = {};
    bool m_pointerWasDown = false;
    glm::vec2 m_mousePositionNormalized = {};
    /** the smoothed parallax position (WE scene+0x300) in scene units, y up; starts at the centre */
    glm::vec2 m_parallaxPosition = {};
    glm::vec2 m_parallaxPositionNormalized = { 0.5f, 0.5f };
    std::shared_ptr<const CFBO> _rt_4FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_8FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_Bloom = nullptr;
    std::shared_ptr<const CFBO> _rt_shadowAtlas = nullptr;
};
} // namespace WallpaperEngine::Render::Wallpaper
