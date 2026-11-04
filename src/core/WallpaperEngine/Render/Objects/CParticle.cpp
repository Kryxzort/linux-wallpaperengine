#include "CParticle.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Health.h"

#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Maths.h"
#include "WallpaperEngine/Render/Utils/NoiseUtils.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <cmath>
#include <glad/glad.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Utils;
using namespace WallpaperEngine::Data::Model;

CParticle::CParticle (Wallpapers::CScene& scene, const Particle& particle) :
    CObject (scene, particle), CRenderable (scene, particle, *particle.material->material),
    Scripting::ScriptableObject (scene, particle), m_particle (particle), m_rng (scene.getParticleRng ()) {
    this->registerProperty ("scale", *particle.scale->value);
    this->registerProperty ("angles", *particle.angles->value);
    this->registerProperty ("visible", *particle.visible->value);
    this->registerProperty ("parallaxDepth", *particle.parallaxDepth->value);
    // the instanceoverride values are scriptable like any property (the editor puts scripts on them: 11 library scenes,
    // rate / size / count / speed / controlpoint1 / colorn / alpha; 2290002190's audio-driven rate). They weren't
    // registered, so their scripts never ran and the value stayed at its default
    const auto& io = particle.instanceOverride;
    const std::pair<const char*, const UserSettingUniquePtr*> overrides[] = {
	{ "alpha", &io.alpha }, { "size", &io.size }, { "lifetime", &io.lifetime }, { "rate", &io.rate },
	{ "speed", &io.speed }, { "count", &io.count }, { "color", &io.color }, { "colorn", &io.colorn },
    };
    for (const auto& [name, setting] : overrides) {
	if (*setting != nullptr) {
	    this->registerProperty (name, *(*setting)->value);
	}
    }
    for (int i = 0; i < 8; i++) {
	if (io.controlPoint[i] != nullptr) {
	    this->registerProperty ("controlpoint" + std::to_string (i), *io.controlPoint[i]->value);
	}
    }

    this->detectTexture ();

    // Read renderer configuration early to determine rendering mode
    if (!m_particle.renderers.empty ()) {
	const auto& renderer = m_particle.renderers[0];
	if (renderer.name == "rope" || renderer.name == "ropetrail") {
	    // Both rope and ropetrail use genericropeparticle shader
	    m_useRopeRenderer = true;
	    m_ropeSubdivision = std::max (0, static_cast<int> (renderer.subdivision));
	    m_ropeUVScale = renderer.uvScale;
	    m_ropeUVScrolling = renderer.uvScrolling;
	    m_ropeUVSmoothing = renderer.uvSmoothing;

	    if (renderer.name == "ropetrail") {
		m_useTrailRenderer = true;
		m_trailLength = renderer.length;
		m_ropeSegments = std::max (2, static_cast<int> (renderer.segments));
	    }
	} else if (renderer.name == "spritetrail") {
	    // spritetrail uses genericparticle with TRAILRENDERER combo
	    m_useTrailRenderer = true;
	    m_trailLength = renderer.length;
	    m_trailMaxLength = renderer.maxLength;
	    m_trailMinLength = renderer.minLength;
	}
    }

    // The pool is `maxcount`: the `count` override scales the emitters' rate, not the pool (WE binds count to
    // every emitter's rate and maxtoemitperperiod, and nothing to maxcount; parser 0x1401f3efc..0x1401f4b44)
    m_maxParticles = particle.maxCount > 0 ? particle.maxCount : DEFAULT_MAX_PARTICLES;

    m_particles.resize (m_maxParticles);

    // Calculate buffer sizes based on renderer type
    if (m_useRopeRenderer) {
	// Rope: connects N particles with (N-1) segments, each subdivided into sub-segments (ropeSubSegments)
	const int subdivision = this->ropeSubSegments ();
	const int maxSubSegments = std::max (1, static_cast<int> (m_maxParticles - 1)) * subdivision;
	m_vertices.resize (maxSubSegments * 4 * ROPE_FLOATS_PER_VERTEX);
	m_indices.resize (maxSubSegments * 6);
    } else {
	// Trail particles: (N+1) * 2 vertices for ribbon strip, N * 6 indices for N quads
	// Normal particles: 4 vertices, 6 indices
	const int verticesPerParticle = 4;
	const int indicesPerParticle = 6;

	m_vertices.resize (m_maxParticles * verticesPerParticle * SPRITE_FLOATS_PER_VERTEX);
	m_indices.resize (m_maxParticles * indicesPerParticle);
    }
}

CParticle::CParticle (
    Wallpapers::CScene& scene, const Particle& particle, CParticle& parent, const ParticleChild& child, bool simulateOnly
) :
    CParticle (scene, particle) {
    // an event child's instances only simulate: one renderer per child entry draws them all (each instance with
    // its own shader pass cost a compile per instance; WE's instances share their template's material)
    m_simulateOnly = simulateOnly;
    if (simulateOnly) {
	m_vertices = {};
	m_indices = {};
    }
    // a child system (P2): it simulates in its parent's space, its particles spawning at an offset (the child's
    // origin, plus the particle it belongs to for an event child) turned and scaled by its angles and scale; WE gives
    // the instance that transform (GH 0x1402507e0: translate (particle) × the child's origin/angles/scale)
    m_parentSystem = &parent;
    m_childDef = &child;
    glm::vec3 angles = child.angles;
    glm::mat4 linear (1.0f);
    // the system's own rotation convention (updateMatrices), in its mirrored (y-down) space
    linear = glm::rotate (linear, -angles.z, glm::vec3 (0, 0, 1));
    linear = glm::rotate (linear, angles.y, glm::vec3 (0, 1, 0));
    linear = glm::rotate (linear, -angles.x, glm::vec3 (1, 0, 0));
    linear = glm::scale (linear, child.scale);
    m_childLinear = glm::mat3 (linear);
    m_childLinearInverse = glm::inverse (m_childLinear);
    m_childSizeScale = (std::abs (child.scale.x) + std::abs (child.scale.y)) * 0.5f;
}

CParticle::~CParticle () {
    delete m_pass;

    if (m_vao != 0) {
	glDeleteVertexArrays (1, &m_vao);
    }
    if (m_vbo != 0) {
	glDeleteBuffers (1, &m_vbo);
    }
    if (m_ebo != 0) {
	glDeleteBuffers (1, &m_ebo);
    }

    m_vertices.clear ();
    m_indices.clear ();
}

void CParticle::setup () {
    if (m_initialized) {
	return;
    }

    // Convert origin from screen space to centered space
    // Projection uses ortho(-width/2, width/2, -height/2, height/2)
    // but particle origins are in screen space where (0,0) is top-left
    m_lastScreenWidth = getScene ().getCamera ().getWidth ();
    m_lastScreenHeight = getScene ().getCamera ().getHeight ();

    refreshTransform ();
    takeParentTransform ();

    // Load particle material constants
    if (m_particle.material && m_particle.material->material && !m_particle.material->material->passes.empty ()) {
	auto& firstPass = *m_particle.material->material->passes.begin ();

	// Read overbright constant (brightness multiplier for additive particles)
	auto overbrightIt = firstPass->constants.find ("ui_editor_properties_overbright");
	if (overbrightIt != firstPass->constants.end ()) {
	    m_overbright = overbrightIt->second->value->getFloat ();
	}
    }

    // Texture is resolved by CRenderable base class; read spritesheet data.
    // TextureParser computes spritesheet grid from TEXS frame data (animated textures)
    // or .tex-json metadata (static textures). For GIF-style animated textures (separate
    // GL texture per frame), the parser returns 0 cols/rows since a 1x1 grid can't hold
    // all frames — so no SPRITESHEET mode is needed (frame switching happens via texture ID).
    if (const auto texture = getTexture ()) {
	m_spritesheetCols = static_cast<int> (texture->getSpritesheetCols ());
	m_spritesheetRows = static_cast<int> (texture->getSpritesheetRows ());
	m_spritesheetFrames = static_cast<int> (texture->getSpritesheetFrames ());

	m_spritesheetDuration = texture->getSpritesheetDuration ();
    }

    setupEmitters ();
    setupInitializers ();
    setupOperators ();
    if (!m_simulateOnly) {
	setupPass ();
    }

    // colorn's reference colour (GH 0x1401f2210): the first colour initializer, else the first colorchange
    for (const auto& initializer : m_particle.initializers) {
	if (initializer && initializer->is<ColorRandomInitializer> ()) {
	    m_referenceColorRandom = initializer->as<ColorRandomInitializer> ();
	    break;
	}
    }
    if (m_referenceColorRandom == nullptr) {
	for (const auto& op : m_particle.operators) {
	    if (op && op->is<ColorChangeOperator> ()) {
		m_referenceColorChange = op->as<ColorChangeOperator> ();
		break;
	    }
	}
    }
    m_hasColor = m_referenceColorRandom != nullptr || m_referenceColorChange != nullptr;

    // Setup control points (max 8)
    m_controlPoints.resize (8);
    for (const auto& cp : m_particle.controlPoints) {
	if (cp.id >= 0 && cp.id < 8) {
	    m_ownControlPoints |= 1u << cp.id;
	    m_controlPoints[cp.id].offset = cp.offset;
	    m_controlPoints[cp.id].flags = cp.flags;
	    // Link to mouse if either flags bit 0 is set
	    m_controlPoints[cp.id].linkMouse = (cp.flags & 1) != 0;
	    // bit 1: the offset is in the scene's space; control point 0 ignores it (GH 0x1402504f0)
	    m_controlPoints[cp.id].worldSpace = (cp.flags & 2) != 0 && cp.id != 0;

	    // Initialize position to offset for non-mouse-linked control points
	    // Mouse-linked CPs will have their position updated in update()
	    if (!m_controlPoints[cp.id].linkMouse) {
		if (m_controlPoints[cp.id].worldSpace) {
		    // World space: offset is in screen-centered coords, convert to particle local space
		    m_controlPoints[cp.id].position = cp.offset - m_transformedOrigin;
		} else {
		    // Local space: offset is already relative to particle system center
		    m_controlPoints[cp.id].position = cp.offset;
		}
	    }
	}
    }

    m_opControlPoints = m_controlPoints;

    // the child systems (P2): a static one runs from the start; event ones come with particle events
    for (const auto& child : m_particle.children) {
	if (child.particle == nullptr) {
	    continue;
	}
	ChildSet set;
	set.child = &child;
	set.type = child.type == "eventfollow" ? 2 : child.type == "eventspawn" ? 3 : child.type == "eventdeath" ? 4 : 1;
	m_hasEventChildren |= set.type != 1;
	m_children.push_back (std::move (set));
    }

    m_initialized = true;
    if (m_drawOnly) {
	return;
    }

    for (auto& set : m_children) {
	if (set.type == 1) {
	    auto instance = makeChild (set, glm::vec3 (0.0f));
	    if (instance) {
		set.active.push_back (std::move (instance));
	    }
	}
    }

    prewarm ();
}

void CParticle::prewarm () {
    // `starttime` prewarm (GH 0x140254cc0): step the system in 0.05 s steps (0.2 s from maxcount 500 up)
    // until the simulated time reaches starttime, so it starts at its steady state instead of empty. At
    // least one step, as WE's do-while; not scaled by the `rate` override (that is applied per frame). WE
    // prewarms every new or reused child instance the same way
    if (m_particle.startTime > 0.0f) {
	const float step = m_particle.maxCount < 500 ? 0.05f : 0.2f;
	float simulated = 0.0f;
	do {
	    update (step);
	    simulated += step;
	} while (simulated < m_particle.startTime);
    }
}

void CParticle::takeParentTransform () {
    if (m_parentSystem == nullptr) {
	return;
    }
    m_transformedOrigin = m_parentSystem->m_transformedOrigin;
    m_resolvedScale = m_parentSystem->m_resolvedScale;
    m_resolvedAngle = m_parentSystem->m_resolvedAngle;
    m_transformValid = true;
}

std::unique_ptr<CParticle> CParticle::makeChild (ChildSet& set, const glm::vec3& at) {
    std::unique_ptr<CParticle> instance;
    if (!set.pool.empty ()) {
	// a finished instance is reused: reset (GH 0x140254f50) and prewarmed again
	instance = std::move (set.pool.back ());
	set.pool.pop_back ();
	instance->m_childOffset = at + instance->m_childOrigin;
	instance->m_boundId = -1;
	instance->resetSystem ();
	instance->m_simTime = 0.0f;
	instance->prewarm ();
	return instance;
    }
    try {
	instance = std::make_unique<CParticle> (getScene (), *set.child->particle, *this, *set.child, set.type != 1);
	glm::vec3 origin = set.child->origin;
	origin.y = -origin.y;
	instance->m_childOrigin = origin;
	instance->m_childOffset = at + origin;
	instance->setup ();
    } catch (const std::exception& e) {
	sLog.error ("Cannot create child particle system ", set.child->particleFile, ": ", e.what ());
	WPENGINE_HEALTH ("asset_error", { { "kind", "particle" }, { "path", set.child->particleFile }, { "message", "child system not created: " + WallpaperEngine::Health::brief (e.what ()) } });
	instance.reset ();
    }
    return instance;
}

bool CParticle::childAlive () const {
    // WE keeps an event child while it has particles or an emitter that can still emit (GH 0x1402524c0, inferred)
    if (m_particleCount > 0) {
	return true;
    }
    for (const auto& [emitter, state] : m_emitterStates) {
	if (state->stopped) {
	    continue;
	}
	if (emitter->duration > 0.0f && state->durationLeft <= 0.0f) {
	    continue;
	}
	if (emitter->rate > 0.0f || state->burst > 0 || (emitter->flags & 4) != 0 || state->delay > 0.0f) {
	    return true;
	}
    }
    for (const auto& set : m_children) {
	for (const auto& instance : set.active) {
	    if (instance->childAlive ()) {
		return true;
	    }
	}
    }
    return false;
}

void CParticle::stopEmitters () {
    // a follow child whose particle died stops emitting, it and its children (GH 0x140254ed0)
    for (auto& [emitter, state] : m_emitterStates) {
	state->stopped = true;
    }
    for (auto& set : m_children) {
	for (auto& instance : set.active) {
	    instance->stopEmitters ();
	}
    }
}

void CParticle::spawnEventChildren (int type, const ParticleInstance& source) {
    const uint32_t id = source.id;
    const glm::vec3& at = source.position;
    // per particle event, each event child of that type under its maxcount: a probability roll, then an instance
    // (GH 0x14025cfe0 for follow / spawn, GH 0x14025c2e0 for death)
    for (auto& set : m_children) {
	if (set.type != type && !(type == 3 && set.type == 2)) {
	    continue;
	}
	if (static_cast<int> (set.active.size ()) >= set.child->maxCount) {
	    continue;
	}
	if (WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) > set.child->probability) {
	    continue;
	}
	auto instance = makeChild (set, at);
	if (!instance) {
	    continue;
	}
	if (set.type == 2) {
	    instance->m_boundId = id;
	}
	instance->m_eventSource = source;
	set.active.push_back (std::move (instance));
    }
}

void CParticle::updateChildren (float dt) {
    if (m_children.empty ()) {
	return;
    }
    // where the particles a follow child is bound to are now
    std::unordered_map<uint32_t, uint32_t> where;
    bool follows = false;
    for (const auto& set : m_children) {
	follows |= set.type == 2 && !set.active.empty ();
    }
    if (follows) {
	where.reserve (m_particleCount);
	for (uint32_t i = 0; i < m_particleCount; i++) {
	    where.emplace (m_particles[i].id, i);
	}
    }
    for (auto& set : m_children) {
	for (size_t i = 0; i < set.active.size ();) {
	    auto& instance = set.active[i];
	    if (set.type == 2 && instance->m_boundId >= 0) {
		const auto it = where.find (static_cast<uint32_t> (instance->m_boundId));
		if (it != where.end ()) {
		    instance->m_childOffset = m_particles[it->second].position + instance->m_childOrigin;
		    instance->m_eventSource = m_particles[it->second];
		} else {
		    // its particle died: it stops emitting and lives out its particles (GH 0x14025c2e0, type 2)
		    instance->m_boundId = -1;
		    instance->stopEmitters ();
		}
	    }
	    instance->m_frameTime = m_frameTime;
	    instance->takeParentTransform ();
	    instance->update (dt);
	    if (set.type != 1 && !instance->childAlive ()) {
		set.pool.push_back (std::move (instance));
		set.active.erase (set.active.begin () + static_cast<std::ptrdiff_t> (i));
		continue;
	    }
	    i++;
	}
    }
}

