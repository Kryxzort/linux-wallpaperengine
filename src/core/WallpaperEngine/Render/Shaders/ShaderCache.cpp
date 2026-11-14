#include "ShaderCache.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lz4.h>

namespace WallpaperEngine::Render::Shaders::ShaderCache {
namespace {
// the last byte is the entry format's version: bump it when the layout below changes
constexpr char MAGIC[8] = { 'W', 'P', 'G', 'L', 'S', 'L', '\0', '2' };
// all of an entry's texts together; past this it is no shader work: refused rather than allocated
constexpr uint64_t MAX_TOTAL = 256ull << 20;
constexpr uint32_t MAX_TEXTS = 16;

/** Followed by u32 sizes[keys + values], the build id, the kind, then the LZ4 block of the keys and values in order */
struct Header {
    char magic[8];
    uint32_t idSize;
    uint32_t kindSize;
    uint32_t keys;
    uint32_t values;
    uint32_t packed;
};

struct Search {
    uintptr_t address;
    std::string id;
};

/** dl_iterate_phdr's visitor: the build id note of the module holding `address` */
int visit (dl_phdr_info* info, size_t, void* data) {
    auto* search = static_cast<Search*> (data);
    bool holds = false;
    for (int i = 0; i < info->dlpi_phnum && !holds; i++) {
	const auto& ph = info->dlpi_phdr[i];
	const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
	holds = ph.p_type == PT_LOAD && search->address >= start && search->address < start + ph.p_memsz;
    }
    if (!holds) {
	return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; i++) {
	const auto& ph = info->dlpi_phdr[i];
	if (ph.p_type != PT_NOTE) {
	    continue;
	}
	const uintptr_t align = ph.p_align == 8 ? 8 : 4;
	const auto pad = [align] (uintptr_t n) { return (n + align - 1) & ~(align - 1); };
	const char* p = reinterpret_cast<const char*> (info->dlpi_addr + ph.p_vaddr);
	const char* end = p + ph.p_memsz;
	while (p + sizeof (ElfW (Nhdr)) <= end) {
	    const auto* note = reinterpret_cast<const ElfW (Nhdr)*> (p);
	    const char* name = p + sizeof (ElfW (Nhdr));
	    const char* desc = name + pad (note->n_namesz);
	    const char* next = desc + pad (note->n_descsz);
	    if (next > end) {
		break;
	    }
	    if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && std::memcmp (name, "GNU", 4) == 0) {
		static const char* digits = "0123456789abcdef";
		for (uint32_t b = 0; b < note->n_descsz; b++) {
		    const auto byte = static_cast<unsigned char> (desc[b]);
		    search->id += digits[byte >> 4];
		    search->id += digits[byte & 15];
		}
		return 1;
	    }
	    p = next;
	}
    }
    return 1;
}

/** FNV-1a: only names the file; a hit is decided by comparing the texts themselves */
uint64_t fnv (uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < size; i++) {
	hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    return hash;
}

std::filesystem::path entryPath (
    const std::filesystem::path& dir, const std::string& id, const std::string& kind, const Texts& keys
) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto* text : { &id, &kind }) {
	const uint64_t size = text->size ();
	hash = fnv (fnv (hash, &size, sizeof (size)), text->data (), text->size ());
    }
    for (const auto& key : keys) {
	const uint64_t size = key.size ();
	hash = fnv (fnv (hash, &size, sizeof (size)), key.data (), key.size ());
    }
    char name[24];
    snprintf (name, sizeof (name), "%016llx.", static_cast<unsigned long long> (hash));
    return dir / (name + kind);
}

bool readAll (int fd, char* data, size_t size) {
    while (size > 0) {
	const ssize_t n = ::read (fd, data, size);
	if (n <= 0) {
	    return false;
	}
	data += n;
	size -= static_cast<size_t> (n);
    }
    return true;
}

bool writeAll (int fd, const char* data, size_t size) {
    while (size > 0) {
	const ssize_t n = ::write (fd, data, size);
	if (n <= 0) {
	    return false;
	}
	data += n;
	size -= static_cast<size_t> (n);
    }
    return true;
}
} // namespace

const std::string& buildId () noexcept {
    static const std::string id = [] {
	Search search { reinterpret_cast<uintptr_t> (&visit), "" };
	dl_iterate_phdr (visit, &search);
	return search.id;
    }();
    return id;
}

