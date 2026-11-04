#include "SceneObject.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "ScriptEngine.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Data/Model/PropertyAnimation.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Utils/ScopeGuard.h"

#include <algorithm>
#include <nlohmann/json.hpp>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Data::Model::DynamicValue;

SceneObject* get_opaque (JSValueConst this_val) {
    JSClassID classId;
    return static_cast<SceneObject*> (JS_GetAnyOpaque (this_val, &classId));
}

JSValue get_bloom (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.bloom.enabled->value->getBool ());
}

JSValue get_bloomstrength (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.bloom.strength->value->getFloat ());
}

JSValue get_bloomthreshold (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.bloom.threshold->value->getFloat ());
}

JSValue get_clearenabled (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    // LWE always clears the scene with clearcolor
    return JS_NewBool (ctx, true);
}

JSValue get_clearcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return container->getEngine ().newVec (container->getScene ().getScene ().colors.clear->value->getVec3 ());
}

JSValue get_ambientcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return container->getEngine ().newVec (container->getScene ().getScene ().colors.ambient->value->getVec3 ());
}

JSValue get_skylightcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return container->getEngine ().newVec (container->getScene ().getScene ().colors.skylight->value->getVec3 ());
}

JSValue get_fov (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.fov->value->getFloat ());
}

JSValue get_nearz (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.nearz->value->getFloat ());
}

JSValue get_farz (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.farz->value->getFloat ());
}

JSValue get_camerafade (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.fade->value->getBool ());
}

JSValue get_camerashake (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.shake.enabled->value->getBool ());
}

JSValue get_camerashakespeed (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.speed->value->getFloat ());
}

JSValue get_camerashakeamplitude (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.amplitude->value->getFloat ());
}

JSValue get_camerashakeroughness (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.roughness->value->getFloat ());
}

JSValue get_cameraparallax (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.parallax.enabled->value->getBool ());
}

JSValue get_cameraparallaxamount (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.amount->value->getFloat ());
}

JSValue get_cameraparallaxdelay (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.delay->value->getFloat ());
}

JSValue get_cameraparallaxmouseinfluence (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.mouseInfluence->value->getFloat ());
}

namespace {
using WallpaperEngine::Render::CObject;

/** the scripts' view of a layer: the object behind a name, an index, or an ILayer value (nullptr: none) */
CObject* resolveLayer (JSContext* ctx, SceneObject& container, JSValueConst layer) {
    const auto& layers = container.getScene ().getLayers ();

    if (JS_IsNumber (layer)) {
	// "Get a layer by index": the scene's objects in their order
	int index = -1;
	JS_ToInt32 (ctx, &index, layer);

	return index < 0 || index >= static_cast<int> (layers.size ()) ? nullptr : layers[index];
    }

    if (JS_IsString (layer)) {
	const char* name = JS_ToCString (ctx, layer);

	if (name == nullptr) {
	    return nullptr;
	}

	ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

	for (auto* object : layers) {
	    if (object->getObject ().name == name) {
		return object;
	    }
	}

	return nullptr;
    }

    if (auto* object = container.getEngine ().getAdapters ().object->fromValue (layer)) {
	return object;
    }

    return nullptr;
}

JSValue layerValue (SceneObject& container, CObject* object) {
    auto* scriptable = object == nullptr ? nullptr : object->asOrNull<ScriptableObject> ();

    if (scriptable == nullptr) {
	return JS_UNDEFINED;
    }

    return container.getEngine ().getAdapters ().object->instantiate (*scriptable);
}
} // namespace

JSValue get_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_UNDEFINED;
    }

    auto* container = get_opaque (this_val);

    // no such layer: undefined, not an exception without an error
    return layerValue (*container, resolveLayer (ctx, *container, argv[0]));
}

JSValue get_layer_count (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewInt32 (ctx, static_cast<int> (get_opaque (this_val)->getScene ().getLayers ().size ()));
}

