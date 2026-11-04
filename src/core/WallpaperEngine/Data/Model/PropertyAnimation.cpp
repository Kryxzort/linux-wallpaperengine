#include "PropertyAnimation.h"
#include "DynamicValue.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace WallpaperEngine::Data::Model;

PropertyAnimation::PropertyAnimation (
    std::string name, float fps, int length, const std::string& mode, bool startPaused, bool wrapLoop,
    std::vector<Curve> curves
) :
    m_name (std::move (name)), m_fps (fps), m_secondsPerFrame (1.0f / fps), m_duration (static_cast<float> (length) / fps),
    m_length (length), m_curves (std::move (curves)) {
    if (mode == "mirror") {
	this->m_flags |= Mirror;
    } else if (mode == "single") {
	this->m_flags |= Single;
    }
    if (startPaused) {
	this->m_flags |= StartPaused;
    }
    if (wrapLoop) {
	this->m_flags |= WrapLoop;
	// the key at `length` becomes a copy of the first one, so a loop comes back to where it started: keys past
	// the end go, the value, the step and (as the mirror image of the first key's front handle) the back handle
	// are the first key's
	for (auto& curve : this->m_curves) {
	    if (curve.size () < 2) {
		continue;
	    }
	    while (curve.size () > 1 && curve.back ().frame > length) {
		curve.pop_back ();
	    }
	    if (curve.size () < 2) {
		continue;
	    }
	    if (curve.back ().frame != length) {
		curve.push_back (Keyframe { .frame = length });
	    }
	    const Keyframe first = curve.front ();
	    Keyframe& last = curve.back ();
	    last.value = first.value;
	    last.step = first.step;
	    if (first.frontEnabled) {
		last.backEnabled = true;
		last.back = -first.front;
	    } else {
		last.backEnabled = false;
	    }
	}
    }
}

float PropertyAnimation::evaluate (const Curve& curve, int frame) {
    if (curve.empty ()) {
	return 0.0f;
    }
    if (frame < curve.front ().frame) {
	return curve.front ().value;
    }

    const auto next = std::ranges::find_if (curve, [frame] (const Keyframe& k) { return k.frame > frame; });

    if (next == curve.end ()) {
	return curve.back ().value;
    }

    const Keyframe& a = *std::prev (next);
    const Keyframe& b = *next;

    if (frame == a.frame || b.step) {
	return a.value;
    }

    const float inv = 1.0f / static_cast<float> (b.frame - a.frame);
    const float dv = b.value - a.value;
    const float u = static_cast<float> (frame - a.frame) * inv;
    const float linear = static_cast<float> (frame - a.frame) * inv * dv + a.value;

    if (a.front.x == 0.0f && a.front.y == 0.0f && b.back.x == 0.0f && b.back.y == 0.0f) {
	return linear;
    }

    // control points over the normalised segment: x in [0, 1] (the handles' x are half-segments), y in value units
    const float x1 = a.front.x * 0.5f;
    const float x2 = b.back.x * 0.5f + 1.0f;
    const float y1 = a.value + a.front.y;
    const float y2 = b.value + b.back.y;

    if (y1 == x1 * dv + a.value && y2 == x2 * dv + a.value) {
	return linear;
    }

    // x(t) = u: Newton steps kept inside a bisection bracket, at most 24 of them
    const float tolerance = std::max (inv * 0.01f, FLT_EPSILON);
    float t = u;
    float lo = 0.0f;
    float hi = 1.0f;

    for (int i = 0; i < 24; i++) {
	const float s = 1.0f - t;
	const float x = 3.0f * s * s * t * x1 + 3.0f * s * t * t * x2 + t * t * t - u;

	if (std::fabs (x) <= tolerance) {
	    break;
	}
	if (x < 0.0f) {
	    lo = t;
	} else {
	    hi = t;
	}

	float next_t = (hi + lo) * 0.5f;
	const float dx = 6.0f * s * t * (x2 - x1) + 3.0f * s * s * x1 + 3.0f * t * t * (1.0f - x2);

	if (std::fabs (dx) > FLT_EPSILON) {
	    if (const float newton = t - x / dx; lo < newton && newton < hi) {
		next_t = newton;
	    }
	}
	if (next_t == t) {
	    break;
	}
	t = next_t;
    }

    const float s = 1.0f - t;
    return s * s * s * a.value + 3.0f * s * s * t * y1 + 3.0f * s * t * t * y2 + t * t * t * b.value;
}

void PropertyAnimation::play () {
    if ((this->m_flags & Finished) != 0) {
	this->m_time = 0.0f;
    }
    this->m_flags &= ~(StartPaused | Finished);
}

void PropertyAnimation::pause () { this->m_flags |= StartPaused; }

void PropertyAnimation::stop () {
    this->m_time = 0.0f;
    this->m_flags &= ~(Finished | Reverse);
    this->m_flags |= StartPaused;
}

bool PropertyAnimation::isPlaying () const { return (this->m_flags & (StartPaused | Finished)) == 0; }

int PropertyAnimation::getFrame () const {
    return static_cast<int> (std::floor (this->m_time / this->m_secondsPerFrame + 1e-4f));
}

void PropertyAnimation::setFrame (int frame) {
    this->m_time = std::clamp (static_cast<float> (frame) * this->m_secondsPerFrame, 0.0f, this->m_duration);
}

int PropertyAnimation::getLength () const { return this->m_length; }

