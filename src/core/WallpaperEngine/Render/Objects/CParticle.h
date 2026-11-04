#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <functional>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <random>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace WallpaperEngine::Render::Objects {

constexpr uint32_t DEFAULT_MAX_PARTICLES = 1000;

/**
 * Runtime particle instance state
 */
struct ParticleInstance {
    // Position and movement
    glm::vec3 position { 0.0f };
    glm::vec3 velocity { 0.0f };
    glm::vec3 acceleration { 0.0f };

    // Rotation
    glm::vec3 rotation { 0.0f };
    glm::vec3 angularVelocity { 0.0f };
    glm::vec3 angularAcceleration { 0.0f };

    // Visual properties
    glm::vec3 color { 1.0f };
    float alpha { 1.0f };
    float size { 20.0f };
    float frame { 0.0f }; // Current animation frame

    // Lifetime
    float lifetime { 1.0f }; // Total lifetime in seconds
    float age { 0.0f }; // Current age in seconds

    /** one random value in [0, 1) per particle, drawn at spawn: oscillate* and turbulence read it (WE: system +0x338) */
    float random { 0.0f };
    /** a per-system serial number: the particle a follow child is bound to (WE binds by particle id) */
    uint32_t id { 0 };

    // Initial values for resets/multipliers
    struct {
	glm::vec3 color { 1.0f };
	float alpha { 1.0f };
	float size { 20.0f };
	float lifetime { 1.0f };
    } initial;

    bool alive { false };

    // Get normalized lifetime position (0.0 to 1.0)
    float getLifetimePos () const { return lifetime > 0.0f ? (age / lifetime) : 1.0f; }

    bool isAlive () const { return alive && age < lifetime; }
};

/**
 * Control point runtime data
 */
struct ControlPointData {
    glm::vec3 position { 0.0f };
    glm::vec3 offset { 0.0f };
    bool linkMouse { false };
    bool worldSpace { false };
    uint32_t flags { 0 };
};

/**
 * Particle emitter function
 */
using EmitterFunc = std::function<void (std::vector<ParticleInstance>&, uint32_t&, float)>;

/**
 * Particle initializer function
 */
using InitializerFunc = std::function<void (ParticleInstance&)>;

/**
 * Particle operator function
 */
using OperatorFunc = std::function<
    void (std::vector<ParticleInstance>&, uint32_t, const std::vector<ControlPointData>&, float, float)>;

class CParticle final : public CRenderable, public Scripting::ScriptableObject {
    friend CObject;

public:
    CParticle (Wallpapers::CScene& scene, const Particle& particle);
    /** a child system's instance (P2), simulated in `parent`'s space */
    CParticle (
	Wallpapers::CScene& scene, const Particle& particle, CParticle& parent, const ParticleChild& child,
	bool simulateOnly
    );
    ~CParticle ();

    void setup () override;
    void render () override;
    void update (float dt);

    // SceneScript's IParticleSystem
    /** play (): emitting again (after pause or stop) */
    void scriptPlay ();
    /** pause (): no new particles; the living ones go on */
    void scriptPause ();
    /** stop (): every particle gone now, and no new ones */
    void scriptStop ();
    /** isPlaying (): emitting, or particles still alive */
    [[nodiscard]] bool scriptIsPlaying () const;
    /** emitParticles (count): `count` more on the next step, paused or not */
    void scriptEmit (uint32_t count);
    /** IParticleSystemInstance: an instanceoverride value by its script name (alpha … controlpoint7), nullptr if none */
    [[nodiscard]] DynamicValue* instanceValue (const std::string& name);

    [[nodiscard]] const Particle& getParticle () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

protected:
    void setupEmitters ();
    void setupInitializers ();
    void setupOperators ();

    /** a uniform draw in [0, 1) from the scene's generator, raised to `exponent` (WE's initializers) */
    float randomT (float exponent);

