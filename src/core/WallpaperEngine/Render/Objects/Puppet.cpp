#include "Puppet.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

namespace WallpaperEngine::Render::Objects::Puppet {
namespace {
// the vertex format's components in file order and their sizes (GH 0x1402873f0 sums these; tables 0x1404c4da0 and
// 0x1404c4e10): position (vec3 or vec4), normal, tangent, blend indices, blend weights, texcoord 0..5 (2-4 floats
// each), colour
constexpr uint32_t kFormatBits[] = { 0x1,      0x10000,  0x2000000, 0x2,      0x4,      0x800000, 0x1000000,
				     0x8,      0x10,     0x20,      0x40,     0x80,     0x100,    0x200,
				     0x400,    0x800,    0x1000,    0x2000,   0x4000,   0x20000,  0x40000,
				     0x80000,  0x100000, 0x200000,  0x400000, 0x8000 };
constexpr uint32_t kFormatSizes[] = { 12, 16, 12, 12, 16, 16, 16, 8, 12, 16, 8, 12, 16,
				      8,  12, 16, 8,  12, 16, 8,  12, 16, 8, 12, 16, 16 };

struct Reader {
    const char* data;
    size_t size;
    size_t at = 0;

    void need (size_t bytes) const {
	if (at + bytes > size) {
	    throw std::runtime_error ("truncated at " + std::to_string (at));
	}
    }
    template <typename T> T get () {
	need (sizeof (T));
	T value;
	std::memcpy (&value, data + at, sizeof (T));
	at += sizeof (T);
	return value;
    }
    uint32_t u32 () { return get<uint32_t> (); }
    int32_t i32 () { return get<int32_t> (); }
    uint16_t u16 () { return get<uint16_t> (); }
    uint8_t u8 () { return get<uint8_t> (); }
    float f32 () { return get<float> (); }
    std::string str () {
	const void* end = at < size ? std::memchr (data + at, 0, size - at) : nullptr;
	if (end == nullptr) {
	    throw std::runtime_error ("unterminated string at " + std::to_string (at));
	}
	std::string value (data + at, static_cast<const char*> (end));
	at = static_cast<size_t> (static_cast<const char*> (end) - data) + 1;
	return value;
    }
    /** a u32 length and that many bytes; returns where they start */
    size_t blob (uint32_t& length) {
	length = u32 ();
	need (length);
	const size_t start = at;
	at += length;
	return start;
    }
    void skipBlob () {
	uint32_t length;
	blob (length);
    }
    glm::mat4 mat4 (size_t from) const {
	glm::mat4 m;
	std::memcpy (&m, data + from, sizeof (m));
	return m;
    }
};

/** the 4-digit version after a 4-letter tag, -1 when the tag differs */
int versionOf (const std::string& marker, const char* tag) {
    if (marker.size () != 8 || marker.compare (0, 4, tag) != 0) {
	return -1;
    }
    return std::atoi (marker.c_str () + 4);
}

void parseMeshes (Reader& r, int version, Model& out) {
    const uint32_t defaultFormat = r.u32 ();
    const uint32_t stringsPerMesh = r.u32 ();
    const uint32_t meshCount = r.u32 ();
    if (meshCount == 0 || meshCount > 64 || stringsPerMesh > 16) {
	throw std::runtime_error ("bad mesh header");
    }

    for (uint32_t m = 0; m < meshCount; m++) {
	Mesh mesh;
	for (uint32_t s = 0; s < stringsPerMesh; s++) {
	    std::string value = r.str ();
	    if (s == 0) {
		mesh.material = std::move (value);
	    }
	}
	const uint32_t flags = version >= 4 ? r.u32 () : 0;
	mesh.flags = flags;
	if (flags & 2) {
	    r.u32 (); // influences per vertex (≤ 4)
	}
	uint32_t format = defaultFormat;
	if (version >= 17) {
	    for (int i = 0; i < 6; i++) {
		r.f32 (); // bounds
	    }
	    format = r.u32 ();
	} else if (version >= 15) {
	    format = r.u32 ();
	}

	const size_t stride = vertexStride (format);
	if (stride == 0) {
	    throw std::runtime_error ("unsupported vertex format");
	}
	// component offsets
	size_t offsets[std::size (kFormatBits)] = {};
	size_t offset = 0;
	for (size_t i = 0; i < std::size (kFormatBits); i++) {
	    offsets[i] = offset;
	    if (format & kFormatBits[i]) {
		offset += kFormatSizes[i];
	    }
	}
	const bool hasPosition = (format & 0x1) || (format & 0x10000);
	const size_t positionAt = (format & 0x1) ? offsets[0] : offsets[1];
	const bool hasBones = (format & 0x800000) && (format & 0x1000000);
	const bool hasUv = format & (0x8 | 0x10 | 0x20);
	const size_t uvAt = (format & 0x8) ? offsets[7] : (format & 0x10) ? offsets[8] : offsets[9];
	if (!hasPosition) {
	    throw std::runtime_error ("vertex format without a position");
	}

	uint32_t vertexBytes, indexBytes;
	const size_t vertices = r.blob (vertexBytes);
	const size_t indices = r.blob (indexBytes);
	const size_t indexSize = (flags & 1) ? 4 : 2;
	if (vertexBytes % stride != 0 || indexBytes % (indexSize * 3) != 0) {
	    throw std::runtime_error ("vertex/index data not a whole number of records");
	}

	const size_t vertexCount = vertexBytes / stride;
	mesh.vertices.resize (vertexCount);
	for (size_t v = 0; v < vertexCount; v++) {
	    const char* base = r.data + vertices + v * stride;
	    Vertex& vertex = mesh.vertices[v];
	    std::memcpy (&vertex.position, base + positionAt, sizeof (glm::vec3));
	    if (format & 0x10000) {
		float w = 0.0f;
		std::memcpy (&w, base + positionAt + 12, sizeof (float));
		vertex.morph = w > 0.0f ? static_cast<uint32_t> (w) : 0;
	    }
	    vertex.uv = glm::vec2 (0.0f);
	    if (hasUv) {
		std::memcpy (&vertex.uv, base + uvAt, sizeof (glm::vec2));
	    }
	    if (format & 0x2) {
		std::memcpy (&vertex.normal, base + offsets[3], sizeof (glm::vec3));
	    }
	    if (format & 0x4) {
		std::memcpy (&vertex.tangent, base + offsets[4], sizeof (glm::vec4));
	    }
	    vertex.bones = glm::uvec4 (0);
	    vertex.weights = glm::vec4 (0.0f);
	    if (hasBones) {
		std::memcpy (&vertex.bones, base + offsets[5], sizeof (glm::uvec4));
		std::memcpy (&vertex.weights, base + offsets[6], sizeof (glm::vec4));
	    }
	}
	const size_t indexCount = indexBytes / indexSize;
	mesh.indices.resize (indexCount);
	for (size_t i = 0; i < indexCount; i++) {
	    uint32_t index = 0;
	    std::memcpy (&index, r.data + indices + i * indexSize, indexSize);
	    if (index >= vertexCount) {
		throw std::runtime_error ("index out of range");
	    }
	    mesh.indices[i] = index;
	}

	if (version >= 21) {
	    if (r.u8 ()) {
		r.u32 ();
		r.skipBlob ();
	    }
	    if (r.u8 ()) {
		r.skipBlob ();
	    }
	}
	if (version >= 23) {
	    const uint32_t groups = r.u32 ();
	    for (uint32_t g = 0; g < groups; g++) {
		r.need (8);
		r.at += 8;
		r.str ();
		r.u32 ();
		const uint32_t a = r.u32 ();
		r.need (size_t (a) * 4);
		r.at += size_t (a) * 4;
		const uint32_t b = r.u32 ();
		r.need (size_t (b) * 4);
		r.at += size_t (b) * 4;
	    }
	}
	out.meshes.push_back (std::move (mesh));
    }
}

// the extra per-skeleton counts MDLA's later versions repeat per animation (MDLS ≥ 2)
struct SkeletonCounts {
    uint32_t targets = 0; // u16 list of 0x80-byte records
    uint32_t constraints = 0; // u32 list
};

void parseSkeleton (Reader& r, int version, Model& out, SkeletonCounts& counts) {
    const uint32_t boneCount = r.u32 ();
    if (boneCount > 0x80) {
	throw std::runtime_error ("too many bones");
    }
    for (uint32_t b = 0; b < boneCount; b++) {
	Bone bone;
	bone.name = r.str ();
	bone.flags = r.u32 ();
	bone.parent = r.i32 ();
	if (bone.parent != -1 && (bone.parent < 0 || static_cast<uint32_t> (bone.parent) >= b)) {
	    throw std::runtime_error ("bone parent out of order");
	}
	uint32_t length;
	const size_t at = r.blob (length);
	float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	std::memcpy (m, r.data + at, std::min<size_t> (length, sizeof (m)));
	std::memcpy (&bone.local, m, sizeof (m));
	r.str (); // simulation settings (JSON)
	out.bones.push_back (std::move (bone));
    }
    if (version >= 2) {
	counts.targets = r.u16 ();
	for (uint32_t i = 0; i < counts.targets; i++) {
	    r.str ();
	    r.u32 ();
	    r.u32 ();
	    r.need (64);
	    r.at += 64;
	}
	if (r.u8 ()) {
	    // explicit matrices for the bones and the targets (the inverse binds WE recomputes anyway)
	    const size_t bytes = size_t (boneCount + counts.targets) * 64;
	    r.need (bytes);
	    r.at += bytes;
	}
	counts.constraints = r.u32 ();
	for (uint32_t i = 0; i < counts.constraints; i++) {
	    r.u32 ();
	    r.u32 ();
	    r.u32 ();
	    const uint32_t flags = version > 3 ? r.u32 () : 0;
	    if (flags & 2) {
		r.u32 ();
		r.f32 ();
	    }
	}
	// the rest of the section (IK chains, physics) is skipped by the section offset
    }
}

/** `count` float tracks (a u32 of flags, a blob of frames + 1 floats each) */
std::vector<std::vector<float>> floatTracks (Reader& r, uint32_t count, size_t samples) {
    std::vector<std::vector<float>> out (count);
    for (uint32_t i = 0; i < count; i++) {
	r.u32 ();
	uint32_t length;
	const size_t at = r.blob (length);
	if (length != samples * 4) {
	    throw std::runtime_error ("float track length isn't (frames + 1) · 4");
	}
	out[i].resize (samples);
	std::memcpy (out[i].data (), r.data + at, length);
    }
    return out;
}

/** MDMP0001 (GH 0x1402873f0): per mesh a u16 target count (≤ 0x100); with targets, the f32 scale and the u32 point
 *  count N; per target a u64 id, its name and N · 6 bytes of snorm16 xyz, then by the mesh's flags N · 6 normal bytes
 *  (0x400), N · 2 alpha bytes (0x1000) and a bone rule (0x2000: u32 bone, u32, f32, f32) */
void parseMorphs (Reader& r, Model& out) {
    const auto snorm = [] (int16_t v) { return std::max (static_cast<float> (v) / 32767.0f, -1.0f); };
    for (auto& mesh : out.meshes) {
	const uint16_t count = r.u16 ();
	if (count > 0x100) {
	    throw std::runtime_error ("too many morph targets");
	}
	if (count == 0) {
	    continue;
	}
	mesh.morphScale = r.f32 ();
	mesh.morphPoints = r.u32 ();
	for (uint16_t t = 0; t < count; t++) {
	    MorphTarget target;
	    target.id = r.get<uint64_t> ();
	    target.name = r.str ();
	    uint32_t length;
	    size_t at = r.blob (length);
	    if (length % 6 != 0) {
		throw std::runtime_error ("morph positions not 6 bytes a point");
	    }
	    // WE takes the smaller of the header's count and the blob's (and requires them equal)
	    mesh.morphPoints = std::min<uint32_t> (mesh.morphPoints, length / 6);
	    const auto read3 = [&] (size_t from, std::vector<glm::vec3>& into, float scale) {
		into.resize (mesh.morphPoints);
		for (uint32_t i = 0; i < mesh.morphPoints; i++) {
		    int16_t v[3];
		    std::memcpy (v, r.data + from + i * 6, sizeof (v));
		    into[i] = glm::vec3 (snorm (v[0]), snorm (v[1]), snorm (v[2])) * scale;
		}
	    };
	    read3 (at, target.positions, mesh.morphScale);
	    if (mesh.flags & 0x400) {
		at = r.blob (length);
		if (length != mesh.morphPoints * 6) {
		    throw std::runtime_error ("morph normals don't match the points");
		}
		read3 (at, target.normals, 1.0f);
	    }
	    if (mesh.flags & 0x1000) {
		at = r.blob (length);
		if (length != mesh.morphPoints * 2) {
		    throw std::runtime_error ("morph alphas don't match the points");
		}
		target.alpha.resize (mesh.morphPoints);
		for (uint32_t i = 0; i < mesh.morphPoints; i++) {
		    int16_t v;
		    std::memcpy (&v, r.data + at + i * 2, sizeof (v));
		    target.alpha[i] = snorm (v);
		}
	    }
	    if (mesh.flags & 0x2000) {
		target.modifierBone = static_cast<int> (r.u32 ());
		target.modifierMode = r.u32 ();
		target.modifierRange.x = r.f32 ();
		target.modifierRange.y = r.f32 ();
	    }
	    mesh.morphs.push_back (std::move (target));
	}
    }
}

void skipFloatTracks (Reader& r, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
	r.u32 ();
	r.skipBlob ();
    }
}

void parseAnimations (Reader& r, int version, const SkeletonCounts& counts, Model& out) {
    const uint32_t count = r.u32 ();
    if (count > 0x400) {
	throw std::runtime_error ("too many animations");
    }
    for (uint32_t a = 0; a < count; a++) {
	Animation animation;
	animation.id = r.u32 ();
	r.u32 ();
	animation.name = r.str ();
	const std::string mode = r.str ();
	animation.mode = mode == "mirror" ? Mode::Mirror : mode == "single" ? Mode::Single : Mode::Loop;
	animation.fps = r.f32 ();
	animation.frames = r.u32 ();
	const uint32_t flags = r.u32 ();
	const uint32_t tracks = r.u32 ();
	const size_t samples = size_t (animation.frames) + 1;
	for (uint32_t t = 0; t < tracks; t++) {
	    Track track;
	    track.flags = r.u32 ();
	    uint32_t length;
	    const size_t at = r.blob (length);
	    if (length != samples * 36) {
		throw std::runtime_error ("track length isn't (frames + 1) · 36");
	    }
	    track.keys.resize (samples);
	    for (size_t k = 0; k < samples; k++) {
		float f[9];
		std::memcpy (f, r.data + at + k * 36, sizeof (f));
		track.keys[k].position = glm::vec3 (f[0], f[1], f[2]);
		track.keys[k].rotation = glm::quat (glm::vec3 (f[3], f[4], f[5]));
		track.keys[k].scale = glm::vec3 (f[6], f[7], f[8]);
	    }
	    animation.tracks.push_back (std::move (track));
	}
	if (version > 1) {
	    skipFloatTracks (r, counts.targets);
	    skipFloatTracks (r, counts.constraints);
	}
	if (version > 2) {
	    const uint32_t curves = r.u32 ();
	    if (curves > 16) {
		throw std::runtime_error ("too many curves");
	    }
	    skipFloatTracks (r, curves);
	    if (r.u8 ()) {
		animation.boneAlpha = floatTracks (r, tracks, samples);
	    }
	}
	if (version > 3 && r.u8 ()) {
	    // blend-shape weights per mesh: flags (bit 0), a float (the mesh's morph scale again), a u16 count, then per
	    // track the target's index and frames + 1 floats
	    animation.shapeWeights.resize (out.meshes.size ());
	    for (size_t m = 0; m < out.meshes.size (); m++) {
		if (r.u32 () & 1) {
		    r.u32 ();
		    const uint16_t shapes = r.u16 ();
		    for (uint16_t s = 0; s < shapes; s++) {
			const uint16_t index = r.u16 ();
			uint32_t length;
			const size_t at = r.blob (length);
			if (length != samples * 4) {
			    throw std::runtime_error ("blend-shape track length isn't (frames + 1) · 4");
			}
			std::vector<float> weights (samples);
			std::memcpy (weights.data (), r.data + at, length);
			animation.shapeWeights[m].emplace_back (index, std::move (weights));
		    }
		}
	    }
	}
	if (version > 4) {
	    for (int i = 0; i < 6; i++) {
		r.u32 ();
	    }
	}
	if (version > 5 && r.u8 ()) {
	    skipFloatTracks (r, tracks);
	}
	if (flags & 1) {
	    animation.rootMotion = true;
	    r.need (18);
	    r.at += 18; // root motion
	}
	const uint32_t events = r.u32 ();
	if (events > 0x10000) {
	    throw std::runtime_error ("too many animation events");
	}
	for (uint32_t e = 0; e < events; e++) {
	    // a frame, then a name (Inferred from the layout: the reader takes a u32 and a string per event)
	    const uint32_t frame = r.u32 ();
	    animation.events.emplace_back (frame, r.str ());
	}
	if (animation.fps > 0.0f && animation.frames > 0) {
	    out.animations.push_back (std::move (animation));
	}
    }
}

void parseAttachments (Reader& r, Model& out) {
    const uint16_t count = r.u16 ();
    for (uint16_t i = 0; i < count; i++) {
	Attachment attachment;
	attachment.bone = r.u16 ();
	attachment.name = r.str ();
	r.need (64);
	attachment.matrix = r.mat4 (r.at);
	r.at += 64;
	out.attachments.push_back (std::move (attachment));
    }
}
} // namespace

