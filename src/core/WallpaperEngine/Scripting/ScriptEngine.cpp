#include "ScriptEngine.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "ScriptPropertiesObject.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Builtins.generated.h"
#include "WallpaperEngine/Utils/ScopeGuard.h"
#include "quickjs.h"
#include "stb_image.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <future>
#include <optional>
#include <poll.h>
#include <ranges>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace WallpaperEngine::Render::Objects {
class CSound;
}
using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Data::Model;

extern char** environ;

namespace {
JSValue scriptengine_layer_prop (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
}
extern float g_Time;
extern float g_TimeLast;

void scriptengine_dump (JSContext* ctx, JSValueConst obj) {
    JSPropertyEnum* props;
    uint32_t len;

    if (JS_GetOwnPropertyNames (ctx, &props, &len, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
	return;
    }

    for (uint32_t i = 0; i < len; ++i) {
	const char* name = JS_AtomToCString (ctx, props[i].atom);

	JSValue val = JS_GetProperty (ctx, obj, props[i].atom);

	const char* value_str = JS_ToCString (ctx, val);

	printf ("%s = %s\n", name, value_str ? value_str : "<non-string>");

	JS_FreeCString (ctx, value_str);
	JS_FreeValue (ctx, val);
	JS_FreeCString (ctx, name);
    }

    js_free (ctx, props);
}

// `import … from 'WEMath'`: Wallpaper Engine's own script modules, `scripts/jsmodules/<lowercase name>.js` of its
// assets (wemath.js, wecolor.js, wevector.js), loaded from the user's install at run time (never shipped here)
JSModuleDef* scriptengine_module_loader (JSContext* ctx, const char* module, void* opaque) {
    const auto* scriptEngine = static_cast<ScriptEngine*> (opaque);
    std::string name = module;

    if (name.empty () || name.find_first_of ("/\\.") != std::string::npos) {
	JS_ThrowReferenceError (ctx, "could not load module '%s'", module);
	return nullptr;
    }

    std::ranges::transform (name, name.begin (), [] (unsigned char c) { return std::tolower (c); });
    const std::string path = "scripts/jsmodules/" + name + ".js";
    const std::string source = scriptEngine->readScriptAsset (path);

    if (source.empty ()) {
	JS_ThrowReferenceError (ctx, "could not load module '%s' (no %s in the assets)", module, path.c_str ());
	return nullptr;
    }

    JSValue function = JS_Eval (
	ctx, source.c_str (), source.size (), module, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
    );

    if (JS_IsException (function)) {
	return nullptr;
    }

    // the context keeps the module; the compiled function value is only a handle to it
    auto* definition = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (function));
    JS_FreeValue (ctx, function);
    return definition;
}

JSValue ScriptEngine::dynamicToJs (DynamicValue& value) const {
    switch (value.getType ()) {
	case DynamicValue::Null:
	    return JS_NULL;
	case DynamicValue::String:
	    return JS_NewString (this->m_context, value.getString ().c_str ());
	case DynamicValue::Float:
	    return JS_NewFloat64 (this->m_context, value.getFloat ());
	case DynamicValue::Int:
	    return JS_NewInt32 (this->m_context, value.getInt ());
	case DynamicValue::Boolean:
	    return JS_NewBool (this->m_context, value.getBool ());
	case DynamicValue::Vec2:
	    return this->newVec (value.getVec2 ());
	case DynamicValue::Vec3:
	    return this->newVec (value.getVec3 ());
	case DynamicValue::Vec4:
	    return this->newVec (value.getVec4 ());
	default:
	    return JS_UNDEFINED;
    }
}

static bool jsToDynamicValue (JSContext* ctx, JSValueConst val, DynamicValue& target);

bool ScriptEngine::jsToDynamic (JSValueConst value, DynamicValue& target) const {
    return jsToDynamicValue (this->m_context, value, target);
}

JSValue ScriptEngine::newVec (int components, const float* values) const {
    JSValue ctor = this->m_vecCtors[components - 2];

    if (!JS_IsFunction (this->m_context, ctor)) {
	return JS_UNDEFINED;
    }

    JSValue args[4];

    for (int i = 0; i < components; i++) {
	args[i] = JS_NewFloat64 (this->m_context, values[i]);
    }

    return JS_CallConstructor (this->m_context, ctor, components, args);
}

JSValue ScriptEngine::newVec (const glm::vec2& value) const { return this->newVec (2, &value.x); }
JSValue ScriptEngine::newVec (const glm::vec3& value) const { return this->newVec (3, &value.x); }
JSValue ScriptEngine::newVec (const glm::vec4& value) const { return this->newVec (4, &value.x); }

std::string ScriptEngine::readScriptAsset (const std::string& path) const {
    try {
	return this->m_scene.getAssetLocator ().readString (path);
    } catch (const std::exception&) {
	return {};
    }
}

/**
 * Writes a script's return value into the property. `undefined`/`null` (no `return`) leave it as it is: "you can
 * choose to not return any value, then the property will not be modified" (WE's SceneScript docs, `update`).
 *
 * @return false when nothing was written
 */
static bool jsToDynamicValue (JSContext* ctx, JSValueConst val, DynamicValue& target) {
    const int tag = JS_VALUE_GET_TAG (val);

    if (tag == JS_TAG_INT) {
	target.update (JS_VALUE_GET_INT (val), DynamicValue::UpdateSource::Script);
	return true;
    }

    if (tag == JS_TAG_BOOL) {
	target.update (static_cast<bool> (JS_VALUE_GET_BOOL (val)), DynamicValue::UpdateSource::Script);
	return true;
    }

    if (JS_TAG_IS_FLOAT64 (tag)) {
	target.update (static_cast<float> (JS_VALUE_GET_FLOAT64 (val)), DynamicValue::UpdateSource::Script);
	return true;
    }

    if (tag == JS_TAG_STRING) {
	const char* str = JS_ToCString (ctx, val);
	target.update (std::string (str == nullptr ? "" : str), DynamicValue::UpdateSource::Script);
	JS_FreeCString (ctx, str);
	return true;
    }

    if (tag != JS_TAG_OBJECT) {
	return false;
    }

    // a Vec2/Vec3/Vec4 (or any object with numeric x, y[, z[, w]]): as many components as it has
    JSValue components[4] = {
	JS_GetPropertyStr (ctx, val, "x"),
	JS_GetPropertyStr (ctx, val, "y"),
	JS_GetPropertyStr (ctx, val, "z"),
	JS_GetPropertyStr (ctx, val, "w"),
    };
    double v[4] = { 0.0, 0.0, 0.0, 0.0 };
    int count = 0;

    while (count < 4 && JS_IsNumber (components[count])) {
	JS_ToFloat64 (ctx, &v[count], components[count]);
	count++;
    }

    for (auto& component : components) {
	JS_FreeValue (ctx, component);
    }

    switch (count) {
	case 2:
	    target.update (glm::vec2 (v[0], v[1]), DynamicValue::UpdateSource::Script);
	    return true;
	case 3:
	    target.update (glm::vec3 (v[0], v[1], v[2]), DynamicValue::UpdateSource::Script);
	    return true;
	case 4:
	    target.update (glm::vec4 (v[0], v[1], v[2], v[3]), DynamicValue::UpdateSource::Script);
	    return true;
	default:
	    return false;
    }
}

ScriptEngine::ScriptEngine (Wallpapers::CScene& scene) : m_scene (scene) {
    this->m_runtime = JS_NewRuntime ();

    if (!this->m_runtime) {
	sLog.exception ("ScriptEngine: Failed to create JS runtime");
    }

    // debug leaks on termination
    JS_SetDumpFlags (this->m_runtime, JS_DUMP_LEAKS);

    this->m_context = JS_NewContext (this->m_runtime);

    if (!this->m_context) {
	JS_FreeRuntime (this->m_runtime);
	sLog.exception ("ScriptEngine: Failed to create JS context");
    }

    this->m_globalThis = JS_GetGlobalObject (this->m_context);
    JS_SetContextOpaque (this->m_context, this);

    this->m_adapters = {
	.object
	= std::unique_ptr<Adapters::ScriptableObjectAdapter> (new Adapters::ScriptableObjectAdapter (*this, "ILayer")),
    };

    JS_SetModuleLoaderFunc (this->m_runtime, nullptr, scriptengine_module_loader, this);
    // Wallpaper Engine's script classes (Vec2/3/4, Mat3/4, createScriptProperties, …) before anything else
    this->installPrelude ();

    this->m_engineObject = std::make_unique<EngineObject> (*this, scene);
    this->m_inputObject = std::make_unique<InputObject> (*this, scene);
    this->m_sceneObject = std::make_unique<SceneObject> (*this, scene);
    this->m_consoleObject = std::make_unique<ConsoleObject> (*this, scene);
    this->m_scriptPropertiesObject = std::make_unique<ScriptPropertiesObject> (*this, scene);

    // add engine to the global
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "engine", this->m_engineObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "input", this->m_inputObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "thisScene", this->m_sceneObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "console", this->m_consoleObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "shared", JS_NewObject (this->m_context), JS_PROP_ENUMERABLE
    );
    JS_SetPropertyStr (
	this->m_context, this->m_globalThis, "__layerProp",
	JS_NewCFunction (this->m_context, scriptengine_layer_prop, "__layerProp", 2)
    );
    // the rest of the API in JS (engine.is*(), localStorage, …), on top of the globals above
    this->installBuiltins ();

