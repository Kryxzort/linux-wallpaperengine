#include "WallpaperEngine/Render/Objects/CGroup.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CModel.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"

#include "CScene.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Health.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Render/Objects/CText.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Render::Wallpapers;
using JSON = WallpaperEngine::Data::JSON::JSON;

CScene::CScene (const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext, wp_mouse_input* input) :
    CWallpaper (wallpaper, context, audioContext, input) {
    // WPENGINE_PARTICLE_SEED: the same particles on every run (tests); else a random seed
    if (const char* env = std::getenv ("WPENGINE_PARTICLE_SEED"); env != nullptr && *env != '\0') {
	this->m_particleRng.seed (static_cast<std::mt19937::result_type> (std::strtoul (env, nullptr, 0)));
    } else {
	std::random_device device;
	this->m_particleRng.seed (device ());
    }
    // caller should check this, if not a std::bad_cast is good to throw
    auto scene = wallpaper.as<Scene> ();

    // setup scripting engine
    this->m_scriptEngine = std::make_unique<Scripting::ScriptEngine> (*this);
    // setup the scene camera
    this->m_camera = std::make_unique<Camera> (*this, scene->camera);

    float width = scene->camera.projection.width;
    float height = scene->camera.projection.height;

    // detect size if the orthogonal project is auto
    if (scene->camera.projection.isAuto) {
	glm::vec4 boundingBox = { 0.0f, 0.0f, 0.0f, 0.0f };

	for (const auto& object : scene->objects) {
	    if (!object->is<Image> ()) {
		continue;
	    }

	    const auto* image = object->as<Image> ();
	    if (!image->origin || !image->origin->value) {
		continue;
	    }

	    const glm::vec3 origin = image->origin->value->getVec3 ();
	    const glm::vec2 halfSize = (image->size * image->scale->value->getVec2 ()) / 2.0f;

	    // get the actual top-left corner
	    const glm::vec2 topLeftCorner = { glm::min (origin.x - halfSize.x, origin.x + halfSize.x),
					      glm::min (origin.y - halfSize.y, origin.y + halfSize.y) };
	    const glm::vec2 bottomRightCorner = { glm::max (origin.x - halfSize.x, origin.x + halfSize.x),
						  glm::max (origin.y - halfSize.y, origin.y + halfSize.y) };

	    boundingBox.x = glm::min (topLeftCorner.x, boundingBox.x);
	    boundingBox.y = glm::min (topLeftCorner.y, boundingBox.y);
	    boundingBox.z = glm::max (bottomRightCorner.x, boundingBox.z);
	    boundingBox.w = glm::max (bottomRightCorner.y, boundingBox.w);
	}

	width = boundingBox.z - boundingBox.x;
	height = boundingBox.w - boundingBox.y;
    }

    if (scene->camera.projection.isPerspective) {
	// a 3D scene: WE renders it at the output's size (GH 0x1401b3f70 takes the aspect from the render target's);
	// LWE renders the scene before it knows the output, so it takes a 1920x1080 target (scaled to the output)
	width = 1920.0f;
	height = 1080.0f;
	this->m_camera->setPerspectiveProjection (width, height);
    } else {
	// TODO: CONVERSION
	this->m_camera->setOrthogonalProjection (width, height);
    }
    // WE starts the smoothed parallax position at the centre (GH 0x1401b6c60), g_ParallaxPosition at 0.5 0.5
    this->m_parallaxPosition = { width / 2.0f, height / 2.0f };

    // an HDR scene renders into half float targets (WPENGINE_HDR=0: as WE below Ultra post-processing, LDR)
    const char* hdrSetting = std::getenv ("WPENGINE_HDR");
    this->m_hdr = scene->camera.bloom.hdr->value->getBool ()
	&& !(hdrSetting != nullptr && std::string_view (hdrSetting) == "0");
    this->m_sceneFormat = this->m_hdr ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;

    // setup framebuffers here as they're required for the scene setup
    this->setupFramebuffers ();

    // a 3D scene's models depth test against the scene's target
    if (this->m_camera->isPerspective ()) {
	this->getFBO ()->attachDepthBuffer ();
    }

    const uint32_t sceneWidth = this->m_camera->getWidth ();
    const uint32_t sceneHeight = this->m_camera->getHeight ();

    this->_rt_shadowAtlas = this->create (
	"_rt_shadowAtlas", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth, sceneHeight },
	{ sceneWidth, sceneHeight }
    );
    this->alias ("_alias_lightCookie", "_rt_shadowAtlas");

    // set clear color
    const glm::vec3 clearColor = scene->colors.clear->value->getVec3 ();

    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);

    // create all objects based off their dependencies
    for (const auto& object : scene->objects) {
	this->createObject (*object);
    }

    // copy over objects by render order
    for (const auto& object : scene->objects) {
	this->addObjectToRenderOrder (*object);
    }

    // the layers scripts see (thisScene.getLayer (index), enumerateLayers, sortLayer): the scene's objects in order
    for (const auto& object : scene->objects) {
	if (const auto it = this->m_objects.find (object->id); it != this->m_objects.end ()) {
	    this->m_layers.push_back (it->second);
	}
    }

    this->setupBloom (*scene, sceneWidth, sceneHeight);

    if (WallpaperEngine::Health::enabled ()) {
	this->reportHealthBuild ();
    }
}

namespace {
const char* healthType (const Object& object) {
    if (object.is<Text> ()) {
	return "text";
    }
    if (object.is<Image> ()) {
	return object.as<Image> ()->model != nullptr && object.as<Image> ()->model->passthrough ? "compose" : "image";
    }
    if (object.is<Sound> ()) {
	return "sound";
    }
    if (object.is<Particle> ()) {
	return "particle";
    }
    if (object.is<MeshObject> ()) {
	return "model";
    }
    if (object.is<Light> ()) {
	return "light";
    }
    if (object.cameraZoom != nullptr) {
	return "camera";
    }
    return "group";
}

bool healthFinite (const glm::vec3& v) { return std::isfinite (v.x) && std::isfinite (v.y) && std::isfinite (v.z); }
} // namespace

