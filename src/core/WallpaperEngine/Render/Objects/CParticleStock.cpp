// F6: the particle operators and initializers only Wallpaper Engine's stock presets use (no library scene does).
// Decoded from wallpaper64.exe 2.8.485: the operator interpreter GH 0x140265800 (jump table 0x140271798, opcodes
// 0xb–0x1a), the particle parser GH 0x1401f3120 (its stores, asm) and its default functions (GH 0x1401e7040…
// 0x1401ee010). Each function says what is WE's and what is inferred.

#include "CParticle.h"

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Maths.h"
#include "WallpaperEngine/Render/Utils/NoiseUtils.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Camera.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <glm/gtc/constants.hpp>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Utils;

namespace {
const nlohmann::json* member (const nlohmann::json& j, const char* key) {
    const auto it = j.find (key);
    if (it == j.end () || it->is_null ()) {
	return nullptr;
    }
    // a user-bound value as the editor writes it: {"user": …, "value": …}
    if (it->is_object ()) {
	const auto value = it->find ("value");
	return value == it->end () ? nullptr : &*value;
    }
    return &*it;
}

float num (const nlohmann::json& j, const char* key, float fallback) {
    const auto* v = member (j, key);
    if (v == nullptr) {
	return fallback;
    }
    if (v->is_number ()) {
	return v->get<float> ();
    }
    if (v->is_boolean ()) {
	return v->get<bool> () ? 1.0f : 0.0f;
    }
    if (v->is_string ()) {
	return std::strtof (v->get<std::string> ().c_str (), nullptr);
    }
    return fallback;
}

int integer (const nlohmann::json& j, const char* key, int fallback) {
    return static_cast<int> (num (j, key, static_cast<float> (fallback)));
}

std::string text (const nlohmann::json& j, const char* key, const char* fallback) {
    const auto* v = member (j, key);
    return v != nullptr && v->is_string () ? v->get<std::string> () : std::string (fallback);
}

/** "x y z" (WE's strtod per component, the rest 0), or a number for all three */
glm::vec3 vec3 (const nlohmann::json& j, const char* key, const glm::vec3& fallback) {
    const auto* v = member (j, key);
    if (v == nullptr) {
	return fallback;
    }
    if (v->is_number ()) {
	return glm::vec3 (v->get<float> ());
    }
    if (!v->is_string ()) {
	return fallback;
    }
    const std::string s = v->get<std::string> ();
    glm::vec3 out (0.0f);
    const char* at = s.c_str ();
    for (int i = 0; i < 3 && *at != '\0'; i++) {
	char* end = nullptr;
	out[i] = std::strtof (at, &end);
	if (end == at) {
	    break;
	}
	at = end;
    }
    return out;
}

/** WE's particle space is y up, LWE's y down: a position or direction from the JSON, mirrored */
glm::vec3 mirrored (glm::vec3 v) {
    v.y = -v.y;
    return v;
}

glm::vec3 controlPointOf (const std::vector<ControlPointData>& points, int index) {
    return index >= 0 && index < static_cast<int> (points.size ()) ? points[index].position : glm::vec3 (0.0f);
}

glm::vec3 hsvToRgb (float h, float s, float v) {
    h = h - std::floor (h);
    const float c = v * s;
    const float x = c * (1.0f - std::abs (std::fmod (h * 6.0f, 2.0f) - 1.0f));
    const float m = v - c;
    glm::vec3 rgb;
    switch (static_cast<int> (h * 6.0f) % 6) {
	case 0: rgb = { c, x, 0 }; break;
	case 1: rgb = { x, c, 0 }; break;
	case 2: rgb = { 0, c, x }; break;
	case 3: rgb = { 0, x, c }; break;
	case 4: rgb = { x, 0, c }; break;
	default: rgb = { c, 0, x }; break;
    }
    return rgb + glm::vec3 (m);
}

void kill (ParticleInstance& p) {
    p.age = p.lifetime;
    p.alive = false;
}

/**
 * The remap operators' value names (GH 0x140286d90's table, in order) and transform functions (GH 0x140286f60).
 * remapvalue / remapinitialvalue: input → its component → (x − inmin) / (inmax − inmin) → the transform at
 * transforminputscale → outmin + f · (outmax − outmin) → `operation` (remap: set, multiply, add, subtract) on the
 * output. The enums, keys and defaults are WE's (GH 0x1401ed170: output size, ranges 0..1, controlpoints 0 / 1,
 * transforminputscale 2, transformoctaves 3); the per-function shapes and the clamp of the normalised input are
 * inferred (the interpreter's case 0x13 is 21 KB of SIMD, not read through)
 */
enum RemapValue {
    RLifetimeFraction, RMaxLifetime, RSize, ROpacity, RSpeed, RRotation, RAngularSpeed, RDistanceToControlPoint,
    RPositionBetweenTwoControlPoints, RRuntime, RTimeOfDay, RParticleSystemTime, RLayerTime, RColor, RPosition, RVelocity,
    RControlPoint, RDeltaToControlPoint, RDirectionToControlPoint, RLayerOrigin
};

int remapEnum (const std::string& name, int fallback) {
    static const char* names[] = { "lifetimefraction", "maxlifetime", "size", "opacity", "speed", "rotation",
				   "angularspeed", "distancetocontrolpoint", "positionbetweentwocontrolpoints",
				   "runtime", "timeofday", "particlesystemtime", "layertime", "color", "position",
				   "velocity", "controlpoint", "deltatocontrolpoint", "directiontocontrolpoint",
				   "layerorigin" };
    for (int i = 0; i < static_cast<int> (std::size (names)); i++) {
	if (name == names[i]) {
	    return i;
	}
    }
    return fallback;
}

struct Remap {
    int operation = 0; // remap, multiply, add, subtract
    int input = RLifetimeFraction;
    int output = RSize;
    int inputComponent = 0; // all, x, y, z, sum, average, max, min
    int outputComponent = 0;
    int function = 0; // none, sine, square, saw, triangle, simplexnoise, fbmnoise
    float inMin = 0.0f, inMax = 1.0f;
    /** the output range: a value or "x y z" (the parser keeps both forms, inputrangemin … outputrangemax) */
    glm::vec3 outMin = glm::vec3 (0.0f), outMax = glm::vec3 (1.0f);
    /** blendinstart / blendinend: how much of the result applies over the particle's life (inferred: a clamped ramp) */
    float blendStart = 0.0f, blendEnd = 0.0f;
    int cp0 = 0, cp1 = 1;
    float inputScale = 2.0f;
    int octaves = 3;

