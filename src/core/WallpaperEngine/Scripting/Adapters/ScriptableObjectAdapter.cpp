#include "ScriptableObjectAdapter.h"

#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <ranges>
#include <utility>

#include "WallpaperEngine/Data/Model/PropertyAnimation.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/Puppet.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "WallpaperEngine/Utils/ScopeGuard.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;
using WallpaperEngine::Render::Objects::CImage;
using WallpaperEngine::Render::Objects::CRenderable;
using WallpaperEngine::Render::Objects::CParticle;
using WallpaperEngine::Render::Objects::CSound;
using WallpaperEngine::Render::Objects::CText;
namespace Puppet = WallpaperEngine::Render::Objects::Puppet;
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

/** an IAnimation: the keyframe animation of one property */
struct OpaqueAnimation {
    DynamicValue& value;
};

/** an IVideoTexture: the video an image layer shows */
struct OpaqueVideoTexture {
    ScriptableObjectAdapter& adapter;
    CImage& image;
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

std::string jsString (JSContext* ctx, JSValueConst value) {
    const char* str = JS_ToCString (ctx, value);
    std::string result = str == nullptr ? "" : str;
    JS_FreeCString (ctx, str);
    return result;
}

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
    Method_GetAnimation,
    Method_World,
    Method_ParentWorld,
    Method_SetParent,
    Method_Attachments,
    Method_Valid,
    Method_GetVideoTexture,
    Method_AnimLayers,
    Method_AnimLayerSet,
    Method_AnimLayerCreate,
    Method_AnimLayerDestroy,
    Method_Bones,
    Method_BoneOverride,
    Method_Shapes,
    Method_ShapeOverride,
    Method_EmitParticles,
    Method_ParticleGet,
    Method_ParticleSet,
};

constexpr std::pair<const char*, LayerMethod> kMethods[] = {
    { "getEffect", Method_GetEffect },	 { "getEffectCount", Method_GetEffectCount },
    { "getTextureAnimation", Method_GetTextureAnimation },
    { "getParent", Method_GetParent },	 { "getChildren", Method_GetChildren },
    { "play", Method_Play },		 { "stop", Method_Stop },
    { "pause", Method_Pause },		 { "isPlaying", Method_IsPlaying },
    { "getAnimation", Method_GetAnimation },
    // the natives under the JS ILayer methods (builtins.js: getTransformMatrix, setParent, getAttachment*, …)
    { "__world", Method_World },	 { "__parentWorld", Method_ParentWorld },
    { "__setParent", Method_SetParent }, { "__attachments", Method_Attachments },
    { "__valid", Method_Valid },
    { "getVideoTexture", Method_GetVideoTexture },
    // the natives under the JS IAnimationLayer and the bone methods (builtins.js)
    { "__animLayers", Method_AnimLayers },	   { "__animLayerSet", Method_AnimLayerSet },
    { "__animLayerCreate", Method_AnimLayerCreate }, { "__animLayerDestroy", Method_AnimLayerDestroy },
    { "__bones", Method_Bones },		   { "__boneOverride", Method_BoneOverride },
    { "__shapes", Method_Shapes },		   { "__shapeOverride", Method_ShapeOverride },
    { "emitParticles", Method_EmitParticles },
    { "__particleGet", Method_ParticleGet },	   { "__particleSet", Method_ParticleSet },
};

bool matrixFrom (JSContext* ctx, JSValueConst array, glm::mat4& out) {
    if (!JS_IsObject (array)) {
	return false;
    }
    float* f = &out[0][0];
    for (uint32_t i = 0; i < 16; i++) {
	JSValue v = JS_GetPropertyUint32 (ctx, array, i);
	double d = 0.0;
	if (JS_ToFloat64 (ctx, &d, v) < 0) {
	    JS_FreeValue (ctx, v);
	    return false;
	}
	JS_FreeValue (ctx, v);
	f[i] = static_cast<float> (d);
    }
    return true;
}

