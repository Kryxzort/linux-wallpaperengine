#include "ScriptableObjectAdapter.h"

#include <cstring>
#include <ranges>
#include <utility>

#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "WallpaperEngine/Utils/ScopeGuard.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;
using WallpaperEngine::Render::Objects::CImage;
using WallpaperEngine::Render::Objects::CRenderable;
using WallpaperEngine::Render::Objects::CSound;
using WallpaperEngine::Render::Objects::CText;
using WallpaperEngine::Scripting::ScriptableObject;

#define SCRIPTABLE_OPAQUE_MAGIC 0xdeadbeef

struct OpaqueScriptableObjectAdapter {
    unsigned int magic;
    ScriptableObjectAdapter& adapter;
    ScriptableObject& object;
    /** what scripts store on the layer under names it doesn't have (kept: one JS object per layer) */
    JSValue extras;
};

/** an IEffect: one effect of an image layer */
struct OpaqueEffect {
    ScriptableObjectAdapter& adapter;
    const ImageEffect& effect;
};

/** an ITextureAnimation: the texture animation of an image layer */
struct OpaqueTextureAnimation {
    ScriptableObjectAdapter& adapter;
    CImage& image;
};

namespace {
OpaqueScriptableObjectAdapter* layerOf (JSValueConst value) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (value, &classId));

    if (container == nullptr || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return nullptr;
    }

    return container;
}

/** the C string of an atom, freed at scope exit */
struct AtomName {
    JSContext* ctx;
    const char* str;

    AtomName (JSContext* c, JSAtom atom) : ctx (c), str (JS_AtomToCString (c, atom)) { }
    ~AtomName () { JS_FreeCString (ctx, str); }
};

/** names scripts use that differ from the registered property's */
const char* propertyAlias (const char* name) {
    if (strcmp (name, "pointsize") == 0) {
	return "pointSize";
    }

    return name;
}

enum LayerMethod {
    Method_GetEffect,
    Method_GetEffectCount,
    Method_GetTextureAnimation,
    Method_GetParent,
    Method_GetChildren,
    Method_Play,
    Method_Stop,
    Method_Pause,
    Method_IsPlaying,
};

constexpr std::pair<const char*, LayerMethod> kMethods[] = {
    { "getEffect", Method_GetEffect },	 { "getEffectCount", Method_GetEffectCount },
    { "getTextureAnimation", Method_GetTextureAnimation },
    { "getParent", Method_GetParent },	 { "getChildren", Method_GetChildren },
    { "play", Method_Play },		 { "stop", Method_Stop },
    { "pause", Method_Pause },		 { "isPlaying", Method_IsPlaying },
};

JSValue newEffect (JSContext* ctx, ScriptableObjectAdapter& adapter, const ImageEffect& effect) {
    JSValue result = JS_NewObjectClass (ctx, static_cast<int> (adapter.getEffectClassId ()));
    JS_SetOpaque (result, new OpaqueEffect { .adapter = adapter, .effect = effect });
    return result;
}

JSValue layer_method (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    auto* layer = layerOf (this_val);

    if (layer == nullptr) {
	return JS_ThrowTypeError (ctx, "not a layer");
    }

    auto& object = layer->object;
    auto& adapter = layer->adapter;
    auto* image = object.asOrNull<CImage> ();
    auto* sound = object.asOrNull<CSound> ();

    switch (static_cast<LayerMethod> (magic)) {
	case Method_GetEffect: {
	    if (image == nullptr || argc < 1) {
		return JS_UNDEFINED;
	    }

	    const auto& effects = image->getImage ().effects;

	    if (JS_IsNumber (argv[0])) {
		int index = -1;
		JS_ToInt32 (ctx, &index, argv[0]);

		if (index < 0 || index >= static_cast<int> (effects.size ())) {
		    return JS_UNDEFINED;
		}

		return newEffect (ctx, adapter, *effects[index]);
	    }

	    const char* name = JS_ToCString (ctx, argv[0]);

	    if (name == nullptr) {
		return JS_EXCEPTION;
	    }

	    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

	    for (const auto& effect : effects) {
		if (effect->name == name) {
		    return newEffect (ctx, adapter, *effect);
		}
	    }

	    return JS_UNDEFINED;
	}
	case Method_GetEffectCount:
	    return JS_NewInt32 (ctx, image == nullptr ? 0 : static_cast<int> (image->getImage ().effects.size ()));
	case Method_GetTextureAnimation: {
	    if (image == nullptr) {
		return JS_UNDEFINED;
	    }

	    JSValue result = JS_NewObjectClass (ctx, static_cast<int> (adapter.getTextureAnimationClassId ()));
	    JS_SetOpaque (result, new OpaqueTextureAnimation { .adapter = adapter, .image = *image });
	    return result;
	}
	case Method_GetParent: {
	    const auto& parent = object.getObject ().parent;
	    const auto* parentObject = parent.has_value () ? object.getScene ().getObject (*parent) : nullptr;
	    auto* scriptable = parentObject == nullptr
		? nullptr
		: const_cast<WallpaperEngine::Render::CObject*> (parentObject)->asOrNull<ScriptableObject> ();

	    return scriptable == nullptr ? JS_NULL : adapter.instantiate (*scriptable);
	}
	case Method_GetChildren: {
	    JSValue result = JS_NewArray (ctx);
	    uint32_t count = 0;

	    for (const auto* cur : object.getScene ().getObjectsByRenderOrder ()) {
		if (!cur->getObject ().parent.has_value () || *cur->getObject ().parent != object.getId ()) {
		    continue;
		}

		if (auto* child = const_cast<WallpaperEngine::Render::CObject*> (cur)->asOrNull<ScriptableObject> ()) {
		    JS_SetPropertyUint32 (ctx, result, count++, adapter.instantiate (*child));
		}
	    }

	    return result;
	}
	case Method_Play:
	    if (sound != nullptr) {
		sound->play ();
	    }
	    return JS_UNDEFINED;
	case Method_Stop:
	    if (sound != nullptr) {
		sound->stop ();
	    }
	    return JS_UNDEFINED;
	case Method_Pause:
	    if (sound != nullptr) {
		sound->pause ();
	    }
	    return JS_UNDEFINED;
	case Method_IsPlaying:
	    return JS_NewBool (ctx, sound != nullptr && sound->isPlaying ());
    }

    return JS_UNDEFINED;
}
} // namespace