JSValue enumerate_layers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    JSValue result = JS_NewArray (ctx);
    uint32_t count = 0;

    for (auto* object : container->getScene ().getLayers ()) {
	JSValue layer = layerValue (*container, object);

	if (!JS_IsUndefined (layer)) {
	    JS_SetPropertyUint32 (ctx, result, count++, layer);
	}
    }

    return result;
}

JSValue get_layer_index (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    auto* object = argc < 1 ? nullptr : resolveLayer (ctx, *container, argv[0]);
    const auto& layers = container->getScene ().getLayers ();
    const auto it = std::ranges::find (layers, object);

    return JS_NewInt32 (ctx, object == nullptr || it == layers.end () ? -1 : static_cast<int> (it - layers.begin ()));
}

JSValue sort_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    auto* object = argc < 2 ? nullptr : resolveLayer (ctx, *container, argv[0]);
    int index = 0;

    if (object == nullptr) {
	return JS_FALSE;
    }

    JS_ToInt32 (ctx, &index, argv[1]);
    return JS_NewBool (ctx, container->getScene ().sortLayer (object, index));
}

namespace {
/** createLayer ('models/x.json'): the object that asset makes (an image model, a particle system, a sound) */
std::string configForAsset (const std::string& file) {
    nlohmann::json config = nlohmann::json::object ();
    const auto ends = [&file] (const char* suffix) {
	const std::string s = suffix;
	return file.size () >= s.size () && file.compare (file.size () - s.size (), s.size (), s) == 0;
    };

    if (file.rfind ("particles/", 0) == 0) {
	config["particle"] = file;
    } else if (ends (".mp3") || ends (".wav") || ends (".ogg") || file.rfind ("sounds/", 0) == 0) {
	config["sound"] = file;
    } else if (ends (".mdl")) {
	config["model"] = file;
    } else {
	config["image"] = file;
    }

    return config.dump ();
}

std::string jsString (JSContext* ctx, JSValueConst value) {
    const char* str = JS_ToCString (ctx, value);
    std::string result = str == nullptr ? "" : str;
    JS_FreeCString (ctx, str);
    return result;
}
} // namespace

// IScene.createLayer (an asset path, an IAssetHandle from engine.registerAsset, or a layer's configuration as
// scene.json writes one): the layer, built now and drawn from this frame; undefined when it can't be built
JSValue create_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    if (argc < 1) {
	return JS_UNDEFINED;
    }

    std::string config;

    if (JS_IsString (argv[0])) {
	config = configForAsset (jsString (ctx, argv[0]));
    } else if (JS_IsObject (argv[0])) {
	JSValue file = JS_GetPropertyStr (ctx, argv[0], "file");
	JSValue image = JS_GetPropertyStr (ctx, argv[0], "image");
	const bool handle = JS_IsString (file) && JS_IsUndefined (image);

	if (handle) {
	    config = configForAsset (jsString (ctx, file));
	} else {
	    // Wallpaper Engine's own serialiser (vectors as "x y z", baseclasses.js _Internal.stringifyConfig)
	    JSValue internal = JS_GetPropertyStr (ctx, container->getEngine ().getGlobalThis (), "_Internal");
	    JSValue stringify = JS_IsObject (internal) ? JS_GetPropertyStr (ctx, internal, "stringifyConfig") : JS_UNDEFINED;
	    JSValue text = JS_IsFunction (ctx, stringify) ? JS_Call (ctx, stringify, internal, 1, argv)
							   : JS_JSONStringify (ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
	    if (JS_IsException (text)) {
		JS_FreeValue (ctx, file);
		JS_FreeValue (ctx, image);
		JS_FreeValue (ctx, stringify);
		JS_FreeValue (ctx, internal);
		return JS_EXCEPTION;
	    }
	    config = jsString (ctx, text);
	    JS_FreeValue (ctx, text);
	    JS_FreeValue (ctx, stringify);
	    JS_FreeValue (ctx, internal);
	}
	JS_FreeValue (ctx, file);
	JS_FreeValue (ctx, image);
    } else {
	return JS_UNDEFINED;
    }

    return layerValue (*container, container->getScene ().createLayer (config));
}