void CScene::reportHealthBuild () const {
    const auto& scene = this->getScene ();
    const float width = static_cast<float> (this->getWidth ());
    const float height = static_cast<float> (this->getHeight ());

    WallpaperEngine::Health::report (
	"scene", { { "width", this->getWidth () }, { "height", this->getHeight () }, { "objects", scene.objects.size () },
		   { "built", this->m_objects.size () - (this->m_bloomObject != nullptr ? 1 : 0) },
		   { "perspective", this->m_camera->isPerspective () }, { "hdr", this->m_hdr } }
    );

    for (const auto& object : scene.objects) {
	const bool built = this->m_objects.contains (object->id);
	nlohmann::json record = { { "object", object->id }, { "object_name", object->name },
				  { "type", healthType (*object) }, { "built", built } };
	if (object->parent.has_value ()) {
	    record["parent"] = *object->parent;
	}
	if (object->is<Image> ()) {
	    const auto* image = object->as<Image> ();
	    record["visible"] = image->visible->value->getBool ();
	    record["alpha"] = image->alpha->value->getFloat ();
	    record["effects"] = image->effects.size ();
	} else if (object->groupVisible != nullptr) {
	    record["visible"] = object->groupVisible->value->getBool ();
	}
	record["effectively_visible"] = this->isEffectivelyVisible (*object);

	// a layer that wasn't built: where it would have been, from the scene data (its size × scale about its origin,
	// parents included, unrotated) — how much of the picture it takes with it
	if (!built && object->is<Image> ()) {
	    const auto* image = object->as<Image> ();
	    const CObject probe (const_cast<CScene&> (*this), *object);
	    const auto transform = probe.resolveTransform (*object);
	    glm::vec2 size = image->size;
	    if ((size.x == 0.0f || size.y == 0.0f) && image->model != nullptr && image->model->width.has_value ()
		&& image->model->height.has_value ()) {
		size = { static_cast<float> (*image->model->width), static_cast<float> (*image->model->height) };
	    }
	    glm::vec4 box;
	    if (image->model != nullptr && (image->model->fullscreen || image->model->passthrough)) {
		box = { 0.0f, 0.0f, width, height };
	    } else {
		const glm::vec2 half = glm::abs (size * glm::vec2 (transform.scale)) / 2.0f;
		// WE's world is y up: the canvas row is height − y
		box = { transform.origin.x - half.x, height - transform.origin.y - half.y, transform.origin.x + half.x,
			height - transform.origin.y + half.y };
	    }
	    const float onW = std::max (0.0f, std::min (box.z, width) - std::max (box.x, 0.0f));
	    const float onH = std::max (0.0f, std::min (box.w, height) - std::max (box.y, 0.0f));
	    record["estimated_bbox"] = { box.x, box.y, box.z, box.w };
	    record["estimated_on_canvas_px"] = std::isfinite (onW * onH) ? onW * onH : 0.0f;
	}
	WallpaperEngine::Health::report ("object", record);
    }
}

void CScene::reportHealthLayers (int frame) const {
    const float width = static_cast<float> (this->getWidth ());
    const float height = static_cast<float> (this->getHeight ());

    for (const auto& [id, cur] : this->m_objects) {
	if (cur == this->m_bloomObject) {
	    continue;
	}
	const Object& object = cur->getObject ();
	nlohmann::json record;

	if (cur->is<Objects::CImage> ()) {
	    record = cur->as<Objects::CImage> ()->healthLayer ();
	} else if (cur->is<Objects::CParticle> () || cur->is<Objects::CModel> ()) {
	    const auto transform = cur->resolveTransform (object);
	    const bool finite = healthFinite (transform.origin) && healthFinite (transform.scale) && std::isfinite (transform.angle);
	    record = { { "finite", finite } };
	    if (!this->m_camera->isPerspective ()) {
		// a 2D scene: where the system's origin is on the canvas (y down)
		const float x = transform.origin.x, y = height - transform.origin.y;
		record["origin_px"] = { x, y };
		record["origin_on_canvas"] = x >= 0.0f && x <= width && y >= 0.0f && y <= height;
	    }
	} else {
	    continue;
	}
	record["object"] = id;
	record["object_name"] = object.name;
	record["type"] = healthType (object);
	record["frame"] = frame;
	record["effectively_visible"] = this->isEffectivelyVisible (object);
	record["drawn_by"] = this->drawingOwnerOf (object).has_value () ? nlohmann::json (*this->drawingOwnerOf (object)) : nlohmann::json ("scene");
	WallpaperEngine::Health::report ("layers", record);
    }
}