/** a glm matrix as the 16 numbers of SceneScript's Mat4.m (column-major, as glm keeps it) */
JSValue matrixArray (JSContext* ctx, const glm::mat4& m) {
    JSValue result = JS_NewArray (ctx);
    const float* f = &m[0][0];
    for (uint32_t i = 0; i < 16; i++) {
	JS_SetPropertyUint32 (ctx, result, i, JS_NewFloat64 (ctx, f[i]));
    }
    return result;
}

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
    auto* particle = object.asOrNull<CParticle> ();

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

	    // "the current parent layer or undefined if the layer is not parented" (WE's docs)
	    return scriptable == nullptr ? JS_UNDEFINED : adapter.instantiate (*scriptable);
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
	    } else if (particle != nullptr) {
		particle->scriptPlay ();
	    }
	    return JS_UNDEFINED;
	case Method_Stop:
	    if (sound != nullptr) {
		sound->stop ();
	    } else if (particle != nullptr) {
		particle->scriptStop ();
	    }
	    return JS_UNDEFINED;
	case Method_Pause:
	    if (sound != nullptr) {
		sound->pause ();
	    } else if (particle != nullptr) {
		particle->scriptPause ();
	    }
	    return JS_UNDEFINED;
	case Method_IsPlaying:
	    if (particle != nullptr) {
		return JS_NewBool (ctx, particle->scriptIsPlaying ());
	    }
	    return JS_NewBool (ctx, sound != nullptr && sound->isPlaying ());
	case Method_EmitParticles: {
	    // "emit x number of particles instantly" (no count: one)
	    int count = 1;
	    if (argc > 0 && !JS_IsUndefined (argv[0])) {
		JS_ToInt32 (ctx, &count, argv[0]);
	    }
	    if (particle != nullptr && count > 0) {
		particle->scriptEmit (static_cast<uint32_t> (count));
	    }
	    return JS_UNDEFINED;
	}
	case Method_ParticleGet: {
	    DynamicValue* value = particle == nullptr || argc < 1 ? nullptr : particle->instanceValue (jsString (ctx, argv[0]));
	    return value == nullptr ? JS_UNDEFINED : adapter.getEngine ().dynamicToJs (*value);
	}
	case Method_ParticleSet: {
	    DynamicValue* value = particle == nullptr || argc < 2 ? nullptr : particle->instanceValue (jsString (ctx, argv[0]));
	    return JS_NewBool (ctx, value != nullptr && adapter.getEngine ().jsToDynamic (argv[1], *value));
	}
	case Method_GetAnimation: {
	    // getAnimation (name?): the keyframe animation of the property whose script runs now (a script's own:
	    // `thisObject.getAnimation ()`), else of a property of this layer, by name when one is given
	    std::string name;
	    if (argc > 0 && JS_IsString (argv[0])) {
		const char* str = JS_ToCString (ctx, argv[0]);
		name = str == nullptr ? "" : str;
		JS_FreeCString (ctx, str);
	    }
	    auto matches = [&] (DynamicValue& value) {
		const auto& animation = value.getAnimation ();
		return animation != nullptr && (name.empty () || animation->getName () == name);
	    };
	    DynamicValue* found = nullptr;
	    if (const auto* running = adapter.getEngine ().getRunningModule ();
		running != nullptr && &running->object == &object && matches (running->value)) {
		found = &running->value;
	    }
	    for (const auto& entry : object.getProperties () | std::views::values) {
		if (found == nullptr && matches (entry.value)) {
		    found = &entry.value;
		}
	    }
	    if (found == nullptr && image != nullptr) {
		for (const auto& effect : image->getImage ().effects) {
		    for (const auto& pass : effect->passOverrides) {
			for (const auto& constant : pass->constants | std::views::values) {
			    if (found == nullptr && constant != nullptr && constant->value != nullptr
				&& matches (*constant->value)) {
				found = constant->value.get ();
			    }
			}
		    }
		}
	    }
	    if (found == nullptr) {
		return JS_UNDEFINED;
	    }
	    JSValue result = JS_NewObjectClass (ctx, static_cast<int> (adapter.getAnimationClassId ()));
	    JS_SetOpaque (result, new OpaqueAnimation { .value = *found });
	    return result;
	}
	case Method_World:
	    // ILayer.getTransformMatrix: the layer's world matrix, T (origin) · Rz · Ry · Rx · S with its parents'
	    // (scene units, y up, as `origin`; no parallax, no camera)
	    return matrixArray (ctx, object.getScene ().worldMatrixOf (object.getObject ()));
	case Method_ParentWorld: {
	    const auto& parent = object.getObject ().parent;
	    const auto* parentObject = parent.has_value () ? object.getScene ().getObject (*parent) : nullptr;
	    return matrixArray (
		ctx, parentObject == nullptr ? glm::mat4 (1.0f) : object.getScene ().worldMatrixOf (parentObject->getObject ())
	    );
	}
	case Method_SetParent: {
	    // __setParent (layer | null, attachment name | undefined): false when the parent would be the layer itself
	    // or one of its children (a cycle)
	    ScriptableObject* parent = argc > 0 ? adapter.fromValue (argv[0]) : nullptr;
	    if (parent != nullptr) {
		for (const auto* cur = &parent->getObject (); cur != nullptr;) {
		    if (cur == &object.getObject ()) {
			return JS_FALSE;
		    }
		    const auto* up = cur->parent.has_value () ? object.getScene ().getObject (*cur->parent) : nullptr;
		    cur = up == nullptr ? nullptr : &up->getObject ();
		}
	    }
	    std::optional<std::string> attachment = std::nullopt;
	    if (argc > 1 && JS_IsString (argv[1])) {
		const char* str = JS_ToCString (ctx, argv[1]);
		attachment = std::string (str == nullptr ? "" : str);
		JS_FreeCString (ctx, str);
	    }
	    object.getScene ().setParentOf (object, parent, attachment);
	    return JS_TRUE;
	}
	case Method_GetVideoTexture: {
	    // an image layer whose texture is a video; undefined for any other layer
	    if (image == nullptr || image->getTexture () == nullptr || image->getTexture ()->getVideoPlayer () == nullptr) {
		return JS_UNDEFINED;
	    }
	    JSValue result = JS_NewObjectClass (ctx, static_cast<int> (adapter.getVideoTextureClassId ()));
	    JS_SetOpaque (result, new OpaqueVideoTexture { .adapter = adapter, .image = *image });
	    return result;
	}
	case Method_AnimLayers: {
	    // [{ key, name, fps, frames, playing, visible, rate, blend, time, ends }]: the puppet's layers now
	    JSValue result = JS_NewArray (ctx);
	    if (image == nullptr) {
		return result;
	    }
	    uint32_t count = 0;
	    for (const auto& state : image->getPuppetLayers ()) {
		const auto* animation = image->puppetAnimationOf (state);
		JSValue entry = JS_NewObject (ctx);
		JS_SetPropertyStr (ctx, entry, "key", JS_NewInt32 (ctx, state.key));
		JS_SetPropertyStr (ctx, entry, "name", JS_NewString (ctx, state.name.c_str ()));
		JS_SetPropertyStr (ctx, entry, "fps", JS_NewFloat64 (ctx, animation ? animation->fps : 0.0));
		JS_SetPropertyStr (ctx, entry, "frames", JS_NewInt32 (ctx, animation ? static_cast<int> (animation->frames) : 0));
		JS_SetPropertyStr (ctx, entry, "playing", JS_NewBool (ctx, state.playing && !state.clock.finished));
		JS_SetPropertyStr (ctx, entry, "visible", JS_NewBool (ctx, state.layer->visible->value->getBool ()));
		JS_SetPropertyStr (ctx, entry, "rate", JS_NewFloat64 (ctx, state.layer->rate->value->getFloat ()));
		JS_SetPropertyStr (ctx, entry, "blend", JS_NewFloat64 (ctx, state.layer->blend->value->getFloat ()));
		JS_SetPropertyStr (ctx, entry, "time", JS_NewFloat64 (ctx, state.clock.time));
		JS_SetPropertyStr (ctx, entry, "ends", JS_NewInt64 (ctx, state.clock.ends));
		JS_SetPropertyUint32 (ctx, result, count++, entry);
	    }
	    return result;
	}
	case Method_AnimLayerSet: {
	    // __animLayerSet (key, field, value): playing, time (seconds), stop, rate, blend, visible, name
	    int key = -1;
	    if (image == nullptr || argc < 2 || JS_ToInt32 (ctx, &key, argv[0]) < 0) {
		return JS_FALSE;
	    }
	    auto* state = image->findPuppetLayer (key);
	    if (state == nullptr) {
		return JS_FALSE;
	    }
	    const std::string field = jsString (ctx, argv[1]);
	    double number = 0.0;
	    if (argc > 2 && !JS_IsString (argv[2])) {
		JS_ToFloat64 (ctx, &number, argv[2]);
	    }
	    if (field == "playing") {
		const bool play = argc > 2 && JS_ToBool (ctx, argv[2]) != 0;
		// a finished single animation plays again from its start
		if (play && state->clock.finished) {
		    state->clock.time = 0.0f;
		    state->clock.finished = false;
		    state->clock.reversed = false;
		}
		state->playing = play;
	    } else if (field == "stop") {
		state->playing = false;
		state->clock.time = 0.0f;
		state->clock.finished = false;
		state->clock.reversed = false;
	    } else if (field == "time") {
		const auto* animation = image->puppetAnimationOf (*state);
		const float len = animation == nullptr ? 0.0f : Puppet::length (*animation);
		state->clock.time = std::clamp (static_cast<float> (number), 0.0f, len);
		state->clock.finished = false;
	    } else if (field == "rate") {
		state->layer->rate->value->update (static_cast<float> (number), DynamicValue::UpdateSource::Script);
	    } else if (field == "blend") {
		state->layer->blend->value->update (static_cast<float> (number), DynamicValue::UpdateSource::Script);
	    } else if (field == "visible") {
		state->layer->visible->value->update (argc > 2 && JS_ToBool (ctx, argv[2]) != 0, DynamicValue::UpdateSource::Script);
	    } else if (field == "name") {
		state->name = argc > 2 ? jsString (ctx, argv[2]) : "";
	    } else {
		return JS_FALSE;
	    }
	    image->puppetPoseChanged ();
	    return JS_TRUE;
	}
	case Method_AnimLayerCreate: {
	    // __animLayerCreate (animation name | id, single): the new layer's key, -1 without such an animation
	    if (image == nullptr || argc < 1) {
		return JS_NewInt32 (ctx, -1);
	    }
	    int id = -1;
	    std::string animationName;
	    if (JS_IsString (argv[0])) {
		animationName = jsString (ctx, argv[0]);
	    } else {
		JS_ToInt32 (ctx, &id, argv[0]);
	    }
	    const bool single = argc > 1 && JS_ToBool (ctx, argv[1]) != 0;
	    return JS_NewInt32 (ctx, image->createPuppetLayer (animationName, id, single));
	}
	case Method_AnimLayerDestroy: {
	    int key = -1;
	    if (image == nullptr || argc < 1 || JS_ToInt32 (ctx, &key, argv[0]) < 0) {
		return JS_FALSE;
	    }
	    return JS_NewBool (ctx, image->destroyPuppetLayer (key));
	}
	case Method_Bones: {
	    // [{ name, parent, local, world }]: the posed bones (matrices as Mat4.m, the mesh's space)
	    JSValue result = JS_NewArray (ctx);
	    if (image == nullptr) {
		return result;
	    }
	    std::vector<glm::mat4> local, world;
	    image->puppetBonePose (local, world);
	    const auto& bones = image->getPuppetBones ();
	    for (uint32_t b = 0; b < bones.size (); b++) {
		JSValue entry = JS_NewObject (ctx);
		JS_SetPropertyStr (ctx, entry, "name", JS_NewString (ctx, bones[b].name.c_str ()));
		JS_SetPropertyStr (ctx, entry, "parent", JS_NewInt32 (ctx, bones[b].parent));
		JS_SetPropertyStr (ctx, entry, "local", matrixArray (ctx, local[b]));
		JS_SetPropertyStr (ctx, entry, "world", matrixArray (ctx, world[b]));
		JS_SetPropertyUint32 (ctx, result, b, entry);
	    }
	    return result;
	}
	case Method_BoneOverride: {
	    // __boneOverride (bone index, Mat4.m | null): a local matrix over the animations', or back to them
	    int bone = -1;
	    if (image == nullptr || argc < 1 || JS_ToInt32 (ctx, &bone, argv[0]) < 0) {
		return JS_FALSE;
	    }
	    glm::mat4 local (1.0f);
	    if (argc > 1 && matrixFrom (ctx, argv[1], local)) {
		image->setBoneOverride (bone, local);
	    } else {
		image->setBoneOverride (bone, std::nullopt);
	    }
	    return JS_TRUE;
	}
	case Method_Shapes: {
	    // [{ name, weight }]: the puppet's blend shapes (MDMP targets, every mesh's in order), weighted now
	    JSValue result = JS_NewArray (ctx);
	    if (image == nullptr) {
		return result;
	    }
	    const auto names = image->blendShapeNames ();
	    for (uint32_t i = 0; i < names.size (); i++) {
		JSValue entry = JS_NewObject (ctx);
		JS_SetPropertyStr (ctx, entry, "name", JS_NewString (ctx, names[i].c_str ()));
		JS_SetPropertyStr (ctx, entry, "weight", JS_NewFloat64 (ctx, image->blendShapeWeight (static_cast<int> (i))));
		JS_SetPropertyUint32 (ctx, result, i, entry);
	    }
	    return result;
	}
	case Method_ShapeOverride: {
	    // __shapeOverride (index, weight | null): a script's weight over the animations', or back to them
	    int index = -1;
	    if (image == nullptr || argc < 1 || JS_ToInt32 (ctx, &index, argv[0]) < 0) {
		return JS_FALSE;
	    }
	    double weight = 0.0;
	    if (argc > 1 && JS_IsNumber (argv[1]) && JS_ToFloat64 (ctx, &weight, argv[1]) == 0) {
		image->setBlendShapeWeight (index, static_cast<float> (weight));
	    } else {
		image->setBlendShapeWeight (index, std::nullopt);
	    }
	    return JS_TRUE;
	}
	case Method_Valid:
	    return JS_NewBool (ctx, object.getScene ().isLayerAlive (&object));
	case Method_Attachments: {
	    // [{ name, m }]: a puppet's attachments, `m` their matrix in the layer's own space (posed now)
	    JSValue result = JS_NewArray (ctx);
	    if (image == nullptr) {
		return result;
	    }
	    uint32_t count = 0;
	    for (const auto& name : image->attachmentNames ()) {
		glm::vec2 offset (0.0f);
		float angle = 0.0f;
		if (!image->attachmentTransform (name, offset, angle)) {
		    continue;
		}
		const glm::mat4 local = glm::rotate (
		    glm::translate (glm::mat4 (1.0f), glm::vec3 (offset, 0.0f)), angle, glm::vec3 (0.0f, 0.0f, 1.0f)
		);
		JSValue entry = JS_NewObject (ctx);
		JS_SetPropertyStr (ctx, entry, "name", JS_NewString (ctx, name.c_str ()));
		JS_SetPropertyStr (ctx, entry, "m", matrixArray (ctx, local));
		JS_SetPropertyUint32 (ctx, result, count++, entry);
	    }
	    return result;
	}
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
	return engine.dynamicToJs (it->second.value, ScriptEngine::scriptScaleOf (name));
    }

    // IImageLayer.size / ITextLayer.size: the layer's size in pixels
    if (strcmp (name, "size") == 0) {
	if (const auto* image = object.asOrNull<CImage> ()) {
	    return engine.newVec (image->getSize ());
	}
    }

    // ITextLayer alignment (live: a script can switch it) and its other fields as the scene set them
    if (const auto* text = object.asOrNull<CText> ()) {
	const auto& data = text->getText ();
	if (strcmp (name, "horizontalalign") == 0) {
	    return JS_NewString (ctx, text->getHorizontalAlign ().c_str ());
	}
	if (strcmp (name, "verticalalign") == 0) {
	    return JS_NewString (ctx, text->getVerticalAlign ().c_str ());
	}
	if (strcmp (name, "font") == 0) {
	    return JS_NewString (ctx, data.font.c_str ());
	}
	if (strcmp (name, "anchor") == 0) {
	    return JS_NewString (ctx, data.anchor.c_str ());
	}
	if (strcmp (name, "padding") == 0) {
	    return JS_NewFloat64 (ctx, data.padding.x);
	}
	if (strcmp (name, "limitrows") == 0) {
	    return JS_NewBool (ctx, data.limitRows);
	}
	if (strcmp (name, "limitwidth") == 0) {
	    return JS_NewBool (ctx, data.limitWidth);
	}
	// LWE draws no text background box (no library scene turns one on)
	if (strcmp (name, "opaquebackground") == 0) {
	    return JS_FALSE;
	}
    } else if (const auto* image = object.asOrNull<CImage> ()) {
	// IImageLayer.alignment (live: a script can switch it)
	if (strcmp (name, "alignment") == 0) {
	    return JS_NewString (ctx, image->getImage ().alignment.c_str ());
	}
    }

    // IParticleSystem.instance (builtins.js IParticleSystemInstance over __particleGet / __particleSet)
    if (object.is<CParticle> () && strcmp (name, "instance") == 0) {
	JSValue of = JS_GetPropertyStr (ctx, engine.getGlobalThis (), "__particleInstanceOf");
	JSValue result = JS_UNDEFINED;
	if (JS_IsFunction (ctx, of)) {
	    JSValueConst args[] = { obj_val };
	    result = JS_Call (ctx, of, JS_UNDEFINED, 1, args);
	}
	JS_FreeValue (ctx, of);
	return result;
    }

    // what the script stored on the layer wins over a method of the same name
    JSValue extra = JS_GetPropertyStr (ctx, container->extras, atomName.str);

    if (!JS_IsUndefined (extra)) {
	return extra;
    }

    if (JSValueConst method = container->adapter.method (name); !JS_IsUndefined (method)) {
	return JS_DupValue (ctx, method);
    }

    // ILayer methods written in JS on top of the natives (builtins.js `__layerMethods`)
    return container->adapter.jsMethod (atomName.str);
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

    // IImageLayer.alignment: the quad is placed from it on the next frame
    if (auto* image = object.asOrNull<CImage> ();
	image != nullptr && !object.is<CText> () && strcmp (name, "alignment") == 0) {
	if (const char* value = JS_ToCString (ctx, val)) {
	    image->setAlignment (value);
	    JS_FreeCString (ctx, value);
	}
	return 1;
    }

    // ITextLayer alignment: the layout follows on the next frame
    if (auto* text = object.asOrNull<CText> ();
	text != nullptr && (strcmp (name, "horizontalalign") == 0 || strcmp (name, "verticalalign") == 0)) {
	if (const char* value = JS_ToCString (ctx, val)) {
	    if (name[0] == 'h') {
		text->setHorizontalAlign (value);
	    } else {
		text->setVerticalAlign (value);
	    }
	    JS_FreeCString (ctx, value);
	}
	return 1;
    }

    const auto& properties = object.getProperties ();

    // a registered property (origin, scale, angles, visible, alpha, color, text, volume, …) takes the value
    if (const auto it = properties.find (name); it != properties.end ()) {
	container->adapter.getEngine ().jsToDynamic (val, it->second.value, ScriptEngine::scriptScaleOf (name));
	return 1;
    }

    // anything else is kept on the layer (one JS object per layer, so it is there on the next getLayer)
    JS_SetPropertyStr (ctx, container->extras, atomName.str, JS_DupValue (ctx, val));
    return 1;
}

