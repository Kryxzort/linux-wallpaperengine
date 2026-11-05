#include "EngineObject.h"
#include "WallpaperEngine/Health.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <cstring>
#include <ranges>
#include <vector>

using namespace WallpaperEngine::Scripting;

static uint32_t EngineInstanceId = 0;
std::map<uint32_t, EngineObject&> engineInstances;

JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

// a User Shortcut property launches a Windows shortcut: nothing to open here, so "no shortcut was called"
JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_FALSE;
}

JSValue engine_get_frametime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    return JS_NewFloat64 (
	ctx,
	it->second.getScene ().getContext ().getContext ().renderTime
	    - it->second.getScene ().getContext ().getContext ().renderTimeLast
    );
}

JSValue engine_get_runtime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    return JS_NewFloat64 (ctx, it->second.getScene ().getContext ().getContext ().renderTime);
}

JSValue engine_get_daytime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    return JS_NewFloat64 (ctx, it->second.getScene ().getContext ().getContext ().daytime);
}

JSValue engine_get_canvas_size (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    // "the size of the wallpaper project defined in the editor": the scene's orthographic size
    const auto& scene = it->second.getScene ();
    return it->second.getEngine ().newVec (glm::vec2 (scene.getWidth (), scene.getHeight ()));
}

JSValue engine_get_screen_resolution (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    // "Screen resolution in pixels": the screen the host shows the scene on (wp_project_hint_size), the scene's own
    // size until a host gives one
    return it->second.getEngine ().newVec (it->second.getScene ().getScreenSize ());
}

JSValue engine_get_user_properties (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    return it->second.getEngine ().userProperties ();
}

// the stop function setInterval/setTimeout return: called without arguments, the timer's id is its bound data
JSValue engine_stop_interval (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_UNDEFINED;
    }

    int id = 0;

    JS_ToInt32 (ctx, &id, func_data[0]);

    it->second.clearInterval (id);

    return JS_UNDEFINED;
}

// the stop function setInterval/setTimeout return: called without arguments, the timer's id is its bound data
JSValue engine_stop_timeout (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_UNDEFINED;
    }

    int id = 0;

    JS_ToInt32 (ctx, &id, func_data[0]);

    it->second.clearTimeout (id);

    return JS_UNDEFINED;
}

JSValue engine_set_interval (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_EXCEPTION;
    }

    int delay = 0;

    if (argc > 1) {
	JS_ToInt32 (ctx, &delay, argv[1]);
    }

    JSValue function = argv[0];

    if (!JS_IsFunction (ctx, function)) {
	return JS_EXCEPTION;
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    // the callback runs with the script that set it on the running-script stack (builtins.js __scriptBound)
    JSValue bound = it->second.getEngine ().bindToRunningScript (function);
    int id = it->second.reserveNextIntervalId (bound, delay);
    JS_FreeValue (ctx, bound);

    JSValue args[] = { JS_NewInt32 (ctx, id) };

    return JS_NewCFunctionData (ctx, engine_stop_interval, 2, magic, 1, args);
}

JSValue engine_set_timeout (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc < 1) {
	return JS_EXCEPTION;
    }

    int delay = 0;

    if (argc > 1) {
	JS_ToInt32 (ctx, &delay, argv[1]);
    }

    JSValue function = argv[0];

    if (!JS_IsFunction (ctx, function)) {
	return JS_EXCEPTION;
    }

    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    // the callback runs with the script that set it on the running-script stack (builtins.js __scriptBound)
    JSValue bound = it->second.getEngine ().bindToRunningScript (function);
    int id = it->second.reserveNextTimeoutId (bound, delay);
    JS_FreeValue (ctx, bound);

    JSValue args[] = { JS_NewInt32 (ctx, id) };

    return JS_NewCFunctionData (ctx, engine_stop_timeout, 2, magic, 1, args);
}

// engine.registerAudioBuffers (resolution) — SceneScript's AudioBuffers (left, right, average; 16, 32 or 64
// bands, index 0 = bass), the same spectrum as the g_AudioSpectrum uniforms. Only at a module's top level, as in
// WE (scenescript64.dll: "registerAudioBuffers can only be called from global scope.")
JSValue engine_register_audio_buffers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    const auto it = engineInstances.find (magic);

    if (it == engineInstances.end ()) {
	return JS_EXCEPTION;
    }

    // the running module is also set while its update () runs: only its evaluation is the global scope
    if (!it->second.getEngine ().isEvaluatingModule ()) {
	return JS_ThrowSyntaxError (ctx, "registerAudioBuffers can only be called from global scope.");
    }

    int32_t resolution = 0;
    if (argc < 1 || JS_ToInt32 (ctx, &resolution, argv [0]) < 0
	|| (resolution != 16 && resolution != 32 && resolution != 64)) {
	return JS_ThrowRangeError (ctx, "Resolution must be either 16, 32 or 64.");
    }

    return it->second.registerAudioBuffers (resolution);
}

