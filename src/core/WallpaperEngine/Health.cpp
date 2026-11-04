#include "Health.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace WallpaperEngine::Health {
namespace {
std::atomic<bool> g_enabled { false };
std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_path;
std::unordered_set<std::string> g_written;
size_t g_lines = 0;
/** a runaway (a script erroring with a new message every frame) can't fill the disk */
constexpr size_t kMaxLines = 50000;

thread_local std::vector<std::pair<std::string, nlohmann::json>> t_scopes;
thread_local Capture* t_capture = nullptr;
} // namespace

void configure () noexcept {
    std::lock_guard lock (g_mutex);
    const char* env = std::getenv ("WPENGINE_HEALTH");
    const std::string path = env != nullptr ? env : "";

    if (path == g_path && (g_file != nullptr || path.empty ())) {
	return;
    }
    if (g_file != nullptr) {
	std::fclose (g_file);
	g_file = nullptr;
    }
    g_written.clear ();
    g_lines = 0;
    g_path = path;
    if (!path.empty ()) {
	g_file = std::fopen (path.c_str (), "a");
    }
    g_enabled.store (g_file != nullptr, std::memory_order_relaxed);
}

bool enabled () noexcept { return g_enabled.load (std::memory_order_relaxed) || t_capture != nullptr; }

Capture::Capture () noexcept : m_previous (t_capture) { t_capture = this; }

Capture::~Capture () { t_capture = m_previous; }

void report (const char* event, nlohmann::json fields) noexcept {
    if (!enabled ()) {
	return;
    }
    try {
	nlohmann::json record = nlohmann::json::object ();
	record["ev"] = event;
	for (const auto& [key, value] : t_scopes) {
	    record[key] = value;
	}
	if (fields.is_object ()) {
	    for (auto& [key, value] : fields.items ()) {
		record[key] = value;
	    }
	}
	// invalid UTF-8 in a message (a font name, a shader log) is replaced, not thrown
	std::string line = record.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace);
	if (t_capture != nullptr) {
	    t_capture->m_records.push_back (nlohmann::json::parse (line));
	}

	std::lock_guard lock (g_mutex);
	if (g_file == nullptr || g_lines >= kMaxLines || !g_written.insert (line).second) {
	    return;
	}
	g_lines++;
	line += '\n';
	std::fwrite (line.data (), 1, line.size (), g_file);
	std::fflush (g_file);
    } catch (...) {
	// a health record never breaks the renderer
    }
}

Scope::Scope (const char* key, int value) noexcept {
    if (enabled ()) {
	try {
	    t_scopes.emplace_back (key, value);
	    m_pushed = true;
	} catch (...) {
	}
    }
}

Scope::Scope (const char* key, const std::string& value) noexcept {
    if (enabled ()) {
	try {
	    t_scopes.emplace_back (key, value);
	    m_pushed = true;
	} catch (...) {
	}
    }
}

Scope::~Scope () {
    if (m_pushed && !t_scopes.empty ()) {
	t_scopes.pop_back ();
    }
}

std::string brief (const std::string& message, size_t max) {
    std::string out = message.substr (0, message.find ("Compiled source code:"));
    while (!out.empty () && (out.back () == '\n' || out.back () == ' ')) {
	out.pop_back ();
    }
    if (out.size () > max) {
	out = out.substr (0, max) + "…";
    }
    return out;
}
} // namespace WallpaperEngine::Health
