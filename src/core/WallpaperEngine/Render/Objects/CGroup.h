#pragma once

#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

namespace WallpaperEngine::Render::Objects {
/**
 * An object that draws nothing (a group, a camera object, an unsupported type): still a layer to scripts
 * (thisScene.getLayer, enumerateLayers), with the transform and visibility its children inherit
 */
class CGroup final : virtual public CObject, public Scripting::ScriptableObject {
public:
    CGroup (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object), ScriptableObject (scene, object) {
	if (object.groupScale != nullptr) {
	    this->registerProperty ("scale", *object.groupScale->value);
	}
	if (object.groupAngles != nullptr) {
	    this->registerProperty ("angles", *object.groupAngles->value);
	}
	if (object.groupVisible != nullptr) {
	    this->registerProperty ("visible", *object.groupVisible->value);
	}
	if (object.groupParallaxDepth != nullptr) {
	    this->registerProperty ("parallaxDepth", *object.groupParallaxDepth->value);
	}
	// a camera object's ICamera zoom / fov (scripts on them run; CScene::updateCamera reads them live)
	if (object.cameraZoom != nullptr) {
	    this->registerProperty ("zoom", *object.cameraZoom->value);
	}
	if (object.cameraFov != nullptr) {
	    this->registerProperty ("fov", *object.cameraFov->value);
	}
    }
};
} // namespace WallpaperEngine::Render::Objects