size_t vertexStride (uint32_t format) {
    // bit 25 is rejected, and exactly one of the two position forms is required
    if ((format & 0x2000000) || ((format & 0x1) != 0) == ((format & 0x10000) != 0)) {
	return 0;
    }
    size_t stride = 0;
    for (size_t i = 0; i < std::size (kFormatBits); i++) {
	if (format & kFormatBits[i]) {
	    stride += kFormatSizes[i];
	}
    }
    return stride;
}

bool parse (const char* data, size_t size, Model& out, std::string& error) {
    out = Model ();
    Reader r { data, size };
    try {
	const int version = versionOf (r.str (), "MDLV");
	// WE takes MDLV up to 0023 (GH 0x1402873f0 compares with "MDLV0023")
	if (version < 1 || version > 23) {
	    error = "not an MDLV file (or newer than MDLV0023)";
	    return false;
	}
	out.version = version;
	parseMeshes (r, version, out);

	// then the sections: a tag, the offset of the section's end, its body
	SkeletonCounts counts;
	while (r.at + 9 <= size) {
	    const std::string marker = r.str ();
	    if (marker.size () != 8 || marker.compare (0, 2, "MD") != 0) {
		break;
	    }
	    const uint32_t end = r.u32 ();
	    if (end < r.at || end > size) {
		throw std::runtime_error ("bad section end for " + marker);
	    }
	    if (const int v = versionOf (marker, "MDLS"); v >= 1) {
		parseSkeleton (r, v, out, counts);
	    } else if (const int v = versionOf (marker, "MDLA"); v >= 1) {
		parseAnimations (r, v, counts, out);
	    } else if (versionOf (marker, "MDAT") >= 1) {
		parseAttachments (r, out);
	    } else if (versionOf (marker, "MDMP") >= 1) {
		parseMorphs (r, out);
	    }
	    r.at = end;
	}
    } catch (const std::exception& ex) {
	error = ex.what ();
	return !out.meshes.empty ();
    }
    // a vertex's morph point must be one of its mesh's (else no target moves it)
    for (auto& mesh : out.meshes) {
	for (auto& vertex : mesh.vertices) {
	    if (vertex.morph > mesh.morphPoints) {
		vertex.morph = 0;
	    }
	}
    }
    // tracks and attachments must match the skeleton
    const auto bones = out.bones.size ();
    out.animations.erase (std::remove_if (out.animations.begin (), out.animations.end (),
					   [bones] (const Animation& a) { return a.tracks.size () != bones; }),
			  out.animations.end ());
    out.attachments.erase (std::remove_if (out.attachments.begin (), out.attachments.end (),
					    [bones] (const Attachment& a) {
						return a.bone < 0 || static_cast<size_t> (a.bone) >= bones;
					    }),
			   out.attachments.end ());
    return true;
}

