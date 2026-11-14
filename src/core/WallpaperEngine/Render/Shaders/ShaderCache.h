#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * Shader text work kept on disk across launches (wp_config_set_shader_cache_dir): the results of a pure function of
 * texts, stored under the texts it was given.
 *
 * Two kinds use it, both of which a scene repeats at every launch and many times within one build (one effect
 * shader serves many passes): "unit", ShaderUnit::compile's WE text passes (keys: everything compile reads), and
 * "glsl", GLSLContext::toGlsl's glslang + SPIRV-Cross translation (keys: its two input texts). One file per entry,
 * `<hash>.<kind>`, named after a hash of the kind and keys; it holds the core's build id, the kind, every key and
 * every value, LZ4-compressed. A hit needs the build id, the kind and every key byte-equal, so another build of the
 * core (whose code is what turns the keys into the values), a hash collision or a torn or corrupt file reads as a
 * miss: the work is done again and the file rewritten. Entries are written to a temporary file and renamed into
 * place, so a reader never sees a partial one. A core without an ELF build id caches nothing: there would be nothing
 * to tell its results from another build's.
 */
namespace WallpaperEngine::Render::Shaders::ShaderCache {
using Texts = std::vector<std::string>;

/** The values stored in `dir` for `kind` and these keys, if a valid entry is there */
[[nodiscard]] std::optional<Texts> load (const std::filesystem::path& dir, const std::string& kind, const Texts& keys) noexcept;

/** Stores `values` for `kind` and these keys in `dir`; a failure leaves the cache as it was */
void store (const std::filesystem::path& dir, const std::string& kind, const Texts& keys, const Texts& values) noexcept;

/** The core's ELF build id as hex, "" when it has none (then nothing is cached) */
[[nodiscard]] const std::string& buildId () noexcept;
} // namespace WallpaperEngine::Render::Shaders::ShaderCache