// __cameraTransforms (): [eye, center, up, zoom] of the scene's camera (camera.eye/center/up, general.zoom)
JSValue camera_transforms (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    const auto& camera = container->getScene ().getScene ().camera;
    JSValue result = JS_NewArray (ctx);
    JS_SetPropertyUint32 (ctx, result, 0, container->getEngine ().newVec (camera.configuration.eye));
    JS_SetPropertyUint32 (ctx, result, 1, container->getEngine ().newVec (camera.configuration.center));
    JS_SetPropertyUint32 (ctx, result, 2, container->getEngine ().newVec (camera.configuration.up));
    JS_SetPropertyUint32 (ctx, result, 3, JS_NewFloat64 (ctx, camera.zoom == nullptr ? 1.0 : camera.zoom->value->getFloat ()));
    return result;
}

// __setCameraTransforms (eye, center, up, zoom): the camera follows from the next frame (a 3D scene's view; the zoom
// in any scene)
JSValue set_camera_transforms (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    auto& camera = const_cast<WallpaperEngine::Data::Model::Scene&> (container->getScene ().getScene ()).camera;
    glm::vec3* targets[] = { &camera.configuration.eye, &camera.configuration.center, &camera.configuration.up };
    for (int i = 0; i < 3 && i < argc; i++) {
	DynamicValue value (*targets[i]);
	if (container->getEngine ().jsToDynamic (argv[i], value)) {
	    *targets[i] = value.getVec3 ();
	}
    }
    if (argc > 3 && camera.zoom != nullptr && JS_IsNumber (argv[3])) {
	container->getEngine ().jsToDynamic (argv[3], *camera.zoom->value);
    }
    return JS_UNDEFINED;
}

// __sceneJson (): the scene file as written (IScene.getInitialLayerConfig, builtins.js)
JSValue scene_json (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    try {
	const auto& scene = container->getScene ();
	return JS_NewString (ctx, scene.getAssetLocator ().readString (scene.getScene ().filename).c_str ());
    } catch (const std::exception&) {
	return JS_UNDEFINED;
    }
}

// IScene.getAnimation (name): the scene's keyframe animation of that name (the first one without a name)
JSValue scene_get_animation (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    const std::string name = argc > 0 && JS_IsString (argv[0]) ? jsString (ctx, argv[0]) : "";
    for (const auto& animated : container->getScene ().getScene ().animatedValues) {
	if (animated.alive && !*animated.alive) {
	    continue;
	}
	const auto& animation = animated.value->getAnimation ();
	if (animation != nullptr && (name.empty () || animation->getName () == name)) {
	    return container->getEngine ().getAdapters ().object->animation (*animated.value);
	}
    }
    return JS_UNDEFINED;
}

// IScene.destroyLayer: true when the layer was in the scene
JSValue destroy_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    auto* object = argc < 1 ? nullptr : resolveLayer (ctx, *container, argv[0]);

    return JS_NewBool (ctx, object != nullptr && container->getScene ().destroyLayer (object));
}

/** IScene's properties are writable ("all properties are writable"): a script's value replaces the scene's */
JSValue scene_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    auto* container = get_opaque (this_val);
    auto* value = container->settableValue (magic);

    if (value != nullptr && argc > 0) {
	container->getEngine ().jsToDynamic (argv[0], *value);
    }

    return JS_UNDEFINED;
}

