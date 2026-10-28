#include <filesystem>
#include <fstream>
#include <memory>

#include "Directory.h"

#include "WallpaperEngine/Assets/AssetLoadException.h"

using namespace WallpaperEngine::FileSystem;
using namespace WallpaperEngine::FileSystem::Adapters;

ReadStreamSharedPtr DirectoryAdapter::open (const std::filesystem::path& path) const {
    auto finalpath = std::filesystem::canonical (this->basepath / path);

    if (finalpath.string ().find (this->basepath.string ()) != 0) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    const auto status = std::filesystem::status (finalpath);

    if (!std::filesystem::exists (finalpath)) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    if (!std::filesystem::is_regular_file (status)) {
	throw std::filesystem::filesystem_error ("Expected file but found a directory", path, std::error_code ());
    }

    return std::make_shared<std::ifstream> (finalpath);
}

bool DirectoryAdapter::exists (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (this->basepath / path);

	if (finalpath.string ().find (this->basepath.string ()) != 0) {
	    return false;
	}

	const auto status = std::filesystem::status (finalpath);

	if (!std::filesystem::exists (finalpath)) {
	    return false;
	}

	if (!std::filesystem::is_regular_file (status)) {
	    return false;
	}

	return true;
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

std::optional<std::filesystem::path> DirectoryAdapter::findIgnoringCase (const std::filesystem::path& path) const {
    // walk the path one component at a time: the exact name when it exists, else the one entry of that
    // directory whose name matches ignoring case (the first in iteration order if several do)
    std::filesystem::path found;

    try {
	for (const auto& component : path.relative_path ()) {
	    const auto name = component.string ();

	    if (name.empty () || name == ".") {
		continue;
	    }

	    if (name == "..") {
		return std::nullopt;
	    }

	    if (std::filesystem::exists (this->basepath / found / component)) {
		found /= component;
		continue;
	    }

	    std::optional<std::filesystem::path> match;

	    for (const auto& entry : std::filesystem::directory_iterator (this->basepath / found)) {
		if (equalsIgnoringCase (entry.path ().filename ().string (), name)) {
		    match = entry.path ().filename ();
		    break;
		}
	    }

	    if (!match.has_value ()) {
		return std::nullopt;
	    }

	    found /= *match;
	}
    } catch (std::filesystem::filesystem_error&) {
	return std::nullopt;
    }

    // same checks as a direct lookup: a regular file inside the base path
    if (found.empty () || !this->exists (found)) {
	return std::nullopt;
    }

    return found;
}

std::filesystem::path DirectoryAdapter::physicalPath (const std::filesystem::path& path) const {
    auto finalpath = std::filesystem::canonical (this->basepath / path);

    if (finalpath.string ().find (this->basepath.string ()) != 0) {
	throw std::filesystem::filesystem_error ("Cannot find file", path, std::error_code ());
    }

    return finalpath;
}

bool DirectoryFactory::handlesMountpoint (const std::filesystem::path& path) const {
    try {
	const auto finalpath = std::filesystem::canonical (path);
	const auto status = std::filesystem::status (finalpath);

	return std::filesystem::exists (finalpath) && std::filesystem::is_directory (status);
    } catch (std::filesystem::filesystem_error&) {
	return false;
    }
}

AdapterSharedPtr DirectoryFactory::create (const std::filesystem::path& path) const {
    auto finalpath = std::filesystem::canonical (path);
    const auto status = std::filesystem::status (finalpath);

    if (!std::filesystem::exists (finalpath)) {
	throw std::filesystem::filesystem_error ("Cannot find directory", path, std::error_code ());
    }

    if (!std::filesystem::is_directory (status)) {
	throw std::filesystem::filesystem_error ("Expected directory but found a file", path, std::error_code ());
    }

    return std::make_unique<DirectoryAdapter> (finalpath);
}