void CParticle::drawFrom (const std::vector<const CParticle*>& instances) {
    // draws other instances' particles in this system's pass: all at once for sprites, one instance at a time for
    // ropes (a rope joins its particles in order)
    if (m_pass == nullptr) {
	return;
    }
    auto draw = [this] (uint32_t count) {
	m_particleCount = count;
	if (count > 0) {
	    if (m_useRopeRenderer) {
		renderRope ();
	    } else {
		renderSprites ();
	    }
	}
	m_particleCount = 0;
    };
    if (m_useRopeRenderer) {
	for (const auto* instance : instances) {
	    const uint32_t n = std::min<uint32_t> (instance->m_particleCount, static_cast<uint32_t> (m_particles.size ()));
	    std::copy_n (instance->m_particles.begin (), n, m_particles.begin ());
	    draw (n);
	}
	return;
    }
    uint32_t total = 0;
    for (const auto* instance : instances) {
	total += instance->m_particleCount;
    }
    if (total > m_particles.size ()) {
	m_particles.resize (total);
	m_vertices.resize (static_cast<size_t> (total) * 4 * SPRITE_FLOATS_PER_VERTEX);
	m_indices.resize (static_cast<size_t> (total) * 6);
    }
    uint32_t at = 0;
    for (const auto* instance : instances) {
	std::copy_n (instance->m_particles.begin (), instance->m_particleCount, m_particles.begin () + at);
	at += instance->m_particleCount;
    }
    draw (total);
}

void CParticle::drawChildren () {
    for (auto& set : m_children) {
	if (set.type == 1) {
	    for (auto& instance : set.active) {
		if (instance->m_particleCount > 0 && instance->m_pass != nullptr) {
		    if (instance->m_useRopeRenderer) {
			instance->renderRope ();
		    } else {
			instance->renderSprites ();
		    }
		}
		instance->drawChildren ();
	    }
	    continue;
	}
	if (set.active.empty ()) {
	    continue;
	}
	if (!set.renderer) {
	    try {
		set.renderer = std::make_unique<CParticle> (getScene (), *set.child->particle, *this, *set.child, false);
		set.renderer->m_drawOnly = true;
		set.renderer->setup ();
	    } catch (const std::exception& e) {
		sLog.error ("Cannot draw child particle system ", set.child->particleFile, ": ", e.what ());
		set.renderer.reset ();
		continue;
	    }
	}
	std::vector<const CParticle*> instances;
	instances.reserve (set.active.size ());
	for (const auto& instance : set.active) {
	    instances.push_back (instance.get ());
	}
	set.renderer->drawFrom (instances);
	for (auto& instance : set.active) {
	    instance->drawChildren ();
	}
    }
}

void CParticle::resetSystem () {
    m_particleCount = 0;
    for (auto& p : m_particles) {
	p.alive = false;
    }
    for (auto& [emitter, state] : m_emitterStates) {
	resetEmitter (*emitter, *state);
    }
    // and its children's instances (GH 0x140255430): event ones go back to the pool, static ones restart
    for (auto& set : m_children) {
	for (size_t i = 0; i < set.active.size ();) {
	    if (set.type == 1) {
		set.active[i]->resetSystem ();
		i++;
	    } else {
		set.active[i]->resetSystem ();
		set.pool.push_back (std::move (set.active[i]));
		set.active.erase (set.active.begin () + static_cast<std::ptrdiff_t> (i));
	    }
	}
    }
}

void CParticle::render () {
    if (!m_initialized) {
	return;
    }

    if (!getScene ().isEffectivelyVisible (m_particle)) {
	// a hidden system (or one under a hidden ancestor: GH 0x140255d00 asks GH 0x1401b50e0) is reset (GH 0x140255d00 → 0x140254f50 / 0x140255430): its particles are cleared and its
	// emitters reloaded, so it starts fresh when shown again (bursts replay). It doesn't advance meanwhile,
	// and the clock follows so the first visible frame's dt is one frame, not the hidden time
	if (m_particleCount > 0) {
	    resetSystem ();
	}
	if (m_time != 0.0) {
	    m_time = this->getContext ().getContext ().renderTime;
	}
	return;
    }

    // the system's place this frame: its own origin/scale/angle under its parents' (live: animated, scripted)
    refreshTransform ();

    // Initialize time on first render to avoid huge dt spike
    if (m_time == 0.0) {
	m_time = this->getContext ().getContext ().renderTime;
	// Skip update on first frame to avoid weird initial burst
	// This ensures all particles start from a clean state
	if (m_useRopeRenderer) {
	    renderRope ();
	} else {
	    renderSprites ();
	}
	return;
    }

    // Update particles
    float dt = this->getContext ().getContext ().renderTime - static_cast<float> (m_time);
    m_time = this->getContext ().getContext ().renderTime;

    if (dt > 0.0f) {
	// Cap dt to prevent simulation instability
	// Also provides more consistent behavior across different FPS
	dt = std::min (dt, 0.1f);
	// the `rate` override is the system's playback speed: it scales the whole step (motion, ageing and
	// emission alike), at least 0.01 (GH 0x140255d00: dt *= max(rate, 0.01)); read every frame
	m_frameTime = dt;
	dt *= std::max (m_particle.instanceOverride.rate->value->getFloat (), 0.01f);
	update (dt);
    }

    // Render particles
    if (m_particleCount > 0 && m_particle.material) {
	if (m_useRopeRenderer) {
	    renderRope ();
	} else {
	    renderSprites ();
	}
    }
    // then the child systems' (drawn after their parent: inferred), in the parent's matrices
    if (!m_children.empty ()) {
	updateMatrices ();
	drawChildren ();
    }
}

namespace {
// WE's RGB -> HSV (inlined in GH 0x1401ff240): h in [0, 1), s = (max − min) / max, v = max; grey (or black) is h = s = 0
glm::vec3 rgbToHsv (const glm::vec3& c) {
    const float max = std::max ({ c.r, c.g, c.b });
    const float min = std::min ({ c.r, c.g, c.b });
    const float d = max - min;
    if (d < 1e-5f || max <= 0.0f) {
	return { 0.0f, 0.0f, max };
    }
    float h;
    if (c.r >= max) {
	h = (c.g - c.b) / d;
    } else if (c.g >= max) {
	h = (c.b - c.r) / d + 2.0f;
    } else {
	h = (c.r - c.g) / d + 4.0f;
    }
    h /= 6.0f;
    if (h < 0.0f) {
	h += 1.0f;
    }
    return { h, d / max, max };
}

// WE's HSV -> RGB (GH 0x1401e57b0)
glm::vec3 hsvToRgb (const glm::vec3& hsv) {
    const float c = hsv.z * hsv.y;
    const float hp = std::fmod (hsv.x * 6.0f, 6.0f);
    const float x = c * (1.0f - std::abs (std::fmod (hp, 2.0f) - 1.0f));
    const float m = hsv.z - c;
    glm::vec3 rgb (0.0f);
    if (hp >= 0.0f && hp < 1.0f) {
	rgb = { c, x, 0.0f };
    } else if (hp >= 1.0f && hp < 2.0f) {
	rgb = { x, c, 0.0f };
    } else if (hp >= 2.0f && hp < 3.0f) {
	rgb = { 0.0f, c, x };
    } else if (hp >= 3.0f && hp < 4.0f) {
	rgb = { 0.0f, x, c };
    } else if (hp >= 4.0f && hp < 5.0f) {
	rgb = { x, 0.0f, c };
    } else if (hp >= 5.0f && hp < 6.0f) {
	rgb = { c, 0.0f, x };
    }
    return rgb + m;
}
} // namespace

void CParticle::patchColor () {
    // the override patcher (GH 0x140251f70 / 0x1401ff240), re-run every step so a colorn bound to a user
    // property, a script or an animation is live. colorn tints only if the object sets it, the particle flags
    // lack 8, and it differs from the reference colour by 0.9/255 or more in some channel
    glm::vec3 reference (1.0f);
    if (m_referenceColorRandom != nullptr) {
	reference = (m_referenceColorRandom->min->value->getVec3 () + m_referenceColorRandom->max->value->getVec3 ()) * 0.5f;
    } else if (m_referenceColorChange != nullptr) {
	reference = m_referenceColorChange->startValue->value->getVec3 ();
    }
    const glm::vec3 colorn = m_particle.instanceOverride.colorn->value->getVec3 ();
    const glm::vec3 diff = glm::abs (colorn - reference);
    m_tint = m_particle.instanceOverride.colornSet && (m_particle.flags & 8) == 0 && colorn.r >= 0.0f
	  && (diff.r >= 0.0035294118f || diff.g >= 0.0035294118f || diff.b >= 0.0035294118f);
    // a system without a colour source is tinted by its spawn colour (template flag 0x200000)
    m_spawnColor = m_tint && !m_hasColor ? colorn : glm::vec3 (1.0f);
    if (m_tint) {
	m_colornHSV = rgbToHsv (colorn);
	m_referenceHSV = rgbToHsv (reference);
    }
}

glm::vec3 CParticle::tintColor (const glm::vec3& color) const {
    // WE's patch op 0xb (GH 0x1401ff240): the colour moves in HSV by (colorn − reference); a colour too grey
    // to have a hue (s < 0.01) takes colorn's hue; s and v are clamped to [0, 1]
    const glm::vec3 hsv = rgbToHsv (color);
    float h = m_colornHSV.x;
    if (hsv.y >= 0.01f) {
	h = hsv.x + m_colornHSV.x - m_referenceHSV.x;
	h -= std::floor (h);
    }
    const float sat = glm::clamp (hsv.y + m_colornHSV.y - m_referenceHSV.y, 0.0f, 1.0f);
    const float val = glm::clamp (hsv.z + m_colornHSV.z - m_referenceHSV.z, 0.0f, 1.0f);
    return hsvToRgb ({ h, sat, val });
}

void CParticle::update (float dt) {
    m_simTime += dt;
    patchColor ();
    m_spawned.clear ();

    // Detect resolution changes and recalculate transformed origin
    float screenWidth = static_cast<float> (getScene ().getWidth ());
    float screenHeight = static_cast<float> (getScene ().getHeight ());

    if (screenWidth != m_lastScreenWidth || screenHeight != m_lastScreenHeight) {
	// Resolution changed - recalculate transformed origin (and the world-space control points)
	m_lastScreenWidth = screenWidth;
	m_lastScreenHeight = screenHeight;
	refreshTransform ();
    }

    // a world-space system (system flags bit 0, the editor's "Worldspace" — wallpaperui.exe lists it among the system's
    // flags: disable colour / lifetime / count / speed / size overrides are 8 / 0x40 / 0x20 / 0x10 / 0x80, bit 1 the
    // sheet blend, bit 2 perspective): its particles stay where they were emitted in the scene when the system moves,
    // turns or scales (a pointer-following emitter leaves a trail). Inferred: LWE keeps them in the system's space and
    // carries them across each change of its transform (parallax, a view effect, excluded)
    if (m_parentSystem == nullptr && (m_particle.flags & 1) != 0) {
	const glm::mat4 now = this->systemMatrix ();
	if (m_worldSpaceSeen && now != m_worldSpaceLast) {
	    const glm::mat4 carry = glm::inverse (now) * m_worldSpaceLast;
	    const glm::mat3 linear (carry);
	    for (uint32_t i = 0; i < m_particleCount; i++) {
		auto& p = m_particles[i];
		p.position = glm::vec3 (carry * glm::vec4 (p.position, 1.0f));
		p.velocity = linear * p.velocity;
		p.acceleration = linear * p.acceleration;
	    }
	}
	m_worldSpaceLast = now;
	m_worldSpaceSeen = true;
    }

    // the control points this step (GH 0x140251f70 → 0x1402504f0). An instanceoverride `controlpointN` replaces the
    // offset unless the control point follows the pointer or has flags 0x10004; live, so a slider or script moves it
    // (P10). A scene-space point (bit 1, not control point 0) is turned into the system's space by the inverse of
    // its whole transform, and so is a pointer one (P34, P35: they ignored the system's scale and angles)
    const glm::mat3 toLocal = localFromScene ();
    const glm::vec2* mousePos = getScene ().getMousePosition ();
    for (int i = 0; i < 8; i++) {
	auto& cp = m_controlPoints[i];
	glm::vec3 offset = cp.offset;
	const auto& override = m_particle.instanceOverride.controlPoint[i];
	if (override != nullptr && (cp.flags & 0x10005) == 0) {
	    offset = override->value->getVec3 ();
	}
	if (cp.linkMouse) {
	    if (mousePos) {
		// the pointer (normalised, y down from the top: the host's) in this centred, y-down space, as the
		// origin (refreshTransform); it was y up, so a trail went up when the pointer went down (3414858021)
		glm::vec3 position;
		position.x = (mousePos->x * screenWidth) - (screenWidth / 2.0f);
		position.y = (mousePos->y * screenHeight) - (screenHeight / 2.0f);
		position.z = 0.0f;
		// its offset, like any, is WE's y-up one
		cp.position = toLocal * (position + glm::vec3 (offset.x, -offset.y, offset.z) - m_transformedOrigin);
	    }
	} else if (cp.worldSpace) {
	    // the scene's space (WE's world: y up from the bottom-left) to this centred one, as the origin's
	    const glm::vec3 centred (offset.x - screenWidth / 2.0f, screenHeight / 2.0f - offset.y, offset.z);
	    cp.position = toLocal * (centred - m_transformedOrigin);
	} else {
	    // WE's particle space is y-up: mirrored, as the emitters' origins
	    cp.position = glm::vec3 (offset.x, -offset.y, offset.z);
	}
    }

    // the operators' control points, in the parent's space for a child: its own ones (in its space: an emitter's
    // default control point 0 is the child's origin) and, with a controlpointstartindex, its parent's from there on
    // for those it doesn't define (FUN_140250a00; inferred)
    if (m_parentSystem != nullptr) {
	const int start = m_childDef->controlPointStartIndex;
	for (int i = 0; i < 8; i++) {
	    auto& cp = m_controlPoints[i];
	    glm::vec3 parentSpace;
	    if ((m_ownControlPoints & (1u << i)) == 0 && start > 0 && start + i < 8) {
		parentSpace = m_parentSystem->m_opControlPoints[start + i].position;
		cp.position = m_childLinearInverse * (parentSpace - m_childOffset);
	    } else if (cp.linkMouse) {
		parentSpace = cp.position;
		cp.position = m_childLinearInverse * (parentSpace - m_childOffset);
	    } else {
		parentSpace = m_childOffset + m_childLinear * cp.position;
	    }
	    m_opControlPoints[i] = cp;
	    m_opControlPoints[i].position = parentSpace;
	}
    } else {
	m_opControlPoints = m_controlPoints;
    }
    // the control points' velocities this step (inheritcontrolpointvelocity) and last positions (the constraints)
    for (int i = 0; i < 8; i++) {
	const glm::vec3 now = m_opControlPoints[i].position;
	m_controlPointVelocity[i] = m_controlPointsSeen && dt > 0.0f ? (now - m_controlPointLast[i]) / dt : glm::vec3 (0.0f);
    }

    // Emit particles
    for (auto& emitter : m_emitters) {
	emitter (m_particles, m_particleCount, dt);
    }
    // the spawned particles' follow and spawn children (GH 0x14025cfe0)
    if (m_hasEventChildren) {
	for (const auto& source : m_spawned) {
	    spawnEventChildren (3, source);
	}
    }

    // Update particle age; size, alpha and colour go back to their spawn values for the operators to multiply
    // (the head of WE's operator interpreter, GH 0x140265800)
    for (uint32_t i = 0; i < m_particleCount; i++) {
	auto& p = m_particles[i];
	p.age += dt;
	p.size = p.initial.size;
	p.alpha = p.initial.alpha;
	p.color = p.initial.color;
    }

    // the forces' step (the interpreter's third argument, GH 0x14025c2e0): dt · min (0.025 / frame time, 1)^0.7, so
    // drag and the forces weaken below 40 fps; the frame time is inferred to be the real one (WE: host +0x14c)
    m_forceDt = dt * std::pow (std::min (0.025f / std::max (m_frameTime, 1e-4f), 1.0f), 0.7f);

    // Apply operators to living particles (including alphafade)
    for (auto& op : m_operators) {
	op (m_particles, m_particleCount, m_opControlPoints, m_simTime, dt);
    }
    for (int i = 0; i < 8; i++) {
	m_controlPointLast[i] = m_opControlPoints[i].position;
    }
    m_controlPointsSeen = true;

    // Update animation frames
    for (uint32_t i = 0; i < m_particleCount; i++) {
	auto& p = m_particles[i];

	if (m_spritesheetFrames > 0) {
	    float lifetimePos = p.getLifetimePos ();
	    float animSpeed = m_particle.sequenceMultiplier > 0.0f ? m_particle.sequenceMultiplier : 1.0f;

	    if (m_particle.animationMode == "randomframe") {
		if (p.frame < 0.0f) {
		    // one draw from the scene's random source per particle (WE: the spawn's own random value,
		    // GH 0x140260d80); was seeded by the particle's slot address, so a reused slot always showed
		    // the same frame
		    std::uniform_int_distribution<int> dist (0, m_spritesheetFrames - 1);
		    p.frame = static_cast<float> (dist (m_rng));
		}
	    } else if (m_particle.animationMode == "once") {
		p.frame = std::min (
		    lifetimePos * m_spritesheetFrames * animSpeed, static_cast<float> (m_spritesheetFrames - 1)
		);
	    } else {
		if (m_spritesheetDuration > 0.0f) {
		    float timeInCycle = std::fmod (p.age * animSpeed, m_spritesheetDuration);
		    float cyclePos = timeInCycle / m_spritesheetDuration;
		    p.frame = std::fmod (cyclePos * m_spritesheetFrames, static_cast<float> (m_spritesheetFrames));
		} else {
		    p.frame = std::fmod (
			lifetimePos * m_spritesheetFrames * animSpeed, static_cast<float> (m_spritesheetFrames)
		    );
		}
	    }
	}
    }

    // Remove dead particles with order-preserving compaction.
    // Particles only die from natural lifetime expiry (age >= lifetime).
    // We never kill based on size — particles may fade in/out with oscillating size.
    // Compaction preserves spawn order so array index 0 is always the oldest particle.
    uint32_t writeIdx = 0;
    std::vector<ParticleInstance> died;
    for (uint32_t readIdx = 0; readIdx < m_particleCount; readIdx++) {
	if (m_particles[readIdx].isAlive ()) {
	    if (writeIdx != readIdx) {
		m_particles[writeIdx] = m_particles[readIdx];
	    }
	    writeIdx++;
	} else if (m_hasEventChildren) {
	    died.push_back (m_particles[readIdx]);
	}
    }
    m_particleCount = writeIdx;

    // the children: follow ones move with their particles, then every instance steps; dead particles' death
    // children start where they died (GH 0x14025c2e0)
    updateChildren (dt);
    for (const auto& source : died) {
	spawnEventChildren (4, source);
    }
}