static JSValue effect_material_count (JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewInt32 (ctx, 1); }
static JSValue effect_material_noop (JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }

// IEffect.setMaterialProperty (name, value): the effect's shader constant of that name (its constantshadervalues
// key) takes the value, live, in every pass that sets it. A name the scene doesn't set is not added
static JSValue effect_set_material_property (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    auto* effect = static_cast<OpaqueEffect*> (JS_GetAnyOpaque (this_val, &classId));
    if (effect == nullptr || argc < 2) {
	return JS_UNDEFINED;
    }
    const std::string name = jsString (ctx, argv[0]);
    for (const auto& pass : effect->effect.passOverrides) {
	const auto it = pass->constants.find (name);
	if (it != pass->constants.end () && it->second != nullptr && it->second->value != nullptr) {
	    effect->adapter.getEngine ().jsToDynamic (argv[1], *it->second->value);
	}
    }
    return JS_UNDEFINED;
}

// IEffect.getMaterial (index): an IMaterial (an IObject; WE's has getAnimation only)
static JSValue effect_get_material (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    int index = 0;
    if (argc > 0) {
	JS_ToInt32 (ctx, &index, argv[0]);
    }
    if (index != 0) {
	return JS_UNDEFINED;
    }
    JSValue material = JS_NewObject (ctx);
    JS_SetPropertyStr (ctx, material, "getAnimation", JS_NewCFunction (ctx, effect_material_noop, "getAnimation", 1));
    return material;
}

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
    if (strcmp (name.str, "setMaterialProperty") == 0) {
	return JS_NewCFunction (ctx, effect_set_material_property, name.str, 2);
    }
    if (strcmp (name.str, "getMaterial") == 0) {
	return JS_NewCFunction (ctx, effect_get_material, name.str, 1);
    }
    // what a "material function" is isn't documented (no library scene calls one)
    if (strcmp (name.str, "executeMaterialFunction") == 0 || strcmp (name.str, "getAnimation") == 0) {
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

namespace {
enum AnimationMethod { Anim_Play, Anim_Stop, Anim_Pause, Anim_IsPlaying, Anim_GetFrame, Anim_SetFrame };

constexpr std::pair<const char*, AnimationMethod> kAnimationMethods[] = {
    { "play", Anim_Play },	   { "stop", Anim_Stop },	  { "pause", Anim_Pause },
    { "isPlaying", Anim_IsPlaying }, { "getFrame", Anim_GetFrame }, { "setFrame", Anim_SetFrame },
};

JSValue animation_method (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    JSClassID classId = 0;
    auto* opaque = static_cast<OpaqueAnimation*> (JS_GetAnyOpaque (this_val, &classId));

    if (opaque == nullptr || opaque->value.getAnimation () == nullptr) {
	return JS_ThrowTypeError (ctx, "not an animation");
    }

    auto& animation = *opaque->value.getAnimation ();

    switch (static_cast<AnimationMethod> (magic)) {
	case Anim_Play:
	    animation.play ();
	    break;
	case Anim_Stop:
	    animation.stop ();
	    break;
	case Anim_Pause:
	    animation.pause ();
	    break;
	case Anim_IsPlaying:
	    return JS_NewBool (ctx, animation.isPlaying ());
	case Anim_GetFrame:
	    return JS_NewInt32 (ctx, animation.getFrame ());
	case Anim_SetFrame: {
	    double frame = 0.0;
	    if (argc > 0) {
		JS_ToFloat64 (ctx, &frame, argv[0]);
	    }
	    animation.setFrame (static_cast<int> (frame));
	    break;
	}
    }

    // the property shows the change on this frame (the scene applies animations before scripts run)
    animation.apply (opaque->value);
    return JS_UNDEFINED;
}
} // namespace

JSValue animation_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;
    auto* opaque = static_cast<OpaqueAnimation*> (JS_GetAnyOpaque (obj_val, &classId));
    const AtomName name (ctx, atom);

    if (opaque == nullptr || name.str == nullptr || opaque->value.getAnimation () == nullptr) {
	return JS_EXCEPTION;
    }

    const auto& animation = *opaque->value.getAnimation ();

    if (strcmp (name.str, "name") == 0) {
	return JS_NewString (ctx, animation.getName ().c_str ());
    }
    if (strcmp (name.str, "frameCount") == 0) {
	return JS_NewInt32 (ctx, animation.getLength ());
    }
    if (strcmp (name.str, "fps") == 0) {
	return JS_NewFloat64 (ctx, animation.getFps ());
    }
    if (strcmp (name.str, "duration") == 0) {
	return JS_NewFloat64 (ctx, animation.getLength () / animation.getFps ());
    }
    if (strcmp (name.str, "rate") == 0) {
	return JS_NewFloat64 (ctx, animation.getRate ());
    }

    for (const auto& [method, id] : kAnimationMethods) {
	if (strcmp (name.str, method) == 0) {
	    return JS_NewCFunctionMagic (ctx, animation_method, method, 1, JS_CFUNC_generic_magic, id);
	}
    }

    return JS_UNDEFINED;
}