    explicit Remap (const nlohmann::json& j) {
	const std::string op = text (j, "operation", "remap");
	operation = op == "multiply" ? 1 : op == "add" ? 2 : op == "subtract" ? 3 : 0;
	input = remapEnum (text (j, "input", "lifetimefraction"), RLifetimeFraction);
	output = remapEnum (text (j, "output", "size"), RSize);
	auto component = [&j] (const char* key) {
	    static const char* names[] = { "all", "x", "y", "z", "sum", "average", "max", "min" };
	    const std::string c = text (j, key, "all");
	    for (int i = 0; i < 8; i++) {
		if (c == names[i]) {
		    return i;
		}
	    }
	    return 0;
	};
	inputComponent = component ("inputcomponent");
	outputComponent = component ("outputcomponent");
	static const char* functions[] = { "none", "sine", "square", "saw", "triangle", "simplexnoise", "fbmnoise" };
	const std::string f = text (j, "transformfunction", "none");
	for (int i = 0; i < 7; i++) {
	    if (f == functions[i]) {
		function = i;
	    }
	}
	inMin = num (j, "inputrangemin", 0.0f);
	inMax = num (j, "inputrangemax", 1.0f);
	outMin = vec3 (j, "outputrangemin", glm::vec3 (0.0f));
	outMax = vec3 (j, "outputrangemax", glm::vec3 (1.0f));
	if (const auto* v = member (j, "outputrangemin"); v != nullptr && v->is_string ()
	    && v->get<std::string> ().find (' ') == std::string::npos) {
	    outMin = glm::vec3 (outMin.x);
	}
	if (const auto* v = member (j, "outputrangemax"); v != nullptr && v->is_string ()
	    && v->get<std::string> ().find (' ') == std::string::npos) {
	    outMax = glm::vec3 (outMax.x);
	}
	blendStart = num (j, "blendinstart", 0.0f);
	blendEnd = num (j, "blendinend", 0.0f);
	cp0 = integer (j, "inputcontrolpoint0", 0);
	cp1 = integer (j, "inputcontrolpoint1", 1);
	inputScale = num (j, "transforminputscale", 2.0f);
	octaves = std::clamp (integer (j, "transformoctaves", 3), 1, 8);
    }

    float transform (float t, float random) const {
	const float x = t * inputScale;
	switch (function) {
	    case 1: return 0.5f + 0.5f * std::sin (x * glm::two_pi<float> ());
	    case 2: return std::fmod (std::abs (x), 1.0f) < 0.5f ? 1.0f : 0.0f;
	    case 3: return x - std::floor (x);
	    case 4: return 1.0f - std::abs (2.0f * (x - std::floor (x)) - 1.0f);
	    case 5: return 0.5f + 0.5f * simplexNoise (x, random * 17.0f, 0.0f);
	    case 6:
		{
		    float sum = 0.0f, amplitude = 0.5f, frequency = 1.0f;
		    for (int o = 0; o < octaves; o++) {
			sum += amplitude * simplexNoise (x * frequency, random * 17.0f, static_cast<float> (o));
			amplitude *= 0.5f;
			frequency *= 2.0f;
		    }
		    return 0.5f + 0.5f * sum;
		}
	    default: return t;
	}
    }
};
/**
 * The operators' blendinstart / blendinend (stock capvelocity and remapvalue set them): the share of the operator's
 * effect over the particle's life, 0 before blendinstart, 1 after blendinend (inferred: a clamped ramp; 1 when unset)
 */
float blendWeight (const ParticleInstance& p, float start, float end) {
    if (start <= 0.0f && end <= 0.0f) {
	return 1.0f;
    }
    const float life = p.lifetime > 0.0f ? p.age / p.lifetime : 1.0f;
    if (end <= start) {
	return life >= start ? 1.0f : 0.0f;
    }
    return std::clamp ((life - start) / (end - start), 0.0f, 1.0f);
}
} // namespace