void CScene::setupBloom (const Scene& scene, const uint32_t sceneWidth, const uint32_t sceneHeight) {
    if (this->m_hdr) {
	// the HDR chain's targets: one per level while the smaller side still halves to ≥ 1, at most 8, RGB161616f
	// (GH 0x1401af5b0), named "_rt_" + (2 << level) + "FrameBuffer"
	int available = 0;
	for (int level = 0; level < 8; level++) {
	    const uint32_t divisor = 2u << level;

	    if (std::min (sceneWidth, sceneHeight) / divisor < 1) {
		break;
	    }

	    this->m_hdrTargets.push_back (this->create (
		"_rt_" + std::to_string (divisor) + "FrameBuffer", TextureFormat_RGB161616f, TextureFlags_ClampUVs, 1.0,
		{ sceneWidth / divisor, sceneHeight / divisor }, { sceneWidth / divisor, sceneHeight / divisor }
	    ));
	    available++;
	}

	// the levels that run: bloomhdriterations, at most the targets there are, at least 1 (GH 0x1401b42b0)
	this->m_hdrLevels = std::max (1, std::min (scene.camera.bloom.hdrIterations->value->getInt (), available));
	this->m_hdrSize = { static_cast<float> (sceneWidth), static_cast<float> (sceneHeight) };
    }

    // the LDR chain's targets (WE makes them only below HDR, GH 0x1401af5b0)
    if (!this->m_hdr) {
	this->_rt_4FrameBuffer = this->create (
	    "_rt_4FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0,
	    { sceneWidth / 4, sceneHeight / 4 }, { sceneWidth / 4, sceneHeight / 4 }
	);
	this->_rt_8FrameBuffer = this->create (
	    "_rt_8FrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0,
	    { sceneWidth / 8, sceneHeight / 8 }, { sceneWidth / 8, sceneHeight / 8 }
	);
	this->_rt_Bloom = this->create (
	    "_rt_Bloom", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	    { sceneWidth / 8, sceneHeight / 8 }
	);
    }

    //
    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code
    // this custom image loads some effect files from the virtual container to achieve the same bloom effect
    // this approach requires of two extra draw calls due to the way the effect works in official WPE
    // (it renders directly to the screen, whereas here we never do that from a scene)
    //

    const auto bloomOrigin = glm::vec3 { sceneWidth / 2, sceneHeight / 2, 0.0f };
    const auto bloomSize = glm::vec2 { sceneWidth, sceneHeight };

    // the bloom passes' constants follow general.bloomstrength/bloomthreshold/bloomtint live (GH 0x1401b42b0 re-applies
    // them to the materials; they were literals taken at load) — connected to the scene's values below
    const glm::vec3 tint = this->getScene ().camera.bloom.tint->value->getVec3 ();
    const JSON constants = {
	{ "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
	{ "bloomthreshold", this->getScene ().camera.bloom.threshold->value->getFloat () },
	{ "bloomtint", std::to_string (tint.x) + " " + std::to_string (tint.y) + " " + std::to_string (tint.z) },
    };
    // the HDR chain takes no pass constants: CScene feeds its uniforms (updateHDRBloom)
    const JSON effect = this->m_hdr
	? JSON { { "file", "effects/wpenginelinux/bloomhdr" + std::to_string (this->m_hdrLevels) + ".json" },
		 { "id", 15242000 },
		 { "name", "" } }
	: JSON { { "file", "effects/wpenginelinux/bloomeffect.json" },
		 { "id", 15242000 },
		 { "name", "" },
		 { "passes",
		   JSON::array (
		       { { { "constantshadervalues", constants } },
			 { { "constantshadervalues", constants } },
			 { { "constantshadervalues", constants } } }
		   ) } };
    const JSON bloom
	= { { "image", "models/wpenginelinux.json" },
	    { "name", "bloomimagewpenginelinux" },
	    { "visible", true },
	    { "scale", "1.0 1.0 1.0" },
	    { "angles", "0.0 0.0 0.0" },
	    { "origin",
	      std::to_string (bloomOrigin.x) + " " + std::to_string (bloomOrigin.y) + " "
		  + std::to_string (bloomOrigin.z) },
	    { "size", std::to_string (bloomSize.x) + " " + std::to_string (bloomSize.y) },
	    { "id", -1 },
	    { "effects", JSON::array ({ effect }) } };

    // the image for the bloom passes, built when bloom is on or a user property can turn it on: `bloom` is read every
    // frame (renderFrame). A scene whose bloom is a plain false never builds it (nor needs WE's bloom materials)
    const auto& bloomEnabled = scene.camera.bloom.enabled;
    const auto& bloomCamera = scene.camera.bloom;
    const bool bloomScripted = bloomEnabled->value->getScriptSource ().has_value ();
    if (bloomEnabled->value->getBool () || bloomEnabled->property != nullptr || bloomScripted) {
	try {
	    this->m_bloomObjectData = ObjectParser::parse (bloom, scene.project);

	    if (this->m_bloomObjectData->is<Image> ()) {
		for (const auto& effect : this->m_bloomObjectData->as<Image> ()->effects) {
		    for (const auto& pass : effect->passOverrides) {
			const std::pair<const char*, DynamicValue*> live[] = {
			    { "bloomstrength", this->getScene ().camera.bloom.strength->value.get () },
			    { "bloomthreshold", this->getScene ().camera.bloom.threshold->value.get () },
			    { "bloomtint", this->getScene ().camera.bloom.tint->value.get () },
			};

			for (const auto& [name, source] : live) {
			    if (const auto constant = pass->constants.find (name); constant != pass->constants.end ()) {
				constant->second->value->connect (source);
			    }
			}
		    }
		}
	    }

	    this->m_bloomObject = this->createObject (*this->m_bloomObjectData);

	    if (this->m_hdr && this->m_bloomObject != nullptr && this->m_bloomObject->is<Objects::CImage> ()) {
		this->connectHDRBloom (*this->m_bloomObject->as<Objects::CImage> ());
	    }
	} catch (const std::exception& e) {
	    sLog.error ("Cannot build the bloom passes, the scene renders without bloom: ", e.what ());
	    WPENGINE_HEALTH ("effect_dropped", { { "object", -1 }, { "object_name", "bloom" }, { "reason", WallpaperEngine::Health::brief (e.what ()) }, { "takes_layer", false } });
	    this->m_bloomObject = nullptr;
	}

	if (this->m_bloomObject != nullptr) {
	    this->m_objectsByRenderOrder.push_back (this->m_bloomObject);

	    // scripts on general's bloom values (e.g. a hover toggling bloom) run like a layer property's, with
	    // thisLayer = the bloom image
	    if (auto* owner = this->m_bloomObject->asOrNull<Scripting::ScriptableObject> ()) {
		const std::pair<const char*, DynamicValue*> scripted[] = {
		    { "general_bloom", bloomCamera.enabled->value.get () },
		    { "general_bloomstrength", bloomCamera.strength->value.get () },
		    { "general_bloomthreshold", bloomCamera.threshold->value.get () },
		};

		for (const auto& [key, value] : scripted) {
		    this->m_scriptEngine->queueScript (key, *value, *owner);
		}
	    }
	}
    }
}

void CScene::connectHDRBloom (Objects::CImage& image) {
    // the image's passes: its base copy, the downsamples (levels 0 … n − 1), the upsamples (n − 1 … 1), the combine
    const auto& passes = image.getPasses ();
    const int n = this->m_hdrLevels;

    if (passes.size () != static_cast<size_t> (2 * n + 1)) {
	sLog.error ("The HDR bloom image has ", passes.size (), " passes, not ", 2 * n + 1, ": its inputs aren't fed");
	return;
    }

    // g_RenderVar0 = the four taps' offsets (x, y) and (z, w) about the pixel, (1, 1, −1, −1) · 2^level / the scene
    // size: one source texel when going down, half of one going up (GH 0x1401b3b10)
    const auto taps = [this] (float scale) {
	auto offsets = std::make_unique<glm::vec4> (
	    scale / this->m_hdrSize.x, scale / this->m_hdrSize.y, -scale / this->m_hdrSize.x, -scale / this->m_hdrSize.y
	);
	const glm::vec4* at = offsets.get ();
	this->m_hdrRenderVars.push_back (std::move (offsets));
	return at;
    };

    for (int level = 0; level < n; level++) {
	auto* pass = passes[1 + level];
	pass->addUniform ("g_RenderVar0", taps (static_cast<float> (1 << level)));

	if (level == 0) {
	    pass->addUniform ("g_BloomStrength", &this->m_hdrStrength);
	    pass->addUniform ("g_BloomBlendParams", &this->m_hdrBlend);
	    pass->addUniform ("g_BloomTint", &this->m_hdrTint);
	}
    }

    for (int up = n - 1; up > 0; up--) {
	auto* pass = passes[1 + n + (n - 1 - up)];
	pass->addUniform ("g_RenderVar0", taps (static_cast<float> (2 << (up - 1))));
	pass->addUniform ("g_BloomScatter", &this->m_hdrScatter);
    }

    this->updateHDRBloom ();
}

void CScene::updateHDRBloom () {
    // GH 0x1401b42b0 (re-run when general's values change): the strength over (scatter^(max (n, 2) − 2) + 1), the
    // soft knee from the threshold and feather, the tint, the upsamples' scatter
    const auto& bloom = this->getScene ().camera.bloom;
    const float scatter = bloom.hdrScatter->value->getFloat ();
    const float threshold = bloom.hdrThreshold->value->getFloat ();
    const float knee = threshold * bloom.hdrFeather->value->getFloat ();

    this->m_hdrStrength = bloom.hdrStrength->value->getFloat ()
	/ (std::pow (scatter, static_cast<float> (std::max (this->m_hdrLevels, 2) - 2)) + 1.0f);
    this->m_hdrBlend = { threshold, threshold - knee, knee + knee, 0.25f / (knee + 0.00001f) };
    this->m_hdrTint = bloom.tint->value->getVec3 ();
    this->m_hdrScatter = scatter;
}

bool CScene::rendersHDR () const { return this->m_hdr; }

CScene::~CScene () {
    // destroy () before the objects go ("called just before the object it belongs to gets destroyed")
    if (this->m_scriptEngine != nullptr) {
	this->m_scriptEngine->dispatchEvent ("destroy", JS_UNDEFINED);
    }

    // bloom object is in the objects list, so no need to explicitly delete it
    this->m_bloomObject = nullptr;

    for (const auto& val : this->m_objects | std::views::values) {
	delete val;
    }

    this->m_objectsByRenderOrder.clear ();
    this->m_objects.clear ();
}

Render::CObject* CScene::createObject (const Object& object) {
    Render::CObject* renderObject = nullptr;

    // ensure the item is not loaded already
    if (const auto current = this->m_objects.find (object.id); current != this->m_objects.end ()) {
	return current->second;
    }

    // check dependencies too!
    for (const auto& cur : object.dependencies) {
	// self-dependency is a possibility...
	if (cur == object.id) {
	    continue;
	}

	const auto dep
	    = std::ranges::find_if (this->getScene ().objects, [&cur] (const auto& o) { return o->id == cur; });

	if (dep != this->getScene ().objects.end ()) {
	    this->createObject (**dep);
	}
    }

    // check if the item has any parent and also create it first
    if (object.parent.has_value ()) {
	int parentId = object.parent.value ();

	const auto dep = std::ranges::find_if (this->getScene ().objects, [&parentId] (const auto& o) {
	    return o->id == parentId;
	});

	if (dep == this->getScene ().objects.end ()) {
	    sLog.exception ("Cannot find parent ", parentId, " for object ", object.id);
	}

	this->createObject (**dep);
    }

    renderObject = this->dispatchObjectType (object);

    if (renderObject != nullptr) {
	this->m_objects.emplace (renderObject->getId (), renderObject);
    }

    return renderObject;
}

Render::CObject* CScene::dispatchObjectType (const Object& object) {
    Render::CObject* renderObject = nullptr;
    // WPENGINE_HEALTH: what fails while this object builds (its textures, shaders, fonts) names it
    const WallpaperEngine::Health::Scope healthObject ("object", object.id);
    const WallpaperEngine::Health::Scope healthName ("object_name", object.name);

    // a text object is an image layer too (WE's text class extends its image class): test it first
    if (object.is<Text> ()) {
	renderObject = new Objects::CText (*this, *object.as<Text> ());
    } else if (object.is<Image> ()) {
	renderObject = new Objects::CImage (*this, *object.as<Image> ());
    } else if (object.is<Sound> ()) {
	renderObject = new Objects::CSound (*this, *object.as<Sound> ());
    } else if (object.is<Particle> ()) {
	const auto& particleData = *object.as<Particle> ();

	if (this->getContext ().getContext ().config.disableParticles) {
	    sLog.debug ("Ignoring particle system (disabled in settings): ", particleData.name);
	    return nullptr;
	}

	if (particleData.material == nullptr || particleData.material->material == nullptr) {
	    sLog.error ("Ignoring particle system without a material: ", particleData.name);
	    WPENGINE_HEALTH ("dropped", { { "object", object.id }, { "object_name", object.name }, { "reason", "particle system without a material" } });
	    return nullptr;
	}

	renderObject = new Objects::CParticle (*this, particleData);
    } else if (object.is<MeshObject> ()) {
	renderObject = new Objects::CModel (*this, *object.as<MeshObject> ());
    } else if (object.cameraZoom != nullptr) {
	// a camera object: nothing to draw, CScene::updateCamera reads its data
	renderObject = new Objects::CGroup (*this, object);
    } else {
	// a group (or a type LWE doesn't draw): nothing to draw, but a layer to scripts
	renderObject = new Objects::CGroup (*this, object);
    }

    try {
	renderObject->setup ();
    } catch (const std::exception& e) {
	sLog.error ("Failed to setup object ", object.id, ": ", e.what ());
	WPENGINE_HEALTH ("dropped", { { "object", object.id }, { "object_name", object.name }, { "reason", WallpaperEngine::Health::brief (e.what ()) } });
	delete renderObject;
	renderObject = nullptr;
    }

    if (renderObject != nullptr && renderObject->is<Objects::CSound> ()) {
	this->m_sounds.push_back (renderObject->as<Objects::CSound> ());
    }

    return renderObject;
}

void CScene::addObjectToRenderOrder (const Object& object) {
    const auto obj = this->m_objects.find (object.id);

    // ignores not created objects like particle systems
    if (obj == this->m_objects.end ()) {
	return;
    }

    // take into account any dependency first
    for (const auto& dep : object.dependencies) {
	// self-dependency is possible
	if (dep == object.id) {
	    continue;
	}

	// add the dependency to the list if it's created
	auto depIt = std::ranges::find_if (this->getScene ().objects, [&dep] (const auto& o) { return o->id == dep; });

	if (depIt != this->getScene ().objects.end ()) {
	    this->addObjectToRenderOrder (**depIt);
	} else {
	    sLog.error ("Cannot find dependency ", dep, " for object ", object.id);
	}
    }

    // ensure we're added only once to the render list
    const auto renderIt = std::ranges::find_if (this->m_objectsByRenderOrder, [&object] (const auto& o) {
	return o->getId () == object.id;
    });

    if (renderIt == this->m_objectsByRenderOrder.end ()) {
	this->m_objectsByRenderOrder.emplace_back (obj->second);
    }
}

Scripting::ScriptEngine& CScene::getScriptEngine () const { return *this->m_scriptEngine; }

void CScene::advanceAnimations () {
    auto& animated = const_cast<Data::Model::Scene&> (this->getScene ()).animatedValues;
    const float dt = std::max (
	0.0f, this->getContext ().getContext ().renderTime - this->getContext ().getContext ().renderTimeLast
    );

    std::erase_if (animated, [] (const Data::Model::AnimatedValue& cur) { return cur.alive && !*cur.alive; });

    for (const auto& [value, alive] : animated) {
	const auto& animation = value->getAnimation ();

	if (animation == nullptr) {
	    continue;
	}

	animation->advance (dt);
	animation->apply (*value);

	// the events it passed reach the property's script (animationEvent)
	if (auto fired = animation->takeFiredEvents (); !fired.empty ()) {
	    this->m_scriptEngine->animationEvents (*value, fired);
	}
    }
}
Camera& CScene::getCamera () const { return *this->m_camera; }

void CScene::renderFrame () {
    // ensure the virtual mouse position is up to date
    this->updateMouse ();
    // the camera: shake, zoom and the mouse parallax
    this->updateCamera ();

    // keyframe animations first, so a property script's update(value) sees this frame's animated value
    this->advanceAnimations ();
    // run a tick in the javascript logic
    this->getScriptEngine ().tick ();

    // the cursor's events for the scripts (after their tick: their init has run)
    this->updateCursorEvents ();

    // advance the sound objects' playlists (next file, `random`'s silence) on the host's clock
    const double soundDt = std::max (
	0.0,
	static_cast<double> (this->getContext ().getContext ().renderTime - this->getContext ().getContext ().renderTimeLast)
    );

    for (auto* sound : this->m_sounds) {
	sound->tick (soundDt);
    }

    // texture animations a script took off the shared clock (ITextureAnimation) run on their own
    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (cur->is<Objects::CImage> ()) {
	    cur->as<Objects::CImage> ()->advanceTextureAnimation (soundDt);
	}
    }

    // update main textures for images
    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (!cur->is<Objects::CImage> ()) {
	    continue;
	}

	const Objects::CImage* image = cur->as<Objects::CImage> ();

#if !NDEBUG
	const std::string message = "Updating texture " + image->getImage ().model->filename;

	glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, message.c_str ());
