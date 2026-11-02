#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace WallpaperEngine::Data::Model {
class DynamicValue;

/**
 * A property's keyframe animation (`"animation": {"c0": [...], ..., "options": {...}}` next to its `value`),
 * evaluated the way Wallpaper Engine 2.8 does it (wallpaper64.exe; docs/player/spikes/we-lwe-gap-audit.md G01/G02):
 *
 *  - one curve per component (`c0`..`c3`), each a list of keyframes `{frame, value, back, front, step}` whose
 *    frames strictly increase (a key that doesn't is dropped);
 *  - between two keys A and B the value is a cubic bezier with control points (0, A.value),
 *    (A.front.x / 2, A.value + A.front.y), (1 + B.back.x / 2, B.value + B.back.y), (1, B.value) over the
 *    normalised segment, solved for x by Newton steps guarded by bisection; all four handle numbers zero (or the
 *    control points on the line) is linear; a `step` B holds A's value; before the first key / after the last the
 *    end values hold;
 *  - the curve is evaluated at whole frames and blended linearly between frame `n` and `n + 1` by the time left
 *    over;
 *  - the clock: `fps`, `length` (frames), `mode` `loop` / `mirror` (ping-pong) / `single` (plays once, holds),
 *    `startpaused` (does not advance), `wraploop` (a last key at `length` copies the first one).
 */
class PropertyAnimation {
public:
    struct Keyframe {
	int frame = 0;
	float value = 0.0f;
	bool step = false;
	bool backEnabled = false;
	bool frontEnabled = false;
	glm::vec2 back = {};
	glm::vec2 front = {};
    };

    using Curve = std::vector<Keyframe>;

    enum Flags : unsigned int {
	Mirror = 0x1,
	Single = 0x2,
	Random = 0x4,
	WrapLoop = 0x10,
	StartPaused = 0x20000000,
	Finished = 0x40000000,
	Reverse = 0x80000000,
    };

    /**
     * @param fps frames per second (> 0)
     * @param length the timeline's length in frames (> 0)
     * @param mode "loop" (or anything else), "mirror" or "single"
     */
    PropertyAnimation (std::string name, float fps, int length, const std::string& mode, bool startPaused, bool wrapLoop,
		       std::vector<Curve> curves);

    /** Advances the timeline by `dt` seconds of the scene clock (honours the mode, pause and the end of `single`) */
    void advance (float dt);
    /** The animated value of component `component` at the current time (0 when that curve has no keys) */
    [[nodiscard]] float sample (size_t component) const;
    /** Writes the animated value into `value`, keeping its type; components without a curve keep their value */
    void apply (DynamicValue& value) const;

    [[nodiscard]] const std::string& getName () const;
    [[nodiscard]] size_t getComponentCount () const;
    [[nodiscard]] float getTime () const;
    [[nodiscard]] unsigned int getFlags () const;

    /** The value of a curve at a whole frame (exposed for tests) */
    [[nodiscard]] static float evaluate (const Curve& curve, int frame);

private:
    std::string m_name;
    float m_secondsPerFrame;
    float m_duration;
    int m_length;
    unsigned int m_flags = 0;
    float m_time = 0.0f;
    std::vector<Curve> m_curves;
};

using PropertyAnimationSharedPtr = std::shared_ptr<PropertyAnimation>;

/** A value with a keyframe animation, as a scene keeps it to advance it every frame */
struct AnimatedValue {
    DynamicValue* value;
    /** false once `value` is destroyed */
    std::shared_ptr<const bool> alive;
};
} // namespace WallpaperEngine::Data::Model