    // a user property the user changes reaches applyUserProperties on the next tick
    for (const auto& [name, property] : scene.getScene ().project.properties) {
	this->m_propertyListeners.push_back (
	    property->listen ([this, key = name] (const DynamicValue&, DynamicValue::UpdateSource source) {
		if (source == DynamicValue::UpdateSource::User) {
		    this->m_changedProperties.insert (key);
		}
	    })
	);
    }
}

ScriptEngine::~ScriptEngine () {
    for (auto& unlisten : this->m_propertyListeners) {
	unlisten ();
    }

    for (const auto& module : this->m_scriptModules) {
	JS_FreeValue (this->m_context, module->ns);
    }

    for (auto& ctor : this->m_vecCtors) {
	JS_FreeValue (this->m_context, ctor);
    }

    for (const auto& events : this->m_queuedEvents | std::views::values) {
	JS_FreeValue (this->m_context, events);
    }

    JS_FreeValue (this->m_context, this->m_globalThis);

    this->m_adapters.object.reset ();

    this->m_consoleObject.reset ();
    this->m_engineObject.reset ();
    this->m_inputObject.reset ();
    this->m_sceneObject.reset ();
    this->m_scriptPropertiesObject.reset ();
    this->m_scriptModules.clear ();

    if (this->m_context) {
	JS_FreeContext (this->m_context);
    }
    if (this->m_runtime) {
	JS_FreeRuntime (this->m_runtime);
    }
}

/// Helper to check for and log JS exceptions
static void logJSException (JSContext* ctx, const char* context) {
    JSValue exc = JS_GetException (ctx);
    if (!JS_IsNull (exc) && !JS_IsUndefined (exc)) {
	const char* str = JS_ToCString (ctx, exc);
	if (str) {
	    sLog.error ("ScriptEngine [", context, "]: ", str);
	    JS_FreeCString (ctx, str);
	}
    }
    JS_FreeValue (ctx, exc);
}

void ScriptEngine::installBuiltins () {
    if (this->m_builtinsInstalled || !this->m_context) {
	return;
    }

    JSValue result = JS_Eval (
	this->m_context, SCENE_SCRIPT_BUILTINS, strlen (SCENE_SCRIPT_BUILTINS), "<scene-script-builtins>",
	JS_EVAL_TYPE_GLOBAL
    );
    if (JS_IsException (result)) {
	logJSException (this->m_context, "installBuiltins");
    }
    JS_FreeValue (this->m_context, result);
    this->m_builtinsInstalled = true;
}

