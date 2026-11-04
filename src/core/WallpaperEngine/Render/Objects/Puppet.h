#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

/**
 * Puppet warp: WE's MDL puppet files (MDLV mesh, MDLS skeleton, MDLA animations, MDAT attachments) and the
 * animation-layer mixing that poses them. Decoded from wallpaper64.exe 2.8.485:
 *  - the loader GH 0x1402873f0 (sections, vertex layout, bones, tracks, attachments);
 *  - the layer clock GH 0x1401d4d00 / 0x1401d6a00 and its frame lookup GH 0x1401a00a0;
 *  - the layer weight GH 0x140291f60 and the per-frame mixing GH 0x140228470 with its normal blend GH 0x140223b90
 *    and additive blend GH 0x140224390;
 *  - the bind world matrices GH 0x1402261c0.
 * Everything here is in the mesh's own space: x right, y up, the image's centre at 0, in the image's pixels.
 */
namespace WallpaperEngine::Render::Objects::Puppet {
struct Vertex {
    glm::vec3 position;
    glm::vec2 uv;
    glm::uvec4 bones;
    glm::vec4 weights;
    /** format bit 0x2 (static meshes light with it; 0 0 1 when absent) */
    glm::vec3 normal = glm::vec3 (0.0f, 0.0f, 1.0f);
    /** format bit 0x4: xyz and the bitangent's sign in w (1 0 0 1 when absent) */
    glm::vec4 tangent = glm::vec4 (1.0f, 0.0f, 0.0f, 1.0f);
    /**
     * the vec4 position's w (format 0x10000): 1 + the vertex's point in its mesh's morph targets, 0 for a vertex no
     * target moves (genericimage4's MORPHING reads a_PositionVec4.w)
     */
    uint32_t morph = 0;
};

/**
 * A blend shape (MDMP0001, GH 0x1402873f0): per point (a vertex's `morph` − 1) a position delta, read as snorm16
 * × the mesh's morph scale (2.8.485's morph_vertex_v1.h: DecodeMorphSnorm16, × g_MorphPositionScale), and, with the
 * mesh's flag 0x1000, an alpha (snorm16, the morph map's w: v_VertexAlpha mixes it in by the weight)
 */
struct MorphTarget {
    uint64_t id = 0;
    std::string name;
    std::vector<glm::vec3> positions;
    /** with mesh flag 0x400 */
    std::vector<glm::vec3> normals;
    /** with mesh flag 0x1000, else empty (1) */
    std::vector<float> alpha;
    /** with mesh flag 0x2000 (MORPHING_MODIFIERS): a bone and its rule (u32, f32, f32): parsed, not applied */
    int modifierBone = -1;
    uint32_t modifierMode = 0;
    glm::vec2 modifierRange = glm::vec2 (0.0f);
};

struct Mesh {
    std::string material;
    /** the mesh's flags (v4+): 1 32-bit indices, 2 an influence count, 0x400 / 0x1000 / 0x2000 morph normals / alpha /
     *  modifiers */
    uint32_t flags = 0;
    /** MDMP: the snorm16 deltas' scale and the targets (points: their delta count) */
    float morphScale = 0.0f;
    uint32_t morphPoints = 0;
    std::vector<MorphTarget> morphs;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct Bone {
    std::string name;
    uint32_t flags = 0;
    int parent = -1;
    /** the bind transform relative to the parent */
    glm::mat4 local = glm::mat4 (1.0f);
};

/** one sample of a bone: WE converts the stored Euler angles to a quaternion at load (R = Rz · Ry · Rx) */
struct Key {
    glm::vec3 position = glm::vec3 (0.0f);
    glm::quat rotation = glm::quat (1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3 (1.0f);
};

struct Track {
    /** bit 0: the bone isn't animated by this animation (it keeps the pose under it) */
    uint32_t flags = 0;
    /** frames + 1 samples */
    std::vector<Key> keys;
};

enum class Mode { Loop, Mirror, Single };

struct Animation {
    uint32_t id = 0;
    std::string name;
    Mode mode = Mode::Loop;
    float fps = 30.0f;
    uint32_t frames = 0;
    std::vector<Track> tracks;
    /**
     * MDLA v3+: the bones' alpha tracks (a flag byte, then per bone its flags and frames + 1 floats; empty when the
     * flag is 0). genericimage4's SKINNING_ALPHA: v_VertexAlpha = saturate (Σ weight · alpha[bone])
     */
    std::vector<std::vector<float>> boneAlpha;
    /**
     * MDLA v4+: blend-shape weight tracks, per mesh (flags bit 0, a float, a u16 count), per track the target's index
     * and frames + 1 weights
     */
    std::vector<std::vector<std::pair<uint16_t, std::vector<float>>>> shapeWeights;
    /** flags bit 0: the root-motion record (18 bytes, not decoded) */
    bool rootMotion = false;
    /** the animation's events (MDLA's last list: a frame, a name): SceneScript's animationEvent on the layer */
    std::vector<std::pair<uint32_t, std::string>> events;
};


struct Attachment {
    int bone = -1;
    std::string name;
    /** relative to the bone */
    glm::mat4 matrix = glm::mat4 (1.0f);
};

struct Model {
    int version = 0;
    std::vector<Mesh> meshes;
    std::vector<Bone> bones;
    std::vector<Animation> animations;
    std::vector<Attachment> attachments;
};

/** The size in bytes of a vertex of this MDLV format word, 0 when WE rejects it (GH 0x1402873f0, tables 0x1404c4da0) */
size_t vertexStride (uint32_t format);

/** Parses a whole .mdl file. `error` says why it failed */
bool parse (const char* data, size_t size, Model& out, std::string& error);

/** A layer's clock (GH 0x1401d4d00): time in seconds within [0, length] */
struct Clock {
    float time = 0.0f;
    bool reversed = false;
    bool finished = false;
    /** how often the animation reached its end (a loop's wrap, a mirror's turn at the end, a single's finish) */
    uint32_t ends = 0;
};

/** The animation's length in seconds: frames / fps */
float length (const Animation& animation);

/** Advances the clock by dt (already × the layer's rate): loop wraps, mirror bounces, single stops (GH 0x1401d6a00) */
void advance (const Animation& animation, Clock& clock, float dt);
/** advance, in `mode` instead of the animation's own (SceneScript's playSingleAnimation plays one once) */
void advance (const Animation& animation, Clock& clock, float dt, Mode mode);

/**
 * The events of `animation` the clock passed going from `before` to `after` (`ends` the clock's ends counter before
 * the step: a loop that wrapped passed its end and its start). Frame f is at f / fps; the first step (`before` < 0)
 * includes frame 0
 */
std::vector<std::pair<std::string, int>> passedEvents (const Animation& animation, float before, const Clock& after,
						       uint32_t endsBefore, bool reversedBefore);

/**
 * The layer's weight (GH 0x140291f60): blend, × the blend-in ramp t / min (length / 2, blendtime) while `blendIn`
 * (cleared once it reaches 1, except in a single animation) and × the blend-out ramp (length − t) / that
 */
float layerWeight (const Animation& animation, const Clock& clock, float blend, bool& blendIn, bool blendOut,
                   float blendTime);

/** The bind pose: every bone's local bind matrix, decomposed */
std::vector<Key> bindPose (const Model& model);

/**
 * Mixes one layer into `pose` (GH 0x140223b90 / 0x140224390). The animation is sampled between frames
 * a = ⌊t · fps⌋ and a + 1 (nlerp), then:
 *  - normal: pose = lerp (pose, sample, weight) (rotation nlerp on the shorter arc);
 *  - additive: position += (sample − bind) · weight; rotation = nlerp (1, sample · bind⁻¹, weight) · rotation;
 *    scale += (sample − bind) · weight.
 * A track with flag bit 0 leaves its bone alone
 */
void mix (std::vector<Key>& pose, const std::vector<Key>& bind, const Animation& animation, float time, float weight,
          bool additive);

/** World matrices of a pose: world = parent world · local */
void worldMatrices (const Model& model, const std::vector<Key>& pose, std::vector<glm::mat4>& world);

/** The bind world matrices (GH 0x1402261c0) */
void bindWorldMatrices (const Model& model, std::vector<glm::mat4>& world);

/** A float track's value at `time` (frames a, b and the fraction as `mix`) */
float sampleFloat (const Animation& animation, const std::vector<float>& track, float time);

/**
 * Mixes one layer's bone alphas and blend-shape weights into `alpha` (per bone, from 1) and `weights` (per mesh,
 * per target, from 0): normal lerps toward the sample by the layer's weight, additive adds (sample − rest) × weight.
 * **Inferred**: WE's blend for these (the mixer GH 0x140228470 wasn't read that far); the bones' rule is
 * the position's
 */
void mixExtras (std::vector<float>& alpha, std::vector<std::vector<float>>& weights, const Animation& animation,
		float time, float weight, bool additive);

/**
 * A morphed position: position + scale · Σ weightₜ · deltaₜ[point] over the mesh's targets (genericimage4's
 * MORPHING before SKINNING), and its alpha Π (alphaₜ · weightₜ + 1 − weightₜ)
 */
glm::vec3 morph (const Mesh& mesh, const Vertex& vertex, const std::vector<float>& weights, float& alpha);

/** Skins `position` (the vertex's, morphed) by the vertex's bones and weights, as skin () */
glm::vec3 skin (const glm::vec3& position, const Vertex& vertex, const std::vector<glm::mat4>& skinning);

/** Skins a vertex: Σ weightᵢ · world[boneᵢ] · bindWorld⁻¹[boneᵢ] · position; weights summing to 0 keep it */
glm::vec3 skin (const Vertex& vertex, const std::vector<glm::mat4>& skinning);
} // namespace WallpaperEngine::Render::Objects::Puppet