float length (const Animation& animation) {
    return animation.fps > 0.0f ? static_cast<float> (animation.frames) / animation.fps : 0.0f;
}

void advance (const Animation& animation, Clock& clock, float dt) { advance (animation, clock, dt, animation.mode); }

void advance (const Animation& animation, Clock& clock, float dt, Mode mode) {
    const float len = length (animation);
    if (len <= 0.0f || clock.finished) {
	return;
    }
    switch (mode) {
	case Mode::Single:
	    clock.time += dt;
	    if (clock.time >= len) {
		clock.time = len;
		clock.finished = true;
		clock.ends++;
	    }
	    break;
	case Mode::Mirror:
	    clock.time += clock.reversed ? -dt : dt;
	    if (!clock.reversed && clock.time >= len) {
		clock.time = len - std::fmod (clock.time, len);
		clock.reversed = true;
		clock.ends++;
	    } else if (clock.reversed && clock.time <= 0.0f) {
		clock.time = std::fabs (std::fmod (clock.time, len));
		clock.reversed = false;
	    }
	    break;
	case Mode::Loop:
	    clock.time += dt;
	    if (clock.time < 0.0f) {
		clock.time = std::fmod (clock.time + len, len);
	    }
	    if (clock.time >= len) {
		clock.time = std::fmod (clock.time, len);
		clock.ends++;
	    }
	    break;
    }
}