// Used only when the assets have no scripts/jsclasses/baseclasses.js: enough of Vec2/Vec3/Vec4 to pass values in and
// out of scripts (constructors as SceneScript documents them), none of their methods
static constexpr const char* FALLBACK_VECTOR_CLASSES = R"JS(
globalThis.Vec2 = class Vec2 {
  constructor (x, y) {
    if (typeof x === 'string') { [x, y] = x.split (' ').map (parseFloat); }
    else if (typeof x === 'object' && x !== null) { [x, y] = [x.x, x.y]; }
    this.x = x ?? 0; this.y = y ?? this.x;
  }
};
globalThis.Vec3 = class Vec3 {
  constructor (x, y, z) {
    if (typeof x === 'string') { [x, y, z] = x.split (' ').map (parseFloat); }
    else if (typeof x === 'object' && x !== null) { [x, y, z] = [x.x, x.y, x.z ?? 0]; }
    this.x = x ?? 0; this.y = y ?? this.x; this.z = z ?? (y === undefined ? this.x : 0);
  }
};
globalThis.Vec4 = class Vec4 {
  constructor (x, y, z, w) {
    if (typeof x === 'string') { [x, y, z, w] = x.split (' ').map (parseFloat); }
    else if (typeof x === 'object' && x !== null) { [x, y, z, w] = [x.x, x.y, x.z ?? 0, x.w ?? 0]; }
    this.x = x ?? 0; this.y = y ?? this.x; this.z = z ?? (y === undefined ? this.x : 0); this.w = w ?? (y === undefined ? this.x : 0);
  }
};
)JS";

void ScriptEngine::installPrelude () {
    JSContext* ctx = this->m_context;
    // what Wallpaper Engine evaluates before any script: its own JS classes, read from the user's WE assets
    const std::string path = "scripts/jsclasses/baseclasses.js";
    const std::string prelude = this->readScriptAsset (path);
    bool ok = false;

    if (prelude.empty ()) {
	sLog.error ("ScriptEngine: no ", path, " in the assets; scripts get bare Vec2/Vec3/Vec4 classes");
    } else {
	JSValue result = JS_Eval (ctx, prelude.c_str (), prelude.size (), path.c_str (), JS_EVAL_TYPE_GLOBAL);
	ok = !JS_IsException (result);

	if (!ok) {
	    logJSException (ctx, path.c_str ());
	}

	JS_FreeValue (ctx, result);
    }

    if (!ok) {
	JSValue result = JS_Eval (
	    ctx, FALLBACK_VECTOR_CLASSES, strlen (FALLBACK_VECTOR_CLASSES), "<vector-classes>", JS_EVAL_TYPE_GLOBAL
	);

	if (JS_IsException (result)) {
	    logJSException (ctx, "vector-classes");
	}

	JS_FreeValue (ctx, result);
    }

    // the class bindings are global lexical declarations (not properties of globalThis): look them up by name
    const char* names[] = { "Vec2", "Vec3", "Vec4" };

    for (int i = 0; i < 3; i++) {
	this->m_vecCtors[i] = JS_Eval (ctx, names[i], strlen (names[i]), "<vector-class>", JS_EVAL_TYPE_GLOBAL);

	if (JS_IsException (this->m_vecCtors[i])) {
	    logJSException (ctx, names[i]);
	    this->m_vecCtors[i] = JS_UNDEFINED;
	}
    }
}

// ---------------------------------------------------------------------------
// Layer-script API (Phase 2)
// ---------------------------------------------------------------------------

void ScriptEngine::ensureLayerRegistry () {
    if (this->m_layerRegistryReady || !this->m_context) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (ctx, globalObj, "__textLayers", JS_NewObject (ctx));
    JS_FreeValue (ctx, globalObj);
    this->m_layerRegistryReady = true;
}

namespace {
/** what a text-layer script may export besides init/update/destroy: the events dispatchEvent delivers */
constexpr const char* kLayerEvents[] = {
    "applyUserProperties",   "resizeScreen",	      "cursorEnter",	      "cursorLeave",
    "cursorMove",	     "cursorDown",	      "cursorUp",	      "cursorClick",
    "mediaPlaybackChanged",  "mediaPropertiesChanged", "mediaThumbnailChanged", "mediaTimelineChanged",
    "mediaStatusChanged",
};

// __layerProp (id, name): a text-layer script property's live value (bound to a user property: follows it)
JSValue scriptengine_layer_prop (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (ctx));
    int id = 0;

    if (engine == nullptr || argc < 2 || JS_ToInt32 (ctx, &id, argv[0]) < 0) {
	return JS_UNDEFINED;
    }

    const char* name = JS_ToCString (ctx, argv[1]);

    if (name == nullptr) {
	return JS_UNDEFINED;
    }

    JSValue result = engine->layerScriptProperty (id, name);
    JS_FreeCString (ctx, name);
    return result;
}
} // namespace