    /** an emitter's timers (WE's emitter record, GH 0x14025cfe0 / 0x140254f50) */
    struct EmitterState {
	float delay { 0.0f };
	float durationLeft { 0.0f };
	float accumulator { 0.0f };
	/** > 0: the time left of a periodic "on" window; <= 0: minus the time left of an "off" one */
	float periodicTimer { 0.0f };
	uint32_t burst { 0 };
	/** a follow child's particle died: no more emission (WE's bit 31, GH 0x140254ed0) */
	bool stopped { false };
    };
    std::shared_ptr<EmitterState> addEmitterState (const ParticleEmitter& emitter);
    static void resetEmitter (const ParticleEmitter& emitter, EmitterState& state);
    /** how many particles the emitter spawns this step (rate × count, burst, delay, duration, periodic windows) */
    /** an audio-responsive emitter's rate factor this frame, 0..1 */
    [[nodiscard]] float audioLevel (const ParticleEmitter& emitter) const;
    uint32_t emitCount (const ParticleEmitter& emitter, EmitterState& state, float dt);
    /** the emitter's own count this step (emitCount adds a script's and holds it while halted) */
    uint32_t emitterCount (const ParticleEmitter& emitter, EmitterState& state, float dt);
    /** WE's hidden-system reset (GH 0x140255d00): every particle gone, every emitter reloaded */
    void resetSystem ();
    /** re-evaluates the `colorn` tint for this step (WE's override patcher GH 0x1401ff240) */
    void patchColor ();
    /** a colour shifted in HSV by (colorn − reference), WE's patch op 0xb */
    glm::vec3 tintColor (const glm::vec3& color) const;

    /** the starttime prewarm (GH 0x140254cc0) */
    void prewarm ();

    /** the child systems of one entry of `children` (P2): its live instances and the finished ones kept for reuse */
    struct ChildSet {
	const ParticleChild* child { nullptr };
	/** 1 static, 2 eventfollow, 3 eventspawn, 4 eventdeath (WE's numbering) */
	int type { 1 };
	std::vector<std::unique_ptr<CParticle>> active;
	std::vector<std::unique_ptr<CParticle>> pool;
	/** an event child's one drawing instance (it never simulates): the live instances' particles go through it */
	std::unique_ptr<CParticle> renderer;
    };
    std::unique_ptr<CParticle> makeChild (ChildSet& set, const glm::vec3& at);
    /** the event children of a particle event: 3 = spawned (follow and spawn children), 4 = died */
    void spawnEventChildren (int type, uint32_t id, const glm::vec3& at);
    void updateChildren (float dt);
    void drawChildren ();
    /** draws these instances' particles with this system's pass */
    void drawFrom (const std::vector<const CParticle*>& instances);
    [[nodiscard]] bool childAlive () const;
    void stopEmitters ();
    /** from the scene's centred space to the system's: the inverse of its rotation and scale */
    [[nodiscard]] glm::mat3 localFromScene () const;
    /** the object whose target a system draws into (a child: its top-level system's) */
    [[nodiscard]] const Object& drawingObject () const;
    /** a child takes its parent's place and matrices */
    void takeParentTransform ();

    // Emitter creators
    EmitterFunc createBoxEmitter (const ParticleEmitter& emitter);
    EmitterFunc createSphereEmitter (const ParticleEmitter& emitter);

