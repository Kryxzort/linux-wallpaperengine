#include "WallpaperParser.h"

#include <cmath>

#include "../Model/Project.h"
#include "../Model/Wallpaper.h"
#include "DynamicValueParser.h"
#include "ObjectParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Data::Parsers;

WallpaperUniquePtr WallpaperParser::parse (const JSON& file, Project& project) {
    switch (project.type) {
	case Project::Type_Scene:
	    return parseScene (file, project);
	case Project::Type_Video:
	    return parseVideo (file, project);
	case Project::Type_Web:
	    return parseWeb (file, project);
	default:
	    sLog.exception ("Unexpected project type value found... This is likely a bug");
    }
}

namespace {
SceneData::Fog parseFog (const JSON& general) {
    // the general fields and their upload (GH 0x1401c7880 registers them, GH 0x1401ba300 builds the shader params);
    // the defaults are ours (no corpus scene leaves them out)
    const auto range = [&general] (const char* start, const char* end, const char* startDensity, const char* endDensity) {
	const float s = general.optional (start, 0.0f);
	const float e = general.optional (end, 1000.0f);
	const float sd = general.optional (startDensity, 0.0f);
	const float ed = general.optional (endDensity, 1.0f);
	// a zero span would divide by zero in CalculateFogPixelState
	return glm::vec4 (s, std::abs (e - s) < 1e-6f ? 1e-6f : e - s, sd, ed - sd);
    };

    return SceneData::Fog {
	.distance = general.optional ("fogdistance", false),
	.height = general.optional ("fogheight", false),
	.distanceColor = general.optional ("fogdistancecolor", glm::vec3 (0.0f)),
	.heightColor = general.optional ("fogheightcolor", glm::vec3 (0.0f)),
	.distanceParams = range ("fogdistancestart", "fogdistanceend", "fogdistancestartdensity", "fogdistanceenddensity"),
	.heightParams = range ("fogheightstart", "fogheightend", "fogheightstartdensity", "fogheightenddensity"),
    };
}
} // namespace