float CParticle::stockDefault (float world, float pixels) const {
    return getScene ().getCamera ().isPerspective () ? world : pixels;
}

OperatorFunc CParticle::createGenericOperator (const GenericParticleOperator& op) {
    const auto& j = op.params;
    const std::string& name = op.name;

    if (name == "capvelocity") {
	// case 0x12 (Observed): v ×= min (1, maxspeed / |v|); maxspeed (1 | 100) carries the speed override
	const float maxSpeed = num (j, "maxspeed", stockDefault (1.0f, 100.0f));
	const float blendStart = num (j, "blendinstart", 0.0f), blendEnd = num (j, "blendinend", 0.0f);
	return [this, maxSpeed, blendStart, blendEnd] (std::vector<ParticleInstance>& particles, uint32_t count,
						       const std::vector<ControlPointData>&, float, float) {
	    const float cap = maxSpeed * speedOverride ();
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		const float speed = glm::length (p.velocity);
		if (speed > cap && speed > 0.0f) {
		    const float w = blendWeight (p, blendStart, blendEnd);
		    p.velocity *= 1.0f + (cap / speed - 1.0f) * w;
		}
	    }
	};
    }

    if (name == "maintaindistancetocontrolpoint") {
	// case 0xb (Observed): the particles first move with the control point (its change since the last step), then
	// each is put back towards `distance` from it: p = cp + (p − cp) · (1 + k · (distance / |p − cp| − 1)), k = 1, or
	// clamp (variablestrength · dt, 0, 1) when variablestrength isn't 0. Defaults (GH 0x1401eb730): distance 1 | 200,
	// controlpoint 0. Inferred: WE measures |p − cp| through a matrix of the control point (GH 0x14024e870, not
	// read); identity here
	const int cp = integer (j, "controlpoint", 0);
	const float distance = num (j, "distance", stockDefault (1.0f, 200.0f));
	const float strength = num (j, "variablestrength", 0.0f);
	return [this, cp, distance, strength] (std::vector<ParticleInstance>& particles, uint32_t count,
					       const std::vector<ControlPointData>& points, float, float dt) {
	    const glm::vec3 centre = controlPointOf (points, cp);
	    const glm::vec3 moved = m_controlPointsSeen && cp >= 0 && cp < 8 ? centre - m_controlPointLast[cp] : glm::vec3 (0.0f);
	    const float k = strength == 0.0f ? 1.0f : std::clamp (strength * dt, 0.0f, 1.0f);
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		p.position += moved;
		const glm::vec3 d = p.position - centre;
		const float length = glm::length (d);
		if (length > 1e-6f) {
		    p.position = centre + d * (1.0f + k * (distance / length - 1.0f));
		}
	    }
	};
    }

    if (name == "maintaindistancebetweencontrolpoints") {
	// case 0xc (Observed): the segment from controlpointstart to controlpointend (defaults 0, 1) carries its
	// particles when it moves: t = clamp (dot (p − S₀, E₀ − S₀) / |E₀ − S₀|², 0, 1) on the last step's segment,
	// p += (S − S₀) + t (E − S) − the unclamped projection of p − S₀ on E₀ − S₀. Nothing moves while either
	// segment is shorter than ~1.2e-7
	const int start = integer (j, "controlpointstart", 0);
	const int end = integer (j, "controlpointend", 1);
	return [this, start, end] (std::vector<ParticleInstance>& particles, uint32_t count,
				   const std::vector<ControlPointData>& points, float, float) {
	    if (!m_controlPointsSeen || start < 0 || start >= 8 || end < 0 || end >= 8) {
		return;
	    }
	    const glm::vec3 s = controlPointOf (points, start), e = controlPointOf (points, end);
	    const glm::vec3 s0 = m_controlPointLast[start], e0 = m_controlPointLast[end];
	    const glm::vec3 now = e - s, before = e0 - s0;
	    const float nowLength = glm::dot (now, now), beforeLength = glm::dot (before, before);
	    if (nowLength <= 1.4210855e-14f || beforeLength <= 1.4210855e-14f) {
		return;
	    }
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		const float along = glm::dot (p.position - s0, before) / beforeLength;
		const float t = std::clamp (along, 0.0f, 1.0f);
		p.position += (s - s0) + t * now - along * before;
	    }
	};
    }

    if (name == "reducemovementnearcontrolpoint") {
	// case 0xd (Observed): t = clamp ((|p − cp| − distanceinner) / (distanceouter − distanceinner), 0, 1),
	// r = clamp ((reductioninner + t · (reductionouter − reductioninner)) · dt, 0, 1), v ×= 1 − r. Defaults
	// (GH 0x1401ebd00): distances 0.5 / 1 (3D) or 100 / 350, reductions 100 / 0
	const int cp = integer (j, "controlpoint", 0);
	const float inner = num (j, "distanceinner", stockDefault (0.5f, 100.0f));
	const float outer = num (j, "distanceouter", stockDefault (1.0f, 350.0f));
	const float reductionInner = num (j, "reductioninner", 100.0f);
	const float reductionOuter = num (j, "reductionouter", 0.0f);
	return [cp, inner, outer, reductionInner, reductionOuter] (std::vector<ParticleInstance>& particles, uint32_t count,
								    const std::vector<ControlPointData>& points, float,
								    float dt) {
	    const glm::vec3 centre = controlPointOf (points, cp);
	    const float span = outer - inner;
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		const float d = glm::length (p.position - centre);
		const float t = span != 0.0f ? std::clamp ((d - inner) / span, 0.0f, 1.0f) : (d >= inner ? 1.0f : 0.0f);
		const float r = std::clamp ((reductionInner + t * (reductionOuter - reductionInner)) * dt, 0.0f, 1.0f);
		p.velocity *= 1.0f - r;
	    }
	};
    }

    if (name == "boids") {
	// case 0x11: each step updates every s-th particle (s = count / 200 + 1, starting at the system's frame
	// counter mod s) with its factors × s (Observed). The forces are inferred (Reynolds' rules: the case is 1.5 KB
	// of SIMD): within neighborthreshold, cohesion pulls towards the neighbours' centre and alignment towards
	// their mean velocity; within separationthreshold, separation pushes away; the speed is capped at maxspeed.
	// Defaults (GH 0x1401ecc60): separationthreshold 0.02 | 20, neighborthreshold 0.2 | 50, maxspeed 1 | 500,
	// separationfactor 0, alignmentfactor 1, cohesionfactor 2
	const float separation = num (j, "separationthreshold", stockDefault (0.02f, 20.0f));
	const float neighbors = num (j, "neighborthreshold", stockDefault (0.2f, 50.0f));
	const float maxSpeed = num (j, "maxspeed", stockDefault (1.0f, 500.0f));
	const float separationFactor = num (j, "separationfactor", 0.0f);
	const float alignmentFactor = num (j, "alignmentfactor", 1.0f);
	const float cohesionFactor = num (j, "cohesionfactor", 2.0f);
	uint32_t frame = 0;
	return [this, separation, neighbors, maxSpeed, separationFactor, alignmentFactor, cohesionFactor, frame] (
		   std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
		   float dt) mutable {
	    const uint32_t stride = count / 200 + 1;
	    const float scale = static_cast<float> (stride) * dt;
	    const float cap = maxSpeed * speedOverride ();
	    for (uint32_t i = frame++ % stride; i < count; i += stride) {
		auto& p = particles[i];
		glm::vec3 centre (0.0f), heading (0.0f), apart (0.0f);
		int n = 0;
		for (uint32_t k = 0; k < count; k++) {
		    if (k == i) {
			continue;
		    }
		    const glm::vec3 d = particles[k].position - p.position;
		    const float length = glm::length (d);
		    if (length < neighbors) {
			centre += particles[k].position;
			heading += particles[k].velocity;
			n++;
		    }
		    if (length < separation && length > 1e-6f) {
			apart -= d / length * (separation - length);
		    }
		}
		if (n > 0) {
		    centre /= static_cast<float> (n);
		    heading /= static_cast<float> (n);
		    p.velocity += (centre - p.position) * cohesionFactor * scale;
		    p.velocity += (heading - p.velocity) * std::min (alignmentFactor * scale, 1.0f);
		}
		p.velocity += apart * separationFactor * scale;
		const float speed = glm::length (p.velocity);
		if (speed > cap && speed > 0.0f) {
		    p.velocity *= cap / speed;
		}
	    }
	};
    }

    if (name == "remapvalue") {
	return [this, remap = Remap (j)] (std::vector<ParticleInstance>& particles, uint32_t count,
					  const std::vector<ControlPointData>& points, float time, float) {
	    for (uint32_t i = 0; i < count; i++) {
		this->applyRemap (&remap, particles[i], points, time);
	    }
	};
    }

    if (name == "inheritvaluefromevent") {
	// case 0x14: an event child's particles take a value of the particle that started it (GH 0x140287030's table,
	// default from GH 0x1401ed620). A follow child's source is its particle this step. The names are WE's; the set /
	// multiply / add forms are read from them (inferred)
	const std::string input = text (j, "input", "setcolor");
	return [this, input] (std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&,
			      float, float) {
	    if (!m_eventSource.has_value ()) {
		return;
	    }
	    for (uint32_t i = 0; i < count; i++) {
		this->inheritFrom (*m_eventSource, particles[i], input);
	    }
	};
    }

    if (name == "collisionplane" || name == "collisionsphere" || name == "collisionquad" || name == "collisionbounds") {
	// the collisions share collisionbehavior (bounce, the default; slide; delete) and bouncefactor (GH 0x1401edab0:
	// it keeps −1 − bouncefactor, so a bounce is v −= (1 + b)(v · n) n). Cases 0x15 plane, 0x16 sphere, 0x18 bounds,
	// 0x19 quad. Inferred: the bounce factor's default (0.5), the bounds being the scene's rectangle, the sphere
	// keeping particles out
	const std::string behaviour = text (j, "collisionbehavior", "bounce");
	const int mode = behaviour == "slide" ? 1 : behaviour == "delete" ? 2 : 0;
	const float bounce = num (j, "bouncefactor", 0.5f);
	const int cp = integer (j, "controlpoint", 0);
	auto respond = [mode, bounce] (ParticleInstance& p, const glm::vec3& normal) {
	    if (mode == 2) {
		kill (p);
		return;
	    }
	    const float into = glm::dot (p.velocity, normal);
	    if (into < 0.0f) {
		p.velocity -= normal * into * (mode == 1 ? 1.0f : 1.0f + bounce);
	    }
	};

	if (name == "collisionplane") {
	    // GH 0x1401edc00: plane "0 1 0", distance −150 (2D)
	    glm::vec3 normal = mirrored (vec3 (j, "plane", glm::vec3 (0.0f, 1.0f, 0.0f)));
	    normal = glm::length (normal) > 0.0f ? glm::normalize (normal) : glm::vec3 (0.0f, -1.0f, 0.0f);
	    const float distance = num (j, "distance", stockDefault (-1.0f, -150.0f));
	    return [normal, distance, cp, respond] (std::vector<ParticleInstance>& particles, uint32_t count,
						    const std::vector<ControlPointData>& points, float, float) {
		const glm::vec3 origin = controlPointOf (points, cp);
		for (uint32_t i = 0; i < count; i++) {
		    auto& p = particles[i];
		    // WE's plane: dot (p, n) = distance, the particles kept on its positive side
		    const float side = glm::dot (p.position - origin, normal) - distance;
		    if (side < 0.0f) {
			p.position -= normal * side;
			respond (p, normal);
		    }
		}
	    };
	}
	if (name == "collisionsphere") {
	    // GH 0x1401ede00: origin "0 0 0" | "0 −200 0", radius 1 | 50
	    const glm::vec3 origin = mirrored (vec3 (j, "origin", stockDefault (0.0f, 1.0f) == 0.0f ? glm::vec3 (0.0f)
										   : glm::vec3 (0.0f, -200.0f, 0.0f)));
	    const float radius = num (j, "radius", stockDefault (1.0f, 50.0f));
	    return [origin, radius, cp, respond] (std::vector<ParticleInstance>& particles, uint32_t count,
						  const std::vector<ControlPointData>& points, float, float) {
		const glm::vec3 centre = controlPointOf (points, cp) + origin;
		for (uint32_t i = 0; i < count; i++) {
		    auto& p = particles[i];
		    const glm::vec3 d = p.position - centre;
		    const float length = glm::length (d);
		    if (length >= radius) {
			continue;
		    }
		    if (length <= 1e-6f) {
			// at the centre: no normal; deleted, or left for the next step
			respond (p, glm::vec3 (0.0f));
			continue;
		    }
		    const glm::vec3 normal = d / length;
		    p.position = centre + normal * radius;
		    respond (p, normal);
		}
	    };
	}
	if (name == "collisionquad") {
	    // GH 0x1401ee010: origin "0 0 0" | "0 −150 0", plane (normal) "0 1 0", forward, size "1 1" | "200 200"
	    const glm::vec3 origin = mirrored (vec3 (j, "origin", stockDefault (0.0f, 1.0f) == 0.0f ? glm::vec3 (0.0f)
										   : glm::vec3 (0.0f, -150.0f, 0.0f)));
	    glm::vec3 normal = mirrored (vec3 (j, "plane", glm::vec3 (0.0f, 1.0f, 0.0f)));
	    normal = glm::length (normal) > 0.0f ? glm::normalize (normal) : glm::vec3 (0.0f, -1.0f, 0.0f);
	    glm::vec3 forward = mirrored (vec3 (j, "forward", glm::vec3 (0.0f, 0.0f, 1.0f)));
	    forward -= normal * glm::dot (forward, normal);
	    forward = glm::length (forward) > 1e-6f ? glm::normalize (forward) : glm::vec3 (0.0f, 0.0f, 1.0f);
	    const glm::vec3 right = glm::cross (normal, forward);
	    const glm::vec3 size = vec3 (j, "size", glm::vec3 (stockDefault (1.0f, 200.0f)));
	    return [origin, normal, forward, right, size, cp, respond] (
		       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>& points,
		       float, float dt) {
		const glm::vec3 centre = controlPointOf (points, cp) + origin;
		for (uint32_t i = 0; i < count; i++) {
		    auto& p = particles[i];
		    const glm::vec3 d = p.position - centre;
		    const float side = glm::dot (d, normal);
		    // crossed the quad's plane this step (it was on the positive side)
		    const float before = side - glm::dot (p.velocity, normal) * dt;
		    if (side >= 0.0f || before < 0.0f) {
			continue;
		    }
		    if (std::abs (glm::dot (d, right)) > size.x / 2.0f || std::abs (glm::dot (d, forward)) > size.y / 2.0f) {
			continue;
		    }
		    p.position -= normal * side;
		    respond (p, normal);
		}
	    };
	}
	// collisionbounds: the scene's rectangle in the system's space (inferred), its walls facing in
	return [this, respond] (std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&,
				float, float) {
	    const float w = static_cast<float> (getScene ().getWidth ()), h = static_cast<float> (getScene ().getHeight ());
	    const glm::vec3 lo = glm::vec3 (-w / 2.0f, -h / 2.0f, 0.0f) - m_transformedOrigin;
	    const glm::vec3 hi = glm::vec3 (w / 2.0f, h / 2.0f, 0.0f) - m_transformedOrigin;
	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		for (int axis = 0; axis < 2; axis++) {
		    if (p.position[axis] < lo[axis]) {
			p.position[axis] = lo[axis];
			glm::vec3 n (0.0f);
			n[axis] = 1.0f;
			respond (p, n);
		    } else if (p.position[axis] > hi[axis]) {
			p.position[axis] = hi[axis];
			glm::vec3 n (0.0f);
			n[axis] = -1.0f;
			respond (p, n);
		    }
		}
	    }
	};
    }

    // collisionbox: its jump-table entry (0x140265eb9) is the interpreter's next-operator label, so WE 2.8.485 runs
    // nothing for it (Observed); collisionmodel needs a model's triangles (not run, reported by the parser)
    return [] (std::vector<ParticleInstance>&, uint32_t, const std::vector<ControlPointData>&, float, float) { };
}