EngineObject::EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_instanceId (++EngineInstanceId), m_classId (0) {
    // the native functions below find this object by their magic (the instance id)
    engineInstances.emplace (this->m_instanceId, *this);
    this->m_definition = { .class_name = "IEngine" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "frametime"),
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_get_frametime, "get", 0, JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "runtime"),
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_get_runtime, "get", 0, JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "timeOfDay"),
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_get_daytime, "get", 0, JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    const std::pair<const char*, JSCFunctionMagic*> live[] = {
	{ "canvasSize", engine_get_canvas_size },
	{ "screenResolution", engine_get_screen_resolution },
	{ "userProperties", engine_get_user_properties },
    };
    for (const auto& [name, getter] : live) {
	JSAtom atom = JS_NewAtom (this->m_engine.getContext (), name);
	JS_DefinePropertyGetSet (
	    this->m_engine.getContext (), this->m_instance, atom,
	    JS_NewCFunctionMagic (this->m_engine.getContext (), getter, "get", 0, JS_CFUNC_generic_magic, this->m_instanceId),
	    JS_UNDEFINED, JS_PROP_ENUMERABLE
	);
	JS_FreeAtom (this->m_engine.getContext (), atom);
    }
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_16",
	JS_NewInt32 (this->m_engine.getContext (), 16), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_32",
	JS_NewInt32 (this->m_engine.getContext (), 32), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_64",
	JS_NewInt32 (this->m_engine.getContext (), 64), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAudioBuffers",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_register_audio_buffers, "registerAudioBuffers", 1,
	    JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setInterval",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_interval, "setInterval", 2, JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setTimeout",
	JS_NewCFunctionMagic (
	    this->m_engine.getContext (), engine_set_timeout, "setTimeout", 2, JS_CFUNC_generic_magic, this->m_instanceId
	),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "openUserShortcut",
	JS_NewCFunction (this->m_engine.getContext (), engine_open_user_shortcut, "openUserShortcut", 0),
	JS_PROP_ENUMERABLE
    );
    // TODO: ADD THE REST OF THE DEFINITION!
}

EngineObject::~EngineObject () {
    for (const auto& buffers : this->m_audioBuffers) {
	for (const auto& array : buffers.arrays) {
	    JS_FreeValue (this->m_engine.getContext (), array);
	}
    }
    this->m_audioBuffers.clear ();
    // clear all the timeouts and intervals
    for (const auto& [id, timeout] : this->m_timeouts) {
	JS_FreeValue (this->m_engine.getContext (), timeout.callback);
    }
    for (const auto& [id, interval] : this->m_intervals) {
	JS_FreeValue (this->m_engine.getContext (), interval.callback);
    }

    engineInstances.erase (this->m_instanceId);
    this->m_intervals.clear ();
    this->m_timeouts.clear ();

    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
}

double EngineObject::now () const { return this->m_scene.getContext ().getContext ().renderTime; }

uint32_t EngineObject::reserveNextTimeoutId (JSValue function, uint64_t duration) {
    const auto id = ++this->m_nextTimeoutId;
    const double seconds = static_cast<double> (duration) / 1000.0;

    // the callback outlives the call that registered it
    this->m_timeouts[id] = Timeout {
	.callback = JS_DupValue (this->m_engine.getContext (), function),
	.duration = seconds,
	.next = this->now () + seconds,
    };

    return id;
}

uint32_t EngineObject::reserveNextIntervalId (JSValue function, uint64_t duration) {
    const auto id = ++this->m_nextIntervalId;
    const double seconds = static_cast<double> (duration) / 1000.0;

    // the callback outlives the call that registered it
    this->m_intervals[id] = Timeout {
	.callback = JS_DupValue (this->m_engine.getContext (), function),
	.duration = seconds,
	.next = this->now () + seconds,
    };

    return id;
}

void EngineObject::clearInterval (uint32_t id) {
    const auto it = this->m_intervals.find (id);

    if (it == this->m_intervals.end ()) {
	return;
    }

    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);

    this->m_intervals.erase (id);
}

void EngineObject::clearTimeout (uint32_t id) {
    const auto it = this->m_timeouts.find (id);

    if (it == this->m_timeouts.end ()) {
	return;
    }

    JS_FreeValue (this->getEngine ().getContext (), it->second.callback);

    this->m_timeouts.erase (id);
}