// IAnimation.rate is the one writable member
int animation_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;
    auto* opaque = static_cast<OpaqueAnimation*> (JS_GetAnyOpaque (obj_val, &classId));
    const AtomName name (ctx, atom);

    if (opaque == nullptr || name.str == nullptr || opaque->value.getAnimation () == nullptr) {
	return -1;
    }
    if (strcmp (name.str, "rate") == 0) {
	double rate = 1.0;
	JS_ToFloat64 (ctx, &rate, val);
	opaque->value.getAnimation ()->setRate (static_cast<float> (rate));
    }
    return 1;
}

void animation_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    delete static_cast<OpaqueAnimation*> (JS_GetAnyOpaque (val, &classId));
}

void texture_animation_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    delete static_cast<OpaqueTextureAnimation*> (JS_GetAnyOpaque (val, &classId));
}

namespace {
using WallpaperEngine::VideoPlayback::MPV::GLPlayer;

GLPlayer* playerOf (JSValueConst value) {
    JSClassID classId = 0;
    auto* video = static_cast<OpaqueVideoTexture*> (JS_GetAnyOpaque (value, &classId));
    if (video == nullptr || video->image.getTexture () == nullptr) {
	return nullptr;
    }
    return video->image.getTexture ()->getVideoPlayer ();
}

enum VideoMethod { Video_Play, Video_Pause, Video_Stop, Video_IsPlaying, Video_GetTime, Video_SetTime, Video_Ended };

constexpr std::pair<const char*, VideoMethod> kVideoMethods[] = {
    { "play", Video_Play },	      { "pause", Video_Pause },		  { "stop", Video_Stop },
    { "isPlaying", Video_IsPlaying }, { "getCurrentTime", Video_GetTime }, { "setCurrentTime", Video_SetTime },
    { "addEndedCallback", Video_Ended },
};

JSValue video_method (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    GLPlayer* player = playerOf (this_val);
    if (player == nullptr) {
	return JS_ThrowTypeError (ctx, "not a video texture");
    }

    switch (static_cast<VideoMethod> (magic)) {
	case Video_Play:
	    // a video that ended starts over
	    if (player->reachedEnd ()) {
		player->seek (0.0);
	    }
	    player->clearPaused ();
	    break;
	case Video_Pause:
	    player->setPaused ();
	    break;
	case Video_Stop:
	    // "stops the video and reverts to the beginning"
	    player->setPaused ();
	    player->seek (0.0);
	    break;
	case Video_IsPlaying:
	    return JS_NewBool (ctx, !player->isPaused () && !player->reachedEnd ());
	case Video_GetTime:
	    return JS_NewFloat64 (ctx, player->getTime ());
	case Video_SetTime: {
	    double time = 0.0;
	    if (argc > 0) {
		JS_ToFloat64 (ctx, &time, argv[0]);
	    }
	    player->seek (time);
	    break;
	}
	case Video_Ended: {
	    JSClassID classId = 0;
	    auto* video = static_cast<OpaqueVideoTexture*> (JS_GetAnyOpaque (this_val, &classId));
	    if (argc > 0 && JS_IsFunction (ctx, argv[0])) {
		video->adapter.addVideoEndedCallback (video->image, argv[0]);
	    }
	    break;
	}
    }
    return JS_UNDEFINED;
}
} // namespace