const Particle& CParticle::getParticle () const { return m_particle; }

const float& CParticle::getBrightness () const { return m_overbright; }

const float& CParticle::getUserAlpha () const { return m_particle.instanceOverride.alpha->value->getFloat (); }

const float& CParticle::getAlpha () const { return m_particle.instanceOverride.alpha->value->getFloat (); }

const glm::vec3& CParticle::getColor () const {
    static const glm::vec3 defaultColor (1.0f);
    if (m_particle.instanceOverride.color && m_particle.instanceOverride.color->value) {
	return m_particle.instanceOverride.color->value->getVec3 ();
    }
    return defaultColor;
}

const glm::vec4& CParticle::getColor4 () const {
    static const glm::vec4 defaultColor (1.0f);
    if (m_particle.instanceOverride.color && m_particle.instanceOverride.color->value) {
	return m_particle.instanceOverride.color->value->getVec4 ();
    }
    return defaultColor;
}

const glm::vec3& CParticle::getCompositeColor () const { return getColor (); }

// ========== EMITTERS ==========

void CParticle::setupEmitters () {
    for (const auto& emitter : m_particle.emitters) {
	EmitterFunc func;

	if (emitter.name == "boxrandom") {
	    func = createBoxEmitter (emitter);
	} else if (emitter.name == "sphererandom") {
	    func = createSphereEmitter (emitter);
	} else {
	    sLog.out ("Unknown emitter type: ", emitter.name);
	    WPENGINE_HEALTH ("unsupported", { { "what", "particle emitter " + emitter.name } });
	    continue;
	}

	if (func) {
	    m_emitters.push_back (std::move (func));
	}
    }
}

std::shared_ptr<CParticle::EmitterState> CParticle::addEmitterState (const ParticleEmitter& emitter) {
    auto state = std::make_shared<EmitterState> ();
    resetEmitter (emitter, *state);
    m_emitterStates.emplace_back (&emitter, state);
    return state;
}

void CParticle::resetEmitter (const ParticleEmitter& emitter, EmitterState& state) {
    // WE reloads delay, duration and the instantaneous burst and zeroes the accumulators (GH 0x140254f50)
    state.delay = emitter.delay;
    state.durationLeft = emitter.duration;
    state.accumulator = 0.0f;
    state.periodicTimer = 0.0f;
    state.burst = emitter.instantaneous;
    state.stopped = false;
}

void CParticle::scriptPlay () { m_scriptHalted = false; }

void CParticle::scriptPause () { m_scriptHalted = true; }

void CParticle::scriptStop () {
    m_scriptHalted = true;
    m_particleCount = 0;
    for (auto& p : m_particles) {
	p.alive = false;
    }
}

bool CParticle::scriptIsPlaying () const { return !m_scriptHalted || m_particleCount > 0; }

void CParticle::scriptEmit (uint32_t count) { m_scriptEmit += count; }

DynamicValue* CParticle::instanceValue (const std::string& name) {
    auto& override = const_cast<ParticleInstanceOverride&> (m_particle.instanceOverride);
    const std::pair<const char*, UserSettingUniquePtr*> values[] = {
	{ "alpha", &override.alpha }, { "size", &override.size },	  { "count", &override.count },
	{ "speed", &override.speed }, { "lifetime", &override.lifetime }, { "rate", &override.rate },
	{ "colorn", &override.colorn },
    };
    for (const auto& [key, setting] : values) {
	if (name == key) {
	    return *setting == nullptr ? nullptr : (*setting)->value.get ();
	}
    }
    // controlpointN: the override of that control point's offset, made from its offset when the scene sets none
    if (name.rfind ("controlpoint", 0) == 0 && name.size () == 13 && name[12] >= '0' && name[12] <= '7') {
	const int index = name[12] - '0';
	auto& setting = override.controlPoint[index];
	if (setting == nullptr) {
	    const glm::vec3 offset = index < static_cast<int> (m_controlPoints.size ()) ? m_controlPoints[index].offset : glm::vec3 (0.0f);
	    setting = std::make_unique<UserSetting> (UserSetting { .value = std::make_unique<DynamicValue> (offset) });
	}
	return setting->value.get ();
    }
    return nullptr;
}

uint32_t CParticle::emitCount (const ParticleEmitter& emitter, EmitterState& state, float dt) {
    // emitParticles: a script's particles come first, paused, stopped or not
    const uint32_t asked = m_scriptEmit;
    m_scriptEmit = 0;
    if (m_scriptHalted) {
	return asked;
    }
    return asked + emitterCount (emitter, state, dt);
}

uint32_t CParticle::emitterCount (const ParticleEmitter& emitter, EmitterState& state, float dt) {
    // WE's emitter step (GH 0x14025cfe0), shared by every emitter type; a stopped emitter (bit 31) emits nothing
    if (state.stopped) {
	return 0;
    }
    if (state.delay > 0.0f) {
	state.delay -= dt;
	return 0;
    }

    // `count` scales the emitter's rate (parser binding of the override's count, 0x1401f3efc), unless the
    // system opts out with flags bit 0x20; read every frame, so a user slider is live
    float rate = emitter.rate;
    if ((m_particle.flags & 0x20) == 0) {
	rate *= m_particle.instanceOverride.count->value->getFloat ();
    }

    // an audio-responsive emitter's rate follows the music (GH 0x14025cfe0 × GH 0x140250d80)
    if (emitter.audioProcessingMode != 0) {
	rate *= this->audioLevel (emitter);
    }

    // random periodic emission (flags bit 2): an "on" window of minperiodicduration..maxperiodicduration, then an
    // "off" one of minperiodicdelay..maxperiodicdelay; each "on" window reloads the instantaneous burst
    if ((emitter.flags & 4) != 0) {
	if (state.periodicTimer <= 0.0f) {
	    state.periodicTimer += dt;
	    if (state.periodicTimer < 0.0f) {
		rate = 0.0f;
	    } else {
		state.periodicTimer = WallpaperEngine::Maths::randomFloat (
		    m_rng, emitter.minPeriodicDuration, emitter.maxPeriodicDuration
		);
		state.burst = emitter.instantaneous;
	    }
	} else {
	    state.periodicTimer -= dt;
	    if (state.periodicTimer < 0.0f) {
		state.periodicTimer
		    = -WallpaperEngine::Maths::randomFloat (m_rng, emitter.minPeriodicDelay, emitter.maxPeriodicDelay);
	    }
	}
    }

    // the instantaneous burst adds to the frame's rate emission (sphererandom used to replace it)
    uint32_t toEmit = state.burst;
    state.burst = 0;

    if (emitter.duration > 0.0f) {
	state.durationLeft -= dt;
	if (state.durationLeft <= 0.0f) {
	    return toEmit;
	}
    }

    if (rate > 0.0f) {
	state.accumulator += dt * rate;
	auto rateEmit = static_cast<uint32_t> (state.accumulator);
	state.accumulator -= static_cast<float> (rateEmit);
	// limitOnePerFrame (flags bit 1)
	if ((emitter.flags & 2) != 0 && rateEmit > 1) {
	    rateEmit = 1;
	}
	toEmit += rateEmit;
    }

    return toEmit;
}

float CParticle::audioLevel (const ParticleEmitter& emitter) const {
    // GH 0x140250d80 on the 16-band spectrum: the loudest band of audioprocessingfrequencystart..end (clamped to
    // 0..15, swapped when reversed; parser GH 0x1401ef740) of the left (mode 1), the right (mode 2) or both
    // channels' mean (any other mode), through audioprocessingbounds, a smoothstep and the exponent
    const auto& recorder = getScene ().getAudioContext ().getRecorder ();
    int start = std::clamp (emitter.audioProcessingFrequencyStart, 0, 15);
    int end = std::clamp (emitter.audioProcessingFrequencyEnd, 0, 15);
    if (end < start) {
	std::swap (start, end);
    }
    const float* left = recorder.audio16 [Audio::PlaybackRecorder::Left];
    const float* right = recorder.audio16 [Audio::PlaybackRecorder::Right];
    float level = 0.0f;
    for (int i = start; i <= end; i++) {
	switch (emitter.audioProcessingMode) {
	    case 1:
		level = std::max (level, left [i]);
		break;
	    case 2:
		level = std::max (level, right [i]);
		break;
	    default:
		level = std::max (level, left [i] + right [i]);
		break;
	}
    }
    if (emitter.audioProcessingMode != 1 && emitter.audioProcessingMode != 2) {
	level *= 0.5f;
    }
    const glm::vec2 bounds = emitter.audioProcessingBounds;
    const double t = std::clamp (
	static_cast<double> ((level - bounds.x) / (bounds.y - bounds.x)), 0.0, 1.0
    );
    const float s = static_cast<float> (t);
    const float shaped = std::pow ((3.0f - (s + s)) * s * s, emitter.audioProcessingExponent);
    return std::isfinite (shaped) ? std::clamp (shaped, 0.0f, 1.0f) : 0.0f;
}

EmitterFunc CParticle::createBoxEmitter (const ParticleEmitter& emitter) {
    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) {
	    controlPointIndex = 0;
	}
    }

    glm::vec3 flippedDirections = emitter.directions;
    flippedDirections.y = -flippedDirections.y;

    const auto state = addEmitterState (emitter);

    return [this, emitter, transformedEmitterOrigin, controlPointIndex, flippedDirections,
	    state] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) {
	    const uint32_t toEmit = emitCount (emitter, *state, dt);

	    // Emit particles
	    for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
		auto& p = particles[count];

		glm::vec3 spawnOrigin = transformedEmitterOrigin;
		if (controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())) {
		    spawnOrigin += m_controlPoints[controlPointIndex].position;
		}

		// Generate random position within box volume centered on origin
		// This creates a centered box (or hollow box if distanceMin > 0)
		glm::vec3 randomPos;
		for (int axis = 0; axis < 3; axis++) {
		    float minDist = emitter.distanceMin[axis];
		    float maxDist = emitter.distanceMax[axis];
		    // Generate value in [minDist, maxDist]
		    float dist = WallpaperEngine::Maths::randomFloat (m_rng, minDist, maxDist);
		    // Randomly flip sign to center the distribution
		    if (WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f) < 0.5f) {
			dist = -dist;
		    }
		    randomPos[axis] = dist;
		}
		randomPos *= flippedDirections;

		p.position = spawnOrigin + randomPos;

		// boxrandom's speedmin/speedmax (× speed): outward from the box's centre, a random direction when the
		// particle is at it (GH 0x14025cfe0, record +0xa0/+0xa4); box particles started at rest
		glm::vec3 velocity (0.0f);
		if (emitter.speedMax != 0.0f || emitter.speedMin != 0.0f) {
		    glm::vec3 direction = randomPos;
		    if (glm::length (direction) < 1e-4f) {
			direction = glm::vec3 (
			    WallpaperEngine::Maths::randomFloat (m_rng, -1.0f, 1.0f) * emitter.directions.x,
			    WallpaperEngine::Maths::randomFloat (m_rng, -1.0f, 1.0f) * emitter.directions.y,
			    WallpaperEngine::Maths::randomFloat (m_rng, -1.0f, 1.0f) * emitter.directions.z
			);
		    }
		    const float length = glm::length (direction);
		    if (length > 0.0f) {
			velocity = direction / length
				 * WallpaperEngine::Maths::randomFloat (m_rng, emitter.speedMin, emitter.speedMax)
				 * speedOverride ();
		    }
		}
		spawnParticle (p, velocity);

		count++;
	    }
	};
}

