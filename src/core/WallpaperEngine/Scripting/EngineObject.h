#pragma once
#include "quickjs.h"

#include <map>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class EngineObject {
public:
    EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~EngineObject ();

    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    JSValue getInstance () const { return m_instance; }
    ScriptEngine& getEngine () const { return m_engine; }
    uint32_t getInstanceId () const { return m_instanceId; }
    uint32_t reserveNextTimeoutId (JSValue function, uint64_t duration);
    uint32_t reserveNextIntervalId (JSValue function, uint64_t duration);
    void clearTimeout (uint32_t id);
    void clearInterval (uint32_t id);

    void tick ();

protected:
    /**
     * Timers run on the scene's clock (the host's wp_time_counter, in seconds), not the wall clock:
     * a host that pauses or steps time pauses or steps them too
     */
    struct Timeout {
	JSValue callback;
	double duration;
	double next;
    };

    /** runs a timer callback, logging what it throws */
    void call (JSValue callback);

    /** @return The scene's current time in seconds (what engine.runtime reports) */
    [[nodiscard]] double now () const;

    uint32_t m_nextTimeoutId = 0;
    uint32_t m_nextIntervalId = 0;
    std::map<uint32_t, Timeout> m_intervals;
    std::map<uint32_t, Timeout> m_timeouts;
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    uint32_t m_instanceId;
    JSClassID m_classId;
    JSClassDef m_definition;
    JSValue m_instance;
};
}