ScriptLayerHandle ScriptEngine::createLayerScript (
    const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
    const std::string& initialText, ScriptableObject& layer
) {
    if (!this->m_context) {
	sLog.error ("ScriptEngine: No JS context available");
	return kInvalidLayerHandle;
    }

    this->ensureLayerRegistry ();

    JSContext* ctx = this->m_context;

    // the layer the script is on: thisLayer is the real layer (its text, and every ILayer member)
    JS_SetPropertyStr (ctx, this->m_globalThis, "__layerSeedLayer", this->m_adapters.object->instantiate (layer));

    const ScriptLayerHandle id = this->m_nextLayerId++;
    this->m_layerObjects[id] = &layer;
    this->m_layerProps[id] = &initialScriptProps;

    // WE scripts come as ES6 modules; this path drives them as plain script evaluation
    std::string body = scriptSource;
    size_t pos;
    while ((pos = body.find ("'use strict';")) != std::string::npos) {
	body.erase (pos, 13);
    }
    while ((pos = body.find ("\"use strict\";")) != std::string::npos) {
	body.erase (pos, 13);
    }
    while ((pos = body.find ("export ")) != std::string::npos) {
	body.erase (pos, 7);
    }

    // The IIFE gives every layer its own closure for top-level vars and functions, so two layers that both define
    // `function update()` don't clobber each other. Lifecycle hooks and events are captured into
    // globalThis.__textLayers[id] so the engine can reach them later. `typeof init === 'function'` is safe even when
    // `init` was never declared — bare-identifier `typeof` never throws.
    std::ostringstream wrapper;
    wrapper
	<< "(function() {\n"
	<< "  var __id = " << id << ";\n"
	// the script properties: the scene's stored values, live (a user property bound to one reaches the script on
	// its next read); a key the scene doesn't store reads its builder default
	<< "  var __defaults = {};\n"
	<< "  var __props = new Proxy(__defaults, {\n"
	<< "    get: function(t, k) {\n"
	<< "      if (typeof k !== 'string') { return t[k]; }\n"
	<< "      var v = globalThis.__layerProp(__id, k);\n"
	<< "      return v === undefined ? t[k] : v;\n"
	<< "    },\n"
	<< "    has: function(t, k) { return (k in t) || globalThis.__layerProp(__id, k) !== undefined; }\n"
	<< "  });\n"
	<< "  var thisLayer = globalThis.__layerSeedLayer, thisObject = thisLayer;\n"
	// `thisScene` and `engine` are the real objects (getLayer, setInterval, …) seen through a layer of the text
	// layer's own clock: time/currentTime/dt/fps and `frametime` (the per-frame delta in seconds, what
	// InsertFPS reads) come from the text object's tick
	<< "  function __withClock(real, own) {\n"
	<< "    if (real === undefined || real === null) { return own; }\n"
	<< "    return new Proxy(own, { get: function(t, k) {\n"
	<< "      if (k in t) { return t[k]; }\n"
	<< "      var v = real[k];\n"
	<< "      return typeof v === 'function' ? v.bind(real) : v;\n"
	<< "    } });\n"
	<< "  }\n"
	<< "  var thisScene = __withClock(globalThis.thisScene, {\n"
	<< "    get time()        { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "    get currentTime() { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "    get dt()          { var c = globalThis.__sceneCtx; return c ? c.dt   : 0; },\n"
	<< "    get fps()         { var c = globalThis.__sceneCtx; return c ? c.fps  : 60; },\n"
	<< "  });\n"
	<< "  var engine = __withClock(globalThis.engine, {\n"
	<< "    get frametime() { var c = globalThis.__sceneCtx; return c ? c.dt : 0; },\n"
	<< "    get time()      { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "  });\n"
	// WE's builder semantics (baseclasses.js): every key gets its default, a combo its first option's value
	<< "  function createScriptProperties() {\n"
	<< "    function add(o) {\n"
	<< "      if (o && o.name !== undefined) {\n"
	<< "        var d = o.value;\n"
	<< "        if (d === undefined && o.options && o.options.length) { d = o.options[0].value; }\n"
	<< "        __defaults[o.name] = d;\n"
	<< "      }\n"
	<< "      return builder;\n"
	<< "    }\n"
	<< "    var builder = {\n"
	<< "      addSlider: add, addCheckbox: add, addCombo: add, addColor: add, addText: add, addTextInput: add,\n"
	<< "      addFile: add, addDirectory: add,\n"
	<< "      finish: function(){ return __props; }\n"
	<< "    };\n"
	<< "    return builder;\n"
	<< "  }\n"
	<< body
	<< "\n"
	// `update` either assigns thisLayer.text or returns the text (both WE conventions); init (value) the same
	<< "  function __adopt(r) { if (typeof r === 'string') thisLayer.text = r; }\n"
	<< "  globalThis.__textLayers[__id] = {\n"
	<< "    thisLayer: thisLayer,\n"
	<< "    thisScene: thisScene,\n"
	<< "    _init:    (typeof init    === 'function') ? function() { __adopt(init(thisLayer.text)); } : null,\n"
	<< "    _destroy: (typeof destroy === 'function') ? destroy : null,\n"
	<< "    _tick:    (typeof update  === 'function') ? function() { __adopt(update(thisLayer.text)); } : null,\n"
	<< "    _events: {\n";
    for (const char* event : kLayerEvents) {
	wrapper << "      " << event << ": (typeof " << event << " === 'function') ? " << event << " : null,\n";
    }
    wrapper << "    },\n"
	    << "    _scriptProperties: (typeof scriptProperties !== 'undefined') ? scriptProperties : __props\n"
	    << "  };\n"
	    << "})();\n";

    const std::string evalScript = wrapper.str ();
    JSValue result = JS_Eval (ctx, evalScript.c_str (), evalScript.size (), "<layer-script>", JS_EVAL_TYPE_GLOBAL);

    JS_SetPropertyStr (ctx, this->m_globalThis, "__layerSeedLayer", JS_UNDEFINED);

    if (JS_IsException (result)) {
	logJSException (ctx, "createLayerScript");
	JS_FreeValue (ctx, result);
	this->m_layerObjects.erase (id);
	this->m_layerProps.erase (id);
	return kInvalidLayerHandle;
    }
    JS_FreeValue (ctx, result);

    // init and the first applyUserProperties run on the next tick, with the property scripts'
    this->m_layerInitialized[id] = false;
    this->m_pendingModules = true;
    return id;
}

JSValue ScriptEngine::layerScriptProperty (ScriptLayerHandle handle, const std::string& name) {
    const auto it = this->m_layerProps.find (handle);

    if (it == this->m_layerProps.end () || it->second == nullptr) {
	return JS_UNDEFINED;
    }

    const auto prop = it->second->find (name);

    if (prop == it->second->end () || prop->second == nullptr || prop->second->value == nullptr) {
	return JS_UNDEFINED;
    }

    return this->dynamicToJs (*prop->second->value);
}