void CParticle::applyRemap (const void* remapPtr, ParticleInstance& p, const std::vector<ControlPointData>& points,
			    float time) const {
    const Remap& remap = *static_cast<const Remap*> (remapPtr);
    const glm::vec3 a = controlPointOf (points, remap.cp0), b = controlPointOf (points, remap.cp1);
    glm::vec3 in (0.0f);
    switch (remap.input) {
	case RLifetimeFraction: in = glm::vec3 (p.lifetime > 0.0f ? p.age / p.lifetime : 1.0f); break;
	case RMaxLifetime: in = glm::vec3 (p.lifetime); break;
	case RSize: in = glm::vec3 (p.size); break;
	case ROpacity: in = glm::vec3 (p.alpha); break;
	case RSpeed: in = glm::vec3 (glm::length (p.velocity)); break;
	case RRotation: in = p.rotation; break;
	case RAngularSpeed: in = glm::vec3 (glm::length (p.angularVelocity)); break;
	case RDistanceToControlPoint: in = glm::vec3 (glm::length (p.position - a)); break;
	case RPositionBetweenTwoControlPoints:
	    {
		const glm::vec3 d = b - a;
		const float l = glm::dot (d, d);
		in = glm::vec3 (l > 0.0f ? glm::dot (p.position - a, d) / l : 0.0f);
		break;
	    }
	case RRuntime:
	case RLayerTime: in = glm::vec3 (static_cast<float> (getContext ().getContext ().renderTime)); break;
	case RTimeOfDay:
	    {
		const std::time_t now = std::time (nullptr);
		const std::tm* local = std::localtime (&now);
		in = glm::vec3 ((local->tm_hour * 3600 + local->tm_min * 60 + local->tm_sec) / 86400.0f);
		break;
	    }
	case RParticleSystemTime: in = glm::vec3 (time); break;
	case RColor: in = p.color; break;
	case RPosition: in = mirrored (p.position); break;
	case RVelocity: in = mirrored (p.velocity); break;
	case RControlPoint: in = mirrored (a); break;
	case RDeltaToControlPoint: in = mirrored (a - p.position); break;
	case RDirectionToControlPoint:
	    {
		const glm::vec3 d = a - p.position;
		in = glm::length (d) > 0.0f ? mirrored (glm::normalize (d)) : glm::vec3 (0.0f);
		break;
	    }
	case RLayerOrigin: in = mirrored (m_transformedOrigin); break;
	default: break;
    }
    float scalar = in.x;
    switch (remap.inputComponent) {
	case 1: scalar = in.x; break;
	case 2: scalar = in.y; break;
	case 3: scalar = in.z; break;
	case 4: scalar = in.x + in.y + in.z; break;
	case 5: scalar = (in.x + in.y + in.z) / 3.0f; break;
	case 6: scalar = std::max ({ in.x, in.y, in.z }); break;
	case 7: scalar = std::min ({ in.x, in.y, in.z }); break;
	default: break;
    }
    const float span = remap.inMax - remap.inMin;
    const float t = std::clamp (span != 0.0f ? (scalar - remap.inMin) / span : 0.0f, 0.0f, 1.0f);
    const glm::vec3 out3 = remap.outMin + remap.transform (t, p.random) * (remap.outMax - remap.outMin);
    const float out = out3.x;
    const float weight = blendWeight (p, remap.blendStart, remap.blendEnd);

    auto apply = [&remap, weight] (float current, float value) {
	float next;
	switch (remap.operation) {
	    case 1: next = current * value; break;
	    case 2: next = current + value; break;
	    case 3: next = current - value; break;
	    default: next = value; break;
	}
	return current + (next - current) * weight;
    };
    auto applyVec = [&] (glm::vec3 v, bool flip) {
	glm::vec3 we = flip ? mirrored (v) : v;
	for (int c = 0; c < 3; c++) {
	    if (remap.outputComponent == 0 || remap.outputComponent - 1 == c || remap.outputComponent > 3) {
		we[c] = apply (we[c], remap.outputComponent == 0 ? out3[c] : out);
	    }
	}
	return flip ? mirrored (we) : we;
    };
    switch (remap.output) {
	case RLifetimeFraction: p.age = std::clamp (apply (p.lifetime > 0.0f ? p.age / p.lifetime : 1.0f, out), 0.0f, 1.0f) * p.lifetime; break;
	case RMaxLifetime: p.lifetime = apply (p.lifetime, out); break;
	case RSize: p.size = apply (p.size, out); break;
	case ROpacity: p.alpha = apply (p.alpha, out); break;
	case RSpeed:
	    {
		const float speed = glm::length (p.velocity);
		if (speed > 0.0f) {
		    p.velocity *= apply (speed, out) / speed;
		}
		break;
	    }
	case RRotation: p.rotation = applyVec (p.rotation, false); break;
	case RAngularSpeed: p.angularVelocity = applyVec (p.angularVelocity, false); break;
	case RColor: p.color = applyVec (p.color, false); break;
	case RPosition: p.position = applyVec (p.position, true); break;
	case RVelocity: p.velocity = applyVec (p.velocity, true); break;
	default: break;
    }
}

