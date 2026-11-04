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
};

struct Mesh {
    std::string material;
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
};

/** The animation's length in seconds: frames / fps */
float length (const Animation& animation);

/** Advances the clock by dt (already × the layer's rate): loop wraps, mirror bounces, single stops (GH 0x1401d6a00) */
void advance (const Animation& animation, Clock& clock, float dt);

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

/** Skins a vertex: Σ weightᵢ · world[boneᵢ] · bindWorld⁻¹[boneᵢ] · position; weights summing to 0 keep it */
glm::vec3 skin (const Vertex& vertex, const std::vector<glm::mat4>& skinning);
} // namespace WallpaperEngine::Render::Objects::Puppet
