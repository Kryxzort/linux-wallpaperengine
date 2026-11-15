#pragma once

#include "Adapters/Types.h"
#include "Adapters/Virtual.h"

#include "../Data/Utils/BinaryReader.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace WallpaperEngine::FileSystem {
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::FileSystem::Adapters;

class Container {
public:
    Container ();
    ~Container () = default;

    /**
     * Opens the given file in read mode
     *
     * @param path The file to open
     * @return The input stream to read file's data off of
     */
    [[nodiscard]] ReadStreamSharedPtr read (const std::filesystem::path& path) const;

    /**
     * Opens the given file and reads it completely into memory
     *
     * @param path The file to open
     * @return The file's contents as std::string
     */
    [[nodiscard]] std::string readString (const std::filesystem::path& path) const;

    /**
     * Tries to resolve the given file into an absolute, real, filesystem path
     * to be used as info for other tools that might require it (like MPV)
     *
     * @param path The path to resolve to a real file
     * @return The full public, absolute path to the given file
     */
    [[nodiscard]] std::filesystem::path physicalPath (const std::filesystem::path& path) const;

    /**
     *
     * @param path Base of the mountpoint
     * @param mountPoint Where in the VFS to mount it
     */
    AdapterSharedPtr mount (const std::filesystem::path& path, const std::filesystem::path& mountPoint);

    /**
     * @return Access to the virtual file system adapter in this container instance
     */
    VirtualAdapter& getVFS () const;

    /**
     * @param factory The factory to register for this container
     */
    void registerAdapterFactory (FactoryUniquePtr factory);

private:
    /**
     * Searches for an adapter to handle the given file
     *
     * @param path The path to the file
     * @return The adapter handling the file
     */
    Adapter& resolveAdapterForFile (const std::filesystem::path& path) const;
    /**
     * Searches for an adapter to handle the given file, and the path to give it: the requested one, or, when no
     * adapter has that exact path, the one an adapter has that matches it ignoring case
     *
     * @param path The path to the file
     * @return The adapter handling the file and the path to open on it
     */
    std::pair<Adapter&, std::filesystem::path> resolve (const std::filesystem::path& path) const;
    /** resolve without m_resolved */
    std::pair<Adapter&, std::filesystem::path> resolveUncached (const std::filesystem::path& path) const;
    /**
     * @param normalized The normalized path
     * @param ignoreCase Whether to match it ignoring case (findIgnoringCase) instead of exactly (exists)
     * @param found The path to open on the adapter (only set when one is returned)
     * @return The first mountpoint's adapter that has the file
     */
    Adapter* findAdapter (const std::string& normalized, bool ignoreCase, std::filesystem::path& found) const;
    /** The factories available for this container */
    std::vector<FactoryUniquePtr> m_factories;
    /** Mountpoints on this container */
    std::vector<std::pair<std::filesystem::path, AdapterSharedPtr>> m_mountpoints;
    /** Virtual file system adapter */
    std::shared_ptr<VirtualAdapter> m_vfs;

    /** what resolve found for a path as asked: the adapter (null: in no mountpoint) and the path to open on it */
    struct Resolved {
	Adapter* adapter;
	std::filesystem::path path;
    };
    /**
     * resolve's answers, so a path asked for again (a scene reads common.h once per shader unit, and probes the same
     * missing zcompat and texture paths, each a directory scan ignoring case) costs one lookup. Kept while the
     * mountpoints and the VFS stay as they were (m_resolvedFor); off while the health report is on, which records
     * every lookup's own outcome. Files are reopened on every read, so a remembered hit reads the file as it is now
     */
    mutable std::unordered_map<std::string, Resolved> m_resolved;
    /** the mountpoint count and VFS generation m_resolved was made for */
    mutable std::pair<size_t, uint64_t> m_resolvedFor { 0, 0 };
    mutable std::mutex m_resolvedLock;
};

using ContainerUniquePtr = std::unique_ptr<Container>;
}