#endif

	image->getTexture ()->update ();

#if !NDEBUG
	glPopDebugGroup ();
#endif
    }

    // bind the vertex array
    glBindVertexArray (this->m_vaoBuffer);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->getWallpaperFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    // the last frame's last pass left alpha masked: the clear sets the scene's alpha (opaque) every frame, as a compose
    // layer's background copy and a blend mode's output alpha read it
    glColorMask (true, true, true, true);
    // (a pass leaves depth writes off: the clear needs them)
    glDepthMask (GL_TRUE);
    // the clear colour every frame: a video texture's player (mpv) leaves its own behind, and a script may change it
    // (IScene.clearcolor)
    const glm::vec3 clearColor = this->getScene ().colors.clear->value->getVec3 ();
    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // a 3D scene's lights for this frame (the models' shading reads them)
    if (this->m_camera->isPerspective ()) {
	this->updateLights ();
    }

    // a copy: a text layer's script runs as it renders, and may create, destroy or sort layers
    const std::vector<CObject*> order = this->drawOrder ();
    for (const auto& cur : order) {
	if (!this->m_destroyed.empty () && std::ranges::find (this->m_destroyed, cur) != this->m_destroyed.end ()) {
	    continue;
	}
	const auto& debug = this->getContext ().getContext ().config;
	if (debug.objectFilter.has_value () && cur->getId () != debug.objectFilter.value ()) {
	    continue;
	}
	if (std::ranges::find (debug.skipObjects, cur->getId ()) != debug.skipObjects.end ()) {
	    continue;
	}
	if (cur == this->m_bloomObject && !this->getScene ().camera.bloom.enabled->value->getBool ()) {
	    continue;
	}
	if (cur == this->m_bloomObject && this->m_hdr) {
	    this->updateHDRBloom ();
	}
	// a compose layer draws its subtree into itself (renderComposed)
	if (this->drawingOwnerOf (cur->getObject ()).has_value ()) {
	    continue;
	}

	cur->render ();
    }

    if (WallpaperEngine::Health::enabled ()) {
	this->m_healthFrames++;
	if (this->m_healthFrames == 1 || this->m_healthFrames == 31) {
	    this->reportHealthLayers (this->m_healthFrames);
	}
    }
}

