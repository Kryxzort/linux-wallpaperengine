#pragma once

#include "../../Data/Utils/BinaryReader.h"
#include <filesystem>
#include <optional>
#include <string_view>

namespace WallpaperEngine::FileSystem::Adapters {
using namespace WallpaperEngine::Data::Utils;
struct Adapter {
    Adapter () = default;
    virtual ~Adapter () = default;

    [[nodiscard]] virtual ReadStreamSharedPtr open (const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual bool exists (const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual std::filesystem::path physicalPath (const std::filesystem::path& path) const = 0;
    /**
     * Wallpaper Engine runs on a case-insensitive filesystem, so scenes reference their files with whatever case
     * the author typed. Called only after no adapter had the exact path.
     *
     * @param path The path as requested
     * @return The path of the file this adapter holds that matches it ignoring ASCII case, if any
     */
    [[nodiscard]] virtual std::optional<std::filesystem::path> findIgnoringCase (const std::filesystem::path& path
    ) const {
	return std::nullopt;
    }
};

/** ASCII case-insensitive equality, the comparison findIgnoringCase uses */
bool equalsIgnoringCase (std::string_view a, std::string_view b);

using AdapterSharedPtr = std::shared_ptr<Adapter>;

struct Factory {
    Factory () = default;
    virtual ~Factory () = default;

    [[nodiscard]] virtual bool handlesMountpoint (const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual AdapterSharedPtr create (const std::filesystem::path& path) const = 0;
};

using FactoryUniquePtr = std::unique_ptr<Factory>;
} // namespace WallpaperEngine::FileSystem::Adapters