std::vector<std::pair<std::string, int>> passedEvents (const Animation& animation, float before, const Clock& after,
						       uint32_t endsBefore, bool reversedBefore) {
    std::vector<std::pair<std::string, int>> out;
    if (animation.events.empty () || animation.fps <= 0.0f) {
	return out;
    }
    // frame positions in frames (time · fps); an event at f is passed when the clock crosses f
    const double from = before < 0.0f ? -1e-6 : static_cast<double> (before) * animation.fps;
    const double to = static_cast<double> (after.time) * animation.fps;
    const double last = static_cast<double> (animation.frames);
    const auto add = [&] (double lo, double hi, bool includeLo) {
	for (const auto& [frame, name] : animation.events) {
	    const double f = frame;
	    if ((includeLo ? f >= lo : f > lo) && f <= hi) {
		out.emplace_back (name, static_cast<int> (frame));
	    }
	}
    };
    const bool wrapped = after.ends != endsBefore;
    if (reversedBefore || after.reversed) {
	// a mirror going back (or turning at the end): the frames between, either way
	if (wrapped && !reversedBefore) {
	    add (from, last, false);
	    add (to, last, true);
	} else {
	    add (std::min (from, to), std::max (from, to), true);
	}
    } else if (wrapped && after.time < before) {
	add (from, last, false); // a loop: up to the end, then from the start
	add (-1.0, to, false);
    } else {
	add (from, to, false);
    }
    return out;
}