std::optional<Texts> load (const std::filesystem::path& dir, const std::string& kind, const Texts& keys) noexcept try {
    const auto& id = buildId ();
    if (id.empty ()) {
	return std::nullopt;
    }
    const int fd = ::open (entryPath (dir, id, kind, keys).c_str (), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
	return std::nullopt;
    }
    struct stat st {};
    std::string file;
    const bool read = ::fstat (fd, &st) == 0 && st.st_size >= static_cast<off_t> (sizeof (Header))
	&& static_cast<uint64_t> (st.st_size) < 2 * MAX_TOTAL
	&& (file.resize (static_cast<size_t> (st.st_size)), readAll (fd, file.data (), file.size ()));
    ::close (fd);
    if (!read) {
	return std::nullopt;
    }
    Header header;
    std::memcpy (&header, file.data (), sizeof (header));
    if (std::memcmp (header.magic, MAGIC, sizeof (MAGIC)) != 0 || header.keys != keys.size ()
	|| header.values == 0 || header.values > MAX_TEXTS || header.idSize != id.size () || header.kindSize != kind.size ()) {
	return std::nullopt;
    }
    const size_t count = header.keys + header.values;
    const size_t textsAt = sizeof (header) + count * sizeof (uint32_t);
    const size_t packedAt = textsAt + id.size () + kind.size ();
    if (file.size () < packedAt || header.packed != file.size () - packedAt
	|| file.compare (textsAt, id.size (), id) != 0 || file.compare (textsAt + id.size (), kind.size (), kind) != 0) {
	return std::nullopt;
    }
    std::vector<uint32_t> sizes (count);
    std::memcpy (sizes.data (), file.data () + sizeof (header), count * sizeof (uint32_t));
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
	total += sizes[i];
	if (i < keys.size () && sizes[i] != keys[i].size ()) {
	    return std::nullopt;
	}
    }
    if (total > MAX_TOTAL) {
	return std::nullopt;
    }
    std::string plain (total, '\0');
    const int unpacked = LZ4_decompress_safe (
	file.data () + packedAt, plain.data (), static_cast<int> (header.packed), static_cast<int> (total)
    );
    if (unpacked < 0 || static_cast<uint64_t> (unpacked) != total) {
	return std::nullopt;
    }
    size_t at = 0;
    for (const auto& key : keys) {
	if (plain.compare (at, key.size (), key) != 0) {
	    return std::nullopt;
	}
	at += key.size ();
    }
    Texts values;
    for (size_t i = keys.size (); i < count; at += sizes[i], i++) {
	values.push_back (plain.substr (at, sizes[i]));
    }
    return values;
} catch (...) {
    return std::nullopt;
}

void store (const std::filesystem::path& dir, const std::string& kind, const Texts& keys, const Texts& values) noexcept try {
    const auto& id = buildId ();
    if (id.empty () || values.empty () || keys.size () + values.size () > MAX_TEXTS) {
	return;
    }
    std::string plain;
    std::vector<uint32_t> sizes;
    for (const auto* texts : { &keys, &values }) {
	for (const auto& text : *texts) {
	    if (plain.size () + text.size () > MAX_TOTAL) {
		return;
	    }
	    plain += text;
	    sizes.push_back (static_cast<uint32_t> (text.size ()));
	}
    }
    const int bound = LZ4_compressBound (static_cast<int> (plain.size ()));
    if (bound <= 0) {
	return;
    }
    const size_t packedAt = sizeof (Header) + sizes.size () * sizeof (uint32_t) + id.size () + kind.size ();
    std::string file (packedAt + static_cast<size_t> (bound), '\0');
    const int packed = LZ4_compress_default (plain.data (), file.data () + packedAt, static_cast<int> (plain.size ()), bound);
    if (packed <= 0) {
	return;
    }
    Header header {};
    std::memcpy (header.magic, MAGIC, sizeof (MAGIC));
    header.idSize = static_cast<uint32_t> (id.size ());
    header.kindSize = static_cast<uint32_t> (kind.size ());
    header.keys = static_cast<uint32_t> (keys.size ());
    header.values = static_cast<uint32_t> (values.size ());
    header.packed = static_cast<uint32_t> (packed);
    char* out = file.data ();
    std::memcpy (out, &header, sizeof (header));
    out += sizeof (header);
    std::memcpy (out, sizes.data (), sizes.size () * sizeof (uint32_t));
    out += sizes.size () * sizeof (uint32_t);
    std::memcpy (out, id.data (), id.size ());
    std::memcpy (out + id.size (), kind.data (), kind.size ());
    file.resize (packedAt + static_cast<size_t> (packed));

    // a name no other writer uses (workers of other scenes may store the same entry at once); the rename makes the
    // entry appear whole or not at all
    static std::atomic<unsigned> serial { 0 };
    const auto path = entryPath (dir, id, kind, keys);
    const auto temporary = path.string () + "." + std::to_string (::getpid ()) + "." + std::to_string (serial++) + ".tmp";
    const int fd = ::open (temporary.c_str (), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
	return;
    }
    const bool written = writeAll (fd, file.data (), file.size ());
    if (::close (fd) != 0 || !written || ::rename (temporary.c_str (), path.c_str ()) != 0) {
	::unlink (temporary.c_str ());
    }
} catch (...) {
}
} // namespace WallpaperEngine::Render::Shaders::ShaderCache