void ScriptEngine::callLayerHook (ScriptLayerHandle handle, const char* name, int argc, JSValueConst argv[], bool event) {
    JSContext* ctx = this->m_context;
    JSValue layers = JS_GetPropertyStr (ctx, this->m_globalThis, "__textLayers");
    JSValue layerObj = JS_IsObject (layers) ? JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle))
					    : JS_UNDEFINED;
    JS_FreeValue (ctx, layers);

    if (!JS_IsObject (layerObj)) {
	JS_FreeValue (ctx, layerObj);
	return;
    }

    JSValue holder = event ? JS_GetPropertyStr (ctx, layerObj, "_events") : JS_DupValue (ctx, layerObj);
    JSValue fn = JS_IsObject (holder) ? JS_GetPropertyStr (ctx, holder, name) : JS_UNDEFINED;

    if (JS_IsFunction (ctx, fn)) {
	JSValue ret = JS_Call (ctx, fn, layerObj, argc, argv);
	if (JS_IsException (ret)) {
	    const std::string tag = std::string ("layer.") + name;
	    logJSException (ctx, tag.c_str ());
	}
	JS_FreeValue (ctx, ret);
    }

    JS_FreeValue (ctx, fn);
    JS_FreeValue (ctx, holder);
    JS_FreeValue (ctx, layerObj);
}

void ScriptEngine::tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return;
    }
    JSContext* ctx = this->m_context;

    JSValue sceneCtx = JS_NewObject (ctx);
    JS_SetPropertyStr (ctx, sceneCtx, "time", JS_NewFloat64 (ctx, time));
    JS_SetPropertyStr (ctx, sceneCtx, "dt", JS_NewFloat64 (ctx, deltaTime));
    JS_SetPropertyStr (ctx, sceneCtx, "fps", JS_NewFloat64 (ctx, fps));
    JS_SetPropertyStr (ctx, this->m_globalThis, "__sceneCtx", sceneCtx);

    // init runs first, from tick (); a layer created after the last tick is started here
    if (const auto it = this->m_layerInitialized.find (handle); it != this->m_layerInitialized.end () && !it->second) {
	this->startPendingScripts ();
    }

    this->callLayerHook (handle, "_tick", 0, nullptr, false);
}

std::string ScriptEngine::layerText (ScriptLayerHandle handle) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return {};
    }

    // thisLayer is the real layer: its text is the layer's `text` value
    if (const auto it = this->m_layerObjects.find (handle); it != this->m_layerObjects.end ()) {
	const auto& properties = it->second->getProperties ();
	if (const auto text = properties.find ("text"); text != properties.end ()) {
	    return text->second.value.getType () == DynamicValue::String ? text->second.value.getString ()
									  : text->second.value.toString ();
	}
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));

    std::string result;
    if (!JS_IsUndefined (layerObj) && !JS_IsNull (layerObj)) {
	JSValue thisLayer = JS_GetPropertyStr (ctx, layerObj, "thisLayer");
	JSValue textVal = JS_GetPropertyStr (ctx, thisLayer, "text");
	if (!JS_IsUndefined (textVal) && !JS_IsNull (textVal)) {
	    const char* cstr = JS_ToCString (ctx, textVal);
	    if (cstr) {
		result.assign (cstr);
		JS_FreeCString (ctx, cstr);
	    }
	}
	JS_FreeValue (ctx, textVal);
	JS_FreeValue (ctx, thisLayer);
    }

    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, layers);
    JS_FreeValue (ctx, globalObj);
    return result;
}

void ScriptEngine::destroyLayer (ScriptLayerHandle handle) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));

    if (!JS_IsUndefined (layerObj) && !JS_IsNull (layerObj)) {
	JSValue fn = JS_GetPropertyStr (ctx, layerObj, "_destroy");
	if (JS_IsFunction (ctx, fn)) {
	    JSValue ret = JS_Call (ctx, fn, layerObj, 0, nullptr);
	    if (JS_IsException (ret)) {
		logJSException (ctx, "layer.destroy");
	    }
	    JS_FreeValue (ctx, ret);
	}
	JS_FreeValue (ctx, fn);
    }
    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, layers);
    JS_FreeValue (ctx, globalObj);

    // Remove the entry from globalThis.__textLayers so GC can reclaim its closures.
    const std::string delScript = "delete globalThis.__textLayers[" + std::to_string (handle) + "];";
    JSValue delResult = JS_Eval (ctx, delScript.c_str (), delScript.size (), "<layer-destroy>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException (delResult)) {
	logJSException (ctx, "layer.destroy.delete");
    }
    JS_FreeValue (ctx, delResult);

    this->m_layerInitialized.erase (handle);
    this->m_layerObjects.erase (handle);
    this->m_layerProps.erase (handle);
}

void ScriptEngine::queueScript (const std::string& key, DynamicValue& currentValue, ScriptableObject& object) {
    const auto source = currentValue.getScriptSource ();

    if (!source.has_value () || !this->m_scriptKeys.insert (key).second) {
	return;
    }

    // evaluated on the next tick: objects queue their scripts while the scene is still being built, and a script's
    // top level may already look up other layers (thisScene.getLayer) or this layer's other properties
    this->m_scriptModules.emplace_back (
	new LoadedModule { .key = key, .source = *source, .value = currentValue, .object = object }
    );
    this->m_pendingModules = true;
}

void ScriptEngine::logModuleError (LoadedModule& module, const char* what) {
    JSValue exception = JS_GetException (this->m_context);
    // a script that throws every frame would flood the log: its first few errors are enough
    constexpr int kMaxErrors = 5;

    if (++module.errors <= kMaxErrors) {
	const char* message = JS_ToCString (this->m_context, exception);
	std::string stack;

	if (JS_IsError (exception)) {
	    JSValue stackValue = JS_GetPropertyStr (this->m_context, exception, "stack");
	    if (const char* str = JS_IsString (stackValue) ? JS_ToCString (this->m_context, stackValue) : nullptr) {
		stack = str;
		JS_FreeCString (this->m_context, str);
	    }
	    JS_FreeValue (this->m_context, stackValue);
	}

	// a native call that returned JS_EXCEPTION without throwing leaves no error object behind
	const bool silent = JS_VALUE_GET_TAG (exception) == JS_TAG_UNINITIALIZED;
	sLog.error (
	    "ScriptEngine [", module.key, ".", what, "]: ",
	    silent ? "a native call failed without an error (an API LWE doesn't implement?)"
		   : (message ? message : "<exception>"),
	    stack.empty () ? "" : "\n", stack,
	    module.errors == kMaxErrors ? " (further errors of this script are not logged)" : ""
	);
	JS_FreeCString (this->m_context, message);
    }

    JS_FreeValue (this->m_context, exception);
}