float layerWeight (const Animation& animation, const Clock& clock, float blend, bool& blendIn, bool blendOut,
		   float blendTime) {
    constexpr float kEpsilon = 1.1920928955078125e-07f;
    const float len = length (animation);
    float weight = blend;
    if (blendIn) {
	float fade = 1.0f;
	if (std::min (len, blendTime) > kEpsilon) {
	    fade = std::min (clock.time / std::min (len * 0.5f, blendTime), 1.0f);
	}
	weight = fade * blend;
	if (animation.mode != Mode::Single && fade >= 1.0f) {
	    blendIn = false;
	}
    }
    if (blendOut && std::min (len, blendTime) > kEpsilon) {
	weight *= std::min ((len - clock.time) / std::min (len * 0.5f, blendTime), 1.0f);
    }
    return weight;
}

std::vector<Key> bindPose (const Model& model) {
    std::vector<Key> pose (model.bones.size ());
    for (size_t b = 0; b < model.bones.size (); b++) {
	glm::vec3 skew;
	glm::vec4 perspective;
	glm::quat rotation;
	if (!glm::decompose (model.bones[b].local, pose[b].scale, rotation, pose[b].position, skew, perspective)) {
	    pose[b] = Key ();
	    pose[b].position = glm::vec3 (model.bones[b].local[3]);
	    continue;
	}
	pose[b].rotation = rotation;
    }
    return pose;
}