EmitterFunc CParticle::createSphereEmitter (const ParticleEmitter& emitter) {
    // Convert emitter origin from screen space (Y down) to centered space (Y up)
    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;

    // Auto-detect control point 0 usage if controlPoint field not specified and CP0 has linkMouse
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) { // Bit 0: linkMouse flag
	    controlPointIndex = 0;
	}
    }

    const auto state = addEmitterState (emitter);

    return [this, emitter, transformedEmitterOrigin, controlPointIndex,
	    state] (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) {
	const uint32_t toEmit = emitCount (emitter, *state, dt);

	for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
	    auto& p = particles[count];

	    // Determine spawn origin (control point or emitter origin)
	    glm::vec3 spawnOrigin = transformedEmitterOrigin;
	    if (controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())) {
		spawnOrigin += m_controlPoints[controlPointIndex].position;
	    }

	    // Spawn at random position on ellipsoid surface
	    glm::vec3 randomPos;

	    // Orthographic particles (flags & 4 == 0): use 2D disk distribution in X/Y plane
	    // Perspective particles (flags & 4 != 0): use 3D spherical shell distribution
	    if ((m_particle.flags & 4) == 0) {
		// 2D disk distribution with random Z offset
		float angle = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, glm::two_pi<float> ());
		float minRadius = emitter.distanceMin.x;
		float maxRadius = emitter.distanceMax.x;

		// Use sqrt for uniform area distribution in annulus
		float minRadiusSq = minRadius * minRadius;
		float maxRadiusSq = maxRadius * maxRadius;
		float radiusXY = std::sqrt (WallpaperEngine::Maths::randomFloat (m_rng, minRadiusSq, maxRadiusSq));

		randomPos = glm::vec3 (
		    radiusXY * std::cos (angle), radiusXY * std::sin (angle),
		    WallpaperEngine::Maths::randomFloat (m_rng, -maxRadius, maxRadius)
		);

		randomPos *= emitter.directions;
	    } else {
		// 3D spherical shell distribution
		float theta = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, glm::two_pi<float> ());
		float cosTheta = WallpaperEngine::Maths::randomFloat (m_rng, -1.0f, 1.0f);
		float sinTheta = std::sqrt (1.0f - cosTheta * cosTheta);

		randomPos = glm::vec3 (sinTheta * std::cos (theta), sinTheta * std::sin (theta), cosTheta);

		// Use cubic root for uniform volume distribution
		float minRadius = emitter.distanceMin.x;
		float maxRadius = emitter.distanceMax.x;
		float minRadiusCubed = minRadius * minRadius * minRadius;
		float maxRadiusCubed = maxRadius * maxRadius * maxRadius;
		float radius = std::cbrt (WallpaperEngine::Maths::randomFloat (m_rng, minRadiusCubed, maxRadiusCubed));

		randomPos *= radius;
		randomPos *= emitter.directions;
	    }

	    // Apply sign property to force positive/negative values per axis
	    // 0 = both, 1 = positive only, -1 = negative only
	    for (int i = 0; i < 3; i++) {
		if (emitter.sign[i] == 1) {
		    randomPos[i] = std::abs (randomPos[i]); // Force positive
		} else if (emitter.sign[i] == -1) {
		    randomPos[i] = -std::abs (randomPos[i]); // Force negative
		}
		// If sign[i] == 0, leave as-is (both positive and negative possible)
	    }
	    p.position = spawnOrigin + randomPos;

	    // Set velocity only if emitter specifies speed (otherwise use initializers); the parser binds the speed
	    // override to it (0x1401f3fe4)
	    glm::vec3 velocity (0.0f);
	    if (emitter.speedMax > 0.0f || emitter.speedMin != 0.0f) {
		// Velocity pointing outward from ellipsoid (randomPos already includes directions scaling)
		glm::vec3 direction
		    = glm::length (randomPos) > 0.0f ? glm::normalize (randomPos) : glm::vec3 (0.0f, 1.0f, 0.0f);
		float speed = WallpaperEngine::Maths::randomFloat (m_rng, emitter.speedMin, emitter.speedMax);
		velocity = direction * speed * speedOverride ();
	    }

	    spawnParticle (p, velocity);

	    count++;
	}
    };
}

void CParticle::spawnParticle (ParticleInstance& p, const glm::vec3& velocity) {
    // WE's spawn (GH 0x140260d80 head): age 0, lifetime 1, the size, the colour, the global alpha, one random
    // value of the particle's own that oscillate* and turbulence read (WE's per-particle array, system +0x338), then
    // the initializer stream
    p.velocity = velocity;
    p.acceleration = glm::vec3 (0.0f);
    p.rotation = glm::vec3 (0.0f);
    p.angularVelocity = glm::vec3 (0.0f);
    p.angularAcceleration = glm::vec3 (0.0f);

    p.color = m_spawnColor;
    p.alpha = m_particle.instanceOverride.alpha->value->getFloat ();
    p.size = 20.0f * sizeOverride ();
    p.lifetime = lifetimeOverride ();
    p.age = 0.0f;
    p.alive = true;
    p.frame = -1.0f;
    p.random = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);

    for (auto& init : m_initializers) {
	init (p);
    }

    // a child system's particles start in its parent's space: at its offset, turned and scaled by it
    if (m_parentSystem != nullptr) {
	p.position = m_childOffset + m_childLinear * p.position;
	p.velocity = m_childLinear * p.velocity;
	p.size *= m_childSizeScale;
    }
    p.id = m_nextParticleId++;

    p.initial.color = p.color;
    p.initial.alpha = p.alpha;
    p.initial.size = p.size;
    p.initial.lifetime = p.lifetime;
    if (m_hasEventChildren) {
	m_spawned.push_back (p);
    }
}

// ========== INITIALIZERS ==========

void CParticle::setupInitializers () {
    for (const auto& initializer : m_particle.initializers) {
	if (!initializer) {
	    continue;
	}

	InitializerFunc func;

	if (initializer->is<ColorRandomInitializer> ()) {
	    func = createColorRandomInitializer (*initializer->as<ColorRandomInitializer> ());
	} else if (initializer->is<SizeRandomInitializer> ()) {
	    func = createSizeRandomInitializer (*initializer->as<SizeRandomInitializer> ());
	} else if (initializer->is<AlphaRandomInitializer> ()) {
	    func = createAlphaRandomInitializer (*initializer->as<AlphaRandomInitializer> ());
	} else if (initializer->is<LifetimeRandomInitializer> ()) {
	    const auto& lifeInit = *initializer->as<LifetimeRandomInitializer> ();
	    m_uniformLifetimes = (lifeInit.min->value->getFloat () == lifeInit.max->value->getFloat ());
	    func = createLifetimeRandomInitializer (lifeInit);
	} else if (initializer->is<VelocityRandomInitializer> ()) {
	    func = createVelocityRandomInitializer (*initializer->as<VelocityRandomInitializer> ());
	} else if (initializer->is<RotationRandomInitializer> ()) {
	    func = createRotationRandomInitializer (*initializer->as<RotationRandomInitializer> ());
	} else if (initializer->is<AngularVelocityRandomInitializer> ()) {
	    func = createAngularVelocityRandomInitializer (*initializer->as<AngularVelocityRandomInitializer> ());
	} else if (initializer->is<TurbulentVelocityRandomInitializer> ()) {
	    func = createTurbulentVelocityRandomInitializer (*initializer->as<TurbulentVelocityRandomInitializer> ());
	} else if (initializer->is<MapSequenceAroundControlPointInitializer> ()) {
	    func = createMapSequenceAroundControlPointInitializer (
		*initializer->as<MapSequenceAroundControlPointInitializer> ()
	    );
	} else if (initializer->is<GenericParticleInitializer> ()) {
	    func = createGenericInitializer (*initializer->as<GenericParticleInitializer> ());
	} else {
	    sLog.out ("Unknown initializer type");
	}

	if (func) {
	    m_initializers.push_back (std::move (func));
	}
    }
}

float CParticle::randomT (float exponent) {
    // WE's spawn interpreter (GH 0x140260d80): one uniform draw from the scene's generator, raised to the
    // initializer's `exponent` unless it is exactly 1
    const float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
    return exponent == 1.0f ? t : std::pow (t, exponent);
}

InitializerFunc CParticle::createColorRandomInitializer (const ColorRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	// one t for all three channels (GH 0x140260d80 case 3): the colour lies on the line from min to max,
	// it multiplies the spawn's colour
	glm::vec3 min = minValue->getVec3 ();
	glm::vec3 max = maxValue->getVec3 ();
	if (m_tint) {
	    // colorn shifts the range's ends in HSV (it used to multiply the colour in RGB)
	    min = tintColor (min);
	    max = tintColor (max);
	}
	const float t = randomT (exponentValue->getFloat ());
	p.color = (min + t * (max - min)) * p.color;
	p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createSizeRandomInitializer (const SizeRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	float exponent = exponentValue->getFloat ();
	float min = minValue->getFloat ();
	float max = maxValue->getFloat ();

	// Apply exponent for non-linear distribution
	float adjustedT = std::pow (t, exponent);
	p.size = (min + adjustedT * (max - min)) * sizeOverride () / 2.0f;
	p.initial.size = p.size;
    };
}

InitializerFunc CParticle::createAlphaRandomInitializer (const AlphaRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	// multiplies the spawn's alpha, the override's (GH 0x140260d80 case 6)
	const float min = minValue->getFloat ();
	p.alpha = (min + randomT (exponentValue->getFloat ()) * (maxValue->getFloat () - min)) * p.alpha;
	p.initial.alpha = p.alpha;
    };
}

InitializerFunc CParticle::createLifetimeRandomInitializer (const LifetimeRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	// GH 0x140260d80 case 1: at least 0.001 s
	const float min = minValue->getFloat ();
	const float t = randomT (exponentValue->getFloat ());
	p.lifetime = std::max (0.001f, (min + t * (maxValue->getFloat () - min)) * lifetimeOverride ());
	p.initial.lifetime = p.lifetime;
    };
}

InitializerFunc CParticle::createVelocityRandomInitializer (const VelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	// a draw per axis, each raised to `exponent` (GH 0x140260d80 case 7)
	const glm::vec3 min = minValue->getVec3 ();
	const glm::vec3 range = maxValue->getVec3 () - min;
	const float exponent = exponentValue->getFloat ();
	glm::vec3 vel;
	for (int i = 0; i < 3; i++) {
	    vel[i] = min[i] + randomT (exponent) * range[i];
	}
	vel *= speedOverride ();
	vel.y = -vel.y;
	p.velocity += vel;
    };
}

InitializerFunc CParticle::createRotationRandomInitializer (const RotationRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	// a draw per axis, each raised to `exponent` (GH 0x140260d80 case 10)
	const glm::vec3 min = minValue->getVec3 ();
	const glm::vec3 range = maxValue->getVec3 () - min;
	const float exponent = exponentValue->getFloat ();
	for (int i = 0; i < 3; i++) {
	    p.rotation[i] = min[i] + randomT (exponent) * range[i];
	}
	// an angle: the parser binds no override to rotationrandom (P15; it took the speed override)
    };
}

InitializerFunc CParticle::createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	glm::vec3 minVec = minValue->getVec3 ();
	glm::vec3 maxVec = maxValue->getVec3 ();
	float exponent = exponentValue->getFloat ();

	// Apply exponent bias to random distribution
	// exponent = 1: uniform distribution
	// exponent -> 0: bias towards max
	// exponent >= 2: bias towards min
	glm::vec3 result;
	for (int i = 0; i < 3; i++) {
	    float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	    t = std::pow (t, exponent);
	    result[i] = minVec[i] + t * (maxVec[i] - minVec[i]);
	}

	p.angularVelocity = result * speedOverride ();
    };
}

InitializerFunc CParticle::createTurbulentVelocityRandomInitializer (const TurbulentVelocityRandomInitializer& init) {
    DynamicValue* speedMin = init.speedMin->value.get ();
    DynamicValue* speedMax = init.speedMax->value.get ();
    DynamicValue* offsetVal = init.offset->value.get ();
    DynamicValue* scaleVal = init.scale->value.get ();
    DynamicValue* forwardVal = init.forward->value.get ();
    DynamicValue* timeScaleVal = init.timeScale->value.get ();
    DynamicValue* phaseMinVal = init.phaseMin->value.get ();
    DynamicValue* phaseMaxVal = init.phaseMax->value.get ();
    DynamicValue* rightVal = init.right->value.get ();

    return [this, speedMin, speedMax, offsetVal, scaleVal, forwardVal, timeScaleVal, phaseMinVal, phaseMaxVal,
	    rightVal] (ParticleInstance& p) {
	// Get direction parameters
	glm::vec3 forward = forwardVal->getVec3 ();
	glm::vec3 right = rightVal->getVec3 ();
	// Y-flip for coordinate system conversion
	forward.y = -forward.y;
	right.y = -right.y;

	if (glm::length (forward) > 0.0001f) {
	    forward = glm::normalize (forward);
	} else {
	    // Default forward direction when not specified (up in centered space)
	    forward = glm::vec3 (0.0f, 1.0f, 0.0f);
	}
	if (glm::length (right) > 0.0001f) {
	    right = glm::normalize (right);
	} else {
	    right = glm::vec3 (1.0f, 0.0f, 0.0f);
	}

	float speed = WallpaperEngine::Maths::randomFloat (m_rng, speedMin->getFloat (), speedMax->getFloat ());
	float scale = scaleVal->getFloat ();
	float offset = offsetVal->getFloat ();
	float timeScale = timeScaleVal->getFloat ();
	float phaseMin = phaseMinVal->getFloat ();
	float phaseMax = phaseMaxVal->getFloat ();

	// Sample noise at particle position + time-based offset.
	// timescale shifts the noise field over time so particles spawned at different
	// times get gradually changing directions (creates smooth evolving vapor stream).
	// Position component provides spatial coherence for nearby particles.
	glm::vec3 noisePos = p.position * 0.1f;
	noisePos += glm::vec3 (m_simTime * timeScale);

	// Phase adds per-particle randomization to noise position
	float phase = WallpaperEngine::Maths::randomFloat (m_rng, phaseMin, phaseMax);
	glm::vec3 samplePos = noisePos + glm::vec3 (phase, phase * 0.7f, phase * 1.3f);

	// Sample curl noise for direction and normalize
	glm::vec3 result = curlNoise (samplePos);
	float len = glm::length (result);
	if (len < 0.0001f) {
	    result = forward;
	} else {
	    result = result / len;
	}

	// Scale limits how far direction can deviate from forward
	if (scale < 2.0f) {
	    float cosAngle = glm::dot (result, forward);
	    float angle = std::acos (glm::clamp (cosAngle, -1.0f, 1.0f)) / glm::pi<float> ();
	    float maxAngle = scale / 2.0f;

	    if (angle > maxAngle && maxAngle > 0.0001f) {
		glm::vec3 axis = glm::cross (result, forward);
		float axisLen = glm::length (axis);
		if (axisLen > 0.0001f) {
		    axis = axis / axisLen;
		    float rotAngle = (angle - maxAngle) * glm::pi<float> ();
		    glm::mat3 rot = glm::mat3 (glm::rotate (glm::mat4 (1.0f), rotAngle, axis));
		    result = rot * result;
		}
	    }
	}

	// Offset rotates result around right axis (tilts up/down)
	if (std::abs (offset) > 0.0001f) {
	    glm::mat3 rot = glm::mat3 (glm::rotate (glm::mat4 (1.0f), -offset, right));
	    result = rot * result;
	}

	// For 2D/orthographic particles (flags & 4 == 0), project direction onto XY plane.
	// curlNoise is 3D but z-drift is meaningless for 2D particles and causes
	// rope segments to diverge in depth, breaking visual connectivity.
	if ((m_particle.flags & 4) == 0) {
	    result.z = 0.0f;
	    float len2d = glm::length (result);
	    if (len2d > 0.0001f) {
		result /= len2d;
	    }
	}

	// Apply speed and instance override
	glm::vec3 finalVel = result * speed * speedOverride ();

	p.velocity += finalVel;
    };
}

InitializerFunc
CParticle::createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init) {
    DynamicValue* controlPointValue = init.controlPoint->value.get ();
    DynamicValue* countValue = init.count->value.get ();
    DynamicValue* speedMinValue = init.speedMin->value.get ();
    DynamicValue* speedMaxValue = init.speedMax->value.get ();
    DynamicValue* countOverride = m_particle.instanceOverride.count->value.get ();

    // Sequence counter shared across all particles spawned with this initializer
    // This creates the circular distribution pattern
    int sequenceIndex = 0;

    return [this, controlPointValue, countValue, speedMinValue, speedMaxValue, sequenceIndex,
	    countOverride] (ParticleInstance& p) mutable {
	int controlPoint = static_cast<int> (controlPointValue->getFloat ());
	// the `count` override scales the sequence's count too (an int× binding in WE's parser), unless the
	// system opts out with flags bit 0x20
	float countScale = (m_particle.flags & 0x20) == 0 ? countOverride->getFloat () : 1.0f;
	int count = std::max (1, static_cast<int> (countValue->getFloat () * countScale));
	sequenceIndex %= count;

	// Calculate angle for this particle in the sequence (evenly distributed around circle)
	float angle = (static_cast<float> (sequenceIndex) / static_cast<float> (count)) * glm::two_pi<float> ();
	sequenceIndex = (sequenceIndex + 1) % count; // Wrap around after reaching count

	// Get control point position to spawn around
	glm::vec3 centerPos = glm::vec3 (0.0f);
	if (controlPoint >= 0 && controlPoint < static_cast<int> (m_controlPoints.size ())) {
	    centerPos = m_controlPoints[controlPoint].position;
	}

	// Set particle position in circular pattern around control point
	// This creates the natural clustering seen in the original
	p.position = centerPos;

	// Set velocity based on angle and speed range
	glm::vec3 speedMin = speedMinValue->getVec3 ();
	glm::vec3 speedMax = speedMaxValue->getVec3 ();
	glm::vec3 speed = WallpaperEngine::Maths::randomVec3 (m_rng, speedMin, speedMax);

	// Flip Y before rotation to convert to centered space
	speed.y = -speed.y;

	// Rotate velocity based on sequence angle (creates outward radial pattern)
	glm::mat3 rotationMatrix = glm::mat3 (
	    std::cos (angle), -std::sin (angle), 0.0f, std::sin (angle), std::cos (angle), 0.0f, 0.0f, 0.0f, 1.0f
	);
	glm::vec3 rotatedSpeed = rotationMatrix * speed * speedOverride ();

	// Set velocity (speed override applied in movement operator)
	p.velocity = rotatedSpeed;
    };
}

