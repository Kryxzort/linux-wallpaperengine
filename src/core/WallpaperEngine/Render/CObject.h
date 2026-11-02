#pragma once

#include <string>

#include "WallpaperEngine/Render/Helpers/ContextAware.h"

#include "WallpaperEngine/Render/Wallpapers/CScene.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
class CObject : public Helpers::ContextAware, public TypeCaster {
public:
    CObject (Wallpapers::CScene& scene, const Object& object);
    virtual ~CObject () override = default;

    virtual void setup ();
    virtual void render ();

    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] const Assets::AssetLocator& getAssetLocator () const;
    [[nodiscard]] int getId () const;
    [[nodiscard]] const Object& getObject () const;

    /** An object's origin, scale and z angle in the scene (Wallpaper Engine's world: x right, y up) */
    struct ResolvedTransform {
	glm::vec3 origin;
	glm::vec3 scale;
	float angle;
    };

    /**
     * The object's transform with its parents': every object type shares WE's one base transform with a parent
     * (GH 0x14020dcc0), so a child's origin, scale and angle are its parent's local space. Bounded by 32 levels
     */
    [[nodiscard]] ResolvedTransform resolveTransform (const Object& object) const;

    /**
     * Computes the object's own transform (origin/scale/angle) without walking the
     * parent chain. Used as the per-node step of resolveTransform.
     */
    [[nodiscard]] static ResolvedTransform localTransform (const Object& object);

private:
    Wallpapers::CScene& m_scene;
    const Object& m_object;
};
} // namespace WallpaperEngine::Render