void scriptableobject_finalizer (JSRuntime* rt, JSValueConst val);

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    auto* container = layerOf (obj_val);

    if (container == nullptr) {
	return JS_EXCEPTION;
    }

    const AtomName atomName (ctx, atom);

    if (atomName.str == nullptr) {
	return JS_EXCEPTION;
    }

    const char* name = propertyAlias (atomName.str);
    auto& object = container->object;
    auto& engine = container->adapter.getEngine ();

    // ILayer.name / id (read-only)
    if (strcmp (name, "name") == 0) {
	return JS_NewString (ctx, object.getObject ().name.c_str ());
    }
    if (strcmp (name, "id") == 0) {
	return JS_NewInt32 (ctx, object.getId ());
    }
    if (strcmp (name, "solid") == 0) {
	return JS_NewBool (ctx, object.isSolid ());
    }

    // a registered property: its live value (looked up, not getProperty: that logs an error per miss)
    const auto& properties = object.getProperties ();

    if (const auto it = properties.find (name); it != properties.end ()) {
	return engine.dynamicToJs (it->second.value);
    }

    // IImageLayer.size / ITextLayer.size: the layer's size in pixels
    if (strcmp (name, "size") == 0) {
	if (const auto* image = object.asOrNull<CImage> ()) {
	    return engine.newVec (image->getSize ());
	}
	if (object.getObject ().is<Text> ()) {
	    return engine.newVec (object.getObject ().as<Text> ()->size);
	}
    }

    // ITextLayer alignment (read; the renderer doesn't lay out by it yet)
    if (object.getObject ().is<Text> ()) {
	const auto* text = object.getObject ().as<Text> ();

	if (strcmp (name, "horizontalalign") == 0) {
	    return JS_NewString (ctx, text->alignment.c_str ());
	}
	if (strcmp (name, "verticalalign") == 0) {
	    return JS_NewString (ctx, text->verticalalign.c_str ());
	}
    }

    // what the script stored on the layer wins over a method of the same name
    JSValue extra = JS_GetPropertyStr (ctx, container->extras, atomName.str);

    if (!JS_IsUndefined (extra)) {
	return extra;
    }

    if (JSValueConst method = container->adapter.method (name); !JS_IsUndefined (method)) {
	return JS_DupValue (ctx, method);
    }

    return JS_UNDEFINED;
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    auto* container = layerOf (obj_val);

    if (container == nullptr) {
	return -1;
    }

    const AtomName atomName (ctx, atom);

    if (atomName.str == nullptr) {
	return -1;
    }

    const char* name = propertyAlias (atomName.str);
    auto& object = container->object;

    if (strcmp (name, "solid") == 0) {
	object.setSolid (JS_ToBool (ctx, val) != 0);
	return 1;
    }

    const auto& properties = object.getProperties ();

    // a registered property (origin, scale, angles, visible, alpha, color, text, volume, …) takes the value
    if (const auto it = properties.find (name); it != properties.end ()) {
	container->adapter.getEngine ().jsToDynamic (val, it->second.value);
	return 1;
    }

    // anything else is kept on the layer (one JS object per layer, so it is there on the next getLayer)
    JS_SetPropertyStr (ctx, container->extras, atomName.str, JS_DupValue (ctx, val));
    return 1;
}