// ========== OPERATORS ==========
//
// WE's operator interpreter (GH 0x140265800) runs the operator byte-code over the particles, four at a time. Before
// it, every particle's size, alpha and colour go back to their spawn values (size always, alpha and colour when the
// system has an operator on them: the interpreter's head copies the base arrays); each operator then multiplies
// them, so alphafade × alphachange × oscillatealpha compose. Forces add to the velocity scaled by the step's "force
// dt" (`m_forceDt`, the interpreter's third argument), positions move by the plain dt. Records (offsets from the
// record start, values splatted ×4): see each operator.

void CParticle::setupOperators () {
    for (const auto& op : m_particle.operators) {
	if (!op) {
	    continue;
	}

	OperatorFunc func;

	if (op->is<MovementOperator> ()) {
	    func = createMovementOperator (*op->as<MovementOperator> ());
	} else if (op->is<AngularMovementOperator> ()) {
	    func = createAngularMovementOperator (*op->as<AngularMovementOperator> ());
	} else if (op->is<AlphaFadeOperator> ()) {
	    func = createAlphaFadeOperator (*op->as<AlphaFadeOperator> ());
	} else if (op->is<SizeChangeOperator> ()) {
	    func = createSizeChangeOperator (*op->as<SizeChangeOperator> ());
	} else if (op->is<AlphaChangeOperator> ()) {
	    func = createAlphaChangeOperator (*op->as<AlphaChangeOperator> ());
	} else if (op->is<ColorChangeOperator> ()) {
	    func = createColorChangeOperator (*op->as<ColorChangeOperator> ());
	} else if (op->is<TurbulenceOperator> ()) {
	    func = createTurbulenceOperator (*op->as<TurbulenceOperator> ());
	} else if (op->is<VortexOperator> ()) {
	    func = createVortexOperator (*op->as<VortexOperator> ());
	} else if (op->is<ControlPointAttractOperator> ()) {
	    func = createControlPointAttractOperator (*op->as<ControlPointAttractOperator> ());
	} else if (op->is<OscillateAlphaOperator> ()) {
	    func = createOscillateAlphaOperator (*op->as<OscillateAlphaOperator> ());
	} else if (op->is<OscillateSizeOperator> ()) {
	    func = createOscillateSizeOperator (*op->as<OscillateSizeOperator> ());
	} else if (op->is<OscillatePositionOperator> ()) {
	    func = createOscillatePositionOperator (*op->as<OscillatePositionOperator> ());
	} else if (op->is<GenericParticleOperator> ()) {
	    func = createGenericOperator (*op->as<GenericParticleOperator> ());
	} else {
	    sLog.out ("Unknown operator type");
	}

	if (func) {
	    m_operators.push_back (std::move (func));
	}
    }
}

float CParticle::speedOverride () const {
    // the parser binds `speed` into every speed-like value unless the particle flags have 0x10 (parser 0x1401f3fe4…)
    return (m_particle.flags & 0x10) == 0 ? m_particle.instanceOverride.speed->value->getFloat () : 1.0f;
}

float CParticle::sizeOverride () const {
    // bound to sizerandom and to sizechange's start and end values unless the flags have 0x80 (0x1401f512b, 0x1401f97d8)
    return (m_particle.flags & 0x80) == 0 ? m_particle.instanceOverride.size->value->getFloat () : 1.0f;
}

float CParticle::lifetimeOverride () const {
    // bound to lifetimerandom unless the flags have 0x40 (0x1401f4f50)
    return (m_particle.flags & 0x40) == 0 ? m_particle.instanceOverride.lifetime->value->getFloat () : 1.0f;
}

namespace {
// the fraction of its life a particle has lived, as the interpreter takes it: age × (1 / lifetime)
float lifeFraction (const ParticleInstance& p) { return p.lifetime > 0.0f ? p.age / p.lifetime : 1.0f; }

// the drag of one step: 1 − min (drag × force dt, 0.9999999) (GH 0x140265800 cases 1 and 2)
float dragKeep (float drag, float forceDt) { return 1.0f - std::min (drag * forceDt, 0.9999999f); }

// a value going from `start` to `end` between the life fractions starttime and endtime, clamped (cases 4–6:
// t = clamp ((life − starttime) × 1 / (endtime − starttime), 0, 1), value = start + t × (end − start))
float lifeRamp (float life, float startTime, float endTime, float start, float end) {
    const float span = endTime - startTime;
    float t = span != 0.0f ? (life - startTime) / span : (life >= startTime ? 1.0f : 0.0f);
    t = std::clamp (t, 0.0f, 1.0f);
    return start + t * (end - start);
}
} // namespace

OperatorFunc CParticle::createMovementOperator (const MovementOperator& op) {
    DynamicValue* dragValue = op.drag->value.get ();
    DynamicValue* gravityValue = op.gravity->value.get ();

    // case 1 (record: gravity +0x10, flags +0x1c, drag +0x20): v' = v + g·dt; p += v'·dt; v = v'·(1 − min (drag·fdt, ~1)).
    // The parser binds gravity, not the velocity, to the speed override
    return [this, dragValue, gravityValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	glm::vec3 gravity = gravityValue->getVec3 () * speedOverride ();
	gravity.y = -gravity.y;
	const float keep = dragKeep (dragValue->getFloat (), m_forceDt);

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const glm::vec3 v = p.velocity + gravity * dt;
	    p.position += v * dt;
	    p.velocity = v * keep;
	}
    };
}

OperatorFunc CParticle::createAngularMovementOperator (const AngularMovementOperator& op) {
    DynamicValue* dragValue = op.drag->value.get ();
    DynamicValue* forceValue = op.force->value.get ();

    // case 2 (record: force x/y/z +0x10/+0x20/+0x30, drag +0x80): the same integration on rotation and angular
    // velocity. Only the force carries the speed override (the initial angular velocity has it already); it scaled
    // the rotation step too, so spin got the override twice
    return [this, dragValue, forceValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	const glm::vec3 force = forceValue->getVec3 () * speedOverride ();
	const float keep = dragKeep (dragValue->getFloat (), m_forceDt);
	const float pi = glm::pi<float> ();
	const float twoPi = glm::two_pi<float> ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const glm::vec3 w = p.angularVelocity + force * dt;
	    p.rotation += w * dt;
	    p.angularVelocity = w * keep;
	    for (int j = 0; j < 3; j++) {
		p.rotation[j] = std::remainder (p.rotation[j], twoPi);
		if (p.rotation[j] > pi) {
		    p.rotation[j] -= twoPi;
		}
	    }
	}
    };
}

OperatorFunc CParticle::createAlphaFadeOperator (const AlphaFadeOperator& op) {
    DynamicValue* fadeInTimeValue = op.fadeInTime->value.get ();
    DynamicValue* fadeOutTimeValue = op.fadeOutTime->value.get ();

    // case 3 (record: fadeintime +0x10, fadeouttime +0x20): alpha ×= life / in before `in`, (1 − life) / (1 − out)
    // after `out`, 1 between
    return [fadeInTimeValue, fadeOutTimeValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	   ) {
	const float fadeIn = fadeInTimeValue->getFloat ();
	const float fadeOut = fadeOutTimeValue->getFloat ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float life = lifeFraction (p);
	    float fade = 1.0f;
	    if (life < fadeIn) {
		fade = fadeIn > 0.0f ? life / fadeIn : 1.0f;
	    } else if (life > fadeOut) {
		fade = fadeOut < 1.0f ? (1.0f - life) / (1.0f - fadeOut) : 0.0f;
	    }
	    p.alpha *= std::max (fade, 0.0f);
	}
    };
}

OperatorFunc CParticle::createSizeChangeOperator (const SizeChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    // case 4 (record: startvalue +0x10, end − start +0x20, starttime +0x30, 1 / (endtime − starttime) +0x40):
    // size ×= the ramp. The parser binds startvalue and endvalue to the size override (patch op 7, 0x1401f97d8), so
    // a system with sizechange takes the override twice: once in sizerandom, once here
    return [this, startTimeValue, endTimeValue, startValueValue, endValueValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	   ) {
	const float startTime = startTimeValue->getFloat ();
	const float endTime = endTimeValue->getFloat ();
	const float size = sizeOverride ();
	const float startValue = startValueValue->getFloat () * size;
	const float endValue = endValueValue->getFloat () * size;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    p.size *= lifeRamp (lifeFraction (p), startTime, endTime, startValue, endValue);
	}
    };
}

OperatorFunc CParticle::createAlphaChangeOperator (const AlphaChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    // case 6: sizechange's record on alpha (no override binding). It used to overwrite alphafade's result
    return [startTimeValue, endTimeValue, startValueValue, endValueValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	   ) {
	const float startTime = startTimeValue->getFloat ();
	const float endTime = endTimeValue->getFloat ();
	const float startValue = startValueValue->getFloat ();
	const float endValue = endValueValue->getFloat ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    p.alpha *= lifeRamp (lifeFraction (p), startTime, endTime, startValue, endValue);
	}
    };
}

OperatorFunc CParticle::createColorChangeOperator (const ColorChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    // case 5: the ramp per channel, multiplying the spawn colour (P31: relative, as LWE had it). Its start and end
    // values are colorn-shifted like colorrandom's (patch op 0xc at parser 0x1401f9b39)
    return [this, startTimeValue, endTimeValue, startValueValue, endValueValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	   ) {
	const float startTime = startTimeValue->getFloat ();
	const float endTime = endTimeValue->getFloat ();
	glm::vec3 startValue = startValueValue->getVec3 ();
	glm::vec3 endValue = endValueValue->getVec3 ();
	if (m_tint) {
	    startValue = tintColor (startValue);
	    endValue = tintColor (endValue);
	}

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float life = lifeFraction (p);
	    p.color *= glm::vec3 (
		lifeRamp (life, startTime, endTime, startValue.r, endValue.r),
		lifeRamp (life, startTime, endTime, startValue.g, endValue.g),
		lifeRamp (life, startTime, endTime, startValue.b, endValue.b)
	    );
	}
    };
}

OperatorFunc CParticle::createTurbulenceOperator (const TurbulenceOperator& op) {
    DynamicValue* scaleValue = op.scale->value.get ();
    DynamicValue* speedMinValue = op.speedMin->value.get ();
    DynamicValue* speedMaxValue = op.speedMax->value.get ();
    DynamicValue* timeScaleValue = op.timeScale->value.get ();
    DynamicValue* maskValue = op.mask->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* rateOverride = m_particle.instanceOverride.rate->value.get ();

    // case 0xe (record: mask x/y/z +0x10/+0x20/+0x30, scale +0x40, speedmin +0x50, speed range +0x60, phasemin +0x70,
    // phase range +0x80, timescale +0xd0, masked axes +0xd4). Per particle, with its one random r:
    //   phase = r·(phasemax − phasemin) + time·timescale; q = (position + phase)·scale
    //   v.x += noise (q.x, q.y, q.z)·speed·mask.x·fdt, v.y += noise (q.z, q.x, q.y)·…, v.z += noise (q.y, q.z, q.x)·…
    // with speed = speedmin + r·(speedmax − speedmin). Each masked axis is a scalar 3D noise in WE's (y-up) space;
    // it was a normalised curl with one phase and speed per operator. phasemin is never read by the case;
    // timescale is bound to the `rate` override (parser 0x1401fb45a)
    return [this, scaleValue, speedMinValue, speedMaxValue, timeScaleValue, maskValue, phaseMinValue, phaseMaxValue,
	    rateOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&,
	       float currentTime, float
	   ) {
	const glm::vec3 mask = maskValue->getVec3 ();
	if (mask.x == 0.0f && mask.y == 0.0f && mask.z == 0.0f) {
	    return;
	}
	const float scale = scaleValue->getFloat ();
	const float speedMin = speedMinValue->getFloat ();
	const float speedRange = speedMaxValue->getFloat () - speedMin;
	const float phaseRange = phaseMaxValue->getFloat () - phaseMinValue->getFloat ();
	const float time = currentTime * timeScaleValue->getFloat () * rateOverride->getFloat ();
	const float speedScale = speedOverride () * m_forceDt;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float phase = p.random * phaseRange + time;
	    // in WE's (y-up) particle space
	    const glm::vec3 q = (glm::vec3 (p.position.x, -p.position.y, p.position.z) + phase) * scale;
	    const float speed = (speedMin + p.random * speedRange) * speedScale;
	    glm::vec3 n (0.0f);
	    if (mask.x != 0.0f) {
		n.x = simplexNoise (q.x, q.y, q.z);
	    }
	    if (mask.y != 0.0f) {
		n.y = simplexNoise (q.z, q.x, q.y);
	    }
	    if (mask.z != 0.0f) {
		n.z = simplexNoise (q.y, q.z, q.x);
	    }
	    const glm::vec3 dv = n * mask * speed;
	    p.velocity += glm::vec3 (dv.x, -dv.y, dv.z);
	}
    };
}

OperatorFunc CParticle::createVortexOperator (const VortexOperator& op) {
    if (op.v2) {
	return createVortexV2Operator (op);
    }

    const int controlPoint = std::clamp (op.controlPoint, 0, 7);
    const bool infiniteAxis = (op.flags & 1) != 0;
    DynamicValue* axisValue = op.axis->value.get ();
    DynamicValue* offsetValue = op.offset->value.get ();
    DynamicValue* distanceInnerValue = op.distanceInner->value.get ();
    DynamicValue* distanceOuterValue = op.distanceOuter->value.get ();
    DynamicValue* speedInnerValue = op.speedInner->value.get ();
    DynamicValue* speedOuterValue = op.speedOuter->value.get ();

    // case 0xf (record: offset +0x10.., axis (normalised) +0x40.., distanceinner +0x70, 1 / (outer − inner) +0x80,
    // speedinner +0x90, speed range +0xa0, infinite-axis mask +0xb0, controlpoint +0xc0): r = p − (cp + offset), less
    // its part along the axis with flags bit 0; t = clamp ((|r| − inner) / (outer − inner), 0, 1);
    // v += (r̂ × axis)·(speedinner + t·(speedouter − speedinner))·fdt. Plain vortex reads no other flag: the ring and
    // centre-force bits (2, 4) belong to vortex_v2, and applied here they pulled 4 scenes' particles to the centre
    return [this, controlPoint, infiniteAxis, axisValue, offsetValue, distanceInnerValue, distanceOuterValue,
	    speedInnerValue, speedOuterValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, float
	   ) {
	// WE's (y-up) axis and offset in this mirrored space
	glm::vec3 axis = axisValue->getVec3 ();
	axis.y = -axis.y;
	const float axisLength = glm::length (axis);
	axis = axisLength > 0.0f ? axis / axisLength : glm::vec3 (0.0f, 0.0f, 1.0f);
	glm::vec3 offset = offsetValue->getVec3 ();
	offset.y = -offset.y;
	const glm::vec3 center
	    = (controlPoint < static_cast<int> (controlPoints.size ()) ? controlPoints[controlPoint].position : glm::vec3 (0.0f))
	    + offset;
	const float inner = distanceInnerValue->getFloat ();
	const float span = distanceOuterValue->getFloat () - inner;
	const float speedInner = speedInnerValue->getFloat ();
	const float speedRange = speedOuterValue->getFloat () - speedInner;
	const float speedScale = speedOverride () * m_forceDt;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    glm::vec3 r = p.position - center;
	    if (infiniteAxis) {
		r -= axis * glm::dot (r, axis);
	    }
	    const float distance = glm::length (r);
	    if (distance <= 0.0f) {
		continue;
	    }
	    const float t = span != 0.0f ? std::clamp ((distance - inner) / span, 0.0f, 1.0f) : 0.0f;
	    // r̂ × axis in WE's space is axis × r̂ here (a mirror flips the cross product)
	    const glm::vec3 tangent = glm::cross (axis, r / distance);
	    p.velocity += tangent * (speedInner + t * speedRange) * speedScale;
	}
    };
}