float PropertyAnimation::getFps () const { return this->m_fps; }

void PropertyAnimation::collectEvents (float from, float to) {
    for (const auto& event : this->m_events) {
	const float at = static_cast<float> (event.frame) * this->m_secondsPerFrame;
	// a frame is passed once: from its time (the start of a span) up to, not including, the next span's start
	if (at >= from && (at < to || (to >= this->m_duration && at <= to))) {
	    this->m_fired.emplace_back (event.name, event.frame);
	}
    }
}

std::vector<std::pair<std::string, int>> PropertyAnimation::takeFiredEvents () {
    std::vector<std::pair<std::string, int>> fired;
    fired.swap (this->m_fired);
    return fired;
}

void PropertyAnimation::advance (float dt) {
    if (this->m_parent != nullptr) {
	// on the parent's clock (inferred from the editor's parent / children links: the curves share one timeline)
	this->m_time = std::clamp (this->m_parent->getTime (), 0.0f, this->m_duration);
	return;
    }
    if ((this->m_flags & (StartPaused | Finished)) != 0) {
	return;
    }
    if ((this->m_flags & Single) != 0 && this->m_duration <= this->m_time) {
	return;
    }
    if (this->m_duration <= 0.0f) {
	return;
    }
    dt *= this->m_rate;
    const float before = this->m_time;
    const bool backwards = (this->m_flags & Reverse) != 0;
    if (backwards) {
	dt = -dt;
    }

    const float t = this->m_time + dt;
    this->m_time = t;

    // the events the timeline passes (animationEvent): forwards [before, t), a loop's wrap in two spans, a mirror's
    // way back (t, before]
    if (!this->m_events.empty () && dt != 0.0f) {
	const float d = this->m_duration;
	if (!backwards) {
	    if (t < d || (this->m_flags & (Single | Mirror)) != 0) {
		this->collectEvents (before, std::min (t, d));
	    } else {
		this->collectEvents (before, d);
		this->collectEvents (0.0f, std::fmod (t, d));
	    }
	} else {
	    for (const auto& event : this->m_events) {
		const float at = static_cast<float> (event.frame) * this->m_secondsPerFrame;
		if (at <= before && at > std::max (t, 0.0f)) {
		    this->m_fired.emplace_back (event.name, event.frame);
		}
	    }
	}
    }

    if ((this->m_flags & Single) != 0) {
	if (t >= this->m_duration) {
	    this->m_flags |= Finished;
	    this->m_time = this->m_duration;
	}
	return;
    }

    if ((this->m_flags & Mirror) == 0) {
	if (this->m_time < 0.0f) {
	    this->m_time = std::fmod (this->m_time + this->m_duration, this->m_duration);
	}
	if (this->m_time >= this->m_duration) {
	    this->m_time = std::fmod (this->m_time, this->m_duration);
	}
	return;
    }

    if ((this->m_flags & Reverse) == 0) {
	if (t >= this->m_duration) {
	    this->m_flags |= Reverse;
	    this->m_time = this->m_duration - std::fmod (t, this->m_duration);
	}
    } else if (t <= 0.0f) {
	this->m_flags &= ~Reverse;
	this->m_time = -std::fmod (t, this->m_duration);
    }
}

float PropertyAnimation::sample (size_t component) const {
    if (component >= this->m_curves.size ()) {
	return 0.0f;
    }

    const float spf = this->m_secondsPerFrame;
    const int frame = static_cast<int> (this->m_time / spf);
    int from = std::min (frame, this->m_length - 1);

    if (from < 1) {
	from = 0;
    }

    const int to = std::min (from + 1, this->m_length);
    const float weight = std::fmod (this->m_time, spf) / spf;
    const auto& curve = this->m_curves [component];

    return evaluate (curve, from) * (1.0f - weight) + evaluate (curve, to) * weight;
}

void PropertyAnimation::apply (DynamicValue& value) const {
    const auto count = this->m_curves.size ();
    const auto pick = [this, count] (size_t i, float current) { return i < count ? this->sample (i) : current; };

    switch (value.getType ()) {
	case DynamicValue::Float:
	case DynamicValue::Int:
	case DynamicValue::Boolean:
	    value.update (pick (0, value.getFloat ()), DynamicValue::UpdateSource::Animation);
	    break;
	case DynamicValue::Vec2: {
	    const glm::vec2 v = value.getVec2 ();
	    value.update (glm::vec2 (pick (0, v.x), pick (1, v.y)), DynamicValue::UpdateSource::Animation);
	    break;
	}
	case DynamicValue::Vec3: {
	    const glm::vec3 v = value.getVec3 ();
	    value.update (glm::vec3 (pick (0, v.x), pick (1, v.y), pick (2, v.z)), DynamicValue::UpdateSource::Animation);
	    break;
	}
	case DynamicValue::Vec4: {
	    const glm::vec4 v = value.getVec4 ();
	    value.update (
		glm::vec4 (pick (0, v.x), pick (1, v.y), pick (2, v.z), pick (3, v.w)), DynamicValue::UpdateSource::Animation
	    );
	    break;
	}
	default:
	    break;
    }
}

const std::string& PropertyAnimation::getName () const { return this->m_name; }

size_t PropertyAnimation::getComponentCount () const { return this->m_curves.size (); }

float PropertyAnimation::getTime () const { return this->m_time; }

unsigned int PropertyAnimation::getFlags () const { return this->m_flags; }