static JSValue effect_material_count (JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32 (ctx, 1); }
static JSValue effect_material_noop (JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }

// IEffect: name (read-only), visible (read/write: an effect's visibility is live since E41), and the material
// calls as no-ops (WE: every effect has at least one material)
JSValue effect_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;
    auto* effect = static_cast<OpaqueEffect*> (JS_GetAnyOpaque (obj_val, &classId));
    const AtomName name (ctx, atom);

    if (effect == nullptr || name.str == nullptr) {
	return JS_EXCEPTION;
    }

    if (strcmp (name.str, "name") == 0) {
	return JS_NewString (ctx, effect->effect.name.c_str ());
    }
    if (strcmp (name.str, "visible") == 0) {
	return JS_NewBool (ctx, effect->effect.visible->value->getBool ());
    }
    if (strcmp (name.str, "getMaterialCount") == 0) {
	return JS_NewCFunction (ctx, effect_material_count, "getMaterialCount", 0);
    }
    if (strcmp (name.str, "setMaterialProperty") == 0 || strcmp (name.str, "executeMaterialFunction") == 0) {
	return JS_NewCFunction (ctx, effect_material_noop, name.str, 1);
    }

    return JS_UNDEFINED;
}

int effect_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;
    auto* effect = static_cast<OpaqueEffect*> (JS_GetAnyOpaque (obj_val, &classId));
    const AtomName name (ctx, atom);

    if (effect == nullptr || name.str == nullptr) {
	return -1;
    }

    if (strcmp (name.str, "visible") == 0) {
	effect->effect.visible->value->update (JS_ToBool (ctx, val) != 0, DynamicValue::UpdateSource::Script);
    }

    return 1;
}

void effect_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    delete static_cast<OpaqueEffect*> (JS_GetAnyOpaque (val, &classId));
}

namespace {
enum TextureAnimationMethod {
    TexAnim_Play,
    TexAnim_Stop,
    TexAnim_Pause,
    TexAnim_IsPlaying,
    TexAnim_GetFrame,
    TexAnim_SetFrame,
    TexAnim_Join,
};

constexpr std::pair<const char*, TextureAnimationMethod> kTextureAnimationMethods[] = {
    { "play", TexAnim_Play },	{ "stop", TexAnim_Stop },	  { "pause", TexAnim_Pause },
    { "isPlaying", TexAnim_IsPlaying }, { "getFrame", TexAnim_GetFrame }, { "setFrame", TexAnim_SetFrame },
    { "join", TexAnim_Join },
};

JSValue texture_animation_method (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    JSClassID classId = 0;
    auto* animation = static_cast<OpaqueTextureAnimation*> (JS_GetAnyOpaque (this_val, &classId));

    if (animation == nullptr) {
	return JS_ThrowTypeError (ctx, "not a texture animation");
    }

    auto& image = animation->image;

    switch (static_cast<TextureAnimationMethod> (magic)) {
	case TexAnim_Play:
	    image.detachTextureAnimation ().playing = true;
	    return JS_UNDEFINED;
	case TexAnim_Pause:
	    image.detachTextureAnimation ().playing = false;
	    return JS_UNDEFINED;
	case TexAnim_Stop:
	    // "stops the animation and reverts to the beginning"
	    image.setTextureFrame (0);
	    image.detachTextureAnimation ().playing = false;
	    return JS_UNDEFINED;
	case TexAnim_IsPlaying: {
	    const auto& state = image.getTextureAnimationState ();
	    return JS_NewBool (ctx, !state.detached || state.playing);
	}
	case TexAnim_GetFrame:
	    return JS_NewInt32 (ctx, image.getTextureFrame ());
	case TexAnim_SetFrame: {
	    double frame = 0.0;
	    if (argc > 0) {
		JS_ToFloat64 (ctx, &frame, argv[0]);
	    }
	    image.setTextureFrame (static_cast<int> (frame));
	    return JS_UNDEFINED;
	}
	case TexAnim_Join:
	    image.joinTextureAnimation ();
	    return JS_UNDEFINED;
    }

    return JS_UNDEFINED;
}
} // namespace

