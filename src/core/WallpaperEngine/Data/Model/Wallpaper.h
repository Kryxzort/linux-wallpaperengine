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
	} projection;

	/** `general.zoom`: scales the view about the scene's centre (with an active camera object's zoom) */
	UserSettingUniquePtr zoom;
    } camera;

    ObjectList objects;
    /** Every value in this scene with a keyframe animation (the scene advances them every frame) */
    std::vector<AnimatedValue> animatedValues = {};
};

class Scene final : public Wallpaper, public SceneData {
public:
    explicit Scene (WallpaperData data, SceneData sceneData) noexcept :
	Wallpaper (std::move (data)), SceneData (std::move (sceneData)) { }

    ~Scene () override = default;
};
} // namespace WallpaperEngine::Data::Model