void ScriptEngine::runPendingJobs () {
    JSContext* jobContext = nullptr;

    // promise reactions (async functions, .then); bounded so a job that keeps queueing jobs can't hang a frame
    for (int i = 0; i < 1024 && JS_IsJobPending (this->m_runtime); i++) {
	if (JS_ExecutePendingJob (this->m_runtime, &jobContext) < 0) {
	    logJSException (jobContext, "job");
	}
    }
}

void ScriptEngine::evaluateModule (LoadedModule& module) {
    JSContext* ctx = this->m_context;
    module.evaluated = true;

    // `thisLayer` and `thisObject` are the object the script is attached to, for this module alone: module-scope
    // bindings, so its event handlers and timer callbacks see them too. Prepended on the first line so that
    // error line numbers stay those of the script
    const std::string source = "const thisLayer = globalThis.__thisLayerBinding, thisObject = thisLayer; " + module.source;

    JSValue function = JS_Eval (
	ctx, source.c_str (), source.size (), module.key.c_str (), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
    );

    if (JS_IsException (function)) {
	this->logModuleError (module, "compile");
	return;
    }

    auto* definition = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (function));

    JS_SetPropertyStr (ctx, this->m_globalThis, "__thisLayerBinding", this->m_adapters.object->instantiate (module.object));
    // createScriptProperties().finish() binds the stored script properties of the module being evaluated
    this->m_runningModule = &module;
    JSValue promise = JS_EvalFunction (ctx, function);
    this->runPendingJobs ();
    this->m_runningModule = nullptr;
    JS_SetPropertyStr (ctx, this->m_globalThis, "__thisLayerBinding", JS_UNDEFINED);

    if (JS_IsException (promise)) {
	this->logModuleError (module, "load");
	return;
    }

    // a module evaluation is a promise (top-level await); a throw at the top level rejects it
    if (JS_PromiseState (ctx, promise) == JS_PROMISE_REJECTED) {
	JS_Throw (ctx, JS_PromiseResult (ctx, promise));
	this->logModuleError (module, "load");
	JS_FreeValue (ctx, promise);
	return;
    }

    JS_FreeValue (ctx, promise);
    module.ns = JS_GetModuleNamespace (ctx, definition);
}

void ScriptEngine::callModule (LoadedModule& module, const char* name, int argc, JSValueConst argv[], bool writeBack) {
    JSContext* ctx = this->m_context;

    if (!JS_IsObject (module.ns)) {
	return;
    }

    JSValue function = JS_GetPropertyStr (ctx, module.ns, name);

    if (!JS_IsFunction (ctx, function)) {
	JS_FreeValue (ctx, function);
	return;
    }

    this->m_runningModule = &module;
    JSValue result = JS_Call (ctx, function, JS_UNDEFINED, argc, argv);
    this->m_runningModule = nullptr;
    JS_FreeValue (ctx, function);

    if (JS_IsException (result)) {
	this->logModuleError (module, name);
    } else if (writeBack) {
	jsToDynamicValue (ctx, result, module.value);
    }

    JS_FreeValue (ctx, result);
}

JSValue ScriptEngine::userPropertyValue (const Property& property) {
    // "color properties are converted to Vec3 automatically"
    return property.getType () == DynamicValue::Vec4 ? this->newVec (property.getVec3 ())
						      : this->dynamicToJs (const_cast<Property&> (property));
}

JSValue ScriptEngine::userProperties () {
    JSValue properties = JS_NewObject (this->m_context);

    for (const auto& [name, property] : this->m_scene.getScene ().project.properties) {
	JS_SetPropertyStr (this->m_context, properties, name.c_str (), this->userPropertyValue (*property));
    }

    return properties;
}

void ScriptEngine::startPendingScripts () {
    if (!this->m_pendingModules) {
	return;
    }

    this->m_pendingModules = false;

    // scripts queued since the last tick: evaluate every module first, then init(value) each, in the order the
    // objects queued them (so one script's init sees the others' top-level state, e.g. `shared`)
    for (size_t i = 0; i < this->m_scriptModules.size (); i++) {
	if (!this->m_scriptModules[i]->evaluated) {
	    this->evaluateModule (*this->m_scriptModules[i]);
	}
    }

    std::vector<LoadedModule*> started;

    for (size_t i = 0; i < this->m_scriptModules.size (); i++) {
	auto& module = *this->m_scriptModules[i];

	if (module.initialized) {
	    continue;
	}

	module.initialized = true;
	started.push_back (&module);
	JSValue args[] = { this->dynamicToJs (module.value) };
	this->callModule (module, "init", 1, args, true);
	JS_FreeValue (this->m_context, args[0]);
    }

    // text-layer scripts: init (value) once, the text it returns adopted
    std::vector<ScriptLayerHandle> startedLayers;

    for (auto& [handle, initialized] : this->m_layerInitialized) {
	if (!initialized) {
	    initialized = true;
	    startedLayers.push_back (handle);
	}
    }

    for (const auto handle : startedLayers) {
	this->callLayerHook (handle, "_init", 0, nullptr, false);
    }

    // WE calls applyUserProperties once on load, after init, with every user property (scripts set up state there
    // that update relies on); later, with the changed ones only (tick)
    if (!started.empty () || !startedLayers.empty ()) {
	JSValue properties = this->userProperties ();

	for (auto* module : started) {
	    this->callModule (*module, "applyUserProperties", 1, &properties, false);
	}
	for (const auto handle : startedLayers) {
	    this->callLayerHook (handle, "applyUserProperties", 1, &properties, true);
	}

	JS_FreeValue (this->m_context, properties);
    }
}