void CScene::updateMouse () {
    // update virtual mouse position first
    // TODO: BETTER NAME THIS AND CHECK LOGIC
    const glm::dvec2 position = this->getLiveMousePosition ();

    // roll over the position to the last
    this->m_mousePositionLast = this->m_mousePosition;

    // position is already inside the background, convert it to viewport space
    this->m_mousePosition.x = position.x / this->getWidth ();
    this->m_mousePosition.y = position.y / this->getHeight ();
    // input.cursorScreenPosition read this, never written (always 0 0)
    this->m_mousePositionNormalized = this->m_mousePosition;

    // the left button: held → xy 1; the frame it went down → z 1 (WE's flags at ctx+0xa4: bit 0 down, bit 1 already
    // seen, so z marks the press)
    const wp_mouse_input* handler = this->getMouseInputHandler ();
    const bool down = handler != nullptr && handler->is_pressed != nullptr
	&& (handler->is_pressed (handler->user_parameter, WP_MOUSE_INPUT_BUTTON_LEFT) & WP_MOUSE_INPUT_BUTTON_LEFT) != 0;

    this->m_pointerState = glm::vec4 (
	down ? 1.0f : 0.0f, down ? 1.0f : 0.0f, down && !this->m_pointerWasDown ? 1.0f : 0.0f, 0.0f
    );
    this->m_pointerWasDown = down;
}

void CScene::updateCursorEvents () {
    auto& scripts = this->getScriptEngine ();
    const auto& layers = scripts.cursorLayers ();

    if (layers.empty () || this->getMouseInputHandler () == nullptr) {
	return;
    }

    // WE's world: x right, y up, scene pixels (the host's pointer is y down)
    const glm::vec2 world (
	this->m_mousePosition.x * static_cast<float> (this->getWidth ()),
	(1.0f - this->m_mousePosition.y) * static_cast<float> (this->getHeight ())
    );
    std::vector<Scripting::ScriptEngine::CursorHit> hits;

    // every solid layer under the cursor gets enter/leave/move, shown or hidden (a trigger layer that hides itself on
    // cursorEnter must not get cursorLeave the next frame); presses and clicks go to the shown ones (cursorFrame)
    for (auto* object : layers) {
	if (!object->isSolid ()) {
	    continue;
	}
	glm::vec2 local {};
	if (const auto* image = object->asOrNull<Objects::CImage> (); image != nullptr && image->hitTest (world, local)) {
	    hits.push_back (
		{ .object = object, .local = local, .visible = this->isEffectivelyVisible (object->getObject ()) }
	    );
	}
    }

    scripts.cursorFrame (hits, world, this->m_pointerState.x > 0.5f);
}

const glm::vec4* CScene::getPointerState () const { return &this->m_pointerState; }

const Scene& CScene::getScene () const { return *this->getWallpaperData ().as<Scene> (); }

int CScene::getWidth () const { return this->m_camera->getWidth (); }

int CScene::getHeight () const { return this->m_camera->getHeight (); }

float CScene::getTime () const { return this->getContext ().getContext ().renderTime; }

float CScene::getDeltaTime () const {
    return this->getContext ().getContext ().renderTime - this->getContext ().getContext ().renderTimeLast;
}

float CScene::getFps () const {
    const float dt = this->getContext ().getContext ().renderTime - this->getContext ().getContext ().renderTimeLast;
    // Guard against the first frame (where g_TimeLast is 0 so dt == g_Time)
    // and division by zero on the very first call.
    if (dt <= 1e-6f) {
	return 60.0f;
    }
    return 1.0f / dt;
}

const glm::vec2* CScene::getMousePosition () const { return &this->m_mousePosition; }

const glm::vec2* CScene::getMousePositionLast () const { return &this->m_mousePositionLast; }

const glm::vec2* CScene::getMousePositionNormalized () const { return &this->m_mousePositionNormalized; }

const glm::vec2* CScene::getParallaxPosition () const { return &this->m_parallaxPositionNormalized; }

namespace {
/** An object's own `visible` (the typed objects keep their own field) */
bool ownVisible (const Object& object) {
    if (object.is<Image> ()) {
	return object.as<Image> ()->visible->value->getBool ();
    }
    if (object.is<Particle> ()) {
	return object.as<Particle> ()->visible->value->getBool ();
    }
    return object.groupVisible == nullptr || object.groupVisible->value->getBool ();
}

/** An object's `parallaxDepth` (images and particles read their own, registered for scripts) */
glm::vec2 parallaxDepthOf (const Object& object) {
    if (object.is<Image> ()) {
	return object.as<Image> ()->parallaxDepth->value->getVec2 ();
    }
    if (object.is<Particle> ()) {
	return object.as<Particle> ()->parallaxDepth->value->getVec2 ();
    }
    return object.groupParallaxDepth == nullptr ? glm::vec2 (1.0f) : object.groupParallaxDepth->value->getVec2 ();
}
} // namespace

std::optional<int> CScene::composeOwnerOf (const Object& object) const {
    if (!this->m_composeOwnersBuilt) {
	this->m_composeOwnersBuilt = true;

	// only an ancestor listed before the object (so built before it: its setup already knows the target)
	std::map<int, std::pair<const Object*, size_t>> byId;
	size_t index = 0;
	for (const auto& cur : this->getScene ().objects) {
	    byId.emplace (cur->id, std::make_pair (cur.get (), index++));
	}
	index = 0;

	for (const auto& cur : this->getScene ().objects) {
	    std::optional<int> owner = std::nullopt;
	    const Object* at = cur.get ();

	    for (int depth = 0; at->parent.has_value () && depth < 32; depth++) {
		const auto parent = byId.find (at->parent.value ());
		if (parent == byId.end () || parent->second.first == cur.get ()) {
		    break;
		}
		at = parent->second.first;
		if (at->is<Image> () && at->as<Image> ()->model->passthrough) {
		    if (parent->second.second < index) {
			owner = at->id;
		    }
		    break;
		}
	    }

	    this->m_composeOwners.emplace (cur->id, owner);
	    index++;
	}
    }

    const auto it = this->m_composeOwners.find (object.id);
    return it == this->m_composeOwners.end () ? std::nullopt : it->second;
}