void CParticle::inheritFrom (const ParticleInstance& source, ParticleInstance& p, const std::string& what) const {
    const bool all = what == "all";
    if (all || what == "setcolor" || what == "setcoloropacity") {
	p.color = source.color;
    }
    if (what == "multiplycolor" || what == "multiplycoloropacity") {
	p.color *= source.color;
    }
    if (all || what == "setopacity" || what == "setcoloropacity") {
	p.alpha = source.alpha;
    }
    if (what == "multiplyopacity" || what == "multiplycoloropacity") {
	p.alpha *= source.alpha;
    }
    if (all || what == "setvelocity") {
	p.velocity = source.velocity;
    }
    if (what == "addvelocity") {
	p.velocity += source.velocity;
    }
    if (all || what == "setsize") {
	p.size = source.size;
    }
    if (what == "multiplysize") {
	p.size *= source.size;
    }
    if (all || what == "setrotation") {
	p.rotation = source.rotation;
    }
    if (what == "addrotation") {
	p.rotation += source.rotation;
    }
    if (all || what == "setangularvelocity") {
	p.angularVelocity = source.angularVelocity;
    }
    if (what == "addangularvelocity") {
	p.angularVelocity += source.angularVelocity;
    }
}

InitializerFunc CParticle::createGenericInitializer (const GenericParticleInitializer& init) {
    const auto& j = init.params;
    const std::string& name = init.name;

    if (name == "hsvcolorrandom") {
	// keys (parser 0x1401f54c3) and defaults (GH 0x1401e7040): hue 0..1, saturation and value 0.5..1, huesteps.
	// Inferred: a uniform draw per channel, the hue snapped to huesteps when set
	const float hueMin = num (j, "huemin", 0.0f), hueMax = num (j, "huemax", 1.0f);
	const float satMin = num (j, "saturationmin", 0.5f), satMax = num (j, "saturationmax", 1.0f);
	const float valMin = num (j, "valuemin", 0.5f), valMax = num (j, "valuemax", 1.0f);
	const int steps = integer (j, "huesteps", 0);
	return [this, hueMin, hueMax, satMin, satMax, valMin, valMax, steps] (ParticleInstance& p) {
	    float h = Maths::randomFloat (m_rng, 0.0f, 1.0f);
	    if (steps > 0) {
		h = std::floor (h * static_cast<float> (steps)) / static_cast<float> (std::max (steps - 1, 1));
	    }
	    h = hueMin + h * (hueMax - hueMin);
	    const float s = Maths::randomFloat (m_rng, satMin, satMax);
	    const float v = Maths::randomFloat (m_rng, valMin, valMax);
	    p.color = hsvToRgb (h, s, v);
	};
    }

    if (name == "colorlist") {
	// `colors` (a list of "r g b", 0..1) and huenoise / saturationnoise / valuenoise (parser 0x1401f57df). Inferred:
	// one colour of the list at random, its hsv jittered by ± the noises
	std::vector<glm::vec3> colors;
	if (const auto it = j.find ("colors"); it != j.end () && it->is_array ()) {
	    for (const auto& c : *it) {
		if (c.is_string ()) {
		    const nlohmann::json wrapped = { { "c", c } };
		    colors.push_back (vec3 (wrapped, "c", glm::vec3 (1.0f)));
		}
	    }
	}
	if (colors.empty ()) {
	    colors.emplace_back (1.0f);
	}
	const float hueNoise = num (j, "huenoise", 0.0f), satNoise = num (j, "saturationnoise", 0.0f),
		    valNoise = num (j, "valuenoise", 0.0f);
	return [this, colors, hueNoise, satNoise, valNoise] (ParticleInstance& p) {
	    const size_t index = std::min (colors.size () - 1, static_cast<size_t> (Maths::randomFloat (m_rng, 0.0f, 1.0f) * colors.size ()));
	    glm::vec3 c = colors[index];
	    if (hueNoise != 0.0f || satNoise != 0.0f || valNoise != 0.0f) {
		const float mx = std::max ({ c.r, c.g, c.b }), mn = std::min ({ c.r, c.g, c.b }), d = mx - mn;
		float h = 0.0f;
		if (d > 0.0f) {
		    h = mx == c.r ? std::fmod ((c.g - c.b) / d, 6.0f) : mx == c.g ? (c.b - c.r) / d + 2.0f : (c.r - c.g) / d + 4.0f;
		    h /= 6.0f;
		}
		const float s = mx > 0.0f ? d / mx : 0.0f;
		c = hsvToRgb (h + Maths::randomFloat (m_rng, -hueNoise, hueNoise),
			      std::clamp (s + Maths::randomFloat (m_rng, -satNoise, satNoise), 0.0f, 1.0f),
			      std::clamp (mx + Maths::randomFloat (m_rng, -valNoise, valNoise), 0.0f, 1.0f));
	    }
	    p.color = c;
	};
    }

    if (name == "inheritcontrolpointvelocity") {
	// keys (parser 0x1401f620f): controlpoint, min, max; defaults 0.1 / 0.2 (GH 0x1401e7b50). Inferred: the
	// particle takes the control point's velocity this step × a draw in min..max
	const int cp = std::clamp (integer (j, "controlpoint", 0), 0, 7);
	const float lo = num (j, "min", 0.1f), hi = num (j, "max", 0.2f);
	return [this, cp, lo, hi] (ParticleInstance& p) {
	    p.velocity += m_controlPointVelocity[cp] * Maths::randomFloat (m_rng, lo, hi);
	};
    }

    if (name == "positionoffsetrandom") {
	// keys (parser 0x1401f6cca): scale, distance, timescale, directions ("1 1 1" | "1 1 0"), sign, octaves;
	// defaults (GH 0x1401e85a0): scale 1 | 0.001, distance 0.1 | 100, timescale 1. Inferred: an offset of up to
	// `distance` along the masked directions from a simplex field of the position · scale and the system's time ·
	// timescale (a random offset where scale is 0)
	const float scale = num (j, "scale", stockDefault (1.0f, 0.001f));
	const float distance = num (j, "distance", stockDefault (0.1f, 100.0f));
	const float timescale = num (j, "timescale", 1.0f);
	const glm::vec3 directions = vec3 (j, "directions", stockDefault (0.0f, 1.0f) == 0.0f ? glm::vec3 (1.0f)
											     : glm::vec3 (1.0f, 1.0f, 0.0f));
	return [this, scale, distance, timescale, directions] (ParticleInstance& p) {
	    glm::vec3 n;
	    if (scale == 0.0f) {
		n = glm::vec3 (Maths::randomFloat (m_rng, -1.0f, 1.0f), Maths::randomFloat (m_rng, -1.0f, 1.0f),
			       Maths::randomFloat (m_rng, -1.0f, 1.0f));
	    } else {
		const glm::vec3 q = p.position * scale + glm::vec3 (m_simTime * timescale);
		n = glm::vec3 (simplexNoise (q.x, q.y, q.z), simplexNoise (q.z, q.x, q.y), simplexNoise (q.y, q.z, q.x));
	    }
	    p.position += mirrored (n * directions) * distance;
	};
    }

    if (name == "mapsequencebetweencontrolpoints") {
	// keys (parser 0x1401f7e6a): count (default 32), bounds, limitbehavior, mirror, controlpointstart / end (0 / 1),
	// flags, arcamount (0.3), arcdirection, sizereductionamount (0.9) (GH 0x1401e91e0). Inferred: the particles in
	// turn at count evenly spaced points from the start to the end control point (the arc and the size reduction
	// are not applied)
	const int count = std::max (1, integer (j, "count", 32));
	const int start = integer (j, "controlpointstart", 0), end = integer (j, "controlpointend", 1);
	int index = 0;
	return [this, count, start, end, index] (ParticleInstance& p) mutable {
	    const glm::vec3 a = controlPointOf (m_controlPoints, start), b = controlPointOf (m_controlPoints, end);
	    const float t = count > 1 ? static_cast<float> (index) / static_cast<float> (count - 1) : 0.0f;
	    index = (index + 1) % count;
	    p.position = a + (b - a) * t;
	};
    }

    if (name == "remapinitialvalue") {
	// remapvalue's keys and enums applied once at spawn (parser 0x1401f835d; the same record layout)
	return [this, remap = Remap (j)] (ParticleInstance& p) {
	    this->applyRemap (&remap, p, m_controlPoints, m_simTime);
	};
    }

    if (name == "inheritinitialvaluefromevent") {
	// inheritvaluefromevent's table, once at spawn from the particle that started the event child (inferred)
	const std::string input = text (j, "input", "setcolor");
	return [this, input] (ParticleInstance& p) {
	    if (m_eventSource.has_value ()) {
		this->inheritFrom (*m_eventSource, p, input);
	    }
	};
    }

    return {};
}
