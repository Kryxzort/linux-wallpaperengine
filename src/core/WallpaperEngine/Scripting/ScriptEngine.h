#pragma once

#include "ConsoleObject.h"
#include "EngineObject.h"
#include "InputObject.h"
#include "SceneObject.h"

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "linux-wallpaperengine/context.h"

namespace WallpaperEngine::Media {
class MediaSource;
}
extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Scripting {
class ScriptPropertiesObject;
class ScriptableObject;
namespace Adapters {
    class ScriptableObjectAdapter;
}
using namespace WallpaperEngine::Data::Model;

// Opaque handle returned by createLayerScript. 0 means invalid / not created.
using ScriptLayerHandle = int;
static constexpr ScriptLayerHandle kInvalidLayerHandle = 0;

class ScriptEngine {
public:
    /**
     * A property script: an ES module attached to one property of one object. Compiled and evaluated on the
     * first tick after it was queued (the whole scene exists by then), `init(value)` once, `update(value)` every
     * tick; a return of `undefined`/`null` leaves the property as it is
     */
    struct LoadedModule {
	std::string key;
	std::string source;
	DynamicValue& value;
	ScriptableObject& object;
	/** the module namespace once evaluated (JS_UNDEFINED before, or when it failed to load) */
	JSValue ns = JS_UNDEFINED;
	bool evaluated = false;
	bool initialized = false;
	int errors = 0;
    };
    struct JSObjectAdapters {
	std::unique_ptr<Adapters::ScriptableObjectAdapter> object;
    };

    ~ScriptEngine ();
    ScriptEngine (Render::Wallpapers::CScene& scene);
    ScriptEngine (const ScriptEngine&) = delete;
    ScriptEngine& operator= (const ScriptEngine&) = delete;

    JSRuntime* getRuntime () const { return m_runtime; }
    JSContext* getContext () const { return m_context; }
    JSValue getGlobalThis () const { return m_globalThis; }
    LoadedModule* getRunningModule () const { return m_runningModule; }
    JSValue dynamicToJs (DynamicValue& value) const;
    /**
     * Writes a script value into `target` (numbers, booleans, strings, Vec2/3/4); undefined/null write nothing
     * @return false when nothing was written
     */
    bool jsToDynamic (JSValueConst value, DynamicValue& target) const;
    /** A new instance of the scripts' own `Vec2`/`Vec3`/`Vec4` class (Wallpaper Engine's baseclasses.js) */
    JSValue newVec (const glm::vec2& value) const;
    JSValue newVec (const glm::vec3& value) const;
    JSValue newVec (const glm::vec4& value) const;
    /** Wallpaper Engine's script assets (`scripts/…`) as the scene's asset locator finds them; empty if absent */
    std::string readScriptAsset (const std::string& path) const;

    /**
     * Queue the property script of `currentValue` (its `getScriptSource`), once per key. The module is
     * compiled and evaluated on the next tick, with `thisLayer`/`thisObject` bound to `object` for that
     * module only; then `init(value)` runs once and `update(value)` every tick, their non-undefined
     * returns written to `currentValue`
     */
    void queueScript (const std::string& key, DynamicValue& currentValue, ScriptableObject& object);

    /**
     * Runs a frame tick in the javascript engine. Dispatches any pending events,
     * timeouts, intervals AND calls any update() functions.
     */
    void tick ();

    // -------------------------------------------------------------------
    // Layer-script API (Phase 2 — dynamic text)
    // -------------------------------------------------------------------
    //
    // Wallpaper Engine text-object scripts follow a lifecycle pattern that
    // cannot be evaluated with the simple `update(value) -> value` contract
    // above. They typically look like:
    //
    //   'use strict';
    //   export var scriptProperties = createScriptProperties()…finish();
    //   export function init()   { /* subscribe to events, cache data    */ }
    //   export function update() { thisLayer.text = computeCurrentText(); }
    //
    // The script mutates a `thisLayer` object in place instead of returning
    // a value, and lifecycle functions are optional. The API below keeps
    // per-layer state alive across frames so `init()` runs once and
    // `update()` re-runs every tick.

    /**
     * Create a persistent "layer script" from a WE text-object script.
     *
     * @param scriptSource The full JS script text.
     * @param initialScriptProps Initial values for scriptProperties entries.
     *        Ownership stays with the caller; we only snapshot current values.
     * @param initialText Initial value of `thisLayer.text` (usually the
     *        static placeholder carried in the JSON).
     * @return A positive handle, or kInvalidLayerHandle if evaluation failed.
     */
    ScriptLayerHandle createLayerScript (
	const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
	const std::string& initialText
    );

    /**
     * Advance a layer by one frame.
     *
     * On the first call, invokes `init()` (if defined) before `update()`.
     * Updates a `thisScene` context visible to the script (time, fps).
     * Silently no-ops if the handle is invalid.
     */
    void tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps);

    /**
     * Read the current value of `thisLayer.text` for the given layer.
     * Returns an empty string if the handle is invalid.
     */
    std::string layerText (ScriptLayerHandle handle);

    /**
     * Tear down a layer: invokes `destroy()` (if defined) and frees state.
     */
    void destroyLayer (ScriptLayerHandle handle);

    const JSObjectAdapters& getAdapters () const { return m_adapters; }
    const Render::Wallpapers::CScene& getScene () const { return m_scene; }

    void notifyTrackMetadataChange (
	const std::optional<std::string>& title, const std::optional<std::string>& artist,
	const std::optional<std::string>& album
    );
    void notifyAlbumArtUrlChange (const std::optional<std::string>& url);
    void notifyPlaybackStateChange (std::optional<wp_media_playback_state> state);
    void notifyPlaybackPositionAndDurationChange (std::optional<double> position, std::optional<double> duration);

private:
    JSValue newVec (int components, const float* values) const;

    void installPrelude ();
    void installBuiltins ();
    void evaluateModule (LoadedModule& module);
    /** calls `name(argv…)` on the module if it exports it; a value other than undefined/null is written back */
    void callModule (LoadedModule& module, const char* name, int argc, JSValueConst argv[], bool writeBack);
    void logModuleError (LoadedModule& module, const char* what);
    void runPendingJobs ();

    // Installs globalThis.__layers and related helpers. Called lazily.
    void ensureLayerRegistry ();

    void queueEvents ();

    JSRuntime* m_runtime = nullptr;
    JSContext* m_context = nullptr;
    JSValue m_globalThis;
    Render::Wallpapers::CScene& m_scene;
    std::unique_ptr<EngineObject> m_engineObject;
    std::unique_ptr<InputObject> m_inputObject;
    std::unique_ptr<SceneObject> m_sceneObject;
    std::unique_ptr<ConsoleObject> m_consoleObject;
    std::unique_ptr<ScriptPropertiesObject> m_scriptPropertiesObject;

    /** property scripts in the order they were queued (init runs in that order) */
    std::vector<std::unique_ptr<LoadedModule>> m_scriptModules = {};
    std::unordered_set<std::string> m_scriptKeys = {};
    bool m_pendingModules = false;
    /** the scripts' Vec2/Vec3/Vec4 constructors, [0] = Vec2 */
    JSValue m_vecCtors[3] = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };

    LoadedModule* m_runningModule = nullptr;

    ScriptLayerHandle m_nextLayerId = 1;
    bool m_layerRegistryReady = false;
    std::map<ScriptLayerHandle, bool> m_layerInitialized;
    bool m_builtinsInstalled = false;

    std::map<std::string, JSValue> m_queuedEvents;

    JSObjectAdapters m_adapters;
};
} // namespace WallpaperEngine::Scripting