OperatorFunc CParticle::createVortexV2Operator (const VortexOperator& op) {
    // vortex_v2 (case 0x10) is not decoded: no library scene uses it. This is LWE's own model of it
    int controlPoint = op.controlPoint;
    DynamicValue* axisValue = op.axis->value.get ();
    DynamicValue* offsetValue = op.offset->value.get ();
    DynamicValue* distanceInnerValue = op.distanceInner->value.get ();
    DynamicValue* distanceOuterValue = op.distanceOuter->value.get ();
    DynamicValue* speedInnerValue = op.speedInner->value.get ();
    DynamicValue* speedOuterValue = op.speedOuter->value.get ();
    DynamicValue* centerForceValue = op.centerForce->value.get ();
    DynamicValue* ringRadiusValue = op.ringRadius->value.get ();
    DynamicValue* ringWidthValue = op.ringWidth->value.get ();
    DynamicValue* ringPullDistanceValue = op.ringPullDistance->value.get ();
    DynamicValue* ringPullForceValue = op.ringPullForce->value.get ();
    const bool infiniteAxis = (op.flags & 1) != 0;
    const bool maintainDistance = (op.flags & 2) != 0;
    const bool ringShape = (op.flags & 4) != 0;

    return [this, controlPoint, axisValue, offsetValue, distanceInnerValue, distanceOuterValue, speedInnerValue,
	    speedOuterValue, centerForceValue, ringRadiusValue, ringWidthValue, ringPullDistanceValue,
	    ringPullForceValue, infiniteAxis, maintainDistance, ringShape] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, float dt
	   ) {
	glm::vec3 axis = axisValue->getVec3 ();
	axis = glm::length (axis) > 0.0f ? glm::normalize (axis) : glm::vec3 (0.0f, 0.0f, 1.0f);
	glm::vec3 center = offsetValue->getVec3 ();
	if (controlPoint >= 0 && controlPoint < static_cast<int> (controlPoints.size ())) {
	    center += controlPoints[controlPoint].position;
	}
	const float distanceInner = distanceInnerValue->getFloat ();
	const float distanceOuter = distanceOuterValue->getFloat ();
	const float speedInner = speedInnerValue->getFloat ();
	const float speedOuter = speedOuterValue->getFloat ();
	const float ringRadius = ringRadiusValue->getFloat ();
	const float ringWidth = ringWidthValue->getFloat ();
	const float ringPullDistance = ringPullDistanceValue->getFloat ();
	const float speedScale = speedOverride () * dt;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    glm::vec3 radial = p.position - center;
	    if (infiniteAxis) {
		radial -= axis * glm::dot (radial, axis);
	    }
	    const float distance = glm::length (radial);
	    glm::vec3 tangent = glm::cross (axis, radial);
	    if (glm::length (tangent) <= 0.001f) {
		continue;
	    }
	    tangent = glm::normalize (tangent);
	    float speed = 0.0f;
	    glm::vec3 radialForce (0.0f);
	    if (ringShape) {
		const float ringInner = ringRadius - ringWidth * 0.5f;
		const float ringOuter = ringRadius + ringWidth * 0.5f;
		if (distance >= ringInner && distance <= ringOuter) {
		    speed = glm::mix (speedInner, speedOuter, (distance - ringInner) / ringWidth);
		} else if (distance > ringOuter && distance <= ringOuter + ringPullDistance) {
		    const float pullT = (distance - ringOuter) / ringPullDistance;
		    speed = speedOuter * (1.0f - pullT);
		    radialForce = -glm::normalize (radial) * ringPullForceValue->getFloat () * pullT;
		}
	    } else {
		const float span = distanceOuter - distanceInner + 0.1f;
		speed = distance < distanceInner ? speedInner
		      : distance > distanceOuter ? speedOuter
						 : glm::mix (speedInner, speedOuter, (distance - distanceInner) / span);
	    }
	    p.velocity += (tangent * speed + radialForce) * speedScale;
	    if (maintainDistance && distance > 0.001f) {
		p.velocity -= glm::normalize (radial) * centerForceValue->getFloat () * speedScale;
	    }
	}
    };
}

OperatorFunc CParticle::createControlPointAttractOperator (const ControlPointAttractOperator& op) {
    const int controlPoint = op.controlPoint;
    const int flags = op.flags;
    DynamicValue* offsetValue = op.offset->value.get ();
    DynamicValue* scaleValue = op.scale->value.get ();
    DynamicValue* thresholdValue = op.threshold->value.get ();
    DynamicValue* deleteValue = op.deleteThreshold->value.get ();

    // case 0xa (record: offset +0x10.., scale +0x40 (× speed), threshold +0x50, deletethreshold² +0x60, flags +0xb0,
    // controlpoint +0xb4): within FLT_MIN < d < threshold of the point, v moves toward it by
    // step = scale·fdt·(1 − d / threshold), no further than d with flags bit 1. Flags bit 0 kills the particles within
    // deletethreshold (GH 0x1402505d0; WE tests the segment the particle moved along this step, this the point).
    // It was a constant pull within half the threshold, from `origin`
    return [this, controlPoint, flags, offsetValue, scaleValue, thresholdValue, deleteValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, float
	   ) {
	if (controlPoint >= static_cast<int> (controlPoints.size ())) {
	    return;
	}
	glm::vec3 offset = offsetValue->getVec3 ();
	offset.y = -offset.y;
	const glm::vec3 center = controlPoints[controlPoint].position + offset;
	const float threshold = thresholdValue->getFloat ();
	const float scale = scaleValue->getFloat () * speedOverride () * m_forceDt;
	const float deleteThreshold = deleteValue->getFloat ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const glm::vec3 toParticle = p.position - center;
	    const float d = glm::length (toParticle);
	    if ((flags & 1) != 0 && d <= deleteThreshold) {
		p.age = p.lifetime;
		continue;
	    }
	    if (!(d > std::numeric_limits<float>::min () && d < threshold)) {
		continue;
	    }
	    float step = scale * (1.0f - d / threshold);
	    if ((flags & 2) != 0 && d < step) {
		step = d;
	    }
	    p.velocity -= toParticle / d * step;
	}
    };
}

OperatorFunc CParticle::createOscillateAlphaOperator (const OscillateAlphaOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();

    // case 8 (record: frequencymin +0x10, range +0x20, phasemin +0x30, range +0x40, scalemin +0x50, range +0x60), with
    // the particle's one random r for all three: f = fmin + r·Δf, φ = pmin + r·Δp,
    // alpha ×= scalemin + Δs·r·(sin (f·(age + φ)) + 1) / 2. It drew three randoms and took cos (w·t + φ)
    return [freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	   ) {
	const float freqMin = freqMinValue->getFloat ();
	const float freqRange = freqMaxValue->getFloat () - freqMin;
	const float phaseMin = phaseMinValue->getFloat ();
	const float phaseRange = phaseMaxValue->getFloat () - phaseMin;
	const float scaleMin = scaleMinValue->getFloat ();
	const float scaleRange = scaleMaxValue->getFloat () - scaleMin;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float f = freqMin + p.random * freqRange;
	    const float phase = phaseMin + p.random * phaseRange;
	    p.alpha *= scaleMin + scaleRange * p.random * (std::sin (f * (p.age + phase)) + 1.0f) * 0.5f;
	}
    };
}

OperatorFunc CParticle::createOscillateSizeOperator (const OscillateSizeOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();

    // case 9: case 8 on the size
    return [freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	   ) {
	const float freqMin = freqMinValue->getFloat ();
	const float freqRange = freqMaxValue->getFloat () - freqMin;
	const float phaseMin = phaseMinValue->getFloat ();
	const float phaseRange = phaseMaxValue->getFloat () - phaseMin;
	const float scaleMin = scaleMinValue->getFloat ();
	const float scaleRange = scaleMaxValue->getFloat () - scaleMin;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float f = freqMin + p.random * freqRange;
	    const float phase = phaseMin + p.random * phaseRange;
	    p.size *= scaleMin + scaleRange * p.random * (std::sin (f * (p.age + phase)) + 1.0f) * 0.5f;
	}
    };
}

OperatorFunc CParticle::createOscillatePositionOperator (const OscillatePositionOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* maskValue = op.mask->value.get ();

    // case 7 (record: mask x/y/z +0x10/+0x20/+0x30, frequencymin +0x40, range +0x50, phasemin +0x60, range +0x70,
    // scalemin +0x80, range +0x90; frequency × speed override, 0x1401fa0a6). The offset is
    // scale·sin (f·(age + φ)), applied as this step's difference; y's phase is φ + 2π·r, z reuses x's:
    //   x += D (φ)·mask.x, y += D (φ + 2πr)·mask.y, z += D (φ)·mask.z, D (φ) = s·(sin (f·(age + φ)) − sin (f·(age + φ − dt)))
    // with f, φ and s from the particle's one random r. It moved by −s·w·sin (w·t + φ)·dt, × speed
    return [this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue, maskValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	const float speed = speedOverride ();
	const float freqMin = freqMinValue->getFloat () * speed;
	const float freqRange = freqMaxValue->getFloat () * speed - freqMin;
	const float phaseMin = phaseMinValue->getFloat ();
	const float phaseRange = phaseMaxValue->getFloat () - phaseMin;
	const float scaleMin = scaleMinValue->getFloat ();
	const float scaleRange = scaleMaxValue->getFloat () - scaleMin;
	const glm::vec3 mask = maskValue->getVec3 ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    const float f = freqMin + p.random * freqRange;
	    const float s = scaleMin + p.random * scaleRange;
	    const float a = p.age + phaseMin + p.random * phaseRange;
	    const float b = a + glm::two_pi<float> () * p.random;
	    const float dx = s * (std::sin (f * a) - std::sin (f * (a - dt)));
	    const float dy = s * (std::sin (f * b) - std::sin (f * (b - dt)));
	    // WE's particle space is y-up
	    p.position += glm::vec3 (dx * mask.x, -dy * mask.y, dx * mask.z);
	}
    };
}

// ========== RENDERING ==========

void CParticle::setupPass () {
    if (!m_particle.material || !m_particle.material->material || m_particle.material->material->passes.empty ()) {
	sLog.error ("No valid material for particle ", m_particle.name);
	WPENGINE_HEALTH ("asset_error", { { "kind", "material" }, { "message", "particle without a valid material: not drawn" } });
	return;
    }

    const auto& firstPass = **m_particle.material->material->passes.begin ();

    // Build override with particle-specific combos
    m_passOverride = std::make_unique<ImageEffectPassOverride> ();
    m_passOverride->combos["THICKFORMAT"] = 1;
    if (m_useRopeRenderer) {
	m_passOverride->shaderOverride = "genericropeparticle";
    }
    if (m_spritesheetFrames > 0) {
	m_passOverride->combos["SPRITESHEET"] = 1;
	// WE cross-fades a sheet's current and next frame (genericparticle.frag) unless the particle flags have
	// bit 1 or the frames are random (GH 0x1401fffb0); the frames stepped
	if ((m_particle.flags & 2) == 0 && m_particle.animationMode != "randomframe") {
	    m_passOverride->combos["SPRITESHEETBLEND"] = 1;
	}
	// WE sets SPRITESHEETBLENDNPOT for a sheet padded to a power-of-two texture (GH 0x1401fffb0; 3445973124's and
	// 3479521040 / 3490034653's birds): its frame walk (common_particles.h) wraps a row at the image's width inside
	// the padded texture. LWE uploads a mip at the size it's stored at (these two sheets: images of their own
	// size, not padded), where the plain walk with frameWidth = 1 / cols is that same walk (checked on the birds:
	// the NPOT walk over that upload cut them up), so it stays off
    }
    if (m_useTrailRenderer) {
	m_passOverride->combos["TRAILRENDERER"] = 1;
    }

    // Force texture 0 to use the input (particle texture) rather than the shader's
    // default "util/white" annotation, which would override it in setupRenderTexture()
    m_passBinds = { { 0, "previous" } };

    // Check if material uses REFRACT combo
    auto refractIt = firstPass.combos.find ("REFRACT");
    m_hasRefract = refractIt != firstPass.combos.end () && refractIt->second != 0;

    // Create the FBO provider for CPass
    m_passFBOProvider = std::make_shared<FBOProvider> (this);

    // For REFRACT: create a copy FBO that shadows _rt_FullFrameBuffer.
    // The REFRACT shader reads g_Texture3 (= _rt_FullFrameBuffer) while we render TO the scene FBO.
    // Reading from the same FBO being rendered to is undefined behavior in OpenGL, causing
    // black reads on NVIDIA. By placing a copy FBO with the same name in our FBOProvider,
    // CPass resolves g_Texture3 to the copy instead. We blit the scene content before each render.
    if (m_hasRefract) {
	auto sceneFBO = getScene ().getTargetFor (drawingObject ());
	float w = static_cast<float> (sceneFBO->getRealWidth ());
	float h = static_cast<float> (sceneFBO->getRealHeight ());
	m_refractFBO = m_passFBOProvider->create (
	    "_rt_FullFrameBuffer", sceneFBO->getFormat (), TextureFlags_ClampUVs, 1.0f, { w, h }, { w, h }
	);
    }

    // Create CPass with the WP particle shader
    m_pass = new Effects::CPass (*this, m_passFBOProvider, firstPass, *m_passOverride, m_passBinds, std::nullopt);

    // Set destination to scene FBO and input to particle texture
    m_pass->setDestination (getScene ().getTargetFor (drawingObject ()));
    m_pass->setInput (getTexture ());

    // Set matrix pointers - CPass will dereference these each frame
    m_pass->setModelViewProjectionMatrix (&m_mvpMatrix);
    m_pass->setModelViewProjectionMatrixInverse (&m_mvpMatrixInverse);
    m_pass->setModelMatrix (&m_modelMatrix);
    m_pass->setViewProjectionMatrix (&m_viewProjectionMatrix);

    // Create OpenGL buffers
    GLint prevVAO = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVAO);

    glGenVertexArrays (1, &m_vao);
    glGenBuffers (1, &m_vbo);
    glGenBuffers (1, &m_ebo);

    glBindVertexArray (m_vao);
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);

    const GLuint program = m_pass->getProgramID ();

    if (m_useRopeRenderer) {
	// Rope vertex layout: 7 attributes, 26 floats/vertex, stride=104 bytes
	// a_PositionVec4(4) + a_TexCoordVec4(4) + a_TexCoordVec4C1(4) + a_TexCoordVec4C2(4)
	// + a_TexCoordVec4C3(4) + a_TexCoordC4(2) + a_Color(4) = 26
	const GLsizei stride = sizeof (float) * ROPE_FLOATS_PER_VERTEX;

	const GLint loc0 = glGetAttribLocation (program, "a_PositionVec4");
	const GLint loc1 = glGetAttribLocation (program, "a_TexCoordVec4");
	const GLint loc2 = glGetAttribLocation (program, "a_TexCoordVec4C1");
	const GLint loc3 = glGetAttribLocation (program, "a_TexCoordVec4C2");
	const GLint loc4 = glGetAttribLocation (program, "a_TexCoordVec4C3");
	const GLint loc5 = glGetAttribLocation (program, "a_TexCoordC4");
	const GLint loc6 = glGetAttribLocation (program, "a_Color");

	if (loc0 >= 0) {
	    glEnableVertexAttribArray (loc0);
	    glVertexAttribPointer (loc0, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 0));
	}
	if (loc1 >= 0) {
	    glEnableVertexAttribArray (loc1);
	    glVertexAttribPointer (loc1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 4));
	}
	if (loc2 >= 0) {
	    glEnableVertexAttribArray (loc2);
	    glVertexAttribPointer (loc2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 8));
	}
	if (loc3 >= 0) {
	    glEnableVertexAttribArray (loc3);
	    glVertexAttribPointer (loc3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 12));
	}
	if (loc4 >= 0) {
	    glEnableVertexAttribArray (loc4);
	    glVertexAttribPointer (loc4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 16));
	}
	if (loc5 >= 0) {
	    glEnableVertexAttribArray (loc5);
	    glVertexAttribPointer (loc5, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 20));
	}
	if (loc6 >= 0) {
	    glEnableVertexAttribArray (loc6);
	    glVertexAttribPointer (loc6, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 22));
	}
    } else {
	// Sprite vertex layout: 5 attributes, 17 floats/vertex, stride=68 bytes
	// a_Position(3) + a_TexCoordVec4(4) + a_Color(4) + a_TexCoordVec4C1(4) + a_TexCoordC2(2) = 17
	const GLsizei stride = sizeof (float) * SPRITE_FLOATS_PER_VERTEX;

	const GLint loc0 = glGetAttribLocation (program, "a_Position");
	const GLint loc1 = glGetAttribLocation (program, "a_TexCoordVec4");
	const GLint loc2 = glGetAttribLocation (program, "a_Color");
	const GLint loc3 = glGetAttribLocation (program, "a_TexCoordVec4C1");
	const GLint loc4 = glGetAttribLocation (program, "a_TexCoordC2");

	if (loc0 >= 0) {
	    glEnableVertexAttribArray (loc0);
	    glVertexAttribPointer (loc0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 0));
	}
	if (loc1 >= 0) {
	    glEnableVertexAttribArray (loc1);
	    glVertexAttribPointer (loc1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 3));
	}
	if (loc2 >= 0) {
	    glEnableVertexAttribArray (loc2);
	    glVertexAttribPointer (loc2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 7));
	}
	if (loc3 >= 0) {
	    glEnableVertexAttribArray (loc3);
	    glVertexAttribPointer (loc3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 11));
	}
	if (loc4 >= 0) {
	    glEnableVertexAttribArray (loc4);
	    glVertexAttribPointer (loc4, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 15));
	}
    }

    glBindVertexArray (prevVAO);

    setupGeometryCallbacks ();
    setupParticleUniforms ();
}