void EngineObject::call (JSValue callback) {
    JSContext* ctx = this->m_engine.getContext ();
    // a callback may clear its own timer (freeing its stored reference): hold one for the call
    JSValue function = JS_DupValue (ctx, callback);
    JSValue result = JS_Call (ctx, function, JS_UNDEFINED, 0, nullptr);

    if (JS_IsException (result)) {
	JSValue exception = JS_GetException (ctx);
	const char* message = JS_ToCString (ctx, exception);
	sLog.error ("ScriptEngine [timer]: ", message ? message : "<exception>");
	WPENGINE_HEALTH ("script_error", { { "object", -1 }, { "property", "timer" }, { "phase", "timer" }, { "message", WallpaperEngine::Health::brief (message ? message : "<exception>", 300) } });
	JS_FreeCString (ctx, message);
	JS_FreeValue (ctx, exception);
    }

    JS_FreeValue (ctx, result);
    JS_FreeValue (ctx, function);
}

JSValue EngineObject::registerAudioBuffers (int resolution) {
    JSContext* ctx = this->m_engine.getContext ();
    JSValue object = JS_NewObject (ctx);
    AudioBuffers buffers { .resolution = resolution, .arrays = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED } };
    const char* names[] = { "left", "right", "average" };

    for (int c = 0; c < 3; c++) {
	JSValue length = JS_NewInt32 (ctx, resolution);
	JSValue array = JS_NewTypedArray (ctx, 1, &length, JS_TYPED_ARRAY_FLOAT32);
	if (JS_IsException (array)) {
	    JS_FreeValue (ctx, object);
	    for (const auto& a : buffers.arrays) {
		JS_FreeValue (ctx, a);
	    }
	    return array;
	}
	buffers.arrays [c] = JS_DupValue (ctx, array);
	JS_DefinePropertyValueStr (ctx, object, names [c], array, JS_PROP_ENUMERABLE);
    }

    this->m_audioBuffers.push_back (buffers);
    this->refreshAudioBuffers ();
    return object;
}

void EngineObject::refreshAudioBuffers () {
    if (this->m_audioBuffers.empty ()) {
	return;
    }

    JSContext* ctx = this->m_engine.getContext ();
    const auto& recorder = this->m_scene.getAudioContext ().getRecorder ();

    for (const auto& buffers : this->m_audioBuffers) {
	for (int c = 0; c < 3; c++) {
	    size_t offset = 0, length = 0, element = 0;
	    JSValue buffer = JS_GetTypedArrayBuffer (ctx, buffers.arrays [c], &offset, &length, &element);
	    if (JS_IsException (buffer)) {
		JS_FreeValue (ctx, JS_GetException (ctx));
		continue;
	    }
	    size_t size = 0;
	    uint8_t* data = JS_GetArrayBuffer (ctx, &size, buffer);
	    const float* bands = recorder.bands (buffers.resolution, c);
	    const size_t bytes = sizeof (float) * buffers.resolution;
	    if (data != nullptr && bands != nullptr && offset + bytes <= size && length >= bytes) {
		std::memcpy (data + offset, bands, bytes);
	    }
	    JS_FreeValue (ctx, buffer);
	}
    }
}

void EngineObject::tick () {
    // the frame's spectrum first: scripts read it in their update () this frame
    this->refreshAudioBuffers ();

    const double now = this->now ();
    // callbacks may add or clear timers: walk a snapshot of the ids, re-finding each one
    std::vector<uint32_t> ids;

    // check any interval and run them if needed
    for (const auto& id : this->m_intervals | std::views::keys) {
	ids.push_back (id);
    }

    for (const auto id : ids) {
	const auto it = this->m_intervals.find (id);

	if (it == this->m_intervals.end ()) {
	    continue;
	}

	auto& timeout = it->second;

	// the host's clock went back (a seek): restart the interval from here instead of stalling
	if (timeout.next - now > timeout.duration) {
	    timeout.next = now + timeout.duration;
	}

	if (timeout.next > now) {
	    continue;
	}

	timeout.next = now + timeout.duration;
	this->call (timeout.callback);
    }

    ids.clear ();

    for (const auto& id : this->m_timeouts | std::views::keys) {
	ids.push_back (id);
    }

    // check any timeout and run them if needed
    for (const auto id : ids) {
	const auto it = this->m_timeouts.find (id);

	if (it == this->m_timeouts.end ()) {
	    continue;
	}

	if (it->second.next - now > it->second.duration) {
	    it->second.next = now + it->second.duration;
	}

	if (it->second.next > now) {
	    continue;
	}

	// a timeout fires once: off the list before its callback runs
	JSValue callback = it->second.callback;
	this->m_timeouts.erase (it);
	this->call (callback);
	JS_FreeValue (this->m_engine.getContext (), callback);
    }
}
