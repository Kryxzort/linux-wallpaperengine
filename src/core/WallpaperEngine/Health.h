#pragma once

#include <string>

#include <nlohmann/json.hpp>

/**
 * Scene health: what the core built, what it left out and why, as JSON lines.
 *
 * Off unless `WPENGINE_HEALTH=<path>` is set when a project loads (`wp_project_load_*`): then every record below is
 * appended to that file, one JSON object per line, `{"ev": <event>, …}`, plus the keys of the scopes open on this
 * thread (`object`, `object_name`, `effect`, `shader`, …) so an error deep in a shader compile names its layer.
 * Identical lines are written once. Nothing the renderer does depends on it: each call site is
 * `WPENGINE_HEALTH (…)`, one load of a bool when it is off.
 *
 * Events: `scene` (size, object count), `object` (one per scene object after the build: id, name, type, parent,
 * built), `dropped` (an object not built, its reason), `effect_dropped`, `shader_error` (glslang / GL compile or
 * link), `missing_asset` (kind: texture, font, model, puppet, particle, material, sound, shader_module), `script_error`
 * (the script's key, object, property, phase), `unsupported` (a feature LWE logs as not supported), `compat` (an
 * HLSL-lenient rewrite or a case-insensitive file match — informational), `layers` (per drawable at render frames 1
 * and 31: effective visibility, alpha, canvas bbox, non-finite transforms), `load_failed`, `renderer_failed`.
 */
namespace WallpaperEngine::Health {
/** Reads `WPENGINE_HEALTH` and opens (or closes) the output; called when a project loads */
void configure () noexcept;
/** True while `WPENGINE_HEALTH` named a file that opened */
[[nodiscard]] bool enabled () noexcept;
/** Appends `{"ev": event, <open scopes>, <fields>}` (fields win over scopes); a no-op when off */
void report (const char* event, nlohmann::json fields = nlohmann::json::object ()) noexcept;

/** A key every record on this thread carries until the scope closes (nests; an inner key shadows) */
class Scope {
public:
    Scope (const char* key, int value) noexcept;
    Scope (const char* key, const std::string& value) noexcept;
    ~Scope ();
    Scope (const Scope&) = delete;
    Scope& operator= (const Scope&) = delete;

private:
    bool m_pushed = false;
};

/** The first `max` bytes of a log message, without a "Compiled source code:" dump */
[[nodiscard]] std::string brief (const std::string& message, size_t max = 600);
} // namespace WallpaperEngine::Health

#define WPENGINE_HEALTH(...)                                                                                           \
    do {                                                                                                               \
	if (::WallpaperEngine::Health::enabled ()) {                                                                   \
	    ::WallpaperEngine::Health::report (__VA_ARGS__);                                                           \
	}                                                                                                              \
    } while (0)
