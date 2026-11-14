#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Shaders {
class GLSLContext {
public:
    /**
     * Types of shaders
     */
    enum UnitType { UnitType_Vertex = 0, UnitType_Fragment = 1 };

    GLSLContext ();
    ~GLSLContext ();

    /**
     * The GLSL 330 the driver compiles for a WE vertex + fragment pair ({"", ""} when it doesn't build). With
     * `cacheDir`, a result stored there by an earlier call with the same texts is used as it is
     * (ShaderCache.h), and a new one is stored; not while the health report or a capture is on, whose records come
     * from the compile itself
     */
    [[nodiscard]] std::pair<std::string, std::string> toGlsl (
	const std::string& vertex, const std::string& fragment,
	const std::optional<std::filesystem::path>& cacheDir = std::nullopt
    );

    [[nodiscard]] static GLSLContext& get ();

private:
    static std::unique_ptr<GLSLContext> sInstance;
};
} // namespace WallpaperEngine::Render::Shaders