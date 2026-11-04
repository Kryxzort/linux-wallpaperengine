#pragma once

#include <memory>

#include <glm/glm.hpp>
#include <utility>

#include "../Utils/TypeCaster.h"
#include "Object.h"
#include "PropertyAnimation.h"
#include "Types.h"

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;

struct WallpaperData {
    std::string filename;
    Project& project;
};

class Wallpaper : public TypeCaster, public WallpaperData {
public:
    explicit Wallpaper (WallpaperData data) noexcept : TypeCaster (), WallpaperData (std::move (data)) { };
    ~Wallpaper () override = default;
};

class Video final : public Wallpaper {
public:
    explicit Video (WallpaperData data) noexcept : Wallpaper (std::move (data)) { }

    ~Video () override = default;
};

class Web final : public Wallpaper {
public:
    explicit Web (WallpaperData data) noexcept : Wallpaper (std::move (data)) { }

    ~Web () override = default;
};

struct SceneData {
    struct {
	UserSettingUniquePtr ambient;
	UserSettingUniquePtr skylight;
	UserSettingUniquePtr clear;
    } colors;
    /**
     * Camera configuration
     */
    struct Camera {
	/** Enable fade effect */
	UserSettingUniquePtr fade;
	/** Used by the software to allow the users to preview the background or not? */
	bool preview;

	/**
	 * Bloom effect configuration
	 */
	struct {
	    /** If bloom is enabled or not */
	    UserSettingUniquePtr enabled;
	    /** Bloom's strength to pass onto the shader */
	    UserSettingUniquePtr strength;
	    /** Bloom's threshold to pass onto the shader */
	    UserSettingUniquePtr threshold;
	    /** Bloom's tint (`g_BloomTint` of the LDR chain, WE's default 1 1 1) */
	    UserSettingUniquePtr tint;
	    /** `general.hdr`: the scene renders in half float and blooms with WE's HDR chain (GH 0x1401afbf0) */
	    UserSettingUniquePtr hdr;
	    /** The HDR chain's `bloomhdr*` (WE's defaults 2, 1, 0.1, 1.619, 8: GH 0x1401b6c60) */
	    UserSettingUniquePtr hdrStrength;
	    UserSettingUniquePtr hdrThreshold;
	    UserSettingUniquePtr hdrFeather;
	    UserSettingUniquePtr hdrScatter;
	    UserSettingUniquePtr hdrIterations;
	} bloom;
	/**
	 * Parallax effect configuration
	 */
	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr amount;
	    UserSettingUniquePtr delay;
	    UserSettingUniquePtr mouseInfluence;
	} parallax;

	/**
	 * Shake effect configuration
	 */
	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr amplitude;
	    UserSettingUniquePtr roughness;
	    UserSettingUniquePtr speed;
	} shake;

	/**
	 * Position configuration
	 */
	struct {
	    glm::vec3 center;
	    glm::vec3 eye;
	    glm::vec3 up;
	} configuration;

	/**
	 * Projection information
	 */
	struct {
	    int width;
	    int height;
	    bool isAuto;
	    UserSettingUniquePtr nearz;
	    UserSettingUniquePtr farz;
	    UserSettingUniquePtr fov;
	    /**
	     * No `orthogonalprojection` (absent or null): a 3D scene, seen through a perspective camera from
	     * `camera.eye/center/up` with general fov/nearz/farz (the scene loader GH 0x1401b6c60 sets the ortho flag
	     * 0x8 only when the member is an object with a width and height)
	     */
	    bool isPerspective = false;
	} projection;

	/** `general.zoom`: scales the view about the scene's centre (with an active camera object's zoom) */
	UserSettingUniquePtr zoom;
    } camera;

    /**
     * Fog (general +0x344…+0x378, registered in GH 0x1401c7880, uploaded by GH 0x1401ba300): distance fog by the
     * distance to the eye, height fog by the world height. g_Fog*Params = (start, end − start, start density, end
     * density − start density)
     */
    struct Fog {
	bool distance = false;
	bool height = false;
	glm::vec3 distanceColor = glm::vec3 (0.0f);
	glm::vec3 heightColor = glm::vec3 (0.0f);
	glm::vec4 distanceParams = glm::vec4 (0.0f, 1.0f, 0.0f, 0.0f);
	glm::vec4 heightParams = glm::vec4 (0.0f, 1.0f, 0.0f, 0.0f);
    } fog = {};
    /** `transparentsorting`: a 3D scene draws its translucent objects back to front */
    bool transparentSorting = false;

    ObjectList objects;
    /** Every value in this scene with a keyframe animation (the scene advances them every frame) */
    std::vector<AnimatedValue> animatedValues = {};
    /** the scene's scripts call `getEffect`: every effect is built, a script may show one hidden at load */
    bool scriptsReachEffects = false;
    /** the scene's scripts take media events, or a material reads the media thumbnail */
    bool usesMedia = false;
};

class Scene final : public Wallpaper, public SceneData {
public:
    explicit Scene (WallpaperData data, SceneData sceneData) noexcept :
	Wallpaper (std::move (data)), SceneData (std::move (sceneData)) { }

    ~Scene () override = default;
};
} // namespace WallpaperEngine::Data::Model