namespace {
glm::quat nlerp (const glm::quat& from, const glm::quat& to, float t) {
    const float sign = glm::dot (from, to) < 0.0f ? -1.0f : 1.0f;
    const glm::quat q = from * (1.0f - t) + to * (t * sign);
    const float len = glm::length (q);
    return len > 0.0f ? q / len : from;
}
} // namespace

void mix (std::vector<Key>& pose, const std::vector<Key>& bind, const Animation& animation, float time, float weight,
	  bool additive) {
    // GH 0x1401a00a0: a = clamp (⌊t / frame⌋, 0, frames − 1), b = min (a + 1, frames), the fraction within the frame
    const float frame = 1.0f / animation.fps;
    const int frames = static_cast<int> (animation.frames);
    const int a = std::clamp (static_cast<int> (time / frame), 0, std::max (frames - 1, 0));
    const int b = std::min (a + 1, frames);
    const float f = std::fmod (time, frame) / frame;

    for (size_t i = 0; i < pose.size () && i < animation.tracks.size (); i++) {
	const Track& track = animation.tracks[i];
	if (track.flags & 1 || track.keys.size () <= static_cast<size_t> (b)) {
	    continue;
	}
	const Key& ka = track.keys[a];
	const Key& kb = track.keys[b];
	Key sample;
	sample.position = ka.position * (1.0f - f) + kb.position * f;
	sample.rotation = nlerp (ka.rotation, kb.rotation, f);
	sample.scale = ka.scale * (1.0f - f) + kb.scale * f;

	Key& out = pose[i];
	if (additive) {
	    out.position += (sample.position - bind[i].position) * weight;
	    out.scale += (sample.scale - bind[i].scale) * weight;
	    const glm::quat delta = sample.rotation * glm::conjugate (bind[i].rotation);
	    out.rotation = glm::normalize (nlerp (glm::quat (1.0f, 0.0f, 0.0f, 0.0f), delta, weight) * out.rotation);
	} else {
	    out.position = sample.position * weight + out.position * (1.0f - weight);
	    out.scale = sample.scale * weight + out.scale * (1.0f - weight);
	    out.rotation = nlerp (out.rotation, sample.rotation, weight);
	}
    }
}