JSValue video_texture_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    const AtomName name (ctx, atom);
    GLPlayer* player = playerOf (obj_val);

    if (player == nullptr || name.str == nullptr) {
	return JS_EXCEPTION;
    }
    if (strcmp (name.str, "duration") == 0) {
	return JS_NewFloat64 (ctx, player->getDuration ());
    }
    if (strcmp (name.str, "rate") == 0) {
	return JS_NewFloat64 (ctx, player->getRate ());
    }
    if (strcmp (name.str, "loop") == 0) {
	return JS_NewBool (ctx, player->getLoop ());
    }
    for (const auto& [method, id] : kVideoMethods) {
	if (strcmp (name.str, method) == 0) {
	    return JS_NewCFunctionMagic (ctx, video_method, method, 1, JS_CFUNC_generic_magic, id);
	}
    }
    return JS_UNDEFINED;
}

int video_texture_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    const AtomName name (ctx, atom);
    GLPlayer* player = playerOf (obj_val);

    if (player == nullptr || name.str == nullptr) {
	return -1;
    }
    if (strcmp (name.str, "rate") == 0) {
	double rate = 1.0;
	JS_ToFloat64 (ctx, &rate, val);
	player->setRate (rate);
    } else if (strcmp (name.str, "loop") == 0) {
	player->setLoop (JS_ToBool (ctx, val) != 0);
    }
    return 1;
}