    // Initializer creators
    InitializerFunc createColorRandomInitializer (const ColorRandomInitializer& init);
    InitializerFunc createSizeRandomInitializer (const SizeRandomInitializer& init);
    InitializerFunc createAlphaRandomInitializer (const AlphaRandomInitializer& init);
    InitializerFunc createLifetimeRandomInitializer (const LifetimeRandomInitializer& init);
    InitializerFunc createVelocityRandomInitializer (const VelocityRandomInitializer& init);
    InitializerFunc createRotationRandomInitializer (const RotationRandomInitializer& init);
    InitializerFunc createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init);
    InitializerFunc createTurbulentVelocityRandomInitializer (const TurbulentVelocityRandomInitializer& init);
    /** a spawned particle's common start (WE's spawn head) and its initializers */
    void spawnParticle (ParticleInstance& p, const glm::vec3& velocity);
    /** the overrides as the parser binds them, 1 where the particle flags opt out (0x10 speed, 0x40 lifetime, 0x80 size) */
    [[nodiscard]] float speedOverride () const;
    [[nodiscard]] float sizeOverride () const;
    [[nodiscard]] float lifetimeOverride () const;
    InitializerFunc
    createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init);

    // Operator creators
    OperatorFunc createMovementOperator (const MovementOperator& op);
    OperatorFunc createAngularMovementOperator (const AngularMovementOperator& op);
    OperatorFunc createAlphaFadeOperator (const AlphaFadeOperator& op);
    OperatorFunc createSizeChangeOperator (const SizeChangeOperator& op);
    OperatorFunc createAlphaChangeOperator (const AlphaChangeOperator& op);
    OperatorFunc createColorChangeOperator (const ColorChangeOperator& op);
    OperatorFunc createTurbulenceOperator (const TurbulenceOperator& op);
    OperatorFunc createVortexOperator (const VortexOperator& op);
    OperatorFunc createVortexV2Operator (const VortexOperator& op);
    OperatorFunc createControlPointAttractOperator (const ControlPointAttractOperator& op);
    OperatorFunc createOscillateAlphaOperator (const OscillateAlphaOperator& op);
    OperatorFunc createOscillateSizeOperator (const OscillateSizeOperator& op);
    OperatorFunc createOscillatePositionOperator (const OscillatePositionOperator& op);

    // Rendering
    void renderSprites ();
    void renderRope ();
    void setupPass ();
    void setupGeometryCallbacks ();
    void setupParticleUniforms ();
    void updateMatrices ();
    /** g_Orientation* from the sprite renderer's orientation, in this system's matrices (WE's GH 0x14024fd30) */
    void updateOrientation ();
    /** The system's origin/scale/angle this frame, with its parents' (P36/G04); moves world-space control points */
    void refreshTransform ();
    void applyParallaxToModelMatrix ();
    void updateParticleViewProjection ();
    void updateParticleRenderVars ();