bool CScene::composesChildren (const Object& image) const {
    // builds the map
    static_cast<void> (this->composeOwnerOf (image));

    return std::ranges::any_of (this->m_composeOwners | std::views::values, [&image] (const auto& owner) {
	return owner.has_value () && owner.value () == image.id;
    });
}

std::optional<int> CScene::drawingOwnerOf (const Object& object) const {
    const auto owner = this->composeOwnerOf (object);

    if (!owner.has_value ()) {
	return std::nullopt;
    }

    // a compose layer that couldn't be built leaves its subtree to the scene
    const CObject* image = this->getObject (owner.value ());
    if (image == nullptr || !image->is<Objects::CImage> () || !image->as<Objects::CImage> ()->drawsSubtree ()) {
	return std::nullopt;
    }

    return owner;
}

std::shared_ptr<const CFBO> CScene::getTargetFor (const Object& object) const {
    const auto owner = this->drawingOwnerOf (object);

    if (!owner.has_value ()) {
	return this->getFBO ();
    }

    return const_cast<CScene*> (this)->getComposeTarget (owner.value ());
}

bool CScene::targetKeepsAlpha (const Object& object) const {
    const auto owner = this->drawingOwnerOf (object);

    if (!owner.has_value ()) {
	return true;
    }

    return this->getObject (owner.value ())->as<Objects::CImage> ()->getImage ().copyBackground->value->getBool ();
}

std::shared_ptr<const CFBO> CScene::getComposeTarget (int id) {
    auto& target = this->m_composeTargets[id];

    if (target == nullptr) {
	const auto& scene = *this->getFBO ();
	target = std::make_shared<CFBO> (
	    "_rt_composeLayer_" + std::to_string (id), scene.getFormat (), TextureFlags_ClampUVs, 1.0f,
	    scene.getRealWidth (), scene.getRealHeight (), scene.getTextureWidth (0), scene.getTextureHeight (0)
	);
    }

    return target;
}

void CScene::renderComposed (int ownerId, const CFBO& target) {
    for (const auto& cur : this->m_objectsByRenderOrder) {
	const auto owner = this->drawingOwnerOf (cur->getObject ());
	if (!owner.has_value () || owner.value () != ownerId) {
	    continue;
	}

	// what draws to whatever is bound (text, particles' masks) draws into the layer
	glBindFramebuffer (GL_FRAMEBUFFER, target.getFramebuffer ());
	glViewport (0, 0, target.getRealWidth (), target.getRealHeight ());
	glColorMask (true, true, true, !this->targetKeepsAlpha (cur->getObject ()));

	cur->render ();
    }
}

bool CScene::isEffectivelyVisible (const Object& object) const {
    const Object* cur = &object;

    for (int depth = 0; depth <= 32; depth++) {
	if (!ownVisible (*cur)) {
	    return false;
	}
	if (!cur->parent.has_value ()) {
	    return true;
	}
	const CObject* parent = this->getObject (cur->parent.value ());
	if (parent == nullptr) {
	    return true;
	}
	cur = &parent->getObject ();
    }

    return true;
}

glm::vec2 CScene::getParallaxOffset (const Object& object) const {
    if (!this->getScene ().camera.parallax.enabled->value->getBool ()
	|| this->getContext ().getContext ().config.disableParallax) {
	return glm::vec2 (0.0f);
    }

    // the topmost ancestor's origin and depth move the whole subtree (GH 0x1401b9b50)
    const Object* root = &object;
    for (int depth = 0; root->parent.has_value () && depth < 32; depth++) {
	const CObject* parent = this->getObject (root->parent.value ());
	if (parent == nullptr) {
	    break;
	}
	root = &parent->getObject ();
    }

    const float amount = this->getScene ().camera.parallax.amount->value->getFloat ();
    const glm::vec2 origin = glm::vec2 (root->origin->value->getVec3 ());

    return (origin - this->m_parallaxPosition) * amount * parallaxDepthOf (*root);
}

void CScene::setParentOf (CObject& object, CObject* parent, const std::optional<std::string>& attachment) {
    // the scene's data is the layers' live state (as their DynamicValues are)
    auto& data = const_cast<Object&> (object.getObject ());
    data.parent = parent == nullptr ? std::nullopt : std::optional<int> (parent->getId ());
    data.attachment = parent == nullptr ? std::nullopt : attachment;
    // which compose layer draws what follows the parents
    this->m_composeOwners.clear ();
    this->m_composeOwnersBuilt = false;
}