void ScriptEngine::dispatchEvent (const char* name, JSValueConst arg, const ScriptableObject* only) {
    JSValueConst args[] = { arg };

    for (size_t i = 0; i < this->m_scriptModules.size (); i++) {
	auto& module = *this->m_scriptModules[i];

	if (only == nullptr || &module.object == only) {
	    this->callModule (module, name, 1, args, false);
	}
    }

    std::vector<ScriptLayerHandle> handles;

    for (const auto& [handle, object] : this->m_layerObjects) {
	if (only == nullptr || object == only) {
	    handles.push_back (handle);
	}
    }

    for (const auto handle : handles) {
	this->callLayerHook (handle, name, 1, args, true);
    }
}

bool ScriptEngine::handlesEvent (const ScriptableObject& object, const char* name) {
    JSContext* ctx = this->m_context;

    for (const auto& module : this->m_scriptModules) {
	if (&module->object != &object || !JS_IsObject (module->ns)) {
	    continue;
	}

	JSValue fn = JS_GetPropertyStr (ctx, module->ns, name);
	const bool found = JS_IsFunction (ctx, fn);
	JS_FreeValue (ctx, fn);

	if (found) {
	    return true;
	}
    }

    for (const auto& [handle, layer] : this->m_layerObjects) {
	if (layer != &object) {
	    continue;
	}

	JSValue layers = JS_GetPropertyStr (ctx, this->m_globalThis, "__textLayers");
	JSValue layerObj = JS_IsObject (layers) ? JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle))
						: JS_UNDEFINED;
	JSValue events = JS_IsObject (layerObj) ? JS_GetPropertyStr (ctx, layerObj, "_events") : JS_UNDEFINED;
	JSValue fn = JS_IsObject (events) ? JS_GetPropertyStr (ctx, events, name) : JS_UNDEFINED;
	const bool found = JS_IsFunction (ctx, fn);

	JS_FreeValue (ctx, fn);
	JS_FreeValue (ctx, events);
	JS_FreeValue (ctx, layerObj);
	JS_FreeValue (ctx, layers);

	if (found) {
	    return true;
	}
    }

    return false;
}

void ScriptEngine::tick () {
    this->startPendingScripts ();

    // user properties changed since the last tick: applyUserProperties gets only those
    if (!this->m_changedProperties.empty ()) {
	JSValue changed = JS_NewObject (this->m_context);
	const auto& properties = this->m_scene.getScene ().project.properties;

	for (const auto& name : this->m_changedProperties) {
	    if (const auto it = properties.find (name); it != properties.end ()) {
		JS_SetPropertyStr (this->m_context, changed, name.c_str (), this->userPropertyValue (*it->second));
	    }
	}

	this->m_changedProperties.clear ();
	this->dispatchEvent ("applyUserProperties", changed);
	JS_FreeValue (this->m_context, changed);
    }

    // run intervals
    this->m_engineObject->tick ();

    // run any pending notifications (property scripts and text-layer scripts)
    if (!this->m_queuedEvents.empty ()) {
	auto events = std::move (this->m_queuedEvents);
	this->m_queuedEvents.clear ();

	for (auto& [name, value] : events) {
	    this->dispatchEvent (name.c_str (), value);
	    JS_FreeValue (this->m_context, value);
	}
    }

    // run all update methods
    for (size_t i = 0; i < this->m_scriptModules.size (); i++) {
	auto& module = *this->m_scriptModules[i];
	JSValue args[] = { this->dynamicToJs (module.value) };
	this->callModule (module, "update", 1, args, true);
	JS_FreeValue (this->m_context, args[0]);
    }

    this->runPendingJobs ();
}

void ScriptEngine::notifyTrackMetadataChange (const MediaProperties& properties) {
    JSContext* ctx = this->m_context;
    JSValue event = JS_NewObject (ctx);

    // MediaPropertiesEvent: title, artist, contentType ("music, video or image"); albumTitle, subTitle, albumArtist,
    // genres (comma-separated) when known. Unknown ones are empty strings (a script printing one shows nothing)
    const std::pair<const char*, const std::string*> fields[] = {
	{ "title", &properties.title },		  { "artist", &properties.artist },
	{ "albumTitle", &properties.albumTitle }, { "albumArtist", &properties.albumArtist },
	{ "subTitle", &properties.subTitle },	  { "genres", &properties.genres },
	{ "contentType", &properties.contentType },
    };

    for (const auto& [name, value] : fields) {
	JS_SetPropertyStr (ctx, event, name, JS_NewString (ctx, value->c_str ()));
    }

    if (const auto it = this->m_queuedEvents.find ("mediaPropertiesChanged"); it != this->m_queuedEvents.end ()) {
	JS_FreeValue (ctx, it->second);
    }
    this->m_queuedEvents.insert_or_assign ("mediaPropertiesChanged", event);
}

void ScriptEngine::notifyMediaStatusChange (bool enabled) {
    JSContext* ctx = this->m_context;
    JSValue event = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, event, "enabled", JS_NewBool (ctx, enabled));

    if (const auto it = this->m_queuedEvents.find ("mediaStatusChanged"); it != this->m_queuedEvents.end ()) {
	JS_FreeValue (ctx, it->second);
    }
    this->m_queuedEvents.insert_or_assign ("mediaStatusChanged", event);
}