void video_texture_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    delete static_cast<OpaqueVideoTexture*> (JS_GetAnyOpaque (val, &classId));
}

void ScriptableObjectAdapter::addVideoEndedCallback (CImage& image, JSValueConst callback) {
    this->m_videoEnded.push_back ({ .image = &image, .callback = JS_DupValue (this->m_engine.getContext (), callback), .ended = false });
}

void ScriptableObjectAdapter::pollVideoEnded () {
    JSContext* ctx = this->m_engine.getContext ();

    for (auto& entry : this->m_videoEnded) {
	const auto* player = entry.image->getTexture () == nullptr ? nullptr : entry.image->getTexture ()->getVideoPlayer ();
	const bool ended = player != nullptr && player->reachedEnd ();
	if (ended && !entry.ended) {
	    JSValue result = JS_Call (ctx, entry.callback, JS_UNDEFINED, 0, nullptr);
	    if (JS_IsException (result)) {
		JSValue exception = JS_GetException (ctx);
		const char* message = JS_ToCString (ctx, exception);
		sLog.error ("ScriptEngine [video ended]: ", message ? message : "<exception>");
		JS_FreeCString (ctx, message);
		JS_FreeValue (ctx, exception);
	    }
	    JS_FreeValue (ctx, result);
	}
	entry.ended = ended;
    }
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

    this->m_videoExoticMethods.get_property = video_texture_property_get;
    this->m_videoExoticMethods.set_property = video_texture_property_set;
    this->m_videoDefinition = {
	.class_name = "IVideoTexture",
	.finalizer = video_texture_finalizer,
	.exotic = &m_videoExoticMethods,
    };
    JS_NewClassID (rt, &this->m_videoClassId);
    JS_NewClass (rt, this->m_videoClassId, &this->m_videoDefinition);

    this->m_animationExoticMethods.get_property = animation_property_get;
    this->m_animationExoticMethods.set_property = animation_property_set;
    this->m_animationDefinition = {
	.class_name = "IAnimation",
	.finalizer = animation_finalizer,
	.exotic = &m_animationExoticMethods,
    };
    JS_NewClassID (rt, &this->m_animationClassId);
    JS_NewClass (rt, this->m_animationClassId, &this->m_animationDefinition);

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
    JS_FreeValue (ctx, this->m_jsMethods);
    for (auto& entry : this->m_videoEnded) {
	JS_FreeValue (ctx, entry.callback);
    }
}

JSValue ScriptableObjectAdapter::jsMethod (const char* name) {
    JSContext* ctx = this->m_engine.getContext ();

    if (JS_IsUndefined (this->m_jsMethods)) {
	JSValue methods = JS_GetPropertyStr (ctx, this->m_engine.getGlobalThis (), "__layerMethods");
	if (!JS_IsObject (methods)) {
	    JS_FreeValue (ctx, methods);
	    return JS_UNDEFINED;
	}
	this->m_jsMethods = methods;
    }

    JSValue method = JS_GetPropertyStr (ctx, this->m_jsMethods, name);
    if (!JS_IsFunction (ctx, method)) {
	JS_FreeValue (ctx, method);
	return JS_UNDEFINED;
    }
    return method;
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

JSValue ScriptableObjectAdapter::animation (DynamicValue& value) {
    JSValue result = JS_NewObjectClass (this->m_engine.getContext (), static_cast<int> (this->m_animationClassId));
    JS_SetOpaque (result, new OpaqueAnimation { .value = value });
    return result;
}

JSValue ScriptableObjectAdapter::effect (const ImageEffect& effect) {
    return newEffect (this->m_engine.getContext (), *this, effect);
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