void CParticle::setupGeometryCallbacks () {
    m_pass->setGeometryCallback (
	// Setup attribs: save current VAO, bind particle VAO
	[this] () {
	    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &m_prevVAO);
	    glBindVertexArray (m_vao);
	},
	// Draw geometry: indexed rendering
	[this] () { glDrawElements (GL_TRIANGLES, m_activeIndexCount, GL_UNSIGNED_INT, nullptr); },
	// Cleanup: restore previous VAO
	[this] () { glBindVertexArray (m_prevVAO); }
    );
}

void CParticle::setupParticleUniforms () {
    // Add particle-specific uniforms from common_particles.h that CPass doesn't provide
    // These are pointer-based: CPass reads the current value each frame
    m_pass->addUniform ("g_ModelMatrixInverse", &m_modelMatrixInverse);
    m_pass->addUniform ("g_OrientationUp", &m_orientationUp);
    m_pass->addUniform ("g_OrientationRight", &m_orientationRight);
    m_pass->addUniform ("g_OrientationForward", &m_orientationForward);
    m_pass->addUniform ("g_ViewUp", &m_viewUp);
    m_pass->addUniform ("g_ViewRight", &m_viewRight);
    m_pass->addUniform ("g_EyePosition", &m_eyePosition);
    m_pass->addUniform ("g_RenderVar0", &m_renderVar0);
    m_pass->addUniform ("g_RenderVar1", &m_renderVar1);

    // REFRACT: set g_RefractAmount (shader default 0.05, may not be applied by CPass's parameter system)
    if (m_hasRefract) {
	m_pass->addUniform ("g_RefractAmount", &m_refractAmount);
    }
}

void CParticle::refreshTransform () {
    // a particle system has a parent like any object (WE's shared base transform, GH 0x14020dcc0): its origin is in
    // its parent's space, rotated and scaled by the parent's, so it follows a moving or animated parent
    const ResolvedTransform transform = this->resolveTransform (m_particle);
    const float width = getScene ().getCamera ().getWidth ();
    const float height = getScene ().getCamera ().getHeight ();

    // from WE's world (y up, (0,0) bottom-left) to this projection's centred, y-down space
    glm::vec3 origin = transform.origin;
    origin.x -= width / 2.0f;
    origin.y = height / 2.0f - origin.y;

    m_resolvedScale = transform.scale;
    m_resolvedAngle = transform.angle;

    if (origin == m_transformedOrigin && m_transformValid) {
	return;
    }

    m_transformedOrigin = origin;
    m_transformValid = true;
    // (the control points follow in update (): a scene-space one stays where it is in the scene)
}

glm::mat3 CParticle::localFromScene () const {
    // the inverse of the system's rotation and scale (updateMatrices' order)
    glm::vec3 angles = m_particle.angles->value->getVec3 ();
    angles.z = m_resolvedAngle;
    glm::mat4 m (1.0f);
    m = glm::rotate (m, -angles.z, glm::vec3 (0, 0, 1));
    m = glm::rotate (m, angles.y, glm::vec3 (0, 1, 0));
    m = glm::rotate (m, -angles.x, glm::vec3 (1, 0, 0));
    glm::vec3 scale = m_resolvedScale;
    for (int i = 0; i < 3; i++) {
	if (std::abs (scale[i]) < 1e-6f) {
	    scale[i] = 1e-6f;
	}
    }
    m = glm::scale (m, scale);
    return glm::inverse (glm::mat3 (m));
}

const Object& CParticle::drawingObject () const {
    // a child draws where its top-level system draws
    return m_parentSystem != nullptr ? m_parentSystem->drawingObject () : this->getObject ();
}

glm::mat4 CParticle::systemMatrix () const {
    // updateMatrices' model matrix without the parallax
    glm::vec3 angles = m_particle.angles->value->getVec3 ();
    angles.z = m_resolvedAngle;
    glm::mat4 m = glm::translate (glm::mat4 (1.0f), m_transformedOrigin);
    m = glm::rotate (m, -angles.z, glm::vec3 (0, 0, 1));
    m = glm::rotate (m, angles.y, glm::vec3 (0, 1, 0));
    m = glm::rotate (m, -angles.x, glm::vec3 (1, 0, 0));
    glm::vec3 scale = m_resolvedScale;
    for (int i = 0; i < 3; i++) {
	if (std::abs (scale[i]) < 1e-6f) {
	    scale[i] = 1e-6f;
	}
    }
    return glm::scale (m, scale);
}

void CParticle::updateMatrices () {
    if (m_parentSystem != nullptr) {
	// a child is drawn in its parent's matrices (its particles are in the parent's space)
	m_modelMatrix = m_parentSystem->m_modelMatrix;
	m_modelMatrixInverse = m_parentSystem->m_modelMatrixInverse;
	m_viewProjectionMatrix = m_parentSystem->m_viewProjectionMatrix;
	m_mvpMatrix = m_parentSystem->m_mvpMatrix;
	m_mvpMatrixInverse = m_parentSystem->m_mvpMatrixInverse;
	m_eyePosition = m_parentSystem->m_eyePosition;
	m_viewUp = m_parentSystem->m_viewUp;
	m_viewRight = m_parentSystem->m_viewRight;
	// its own renderer's orientation, in the parent's matrices
	this->updateOrientation ();
	this->updateParticleRenderVars ();
	return;
    }
    glm::vec3 scale = m_resolvedScale;
    glm::vec3 angles = m_particle.angles->value->getVec3 ();
    // z turns with the parents' (resolveTransform's angle is the chain's sum); x and y are the system's own
    angles.z = m_resolvedAngle;

    m_modelMatrix = glm::mat4 (1.0f);
    m_modelMatrix = glm::translate (m_modelMatrix, m_transformedOrigin);
    this->applyParallaxToModelMatrix ();

    // Negate X and Z rotations to account for Y-flipped coordinate system
    m_modelMatrix = glm::rotate (m_modelMatrix, -angles.z, glm::vec3 (0, 0, 1));
    m_modelMatrix = glm::rotate (m_modelMatrix, angles.y, glm::vec3 (0, 1, 0));
    m_modelMatrix = glm::rotate (m_modelMatrix, -angles.x, glm::vec3 (1, 0, 0));
    m_modelMatrix = glm::scale (m_modelMatrix, scale);
    m_modelMatrixInverse = glm::inverse (m_modelMatrix);

    this->updateParticleViewProjection ();
    m_mvpMatrix = m_viewProjectionMatrix * m_modelMatrix;
    m_mvpMatrixInverse = glm::inverse (m_mvpMatrix);

    m_viewUp = glm::vec3 (0.0f, 1.0f, 0.0f);
    m_viewRight = glm::vec3 (1.0f, 0.0f, 0.0f);

    this->updateOrientation ();
    this->updateParticleRenderVars ();
}

void CParticle::updateOrientation () {
    // WE's sprite orientation (GH 0x14024fd30, the renderer's settings from GH 0x1401efc50): forward F and up U are
    // chosen in WE's world, taken into the system's space by the transpose of its model matrix's 3×3 (row-vector:
    // v' = M·v), normalised, and right is U × F:
    //   screen (0):  F = −view forward; U = the view's up (flags bit 0) or the system's y in the world, made
    //                perpendicular to F
    //   upright (1): U = world y (bit 0) or the system's `axis` in the world; R = view forward × U; F = R × U
    //   fixed (2):   F = `axis`, U = its in-plane up — as given (bit 0) or the system's own, in the world
    // LWE's particle space is WE's with y flipped (M_lwe = Fy·M_we·Fy, `updateMatrices`), and its sprites take
    // (1,0,0)/(0,1,0)/(0,0,1) for WE's identity: R and F go across with Fy, U with −Fy, so with no rotation every
    // mode is the identity LWE always drew
    if (m_particle.renderers.empty () || m_particle.renderers[0].name != "sprite") {
	// the trail and rope renderers have parsers of their own (not read): LWE's identity, as before
	m_orientationRight = glm::vec3 (1.0f, 0.0f, 0.0f);
	m_orientationUp = glm::vec3 (0.0f, 1.0f, 0.0f);
	m_orientationForward = glm::vec3 (0.0f, 0.0f, 1.0f);
	return;
    }
    const ParticleRenderer& renderer = m_particle.renderers[0];
    const glm::mat3 fy (1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const glm::mat3 m = fy * glm::mat3 (m_modelMatrix) * fy;
    const glm::mat3 toLocal = glm::transpose (m);

    const auto& camera = getScene ().getCamera ();
    glm::vec3 viewForward = camera.getCenter () - camera.getEye ();
    viewForward = glm::length (viewForward) > 1e-6f ? glm::normalize (viewForward) : glm::vec3 (0.0f, 0.0f, -1.0f);
    glm::vec3 viewUp = camera.getUp ();
    viewUp = glm::length (viewUp) > 1e-6f ? glm::normalize (viewUp) : glm::vec3 (0.0f, 1.0f, 0.0f);

    glm::vec3 forward, up;
    switch (renderer.orientation) {
	case 1:
	    {
		up = renderer.worldSpace ? glm::vec3 (0.0f, 1.0f, 0.0f) : m * renderer.axis;
		const glm::vec3 right = glm::cross (viewForward, up);
		forward = glm::cross (right, up);
		break;
	    }
	case 2:
	    forward = renderer.worldSpace ? renderer.axis : m * renderer.axis;
	    up = renderer.worldSpace ? renderer.axisUp : m * renderer.axisUp;
	    break;
	default:
	    forward = -viewForward;
	    up = renderer.worldSpace ? viewUp : m * glm::vec3 (0.0f, 1.0f, 0.0f);
	    up -= forward * glm::dot (forward, up);
	    break;
    }

    forward = toLocal * forward;
    up = toLocal * up;
    glm::vec3 right = glm::cross (up, forward);
    const auto unit = [] (const glm::vec3& v, const glm::vec3& fallback) {
	const float length = glm::length (v);
	return length > 1e-12f && std::isfinite (length) ? v / length : fallback;
    };
    forward = unit (forward, glm::vec3 (0.0f, 0.0f, 1.0f));
    up = unit (up, glm::vec3 (0.0f, 1.0f, 0.0f));
    right = unit (right, glm::vec3 (1.0f, 0.0f, 0.0f));

    m_orientationRight = fy * right;
    m_orientationUp = -(fy * up);
    m_orientationForward = fy * forward;
}

void CParticle::applyParallaxToModelMatrix () {
    // as for every object (CScene::getParallaxOffset): a world translation by the root's offset, y flipped
    const glm::vec2 parallax = getScene ().getParallaxOffset (m_particle);

    if (parallax.x == 0.0f && parallax.y == 0.0f) {
	return;
    }

    m_modelMatrix = glm::translate (glm::mat4 (1.0f), glm::vec3 (parallax.x, -parallax.y, 0.0f)) * m_modelMatrix;
}

void CParticle::updateParticleViewProjection () {
    if (getScene ().getCamera ().isPerspective ()) {
	// a 3D scene: the scene's perspective camera, which takes LWE's scene space the system's matrices are in
	m_viewProjectionMatrix = getScene ().getCamera ().getProjection () * getScene ().getCamera ().getLookAt ();
	m_eyePosition = getScene ().getCamera ().getSceneEye ();
    } else if ((m_particle.flags & 4) != 0) {
	// perspective particles (system flags bit 2) of an orthographic scene: the particle render (GH 0x14025bc30)
	// swaps in the scene's perspective camera of `perspectiveoverridefov` (GH 0x1401b4fd0), which puts z 0 where
	// the orthographic camera does. LWE had general.fov from an eye at z 1000 (z 0 drawn 1.16 x at fov 50)
	const auto& camera = getScene ().getCamera ();
	m_viewProjectionMatrix = camera.getPerspective2DViewProjection ();
	m_eyePosition = glm::vec3 (0.0f, 0.0f, camera.getPerspective2DDistance ());
    } else {
	// Orthographic projection from scene camera
	m_viewProjectionMatrix = getScene ().getCamera ().getProjection () * getScene ().getCamera ().getLookAt ();
	// For 2D/orthographic scenes the camera eye is at (0,0,0). The shader's
	// ComputeParticleTrailTangents uses cross(eyeDirection, velocity) to
	// compute the trail ribbon width. With eye at z=0 and particles at z=0,
	// eyeDirection is purely in XY — the cross product yields a Z-only vector
	// that is invisible under orthographic projection. Place the eye at z=1000
	// so the cross product produces a visible XY perpendicular direction.
	m_eyePosition = glm::vec3 (0.0f, 0.0f, 1000.0f);
    }
}

void CParticle::updateParticleRenderVars () {
    m_renderVar0 = glm::vec4 (m_trailLength, m_trailMaxLength, m_trailMinLength, 0.0f);

    if (m_spritesheetFrames > 0 && m_spritesheetCols > 0 && m_spritesheetRows > 0) {
	float frameWidth = 1.0f / static_cast<float> (m_spritesheetCols);
	float frameHeight = 1.0f / static_cast<float> (m_spritesheetRows);
	float textureRatio = 1.0f;
	if (const auto texture = getTexture ()) {
	    // Use atlas dimensions (from resolution vec4) for textureRatio, NOT getRealWidth/Height
	    // which returns per-frame dimensions for animated textures. The shader needs the
	    // per-frame pixel aspect ratio: (atlasH * frameHeight) / (atlasW * frameWidth).
	    const glm::vec4* res = texture->getResolution ();
	    float w = res->x; // atlas/GL texture width
	    float h = res->y; // atlas/GL texture height
	    if (w > 0.0f) {
		textureRatio = (h * frameHeight) / (w * frameWidth);
	    }
	}
	m_renderVar1 = glm::vec4 (frameWidth, frameHeight, static_cast<float> (m_spritesheetFrames), textureRatio);
    } else {
	// No spritesheet - texture ratio is height/width
	float textureRatio = 1.0f;
	if (const auto texture = getTexture ()) {
	    float w = static_cast<float> (texture->getRealWidth ());
	    float h = static_cast<float> (texture->getRealHeight ());
	    if (w > 0.0f) {
		textureRatio = h / w;
	    }
	}
	m_renderVar1 = glm::vec4 (0.0f, 0.0f, 0.0f, textureRatio);
    }
}

void CParticle::renderSprites () {
    if (m_particleCount == 0 || m_pass == nullptr) {
	return;
    }

    // Count alive particles
    uint32_t aliveCount = 0;
    for (uint32_t i = 0; i < m_particleCount; i++) {
	if (m_particles[i].alive) {
	    aliveCount++;
	}
    }

    if (aliveCount == 0) {
	return;
    }

    // Build vertex data in WP shader layout:
    // a_Position(3) + a_TexCoordVec4(uv.x, uv.y, rotZ, size)(4) + a_Color(4)
    //   + a_TexCoordVec4C1(vel.x, vel.y, vel.z, lifetime)(4) + a_TexCoordC2(rotX, rotY)(2) = 17 floats
    uint32_t vertexIndex = 0;
    uint32_t indexOffset = 0;

    for (uint32_t i = 0; i < m_particleCount; i++) {
	const auto& p = m_particles[i];
	if (!p.alive) {
	    continue;
	}

	// Skip particles with invalid values
	if (!std::isfinite (p.position.x) || !std::isfinite (p.position.y) || !std::isfinite (p.position.z)
	    || !std::isfinite (p.size) || p.size <= 0.0f || p.size > 10000.0f) {
	    continue;
	}

	// Compute the lifetime value for the WP shader's ComputeSpriteFrame.
	// The shader computes: floor(frac(lifetime) * numFrames) to get current frame,
	// and frac(lifetime * numFrames) for the blend factor between frames.
	// We encode the CPU-computed p.frame (which accounts for sequenceMultiplier
	// and animation mode) into the lifetime value the shader expects.
	float lifetime = p.getLifetimePos ();

	if (m_spritesheetFrames > 0 && p.frame >= 0.0f) {
	    if (m_particle.animationMode == "randomframe") {
		// Center within the frame to avoid floating-point edge cases
		lifetime = (p.frame + 0.5f) / static_cast<float> (m_spritesheetFrames);
	    } else {
		// Encode frame index + fractional blend: shader reconstructs via
		// floor(lifetime * numFrames) = current frame,
		// frac(lifetime * numFrames) = blend toward next frame
		lifetime = p.frame / static_cast<float> (m_spritesheetFrames);
	    }
	}

	auto addVertex = [&] (float u, float v) {
	    const uint32_t base = vertexIndex * SPRITE_FLOATS_PER_VERTEX;
	    // a_Position (vec3)
	    m_vertices[base + 0] = p.position.x;
	    m_vertices[base + 1] = p.position.y;
	    m_vertices[base + 2] = p.position.z;
	    // a_TexCoordVec4 (vec4: uv.x, uv.y, rotZ, size)
	    m_vertices[base + 3] = u;
	    m_vertices[base + 4] = v;
	    m_vertices[base + 5] = p.rotation.z;
	    m_vertices[base + 6] = p.size;
	    // a_Color (vec4: r, g, b, a)
	    m_vertices[base + 7] = p.color.r;
	    m_vertices[base + 8] = p.color.g;
	    m_vertices[base + 9] = p.color.b;
	    m_vertices[base + 10] = p.alpha;
	    // a_TexCoordVec4C1 (vec4: vel.x, vel.y, vel.z, lifetime)
	    m_vertices[base + 11] = p.velocity.x;
	    m_vertices[base + 12] = p.velocity.y;
	    m_vertices[base + 13] = p.velocity.z;
	    m_vertices[base + 14] = lifetime;
	    // a_TexCoordC2 (vec2: rotX, rotY)
	    m_vertices[base + 15] = p.rotation.x;
	    m_vertices[base + 16] = p.rotation.y;
	    vertexIndex++;
	};

	// 4 vertices for quad corners
	uint32_t baseVertex = vertexIndex;
	addVertex (0.0f, 1.0f); // 0: Bottom-left
	addVertex (1.0f, 1.0f); // 1: Bottom-right
	addVertex (1.0f, 0.0f); // 2: Top-right
	addVertex (0.0f, 0.0f); // 3: Top-left

	// 6 indices forming 2 triangles
	m_indices[indexOffset++] = baseVertex + 0;
	m_indices[indexOffset++] = baseVertex + 1;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 3;
	m_indices[indexOffset++] = baseVertex + 0;
    }

    m_activeIndexCount = static_cast<GLsizei> (indexOffset);
    if (m_activeIndexCount == 0) {
	return;
    }

#if !NDEBUG
    std::string str = "Particles ";
    str += this->getParticle ().name + " (" + std::to_string (this->getId ()) + ", " + this->getParticle ().particleFile
	+ ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    // Upload vertex and index data
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertexIndex * SPRITE_FLOATS_PER_VERTEX * sizeof (float)),
	m_vertices.data (), GL_DYNAMIC_DRAW
    );

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indexOffset * sizeof (uint32_t)), m_indices.data (),
	GL_DYNAMIC_DRAW
    );

    // Update matrices and uniform data
    updateMatrices ();

    // For REFRACT: blit current scene content into the copy FBO before rendering.
    // This gives the shader a snapshot of what's behind the particles for refraction,
    // without a feedback loop (rendering to scene FBO while reading from copy FBO).
    if (m_hasRefract && m_refractFBO) {
	auto sceneFBO = getScene ().getTargetFor (drawingObject ());
	GLint w = static_cast<GLint> (sceneFBO->getRealWidth ());
	GLint h = static_cast<GLint> (sceneFBO->getRealHeight ());
	glBindFramebuffer (GL_READ_FRAMEBUFFER, sceneFBO->getFramebuffer ());
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_refractFBO->getFramebuffer ());
	glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    // The shader's ComputeParticleTrailTangents produces a right vector with a Z component
    // (from cross(eyeDirection, velocity) where eyeDirection has XY offset from model transform).
    // For 2D/ortho particles at z≈0, the ortho near plane sits at ndc.z=-1 — any Z offset from
    // the right vector pushes vertices past the near plane, causing half the quad to be clipped.
    // GL_DEPTH_CLAMP prevents near/far clipping by clamping depth instead.
    glEnable (GL_DEPTH_CLAMP);

    // CPass::render() handles: FBO binding, texture setup, uniforms, blending, draw call, cleanup
    m_pass->render ();

    glDisable (GL_DEPTH_CLAMP);