SceneObject::SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0) {
    JSContext* ctx = this->m_engine.getContext ();
    this->m_definition = { .class_name = "IScene" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (ctx, this->m_classId);

    JS_DupValue (ctx, this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);

    // the properties, read live and writable (the setter's magic is the index settableValue () maps)
    const std::pair<const char*, JSCFunction*> properties[] = {
	{ "bloom", get_bloom },
	{ "bloomstrength", get_bloomstrength },
	{ "bloomthreshold", get_bloomthreshold },
	{ "clearenabled", get_clearenabled },
	{ "clearcolor", get_clearcolor },
	{ "ambientcolor", get_ambientcolor },
	{ "skylightcolor", get_skylightcolor },
	{ "fov", get_fov },
	{ "nearz", get_nearz },
	{ "farz", get_farz },
	{ "camerafade", get_camerafade },
	{ "camerashake", get_camerashake },
	{ "camerashakespeed", get_camerashakespeed },
	{ "camerashakeamplitude", get_camerashakeamplitude },
	{ "camerashakeroughness", get_camerashakeroughness },
	{ "cameraparallax", get_cameraparallax },
	{ "cameraparallaxamount", get_cameraparallaxamount },
	{ "cameraparallaxdelay", get_cameraparallaxdelay },
	{ "cameraparallaxmouseinfluence", get_cameraparallaxmouseinfluence },
    };
    int index = 0;

    for (const auto& [name, getter] : properties) {
	JSAtom atom = JS_NewAtom (ctx, name);
	JS_DefinePropertyGetSet (
	    ctx, this->m_instance, atom, JS_NewCFunction (ctx, getter, "get", 0),
	    JS_NewCFunctionMagic (ctx, scene_set_value, "set", 1, JS_CFUNC_generic_magic, index++), JS_PROP_ENUMERABLE
	);
	JS_FreeAtom (ctx, atom);
    }

    const std::pair<const char*, JSCFunction*> methods[] = {
	{ "getLayer", get_layer },	       { "getLayerCount", get_layer_count }, { "enumerateLayers", enumerate_layers },
	{ "getLayerIndex", get_layer_index }, { "sortLayer", sort_layer },	      { "createLayer", create_layer },
	{ "destroyLayer", destroy_layer },     { "getAnimation", scene_get_animation },
	{ "__cameraTransforms", camera_transforms }, { "__setCameraTransforms", set_camera_transforms },
	{ "__sceneJson", scene_json },
    };

    for (const auto& [name, function] : methods) {
	JS_DefinePropertyValueStr (ctx, this->m_instance, name, JS_NewCFunction (ctx, function, name, 1), JS_PROP_ENUMERABLE);
    }
}

DynamicValue* SceneObject::settableValue (int index) const {
    auto& scene = const_cast<WallpaperEngine::Data::Model::Scene&> (this->m_scene.getScene ());

    switch (index) {
	case 0:
	    return scene.camera.bloom.enabled->value.get ();
	case 1:
	    return scene.camera.bloom.strength->value.get ();
	case 2:
	    return scene.camera.bloom.threshold->value.get ();
	case 4:
	    return scene.colors.clear->value.get ();
	case 5:
	    return scene.colors.ambient->value.get ();
	case 6:
	    return scene.colors.skylight->value.get ();
	case 7:
	    return scene.camera.projection.fov->value.get ();
	case 8:
	    return scene.camera.projection.nearz->value.get ();
	case 9:
	    return scene.camera.projection.farz->value.get ();
	case 10:
	    return scene.camera.fade->value.get ();
	case 11:
	    return scene.camera.shake.enabled->value.get ();
	case 12:
	    return scene.camera.shake.speed->value.get ();
	case 13:
	    return scene.camera.shake.amplitude->value.get ();
	case 14:
	    return scene.camera.shake.roughness->value.get ();
	case 15:
	    return scene.camera.parallax.enabled->value.get ();
	case 16:
	    return scene.camera.parallax.amount->value.get ();
	case 17:
	    return scene.camera.parallax.delay->value.get ();
	case 18:
	    return scene.camera.parallax.mouseInfluence->value.get ();
	default:
	    return nullptr;
    }
}

SceneObject::~SceneObject () { JS_FreeValue (this->m_engine.getContext (), this->m_instance); }