glm::mat4 CScene::localMatrixOf (const Object& object) {
    glm::vec3 angles (0.0f);
    glm::vec3 scale (1.0f);

    if (object.is<Image> ()) {
	angles = object.as<Image> ()->angles->value->getVec3 ();
	scale = object.as<Image> ()->scale->value->getVec3 ();
    } else if (object.is<Particle> ()) {
	angles = object.as<Particle> ()->angles->value->getVec3 ();
	scale = object.as<Particle> ()->scale->value->getVec3 ();
    } else {
	angles = object.groupAngles->value->getVec3 ();
	scale = object.groupScale->value->getVec3 ();
    }

    // WE's object matrix (GH 0x1401b5170): the rotation rows from the angles (GH 0x14020af40: R = Rz · Ry · Rx in
    // column form) scaled per axis, then the origin
    glm::mat4 local = glm::translate (glm::mat4 (1.0f), object.origin->value->getVec3 ());
    local = glm::rotate (local, angles.z, glm::vec3 (0.0f, 0.0f, 1.0f));
    local = glm::rotate (local, angles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
    local = glm::rotate (local, angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
    return glm::scale (local, scale);
}

glm::mat4 CScene::worldMatrixOf (const Object& object) const {
    // world = parent world · local (GH 0x1401b5170), bounded as resolveTransform is
    const Object* chain[33];
    int count = 0;
    const Object* cur = &object;
    chain[count++] = cur;

    while (cur->parent.has_value () && count < 33) {
	const CObject* parent = this->getObject (cur->parent.value ());
	if (parent == nullptr || &parent->getObject () == &object) {
	    break;
	}
	cur = &parent->getObject ();
	chain[count++] = cur;
    }

    glm::mat4 world (1.0f);
    for (int i = count - 1; i >= 0; --i) {
	world = world * localMatrixOf (*chain[i]);
    }

    return world;
}

bool CScene::drawsInWorld (const CObject& object) const {
    if (object.is<Objects::CModel> () || object.is<Objects::CParticle> ()) {
	return true;
    }

    return object.is<Objects::CImage> () && object.as<Objects::CImage> ()->isWorldPlaced ();
}

const std::vector<CObject*>& CScene::drawOrder () {
    if (!this->m_camera->isPerspective () || !this->getScene ().transparentSorting) {
	return this->m_objectsByRenderOrder;
    }

    // transparentsorting: every run of objects placed in the world is drawn back to front by the distance of their
    // origins from the eye (the order of what draws flat over the screen — full-screen layers, the bloom — stays).
    // WE's sort key is not decoded: this is ours
    this->m_sortedOrder = this->m_objectsByRenderOrder;
    const glm::vec3 eye = this->m_camera->getWorldEye ();
    auto distance = [this, &eye] (const CObject* object) {
	return glm::length (glm::vec3 (this->worldMatrixOf (object->getObject ())[3]) - eye);
    };

    auto begin = this->m_sortedOrder.begin ();
    while (begin != this->m_sortedOrder.end ()) {
	if (!this->drawsInWorld (**begin)) {
	    ++begin;
	    continue;
	}
	auto end = begin;
	std::vector<std::pair<float, CObject*>> run;
	while (end != this->m_sortedOrder.end () && this->drawsInWorld (**end)) {
	    run.emplace_back (distance (*end), *end);
	    ++end;
	}
	std::stable_sort (run.begin (), run.end (), [] (const auto& a, const auto& b) { return a.first > b.first; });
	for (auto& [d, object] : run) {
	    *begin++ = object;
	}
	begin = end;
    }

    return this->m_sortedOrder;
}

void CScene::updateLights () {
    // the point and spot lights the models' generated PerformLighting_V1 loops over (GH 0x1401bf270 fills
    // g_LCluster_Origin/Color/Direction/Extra the same way; WE culls them into clusters, LWE takes the first ones):
    //  origin (world position, radius), color (rgb · intensity, exponent), direction (a spot's world x axis,
    //  1 / (cos inner − cos outer)), extra.x (−cos outer / (cos inner − cos outer)); a point light: 0 and 1
    auto& lights = this->m_lights;
    lights.count = 0;

    for (const auto& object : this->getScene ().objects) {
	if (!object->is<Light> () || lights.count >= SceneLights::Max) {
	    continue;
	}

	const auto& light = *object->as<Light> ();
	if (light.kind == LightData::Kind::Other || !this->isEffectivelyVisible (light)) {
	    continue;
	}

	const glm::mat4 world = this->worldMatrixOf (light);
	const int i = lights.count++;
	const float intensity = light.intensity->value->getFloat ();

	lights.origin[i] = glm::vec4 (glm::vec3 (world[3]), light.radius);
	lights.color[i] = glm::vec4 (light.color->value->getVec3 () * intensity, light.exponent);

	if (light.kind == LightData::Kind::Spot) {
	    const glm::vec3 axis = glm::vec3 (world[0]);
	    const glm::vec3 direction = glm::length (axis) > 0.0f ? glm::normalize (axis) : glm::vec3 (1.0f, 0.0f, 0.0f);
	    const float outer = std::cos (glm::radians (light.outerCone));
	    const float inner = std::cos (glm::radians (light.innerCone));
	    const float scale = 1.0f / std::max (inner - outer, 1e-4f);

	    lights.direction[i] = glm::vec4 (direction, scale);
	    lights.extra[i] = glm::vec4 (-outer * scale, 0.0f, 0.0f, 0.0f);
	} else {
	    lights.direction[i] = glm::vec4 (0.0f, 0.0f, -1.0f, 0.0f);
	    lights.extra[i] = glm::vec4 (1.0f, 0.0f, 0.0f, 0.0f);
	}
    }
}

const CScene::SceneLights& CScene::getLights () const { return this->m_lights; }

void CScene::updateCamera () {
    const auto& camera = this->getScene ().camera;

    if (this->m_camera->isPerspective ()) {
	// a 3D scene's view (GH 0x1401b8af0): the active camera object's — the last visible one — places the eye at
	// its world position, looking down its −z axis with its y axis up, at its fov; else the scene's camera at
	// general.fov. Then the shake moves the eye and the centre alike (GH 0x1401c7680 with z = sin t, the amplitude
	// in world units · 0.1). The parallax, the zoom and camera paths of a 3D camera are not done
	glm::vec3 eye = this->m_camera->getEye ();
	glm::vec3 center = this->m_camera->getCenter ();
	glm::vec3 up = this->m_camera->getUp ();
	float fov = this->m_camera->getFov ();

	for (auto it = this->getScene ().objects.rbegin (); it != this->getScene ().objects.rend (); ++it) {
	    const Object& object = **it;

	    if (object.cameraZoom == nullptr || !this->isEffectivelyVisible (object)) {
		continue;
	    }

	    const glm::mat4 world = this->worldMatrixOf (object);
	    eye = glm::vec3 (world[3]);
	    center = eye - glm::vec3 (world[2]);
	    up = glm::vec3 (world[1]);
	    if (object.cameraFov != nullptr) {
		fov = object.cameraFov->value->getFloat ();
	    }
	    break;
	}

	glm::vec3 offset (0.0f);

	if (camera.shake.enabled->value->getBool ()) {
	    const float speed = camera.shake.speed->value->getFloat ();
	    const float t = speed * speed * this->getContext ().getContext ().renderTime;
	    glm::vec3 shake (std::cos (t), std::sin (t * 1.333f), std::sin (t));
	    const float roughness = std::pow (camera.shake.roughness->value->getFloat (), 3.0f);

	    if (roughness > 0.001f && roughness != 1.0f) {
		const float length = glm::length (shake);

		if (length > 0.0f) {
		    shake *= std::pow (length, roughness) / length;
		}
	    }

	    offset = shake * (camera.shake.amplitude->value->getFloat () * 0.1f);
	}

	this->m_camera->setPerspectiveFov (fov);
	this->m_camera->setPerspectiveView (eye + offset, center + offset, up);
	return;
    }

    const float width = this->m_camera->getWidth ();
    const float height = this->m_camera->getHeight ();
    const float dt = std::max (
	0.0f, this->getContext ().getContext ().renderTime - this->getContext ().getContext ().renderTimeLast
    );

    // the camera position, in scene units (y up), relative to the default 2D camera. Its shake (FUN_1401c7680):
    // a point on cos(t), sin(1.333 t) with t = speed² · g_Time, its length raised to `roughness` (unless that is
    // ≤ 0.001 or 1), times amplitude · 0.01 · the scene height (2D: no z)
    glm::vec2 cameraPosition (0.0f);

    if (camera.shake.enabled->value->getBool ()) {
	const float speed = camera.shake.speed->value->getFloat ();
	const float t = speed * speed * this->getContext ().getContext ().renderTime;
	glm::vec2 shake (std::cos (t), std::sin (t * 1.333f));
	const float roughness = std::pow (camera.shake.roughness->value->getFloat (), 3.0f);

	if (roughness > 0.001f && roughness != 1.0f) {
	    const float length = glm::length (shake);

	    if (length > 0.0f) {
		shake *= std::pow (length, roughness) / length;
	    }
	}

	cameraPosition += shake * (height * 0.1f * camera.shake.amplitude->value->getFloat () * 0.1f);
    }

    // the zoom: general.zoom times the active camera object's (the last visible one whose parents are visible)
    float zoom = camera.zoom == nullptr ? 1.0f : camera.zoom->value->getFloat ();

    for (auto it = this->getScene ().objects.rbegin (); it != this->getScene ().objects.rend (); ++it) {
	const Object& object = **it;

	if (object.cameraZoom == nullptr) {
	    continue;
	}

	if (this->isEffectivelyVisible (object)) {
	    zoom *= object.cameraZoom->value->getFloat ();
	    break;
	}
    }

    // the mouse parallax: a target from the cursor (y up), centred at mouseinfluence 0, eased in by
    // k = min (1, (1 − delay / 3) · 10 · dt), or snapped to with a delay ≤ 0
    if (camera.parallax.enabled->value->getBool () && !this->getContext ().getContext ().config.disableParallax) {
	const float influence = camera.parallax.mouseInfluence->value->getFloat ();
	const float delay = camera.parallax.delay->value->getFloat ();
	const float mx = glm::clamp (this->m_mousePosition.x, 0.0f, 1.0f);
	const float my = glm::clamp (this->m_mousePosition.y, 0.0f, 1.0f);
	const glm::vec2 target = glm::vec2 (
				     width * (0.5f * (1.0f - influence) + mx * influence),
				     height * (0.5f * (1.0f - influence) + (1.0f - my) * influence)
				 )
	    + cameraPosition;

	if (delay <= 0.0f) {
	    this->m_parallaxPosition = target;
	} else {
	    const float k = std::min (1.0f, (1.0f - delay / 3.0f) * 10.0f * dt);
	    this->m_parallaxPosition += (target - this->m_parallaxPosition) * k;
	}

	if (width > 0.0f && height > 0.0f) {
	    this->m_parallaxPositionNormalized = glm::clamp (
		this->m_parallaxPosition / glm::vec2 (width, height), glm::vec2 (0.0f), glm::vec2 (1.0f)
	    );
	}
    }

    // the view: zoom about the scene's centre, then the camera position. LWE's world has the centre at 0 and y down
    glm::mat4 adjust = glm::scale (glm::mat4 (1.0f), glm::vec3 (zoom, zoom, 1.0f));
    adjust = glm::translate (adjust, glm::vec3 (-cameraPosition.x, cameraPosition.y, 0.0f));
    this->m_camera->setViewAdjust (adjust);
}

std::mt19937& CScene::getParticleRng () { return this->m_particleRng; }

const std::vector<CObject*>& CScene::getObjectsByRenderOrder () const { return this->m_objectsByRenderOrder; }

bool CScene::scriptsReachEffects () const { return this->getScene ().scriptsReachEffects; }

const std::vector<CObject*>& CScene::getLayers () const { return this->m_layers; }

CObject* CScene::createLayer (const std::string& config) {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    WallpaperEngine::Data::JSON::JSON json;

    try {
	json = WallpaperEngine::Data::JSON::JSON::parse (config);
    } catch (const std::exception& e) {
	sLog.error ("createLayer: the configuration is not JSON: ", e.what ());
	return nullptr;
    }

    if (!json.is_object ()) {
	return nullptr;
    }

    if (this->m_nextCreatedId == 0) {
	int top = 0;
	for (const auto& object : this->getScene ().objects) {
	    top = std::max (top, object->id);
	}
	this->m_nextCreatedId = top + 1;
    }

    const int id = this->m_nextCreatedId++;
    json["id"] = id;
    if (!json.contains ("name") || !json["name"].is_string ()) {
	// the asset it was made from, as the editor names a new layer
	std::string name = "layer";
	for (const char* key : { "image", "particle", "sound", "model" }) {
	    if (json.contains (key) && json[key].is_string ()) {
		name = json[key].get<std::string> ();
		break;
	    }
	}
	if (json.contains ("text")) {
	    name = "text";
	}
	json["name"] = name;
    }
    json.erase ("parent");
    json.erase ("dependencies");

    try {
	this->m_createdObjects.push_back (ObjectParser::parse (json, this->getScene ().project));
    } catch (const std::exception& e) {
	sLog.error ("createLayer: ", e.what ());
	WPENGINE_HEALTH ("dropped", { { "object", id }, { "object_name", json["name"] }, { "reason", WallpaperEngine::Health::brief (std::string ("createLayer: ") + e.what ()) } });
	return nullptr;
    }

    CObject* object = this->dispatchObjectType (*this->m_createdObjects.back ());

    if (object == nullptr) {
	return nullptr;
    }

    this->m_objects.emplace (object->getId (), object);
    this->m_layers.push_back (object);
    // drawn last, before the bloom
    auto at = this->m_bloomObject == nullptr ? this->m_objectsByRenderOrder.end ()
					      : std::ranges::find (this->m_objectsByRenderOrder, this->m_bloomObject);
    this->m_objectsByRenderOrder.insert (at, object);
    this->m_composeOwners.clear ();
    this->m_composeOwnersBuilt = false;
    return object;
}

bool CScene::destroyLayer (CObject* layer) {
    const auto it = std::ranges::find (this->m_layers, layer);

    if (layer == nullptr || it == this->m_layers.end ()) {
	return false;
    }

    this->m_layers.erase (it);
    if (const auto drawn = std::ranges::find (this->m_objectsByRenderOrder, layer);
	drawn != this->m_objectsByRenderOrder.end ()) {
	this->m_objectsByRenderOrder.erase (drawn);
    }
    if (auto* sound = layer->asOrNull<Objects::CSound> ()) {
	sound->stop ();
    }
    if (const auto* scriptable = layer->asOrNull<Scripting::ScriptableObject> ()) {
	this->m_scriptEngine->dispatchEvent ("destroy", JS_UNDEFINED, scriptable);
    }
    this->m_destroyed.push_back (layer);
    return true;
}

bool CScene::isLayerAlive (const CObject* layer) const {
    return std::ranges::find (this->m_destroyed, layer) == this->m_destroyed.end ();
}

bool CScene::sortLayer (CObject* layer, int index) {
    const auto from = std::ranges::find (this->m_layers, layer);

    if (from == this->m_layers.end () || this->m_layers.empty ()) {
	return false;
    }

    index = std::clamp (index, 0, static_cast<int> (this->m_layers.size ()) - 1);
    this->m_layers.erase (from);
    this->m_layers.insert (this->m_layers.begin () + index, layer);

    // the draw order follows: the layer goes right before the layer that now comes after it (or after the one
    // before it, when it is last)
    const auto drawn = std::ranges::find (this->m_objectsByRenderOrder, layer);

    if (drawn == this->m_objectsByRenderOrder.end ()) {
	return true;
    }

    this->m_objectsByRenderOrder.erase (drawn);

    auto at = this->m_objectsByRenderOrder.end ();

    if (static_cast<size_t> (index) + 1 < this->m_layers.size ()) {
	at = std::ranges::find (this->m_objectsByRenderOrder, this->m_layers[index + 1]);
    } else if (index > 0) {
	at = std::ranges::find (this->m_objectsByRenderOrder, this->m_layers[index - 1]);
	if (at != this->m_objectsByRenderOrder.end ()) {
	    ++at;
	}
    }

    // the bloom object stays last
    if (at == this->m_objectsByRenderOrder.end () && this->m_bloomObject != nullptr) {
	at = std::ranges::find (this->m_objectsByRenderOrder, this->m_bloomObject);
    }

    this->m_objectsByRenderOrder.insert (at, layer);
    return true;
}

const CObject* CScene::getObject (int id) const {
    const auto object = this->m_objects.find (id);
    return object == this->m_objects.end () ? nullptr : object->second;
}