JSValue texture_animation_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;
    auto* animation = static_cast<OpaqueTextureAnimation*> (JS_GetAnyOpaque (obj_val, &classId));
    const AtomName name (ctx, atom);

    if (animation == nullptr || name.str == nullptr) {
	return JS_EXCEPTION;
    }

    auto& image = animation->image;

    if (strcmp (name.str, "frameCount") == 0) {
	return JS_NewInt32 (ctx, image.getTextureFrameCount ());
    }
    if (strcmp (name.str, "duration") == 0) {
	return JS_NewFloat64 (ctx, image.getAnimationTime ());
    }
    if (strcmp (name.str, "rate") == 0) {
	return JS_NewFloat64 (ctx, image.getTextureAnimationState ().rate);
    }

    for (const auto& [method, id] : kTextureAnimationMethods) {
	if (strcmp (name.str, method) == 0) {
	    return JS_NewCFunctionMagic (ctx, texture_animation_method, method, 1, JS_CFUNC_generic_magic, id);
	}
    }

    return JS_UNDEFINED;
}

int texture_animation_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;
    auto* animation = static_cast<OpaqueTextureAnimation*> (JS_GetAnyOpaque (obj_val, &classId));
    const AtomName name (ctx, atom);

    if (animation == nullptr || name.str == nullptr) {
	return -1;
    }

    if (strcmp (name.str, "rate") == 0) {
	double rate = 1.0;
	JS_ToFloat64 (ctx, &rate, val);
	animation->image.detachTextureAnimation ().rate = rate;
    }

    return 1;
}

void texture_animation_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    delete static_cast<OpaqueTextureAnimation*> (JS_GetAnyOpaque (val, &classId));
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine), m_exoticMethods (), m_effectExoticMethods (), m_textureAnimationExoticMethods (),
    m_effectDefinition (), m_textureAnimationDefinition (), m_name (std::move (name)) {
    // the layer's registered properties, read and written live
    this->m_exoticMethods.get_property = scriptableobject_property_get;
    this->m_exoticMethods.set_property = scriptableobject_property_set;
    this->registerType (
	{
	    .class_name = m_name.c_str (),
	    .finalizer = scriptableobject_finalizer,
	    .exotic = &m_exoticMethods,
	}
    );

    JSRuntime* rt = engine.getRuntime ();

    this->m_effectExoticMethods.get_property = effect_property_get;
    this->m_effectExoticMethods.set_property = effect_property_set;
    this->m_effectDefinition = {
	.class_name = "IEffect",
	.finalizer = effect_finalizer,
	.exotic = &m_effectExoticMethods,
    };
    JS_NewClassID (rt, &this->m_effectClassId);
    JS_NewClass (rt, this->m_effectClassId, &this->m_effectDefinition);

    this->m_textureAnimationExoticMethods.get_property = texture_animation_property_get;
    this->m_textureAnimationExoticMethods.set_property = texture_animation_property_set;
    this->m_textureAnimationDefinition = {
	.class_name = "ITextureAnimation",
	.finalizer = texture_animation_finalizer,
	.exotic = &m_textureAnimationExoticMethods,
    };
    JS_NewClassID (rt, &this->m_textureAnimationClassId);
    JS_NewClass (rt, this->m_textureAnimationClassId, &this->m_textureAnimationDefinition);

    JSContext* ctx = engine.getContext ();

    for (const auto& [method, id] : kMethods) {
	this->m_methods.emplace (method, JS_NewCFunctionMagic (ctx, layer_method, method, 1, JS_CFUNC_generic_magic, id));
    }
}

ScriptableObjectAdapter::~ScriptableObjectAdapter () {
    JSContext* ctx = this->m_engine.getContext ();

    for (auto& instance : this->m_instances | std::views::values) {
	JS_FreeValue (ctx, instance);
    }
    for (auto& method : this->m_methods | std::views::values) {
	JS_FreeValue (ctx, method);
    }
}

JSValueConst ScriptableObjectAdapter::method (const char* name) const {
    const auto it = this->m_methods.find (name);
    return it == this->m_methods.end () ? JS_UNDEFINED : it->second;
}

JSValue ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    JSContext* ctx = this->m_engine.getContext ();

    if (const auto it = this->m_instances.find (&object); it != this->m_instances.end ()) {
	return JS_DupValue (ctx, it->second);
    }

    JSValue result = JS_NewObjectClass (ctx, static_cast<int> (this->m_classId));
    JS_SetOpaque (
	result, new OpaqueScriptableObjectAdapter {
		    .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .object = object, .extras = JS_NewObject (ctx)
		}
    );
    this->m_instances.emplace (&object, JS_DupValue (ctx, result));

    return result;
}

ScriptableObject* ScriptableObjectAdapter::fromValue (JSValueConst value) const {
    auto* container = layerOf (value);
    return container == nullptr || &container->adapter != this ? nullptr : &container->object;
}

void scriptableobject_finalizer (JSRuntime* rt, JSValueConst val) {
    auto* container = layerOf (val);

    if (container == nullptr) {
	return;
    }

    JS_FreeValueRT (rt, container->extras);
    delete container;
}

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}