#if !NDEBUG
    glPopDebugGroup ();
#endif
}

int CParticle::ropeSubSegments () const {
    // WE sets TRAILSUBDIVISION = the renderer's `subdivision` (GH 0x1401fffb0; default 4 for rope, 1 for ropetrail and
    // spritetrail, GH 0x1401ee560 / 0x1401ee8c0) and genericropeparticle.geom emits that many points between a
    // segment's two ends: subdivision + 1 pieces (it was max (1, subdivision): one piece fewer, a straight rope at 1)
    return std::max (0, m_ropeSubdivision) + 1;
}

void CParticle::renderRope () {
    if (m_particleCount < 2 || m_pass == nullptr) {
	return;
    }

    // Array is already in spawn order (oldest at index 0) thanks to order-preserving
    // compaction in update(). All particles in [0, m_particleCount) are alive.
    const uint32_t aliveCount = m_particleCount;

    // Build vertex data with WE's rope spline, subdivided into ropeSubSegments () pieces per segment.
    //
    // Rope vertex layout (26 floats per vertex, THICKFORMAT):
    // [0-3]   a_PositionVec4:   startPos.xyz, sizeStart
    // [4-7]   a_TexCoordVec4:   endPos.xyz, trailLength
    // [8-11]  a_TexCoordVec4C1: CP0.xyz, trailPosition
    // [12-15] a_TexCoordVec4C2: CP1.xyz, sizeEnd
    // [16-19] a_TexCoordVec4C3: colorEnd.rgba
    // [20-21] a_TexCoordC4:     uvs.xy
    // [22-25] a_Color:          colorStart.rgba

    const uint32_t numSegments = aliveCount - 1;
    const int subdivision = this->ropeSubSegments ();

    // WE's rope segment b -> c (GH 0x140255e50 writes b, c and the neighbours a / d as CP0 / CP1, clamped to the
    // segment's own ends) is the bezier genericropeparticle.vert builds: inner points b + 0.15 (c - a) and
    // c + 0.15 (b - d). Uniform Catmull-Rom (what LWE had) is the same bezier with 1/6
    auto ropeBezier = [] (const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
			  float t) -> glm::vec3 {
	const glm::vec3 c1 = p1 + 0.15f * (p2 - p0);
	const glm::vec3 c2 = p2 + 0.15f * (p1 - p3);
	const float u = 1.0f - t;
	return u * u * u * p1 + 3.0f * u * u * t * c1 + 3.0f * u * t * t * c2 + t * t * t * p2;
    };

    // First pass: evaluate spline to get all interpolated points
    const uint32_t totalPoints = numSegments * subdivision + 1;
    // Store position, size, color (rgba) per point = 3 + 1 + 4 = 8 floats
    std::vector<glm::vec3> splinePositions (totalPoints);
    std::vector<float> splineSizes (totalPoints);
    std::vector<glm::vec4> splineColors (totalPoints); // rgba

    for (uint32_t i = 0; i < numSegments; i++) {
	const auto& p1 = m_particles[i];
	const auto& p2 = m_particles[i + 1];
	const auto& p0 = (i > 0) ? m_particles[i - 1] : p1;
	const auto& p3 = (i + 2 < aliveCount) ? m_particles[i + 2] : p2;

	for (int k = 0; k < subdivision; k++) {
	    float t = static_cast<float> (k) / static_cast<float> (subdivision);
	    uint32_t idx = i * subdivision + k;

	    splinePositions[idx] = ropeBezier (p0.position, p1.position, p2.position, p3.position, t);
	    splineSizes[idx] = glm::mix (p1.size, p2.size, t);
	    splineColors[idx] = glm::mix (glm::vec4 (p1.color, p1.alpha), glm::vec4 (p2.color, p2.alpha), t);
	}
    }
    // Last point is the final particle
    {
	const auto& pLast = m_particles[aliveCount - 1];
	splinePositions[totalPoints - 1] = pLast.position;
	splineSizes[totalPoints - 1] = pLast.size;
	splineColors[totalPoints - 1] = glm::vec4 (pLast.color, pLast.alpha);
    }

    // Second pass: build quads from consecutive spline points.
    // The shader computes UV.v from trailPosition / (trailLength - 1), consuming
    // 1/(trailLength-1) of UV space per quad. Express trailLength and trailPosition
    // in sub-segment units so each sub-segment quad gets the correct UV slice.
    // UV scale divides the effective length, making UVs exceed [0,1] → texture repeats.
    uint32_t vertexIndex = 0;
    uint32_t indexOffset = 0;
    const uint32_t totalSubSegments = totalPoints - 1;
    const float uvScale = (m_ropeUVScale > 0.0f) ? m_ropeUVScale : 1.0f;
    const float trailLength = static_cast<float> (totalSubSegments) / uvScale + 1.0f;
    const float usableLength = trailLength - 1.0f;

    // UV smoothing: distribute UV proportional to arc length instead of uniform index.
    // Per wiki: only when all particle lifetimes match and scrolling is disabled.
    const bool useSmoothing = m_ropeUVSmoothing && m_uniformLifetimes && !m_ropeUVScrolling;
    std::vector<float> cumulativeArcLength;
    float totalArcLength = 0.0f;

    if (useSmoothing) {
	cumulativeArcLength.resize (totalPoints, 0.0f);
	for (uint32_t i = 1; i < totalPoints; i++) {
	    totalArcLength += glm::distance (splinePositions[i], splinePositions[i - 1]);
	    cumulativeArcLength[i] = totalArcLength;
	}
    }

    // UV scrolling: shift UV along the rope over time (1 UV cycle per second)
    float scrollOffset = 0.0f;
    if (m_ropeUVScrolling && usableLength > 0.0f) {
	scrollOffset
	    = std::fmod (static_cast<float> (this->getContext ().getContext ().renderTime), 10000.0f) * usableLength;
    }

    for (uint32_t s = 0; s < totalSubSegments; s++) {
	const glm::vec3& posStart = splinePositions[s];
	const glm::vec3& posEnd = splinePositions[s + 1];
	float sizeStart = splineSizes[s];
	float sizeEnd = splineSizes[s + 1];
	const glm::vec4& colorStart = splineColors[s];
	const glm::vec4& colorEnd = splineColors[s + 1];

	// Neighboring points for shader tangent computation (CP0/CP1)
	const glm::vec3& posPrev = (s > 0) ? splinePositions[s - 1] : posStart;
	const glm::vec3& posAfter = (s + 2 < totalPoints) ? splinePositions[s + 2] : posEnd;

	// Compute trailPosition for UV mapping
	float trailPosition;
	if (useSmoothing && totalArcLength > 0.0f) {
	    // Arc-length parameterization: map cumulative distance to sub-segment space
	    trailPosition = cumulativeArcLength[s] / totalArcLength * static_cast<float> (totalSubSegments);
	} else {
	    trailPosition = static_cast<float> (s);
	}
	trailPosition += scrollOffset;

	auto addRopeVertex = [&] (float uvX, float uvY) {
	    const uint32_t base = vertexIndex * ROPE_FLOATS_PER_VERTEX;

	    // a_PositionVec4: startPos.xyz, sizeStart
	    m_vertices[base + 0] = posStart.x;
	    m_vertices[base + 1] = posStart.y;
	    m_vertices[base + 2] = posStart.z;
	    m_vertices[base + 3] = sizeStart;

	    // a_TexCoordVec4: endPos.xyz, trailLength
	    m_vertices[base + 4] = posEnd.x;
	    m_vertices[base + 5] = posEnd.y;
	    m_vertices[base + 6] = posEnd.z;
	    m_vertices[base + 7] = trailLength;

	    // a_TexCoordVec4C1: CP0.xyz (neighbor before start), trailPosition
	    m_vertices[base + 8] = posPrev.x;
	    m_vertices[base + 9] = posPrev.y;
	    m_vertices[base + 10] = posPrev.z;
	    m_vertices[base + 11] = trailPosition;

	    // a_TexCoordVec4C2: CP1.xyz (neighbor after end), sizeEnd
	    m_vertices[base + 12] = posAfter.x;
	    m_vertices[base + 13] = posAfter.y;
	    m_vertices[base + 14] = posAfter.z;
	    m_vertices[base + 15] = sizeEnd;

	    // a_TexCoordVec4C3: colorEnd.rgba
	    m_vertices[base + 16] = colorEnd.r;
	    m_vertices[base + 17] = colorEnd.g;
	    m_vertices[base + 18] = colorEnd.b;
	    m_vertices[base + 19] = colorEnd.a;

	    // a_TexCoordC4: uvs.xy
	    m_vertices[base + 20] = uvX;
	    m_vertices[base + 21] = uvY;

	    // a_Color: colorStart.rgba
	    m_vertices[base + 22] = colorStart.r;
	    m_vertices[base + 23] = colorStart.g;
	    m_vertices[base + 24] = colorStart.b;
	    m_vertices[base + 25] = colorStart.a;

	    vertexIndex++;
	};

	// Quad: 4 vertices (left/right at start/end of segment)
	uint32_t baseVertex = vertexIndex;
	addRopeVertex (0.0f, 0.0f); // left at start
	addRopeVertex (1.0f, 0.0f); // right at start
	addRopeVertex (1.0f, 1.0f); // right at end
	addRopeVertex (0.0f, 1.0f); // left at end

	// 2 triangles
	m_indices[indexOffset++] = baseVertex + 0;
	m_indices[indexOffset++] = baseVertex + 1;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 3;
	m_indices[indexOffset++] = baseVertex + 0;
    }

    m_activeIndexCount = static_cast<GLsizei> (indexOffset);
    if (m_activeIndexCount == 0) {
	return;
    }

#if !NDEBUG
    std::string str = "Rope particles ";
    str += this->getParticle ().name + " (" + std::to_string (this->getId ()) + ", " + this->getParticle ().particleFile
	+ ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    // Upload vertex and index data
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertexIndex * ROPE_FLOATS_PER_VERTEX * sizeof (float)),
	m_vertices.data (), GL_DYNAMIC_DRAW
    );

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indexOffset * sizeof (uint32_t)), m_indices.data (),
	GL_DYNAMIC_DRAW
    );

    // Update matrices and uniform data
    updateMatrices ();

    // For REFRACT: blit current scene content into the copy FBO before rendering
    if (m_hasRefract && m_refractFBO) {
	auto sceneFBO = getScene ().getTargetFor (drawingObject ());
	GLint w = static_cast<GLint> (sceneFBO->getRealWidth ());
	GLint h = static_cast<GLint> (sceneFBO->getRealHeight ());
	glBindFramebuffer (GL_READ_FRAMEBUFFER, sceneFBO->getFramebuffer ());
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_refractFBO->getFramebuffer ());
	glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glEnable (GL_DEPTH_CLAMP);
    m_pass->render ();
    glDisable (GL_DEPTH_CLAMP);

#if !NDEBUG
    glPopDebugGroup ();
#endif
}