void worldMatrices (const Model& model, const std::vector<Key>& pose, std::vector<glm::mat4>& world) {
    world.resize (model.bones.size ());
    for (size_t b = 0; b < model.bones.size (); b++) {
	const Key& k = pose[b];
	const glm::mat4 local
	    = glm::translate (glm::mat4 (1.0f), k.position) * glm::mat4_cast (k.rotation) * glm::scale (glm::mat4 (1.0f), k.scale);
	const int parent = model.bones[b].parent;
	world[b] = parent >= 0 ? world[parent] * local : local;
    }
}

void bindWorldMatrices (const Model& model, std::vector<glm::mat4>& world) {
    world.resize (model.bones.size ());
    for (size_t b = 0; b < model.bones.size (); b++) {
	const int parent = model.bones[b].parent;
	world[b] = parent >= 0 ? world[parent] * model.bones[b].local : model.bones[b].local;
    }
}

float sampleFloat (const Animation& animation, const std::vector<float>& track, float time) {
    if (track.empty () || animation.fps <= 0.0f) {
	return 0.0f;
    }
    const float frame = 1.0f / animation.fps;
    const int frames = static_cast<int> (animation.frames);
    const int a = std::clamp (static_cast<int> (time / frame), 0, std::max (frames - 1, 0));
    const int b = std::min (a + 1, frames);
    const float f = std::fmod (time, frame) / frame;
    if (static_cast<size_t> (b) >= track.size ()) {
	return track.back ();
    }
    return track[a] * (1.0f - f) + track[b] * f;
}