namespace {
float luminance (const glm::vec3& c) {
    auto channel = [] (float v) { return v <= 0.03928f ? v / 12.92f : std::pow ((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * channel (c.r) + 0.7152f * channel (c.g) + 0.0722f * channel (c.b);
}

/** WCAG's contrast ratio, 1..21 */
float contrast (const glm::vec3& a, const glm::vec3& b) {
    const float la = luminance (a), lb = luminance (b);
    return (std::max (la, lb) + 0.05f) / (std::min (la, lb) + 0.05f);
}

struct Palette {
    glm::vec3 primary, secondary, tertiary, text, highContrast;
};

/**
 * MediaThumbnailEvent's colours from the album art: the most common colour (4 bits per channel, averaged), then the
 * next common ones that differ from those before. WE's own extraction isn't decoded; its documented guarantees are
 * kept: textColor has enough contrast with the primary colour (WCAG 4.5, else black/white), highContrastColor is
 * black or white, whichever contrasts more with the primary colour
 */
std::optional<Palette> paletteOf (const std::string& path) {
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load (path.c_str (), &width, &height, &channels, 3);

    if (pixels == nullptr || width <= 0 || height <= 0) {
	if (pixels != nullptr) {
	    stbi_image_free (pixels);
	}
	return std::nullopt;
    }

    struct Bucket {
	glm::dvec3 sum {0.0};
	int count = 0;
    };
    std::vector<Bucket> buckets (4096);
    // at most ~128x128 samples
    const int stepX = std::max (1, width / 128), stepY = std::max (1, height / 128);

    for (int y = 0; y < height; y += stepY) {
	for (int x = 0; x < width; x += stepX) {
	    const unsigned char* px = pixels + (static_cast<size_t> (y) * width + x) * 3;
	    auto& bucket = buckets[((px[0] >> 4) << 8) | ((px[1] >> 4) << 4) | (px[2] >> 4)];
	    bucket.sum += glm::dvec3 (px[0], px[1], px[2]) / 255.0;
	    bucket.count++;
	}
    }

    stbi_image_free (pixels);

    std::vector<const Bucket*> order;

    for (const auto& bucket : buckets) {
	if (bucket.count > 0) {
	    order.push_back (&bucket);
	}
    }

    std::ranges::sort (order, [] (const Bucket* a, const Bucket* b) { return a->count > b->count; });

    std::vector<glm::vec3> picked;

    for (const auto* bucket : order) {
	const glm::vec3 colour = glm::vec3 (bucket->sum / static_cast<double> (bucket->count));
	const bool distinct = std::ranges::all_of (picked, [&] (const glm::vec3& c) { return glm::distance (c, colour) > 0.25f; });

	if (distinct) {
	    picked.push_back (colour);
	}
	if (picked.size () == 3) {
	    break;
	}
    }

    if (picked.empty ()) {
	return std::nullopt;
    }

    Palette palette {};
    palette.primary = picked[0];
    // a one-colour image: lighter and darker shades of it
    palette.secondary = picked.size () > 1 ? picked[1] : glm::clamp (picked[0] * 0.6f, 0.0f, 1.0f);
    palette.tertiary = picked.size () > 2 ? picked[2] : glm::clamp (picked[0] * 0.6f + 0.4f, 0.0f, 1.0f);

    const glm::vec3 white (1.0f), black (0.0f);
    palette.highContrast = contrast (white, palette.primary) >= contrast (black, palette.primary) ? white : black;
    palette.text = palette.highContrast;

    for (const auto& candidate : { palette.secondary, palette.tertiary }) {
	if (contrast (candidate, palette.primary) >= 4.5f) {
	    palette.text = candidate;
	    break;
	}
    }

    return palette;
}
} // namespace

void ScriptEngine::notifyAlbumArtUrlChange (const std::optional<std::string>& url) {
    // "every time the thumbnail image changes, not between tracks using the same thumbnail image"
    if (this->m_thumbnailSent && url == this->m_lastThumbnail) {
	return;
    }

    this->m_thumbnailSent = true;
    this->m_lastThumbnail = url;

    JSContext* ctx = this->m_context;
    std::optional<Palette> palette;

    if (url.has_value () && url->starts_with ("file://")) {
	palette = paletteOf (url->substr (7));
    }

    JSValue event = JS_NewObject (ctx);

    // without an image WE's colours are unknown: the greys LWE always sent, and white text
    const Palette colours = palette.value_or (Palette {
	.primary = glm::vec3 (0.12f),
	.secondary = glm::vec3 (0.0f),
	.tertiary = glm::vec3 (0.25f),
	.text = glm::vec3 (1.0f),
	.highContrast = glm::vec3 (1.0f),
    });

    JS_SetPropertyStr (ctx, event, "hasThumbnail", JS_NewBool (ctx, palette.has_value ()));
    JS_SetPropertyStr (ctx, event, "primaryColor", this->newVec (colours.primary));
    JS_SetPropertyStr (ctx, event, "secondaryColor", this->newVec (colours.secondary));
    JS_SetPropertyStr (ctx, event, "tertiaryColor", this->newVec (colours.tertiary));
    JS_SetPropertyStr (ctx, event, "textColor", this->newVec (colours.text));
    JS_SetPropertyStr (ctx, event, "highContrastColor", this->newVec (colours.highContrast));

    if (const auto it = this->m_queuedEvents.find ("mediaThumbnailChanged"); it != this->m_queuedEvents.end ()) {
	JS_FreeValue (ctx, it->second);
    }
    this->m_queuedEvents.insert_or_assign ("mediaThumbnailChanged", event);
}

void ScriptEngine::notifyPlaybackStateChange (const std::optional<wp_media_playback_state> state) {
    JSContext* ctx = this->m_context;
    JSValue playbackEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (
	ctx, playbackEvent, "state", JS_NewInt32 (ctx, state.has_value () ? *state : WP_MEDIA_PLAYBACK_STATE_STOPPED)
    );

    if (const auto it = this->m_queuedEvents.find ("mediaPlaybackChanged"); it != this->m_queuedEvents.end ()) {
	JS_FreeValue (ctx, it->second);
    }
    this->m_queuedEvents.insert_or_assign ("mediaPlaybackChanged", playbackEvent);
}

void ScriptEngine::notifyPlaybackPositionAndDurationChange (
    const std::optional<double> position, const std::optional<double> duration
) {
    JSContext* ctx = this->m_context;
    JSValue mediaTimelineEvent = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, mediaTimelineEvent, "position", JS_NewFloat64 (ctx, position.has_value () ? *position : 0));
    JS_SetPropertyStr (ctx, mediaTimelineEvent, "duration", JS_NewFloat64 (ctx, duration.has_value () ? *duration : 0));

    if (const auto it = this->m_queuedEvents.find ("mediaTimelineChanged"); it != this->m_queuedEvents.end ()) {
	JS_FreeValue (ctx, it->second);
    }
    this->m_queuedEvents.insert_or_assign ("mediaTimelineChanged", mediaTimelineEvent);
}