private:
    const Particle& m_particle;

    std::vector<ParticleInstance> m_particles;
    uint32_t m_particleCount { 0 };
    /** a script paused or stopped the system (IParticleSystem): its emitters emit nothing */
    bool m_scriptHalted { false };
    /** particles a script asked for (emitParticles), emitted by the next emitter step */
    uint32_t m_scriptEmit { 0 };
    uint32_t m_maxParticles { DEFAULT_MAX_PARTICLES };

    std::vector<EmitterFunc> m_emitters;
    std::vector<std::pair<const ParticleEmitter*, std::shared_ptr<EmitterState>>> m_emitterStates;
    std::vector<InitializerFunc> m_initializers;
    std::vector<OperatorFunc> m_operators;

    std::vector<ControlPointData> m_controlPoints;
    /** the control points the operators see: m_controlPoints, in the parent's space for a child */
    std::vector<ControlPointData> m_opControlPoints;
    /** the control points the system's JSON defines (bit per id) */
    uint32_t m_ownControlPoints { 0 };

    // child systems (P2)
    std::vector<ChildSet> m_children;
    bool m_hasEventChildren { false };
    /** this step's spawned particles (id, position), for the event children */
    std::vector<std::pair<uint32_t, glm::vec3>> m_spawned;
    uint32_t m_nextParticleId { 0 };
    /** a child instance's parent and definition, its offset (origin + its particle) and turn/scale */
    CParticle* m_parentSystem { nullptr };
    const ParticleChild* m_childDef { nullptr };
    glm::vec3 m_childOrigin { 0.0f };
    glm::vec3 m_childOffset { 0.0f };
    glm::mat3 m_childLinear { 1.0f };
    glm::mat3 m_childLinearInverse { 1.0f };
    float m_childSizeScale { 1.0f };
    /** an event child's instance: simulated, drawn through its child entry's renderer (no GL objects of its own) */
    bool m_simulateOnly { false };
    /** an event child entry's renderer: set up to draw, never simulated (no children, no prewarm) */
    bool m_drawOnly { false };
    /** the particle a follow child is bound to, -1 when none */
    int64_t m_boundId { -1 };

    std::vector<float> m_vertices;
    std::vector<uint32_t> m_indices;

    double m_time { 0.0 };
    /** the system's own clock: the sum of its (rate-scaled) steps, prewarm included (WE: system+0x3f0) */
    float m_simTime { 0.0f };
    /** this step's force dt (drag, attraction, vortex, turbulence) and the last real frame time (prewarm: 1/60 s) */
    float m_forceDt { 0.0f };
    float m_frameTime { 1.0f / 60.0f };

    // colorn (P5): the system's reference colour comes from its first colorrandom (the mid-point of min and
    // max) or else its first colorchange (startvalue); without either it is white and `m_hasColor` is false
    const ColorRandomInitializer* m_referenceColorRandom { nullptr };
    const ColorChangeOperator* m_referenceColorChange { nullptr };
    bool m_hasColor { false };
    /** colorn differs from the reference: colour initializers are shifted in HSV */
    bool m_tint { false };
    glm::vec3 m_colornHSV { 0.0f };
    glm::vec3 m_referenceHSV { 0.0f };
    /** every particle's colour before the initializers: colorn for a system without a colour source, else white */
    glm::vec3 m_spawnColor { 1.0f };

    // CPass-based rendering
    Effects::CPass* m_pass { nullptr };
    std::unique_ptr<ImageEffectPassOverride> m_passOverride;
    std::shared_ptr<FBOProvider> m_passFBOProvider;
    TextureMap m_passBinds;
    GLsizei m_activeIndexCount { 0 };

    // REFRACT support: copy of scene FBO to avoid read-write conflict
    bool m_hasRefract { false };
    std::shared_ptr<CFBO> m_refractFBO;

    // OpenGL buffers
    GLuint m_vao { 0 };
    GLuint m_vbo { 0 };
    GLuint m_ebo { 0 };
    GLint m_prevVAO { 0 };

    // Particle-specific uniform data (stored here, pointed to by CPass)
    glm::mat4 m_modelMatrix { 1.0f };
    glm::mat4 m_modelMatrixInverse { 1.0f };
    glm::mat4 m_mvpMatrix { 1.0f };
    glm::mat4 m_mvpMatrixInverse { 1.0f };
    glm::mat4 m_viewProjectionMatrix { 1.0f };
    glm::vec3 m_orientationUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 m_orientationRight { 1.0f, 0.0f, 0.0f };
    glm::vec3 m_orientationForward { 0.0f, 0.0f, 1.0f };
    glm::vec3 m_viewUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 m_viewRight { 1.0f, 0.0f, 0.0f };
    glm::vec3 m_eyePosition { 0.0f, 0.0f, 1000.0f };
    glm::vec4 m_renderVar0 { 0.0f };
    glm::vec4 m_renderVar1 { 0.0f };

    // Spritesheet animation data
    int m_spritesheetCols { 0 };
    int m_spritesheetRows { 0 };
    int m_spritesheetFrames { 0 };
    float m_spritesheetDuration { 1.0f };

    // Material shader constants
    float m_overbright { 1.0f };
    float m_refractAmount { 0.05f }; // Default from shader annotation

    // Renderer configuration
    bool m_useTrailRenderer { false };
    float m_trailLength { 0.05f };
    float m_trailMaxLength { 10.0f };
    float m_trailMinLength { 0.0f };
    // Rope renderer (rope + ropetrail both use genericropeparticle shader)
    bool m_useRopeRenderer { false };
    int m_ropeSubdivision { 4 }; // Catmull-Rom subdivisions between points (smoothing)
    int m_ropeSegments { 4 }; // ropetrail: historical position snapshots per particle
    float m_ropeUVScale { 1.0f };
    bool m_ropeUVScrolling { false };
    bool m_ropeUVSmoothing { true }; // rope only
    bool m_uniformLifetimes { false }; // true when lifetime min==max (enables UV smoothing)

    // Per-vertex float counts for different renderer types
    static constexpr int SPRITE_FLOATS_PER_VERTEX = 17;
    static constexpr int ROPE_FLOATS_PER_VERTEX = 26;

    // Transformed origin (screen space to centered space conversion)
    glm::vec3 m_transformedOrigin { 0.0f };
    glm::vec3 m_resolvedScale { 1.0f };
    float m_resolvedAngle { 0.0f };
    bool m_transformValid { false };

    // Last known resolution for detecting changes
    float m_lastScreenWidth { 0.0f };
    float m_lastScreenHeight { 0.0f };

    // The scene's random source, shared by every particle system (WE has one, at scene+0x1f08)
    std::mt19937& m_rng;

    bool m_initialized { false };
};
} // namespace WallpaperEngine::Render::Objects