void mixExtras (std::vector<float>& alpha, std::vector<std::vector<float>>& weights, const Animation& animation,
		float time, float weight, bool additive) {
    for (size_t b = 0; b < alpha.size () && b < animation.boneAlpha.size (); b++) {
	if (b < animation.tracks.size () && (animation.tracks[b].flags & 1)) {
	    continue;
	}
	const float sample = sampleFloat (animation, animation.boneAlpha[b], time);
	alpha[b] = additive ? alpha[b] + (sample - 1.0f) * weight : alpha[b] * (1.0f - weight) + sample * weight;
    }
    for (size_t m = 0; m < weights.size () && m < animation.shapeWeights.size (); m++) {
	for (const auto& [index, track] : animation.shapeWeights[m]) {
	    if (index >= weights[m].size ()) {
		continue;
	    }
	    const float sample = sampleFloat (animation, track, time);
	    float& w = weights[m][index];
	    w = additive ? w + sample * weight : w * (1.0f - weight) + sample * weight;
	}
    }
}

glm::vec3 morph (const Mesh& mesh, const Vertex& vertex, const std::vector<float>& weights, float& alpha) {
    alpha = 1.0f;
    if (vertex.morph == 0 || mesh.morphs.empty ()) {
	return vertex.position;
    }
    const uint32_t point = vertex.morph - 1;
    glm::vec3 delta (0.0f);
    for (size_t t = 0; t < mesh.morphs.size () && t < weights.size (); t++) {
	const float w = weights[t];
	if (w == 0.0f) {
	    continue;
	}
	const auto& target = mesh.morphs[t];
	if (point < target.positions.size ()) {
	    delta += target.positions[point] * w;
	}
	if (point < target.alpha.size ()) {
	    alpha *= target.alpha[point] * w + (1.0f - w);
	}
    }
    return vertex.position + delta;
}

glm::vec3 skin (const Vertex& vertex, const std::vector<glm::mat4>& skinning) {
    return skin (vertex.position, vertex, skinning);
}

glm::vec3 skin (const glm::vec3& position, const Vertex& vertex, const std::vector<glm::mat4>& skinning) {
    const glm::vec4 p (position, 1.0f);
    glm::vec3 result (0.0f);
    float total = 0.0f;
    for (int c = 0; c < 4; c++) {
	const float w = vertex.weights[c];
	const uint32_t bone = vertex.bones[c];
	if (w == 0.0f || bone >= skinning.size ()) {
	    continue;
	}
	result += w * glm::vec3 (skinning[bone] * p);
	total += w;
    }
    return total == 0.0f ? position : result;
}
} // namespace WallpaperEngine::Render::Objects::Puppet
