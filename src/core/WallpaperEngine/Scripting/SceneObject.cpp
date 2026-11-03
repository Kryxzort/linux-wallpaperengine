#include "SceneObject.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "ScriptEngine.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Utils/ScopeGuard.h"

#include <algorithm>

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

// createLayer/destroyLayer: not supported (a layer built at run time needs the loader at run time); createLayer
// returns undefined (as a missing layer), destroyLayer false
JSValue create_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_UNDEFINED; }
JSValue destroy_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_FALSE; }

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
	{ "destroyLayer", destroy_layer },
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