SceneUniquePtr WallpaperParser::parseScene (const JSON& file, Project& project) {
    const auto scene = JSON::parse (project.assetLocator->readString (file));
    const auto camera = scene.require ("camera", "Scenes must have a camera section");
    const auto general = scene.require ("general", "Scenes must have a general section");
    // a 3D scene has no `orthogonalprojection` (absent or null): WE's loader (GH 0x1401b6c60) flags a scene orthographic
    // only for an object member, and sees the rest through the perspective camera
    const auto orthogonal = general.optional ("orthogonalprojection");
    const bool perspective = !orthogonal.has_value () || !orthogonal->is_object ();
    const JSON projection = perspective ? JSON::object () : *orthogonal;
    const auto objects = scene.require ("objects", "Scenes must have an objects section");
    const auto& properties = project.properties;

    // TODO: FIND IF THESE DEFAULTS ARE SENSIBLE OR NOT AND PERFORM PROPER VALIDATION WHEN CAMERA PREVIEW AND CAMERA
    // PARALLAX ARE PRESENT

    // every value parsed from here on with a keyframe animation is listed for the scene to advance
    std::vector<AnimatedValue> animatedValues;
    DynamicValueParser::AnimationCollector collector (animatedValues);

    auto result = std::make_unique <Scene> (
        WallpaperData {
            .filename = file,
            .project = project
        }, SceneData {
            .colors = {
                .ambient  = general.user ("ambientcolor", properties, glm::vec3 (0.0f)),
                .skylight = general.user ("skylightcolor", properties, glm::vec3 (0.0f)),
                .clear = general.user ("clearcolor", properties, glm::vec3 (1.0f)),
            },
            .camera = {
                .fade = general.user ("camerafade", properties, false),
                .preview = general.optional ("camerapreview", false),
                .bloom = {
                    .enabled = general.user ("bloom", properties, false),
                    // WE's defaults (its scene constructor, GH 0x1401b6c60: +0x380 2, +0x384 0.65; LWE had 0, 0)
                    .strength = general.user ("bloomstrength", properties, 2.0f),
                    .threshold = general.user ("bloomthreshold", properties, 0.65f),
                    .tint = general.user ("bloomtint", properties, glm::vec3 (1.0f)),
                    // the HDR chain's (keys at +0x388…+0x398, GH 0x1401c7880; hdr is general flag 0x400)
                    .hdr = general.user ("hdr", properties, false),
                    .hdrStrength = general.user ("bloomhdrstrength", properties, 2.0f),
                    .hdrThreshold = general.user ("bloomhdrthreshold", properties, 1.0f),
                    .hdrFeather = general.user ("bloomhdrfeather", properties, 0.1f),
                    .hdrScatter = general.user ("bloomhdrscatter", properties, 1.619f),
                    .hdrIterations = general.user ("bloomhdriterations", properties, 8),
                },
                .parallax = {
                    .enabled = general.user ("cameraparallax", properties, false),
                    // Wallpaper Engine's defaults (its scene constructor, wallpaper64.exe 2.8.485 GH 0x1401b6c60)
                    .amount = general.user ("cameraparallaxamount", properties, 0.5f),
                    .delay = general.user ("cameraparallaxdelay", properties, 0.1f),
                    .mouseInfluence = general.user ("cameraparallaxmouseinfluence", properties, 0.5f),
                },
                .shake = {
                    .enabled = general.user ("camerashake", properties, false),
                    .amplitude = general.user ("camerashakeamplitude", properties, 0.5f),
                    .roughness = general.user ("camerashakeroughness", properties, 1.0f),
                    .speed = general.user ("camerashakespeed", properties, 3.0f),
                },
                .configuration = {
                    .center = camera.require <glm::vec3> ("center", "Camera must have a center position"),
                    .eye = camera.require <glm::vec3> ("eye", "Camera must have an eye position"),
                    .up = camera.require <glm::vec3> ("up", "Camera must have an up position"),
                },
                .projection = {
                    .width  = perspective || projection.optional ("auto", false) ? 0 : projection.require <int> ("width",  "Projection must have a width"),
                    .height = perspective || projection.optional ("auto", false) ? 0 : projection.require <int> ("height", "Projection must have a height"),
                    .isAuto = !perspective && projection.optional ("auto", false),
                    // `general` fields in Wallpaper Engine (the camera block has none), defaults as in WE
                    .nearz = general.user ("nearz", properties, 0.1f),
                    .farz = general.user ("farz", properties, 10000.0f),
                    .fov = general.user ("fov", properties, 50.0f),
                    .overrideFov = general.user ("perspectiveoverridefov", properties, 95.0f),
                    .isPerspective = perspective,
                },
                .zoom = general.user ("zoom", properties, 1.0f)
            },
            .fog = parseFog (general),
            .transparentSorting = general.optional ("transparentsorting", false),
            .objects = parseObjects (objects, project),
        }
    );

    result->animatedValues = std::move (animatedValues);
    const std::string dump = scene.dump ();
    result->scriptsReachEffects = dump.find ("getEffect") != std::string::npos;
    result->usesMedia = dump.find ("mediaPlaybackChanged") != std::string::npos
	|| dump.find ("mediaPropertiesChanged") != std::string::npos
	|| dump.find ("mediaThumbnailChanged") != std::string::npos
	|| dump.find ("mediaTimelineChanged") != std::string::npos
	|| dump.find ("mediaStatusChanged") != std::string::npos || dump.find ("$mediaThumbnail") != std::string::npos;
    return result;
}

VideoUniquePtr WallpaperParser::parseVideo (const JSON& file, Project& project) {
    return std::make_unique<Video> (WallpaperData { .filename = file, .project = project });
}

WebUniquePtr WallpaperParser::parseWeb (const JSON& file, Project& project) {
    return std::make_unique<Web> (WallpaperData {
	.filename = file,
	.project = project,
    });
}

ObjectList WallpaperParser::parseObjects (const JSON& objects, const Project& project) {
    ObjectList result = {};

    for (const auto& cur : objects) {
	result.emplace_back (ObjectParser::parse (cur, project));
    }

    return result;
}