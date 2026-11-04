#pragma once

#include <optional>
#include <string>

#include "Types.h"

namespace WallpaperEngine::Data::Model {
enum PassCommandType { Command_Copy = 0, Command_Swap = 1 };

struct FBO {
    std::string name;
    std::string format;
    float scale;
    bool unique;
    /** `fit`: the longer side at most this many pixels, the aspect kept (0: none; WE takes 1..4096) */
    uint32_t fit = 0;
    /** `width` / `height`: the size instead of the layer's (0: the layer's; WE takes 1..4096) */
    uint32_t width = 0;
    uint32_t height = 0;
    /** `uvs`: "repeat" samples it wrapping */
    bool repeatUVs = false;
};

struct EffectPass {
    /** The material to use for this effect's pass */
    std::optional<MaterialUniquePtr> material;
    /** Texture bindings for this effect's pass */
    TextureMap binds;
    /** The command this material executes (if specified) */
    std::optional<PassCommandType> command;
    /** The source this material renders from (if specified) */
    std::optional<std::string> source;
    /** The target this material renders to (if specified) */
    std::optional<std::string> target;
};

struct Effect {
    /** Effect's name for the UI */
    std::string name;
    /** Effect's description for the UI */
    std::string description;
    /** Effect's group for the UI */
    std::string group;
    /** Effect's preview project */
    std::string preview;
    /** Effect's dependencies */
    std::vector<std::string> dependencies;
    /** The different passes for this effect */
    std::vector<EffectPassUniquePtr> passes;
    /** The fbos declared by this effect */
    std::vector<FBOUniquePtr> fbos;
};
} // namespace WallpaperEngine::Data::Model