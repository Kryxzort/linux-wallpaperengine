#pragma once

#include <array>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "../Utils/TypeCaster.h"
#include "DynamicValue.h"
#include "Effect.h"
#include "Material.h"
#include "Model.h"
#include "Types.h"
#include "UserSetting.h"
#include <memory>

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;

struct ObjectData {
    int id;
    std::string name;
    std::vector<int> dependencies;
    std::optional<int> parent;
    /** The point of origin of the object */
    UserSettingUniquePtr origin;
    /** Transform fields for generic scene/group objects. Typed objects keep their own transform fields. */
    UserSettingUniquePtr groupScale;
    UserSettingUniquePtr groupAngles;
    UserSettingUniquePtr groupVisible;
    /** Mouse parallax depth (x, y) of any object; Wallpaper Engine's default is 1 1. Images and particles also keep
     *  their own copy (registered as a script property), which the parallax reads for them */
    UserSettingUniquePtr groupParallaxDepth;
    /** A camera object (`"camera": "default"`): its `zoom` (null on every other object) */
    UserSettingUniquePtr cameraZoom;
    /** `solid` (default true): the object receives cursor events (WE's docs: "All mouse cursor events will only work on
     *  objects marked as Solid") */
    bool solid = true;
    /** `attachment`: the name of a parent puppet's attachment (MDAT) the object is pinned to; its origin, scale and
     *  angle are then in the attachment's space, which follows its bone (GH 0x14020be30 looks it up by name, the
     *  world transform GH 0x1401b5170 applies it under the parent's) */
    std::optional<std::string> attachment = std::nullopt;
    /** A camera object's `fov` (null otherwise, or when it has none): a 3D scene's view angle while it is active */
    UserSettingUniquePtr cameraFov = nullptr;
};

/**
 * Base class for all objects, represents a single object in the scene
 *
 * @see Image
 * @see Sound
 * @see Particle
 * @see Text
 * @see Light
 */
class Object : public TypeCaster, public ObjectData {
public:
    explicit Object (ObjectData data) noexcept : TypeCaster (), ObjectData (std::move (data)) { };
    ~Object () override = default;
};

/**
 * Overrides effect's passes configuration
 *
 * @see ImageEffect
 * @see EffectPass
 */
struct ImageEffectPassOverride {
    int id;
    ComboMap combos;
    ShaderConstantMap constants;
    TextureMap textures;
    TextureMap usertextures;
    std::optional<std::string> shaderOverride; // Overrides MaterialPass::shader when set
};

/**
 * Override information for an specific effect
 *
 * @see ImageEffect
 * @see Effect
 * @see EffectPass
 * @see ImageEffectPass
 */
struct ImageEffect {
    /** Not sure what it's used for */
    int id;
    /** Effect's name for the editor */
    std::string name;
    /** If this effect is visible or not */
    UserSettingUniquePtr visible;
    /** Pass overrides to apply to the effect's passes */
    std::vector<ImageEffectPassOverrideUniquePtr> passOverrides;
    /** The effect definition */
    EffectUniquePtr effect;
};

/**
 * Animation layers for the puppet warp
 */
struct ImageAnimationLayer {
    int id;
    /** the layer's custom name (SceneScript IAnimationLayer.name, getAnimationLayer (name)) */
    std::string name;
    UserSettingUniquePtr rate;
    UserSettingUniquePtr visible;
    UserSettingUniquePtr blend;
    UserSettingUniquePtr animation;
    /** The layer's reflected fields (GH 0x140292030; defaults from the constructor GH 0x140291d20): flags +0xd0 bit 0
     *  visible (default on), bit 1 additive, bit 2 blendin, bit 3 blendout; blendtime +0x18c (0.5) */
    bool additive = false;
    bool blendIn = false;
    bool blendOut = false;
    float blendTime = 0.5f;
};

struct ImageData {
    /** The scale of the image */
    UserSettingUniquePtr scale;
    /** The rotation of the image */
    UserSettingUniquePtr angles;
    /** If the image is visible or not */
    UserSettingUniquePtr visible;
    /** The alpha of the image */
    UserSettingUniquePtr alpha;
    /** The color of the image */
    UserSettingUniquePtr color;
    // TODO: WRITE A COUPLE OF ENUMS FOR THIS
    /** The alignment of the image */
    std::string alignment;
    /** The size of the image in pixels */
    glm::vec2 size;
    /** Parallax depth used for parallax scrolling */
    UserSettingUniquePtr parallaxDepth;
    /** The color blending mode for this image */
    UserSettingUniquePtr colorBlendMode;
    /** The brightness of the image */
    UserSettingUniquePtr brightness;
    /**
     * A passthrough (compose) layer starts from the background behind it (true, WE's default) or from transparent
     * (image flag 0x40 at +0x304, default set by the constructor GH 0x140212c50; read in GH 0x1402321d0)
     */
    UserSettingUniquePtr copyBackground;
    /** The layer's composite FBOs clamp (true, WE's default) or wrap (image flag 0x8000; GH 0x140214b50) */
    UserSettingUniquePtr clampUVs;
    /** The layer's composite FBOs sample nearest (image flag 0x4000, default false; GH 0x140214b50) */
    UserSettingUniquePtr noInterpolation;
    /** The material in use for this image */
    ModelUniquePtr model;
    /** The effects applied to this image after the material is rendered */
    std::vector<ImageEffectUniquePtr> effects;
    /** The animation layers used in the puppet warp */
    std::vector<ImageAnimationLayerUniquePtr> animationLayers;
};

class Image : public Object, public ImageData {
public:
    explicit Image (ObjectData data, ImageData imageData) noexcept :
	Object (std::move (data)), ImageData (std::move (imageData)) { };
    ~Image () override = default;
};

struct SoundData {
    /** Playback mode as written: `loop` (also when absent or unknown), `random` or `single` */
    // TODO: WRITE AN ENUM FOR THIS
    std::optional<std::string> playbackmode;
    /** The files; one of them is picked at random each time the object starts */
    std::vector<std::string> sounds;
    /** Level: each stream plays at volume² (Wallpaper Engine's mapping; not clamped). Bindable to a user property */
    UserSettingUniquePtr volume;
    /** `random` mode: silence between two files, uniform in [mintime, maxtime] seconds */
    float mintime;
    float maxtime;
    /** Not started at load: only a script's play() starts it */
    bool startsilent;
};

class Sound : public Object, public SoundData {
public:
    explicit Sound (ObjectData data, SoundData soundData) noexcept :
	Object (std::move (data)), SoundData (std::move (soundData)) { };
    ~Sound () override = default;
};

/**
 * A model object (`"model": "models/x.mdl"`): static MDL meshes in the 3D world, each drawn with the material its
 * mesh names (WE's object factory tries `model` first, GH 0x1401be650). Its transform, visibility and parent are the
 * base object's (groupScale/groupAngles/groupVisible)
 */
struct MeshObjectData {
    /** the .mdl file */
    std::string file;
    /** `castshadow` (default false): the model is drawn into the shadow atlas of the lights that cast shadows
     *  (WE's object property, GH 0x140283df0's table; the shadow pass GH 0x1401c4f30 draws the casters) */
    UserSettingUniquePtr castShadow = nullptr;
};

class MeshObject : public Object, public MeshObjectData {
public:
    explicit MeshObject (ObjectData data, MeshObjectData meshData) noexcept :
	Object (std::move (data)), MeshObjectData (std::move (meshData)) { };
    ~MeshObject () override = default;
};

/**
 * A light object (`"light": "lpoint" | "lspot" | "ltube" | "ldirectional"`, WE's type table 0 … 3 at the light's
 * +0x2c0). WE's light property table (GH 0x140283df0): color +0x2cc, intensity +0x2e4, radius +0x2e8, exponent
 * +0x2ec, innercone +0x2f0, outercone +0x2f4 (degrees), controlpoint +0x2d8 (a vec3: a tube's far end, in the
 * light's own space), lightsourcesize +0x314, and the flags at +0x2c4: castshadow bit 0, usecookie bit 1 (GH
 * 0x1401c9430), castvolumetrics bit 2. A spot with usecookie projects its `cookie` texture (GH 0x140282140; default
 * cookie/flashlight1). Its transform is the base object's; a spot shines along its world x axis (GH 0x1401bf270)
 */
struct LightData {
    enum class Kind { Point, Spot, Tube, Directional, Other };
    Kind kind = Kind::Point;
    std::string type;
    UserSettingUniquePtr color;
    UserSettingUniquePtr intensity;
    float radius = 1000.0f;
    float exponent = 1.0f;
    float innerCone = 0.0f;
    float outerCone = 45.0f;
    UserSettingUniquePtr castShadow = nullptr;
    UserSettingUniquePtr useCookie = nullptr;
    std::string cookie = "cookie/flashlight1";
    glm::vec3 controlPoint = glm::vec3 (0.0f);
    float lightSourceSize = 0.0f;
};

class Light : public Object, public LightData {
public:
    explicit Light (ObjectData data, LightData lightData) noexcept :
	Object (std::move (data)), LightData (std::move (lightData)) { };
    ~Light () override = default;
};

/**
 * Particle control points for forces and positions
 */
struct ParticleControlPoint {
    int id;
    uint32_t flags;
    glm::vec3 offset;
    bool lockToPointer;
};

/**
 * Particle emitter configuration
 */
struct ParticleEmitter {
    int id;
    std::string name;
    glm::vec3 directions;
    glm::vec3 distanceMin;
    glm::vec3 distanceMax;
    glm::vec3 origin;
    glm::ivec3 sign;
    uint32_t instantaneous;
    float speedMin;
    float speedMax;
    float rate;
    int controlPoint;
    uint32_t flags;
    float cone;
    float delay;
    float duration;
    glm::vec2 audioProcessingBounds;
    float audioProcessingExponent;
    int audioProcessingFrequencyStart;
    int audioProcessingFrequencyEnd;
    int audioProcessingMode;
    float minPeriodicDelay;
    float maxPeriodicDelay;
    float minPeriodicDuration;
    float maxPeriodicDuration;
};

/**
 * Particle initializer base and implementations
 */
class ParticleInitializerBase : public TypeCaster {
public:
    virtual ~ParticleInitializerBase () = default;
};

class ColorRandomInitializer : public ParticleInitializerBase {
public:
    ColorRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    /** the random draw is pow(rand, exponent) (WE spawn interpreter GH 0x140260d80) */
    UserSettingUniquePtr exponent;
};

class SizeRandomInitializer : public ParticleInitializerBase {
public:
    SizeRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class AlphaRandomInitializer : public ParticleInitializerBase {
public:
    AlphaRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    /** the random draw is pow(rand, exponent) (WE spawn interpreter GH 0x140260d80) */
    UserSettingUniquePtr exponent;
};

class LifetimeRandomInitializer : public ParticleInitializerBase {
public:
    LifetimeRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    /** the random draw is pow(rand, exponent) (WE spawn interpreter GH 0x140260d80) */
    UserSettingUniquePtr exponent;
};

class VelocityRandomInitializer : public ParticleInitializerBase {
public:
    VelocityRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    /** the random draw is pow(rand, exponent) (WE spawn interpreter GH 0x140260d80) */
    UserSettingUniquePtr exponent;
};

class RotationRandomInitializer : public ParticleInitializerBase {
public:
    RotationRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    /** the random draw is pow(rand, exponent) (WE spawn interpreter GH 0x140260d80) */
    UserSettingUniquePtr exponent;
};

class AngularVelocityRandomInitializer : public ParticleInitializerBase {
public:
    AngularVelocityRandomInitializer (
	UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent
    ) : min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class TurbulentVelocityRandomInitializer : public ParticleInitializerBase {
public:
    TurbulentVelocityRandomInitializer (
	UserSettingUniquePtr speedMin, UserSettingUniquePtr speedMax, UserSettingUniquePtr scale,
	UserSettingUniquePtr offset, UserSettingUniquePtr forward, UserSettingUniquePtr timeScale,
	UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax, UserSettingUniquePtr right
    ) :
	speedMin (std::move (speedMin)), speedMax (std::move (speedMax)), scale (std::move (scale)),
	offset (std::move (offset)), forward (std::move (forward)), timeScale (std::move (timeScale)),
	phaseMin (std::move (phaseMin)), phaseMax (std::move (phaseMax)), right (std::move (right)) { }
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr offset;
    UserSettingUniquePtr forward;
    UserSettingUniquePtr timeScale;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr right;
};

class MapSequenceAroundControlPointInitializer : public ParticleInitializerBase {
public:
    MapSequenceAroundControlPointInitializer (
	UserSettingUniquePtr controlPoint, UserSettingUniquePtr count, UserSettingUniquePtr speedMin,
	UserSettingUniquePtr speedMax
    ) :
	controlPoint (std::move (controlPoint)), count (std::move (count)), speedMin (std::move (speedMin)),
	speedMax (std::move (speedMax)) { }
    UserSettingUniquePtr controlPoint;
    UserSettingUniquePtr count;
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
};

using ParticleInitializerUniquePtr = std::unique_ptr<ParticleInitializerBase>;

/**
 * Particle operator base and implementations
 */
class ParticleOperatorBase : public TypeCaster {
public:
    virtual ~ParticleOperatorBase () = default;
};

class MovementOperator : public ParticleOperatorBase {
public:
    MovementOperator (UserSettingUniquePtr drag, UserSettingUniquePtr gravity) :
	drag (std::move (drag)), gravity (std::move (gravity)) { }
    UserSettingUniquePtr drag;
    UserSettingUniquePtr gravity;
};

class AngularMovementOperator : public ParticleOperatorBase {
public:
    AngularMovementOperator (UserSettingUniquePtr drag, UserSettingUniquePtr force) :
	drag (std::move (drag)), force (std::move (force)) { }
    UserSettingUniquePtr drag;
    UserSettingUniquePtr force;
};

class AlphaFadeOperator : public ParticleOperatorBase {
public:
    AlphaFadeOperator (UserSettingUniquePtr fadeInTime, UserSettingUniquePtr fadeOutTime) :
	fadeInTime (std::move (fadeInTime)), fadeOutTime (std::move (fadeOutTime)) { }
    UserSettingUniquePtr fadeInTime;
    UserSettingUniquePtr fadeOutTime;
};

class SizeChangeOperator : public ParticleOperatorBase {
public:
    SizeChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class AlphaChangeOperator : public ParticleOperatorBase {
public:
    AlphaChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class ColorChangeOperator : public ParticleOperatorBase {
public:
    ColorChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class TurbulenceOperator : public ParticleOperatorBase {
public:
    TurbulenceOperator (
	UserSettingUniquePtr scale, UserSettingUniquePtr speedMin, UserSettingUniquePtr speedMax,
	UserSettingUniquePtr timeScale, UserSettingUniquePtr mask, UserSettingUniquePtr phaseMin,
	UserSettingUniquePtr phaseMax, UserSettingUniquePtr audioProcessingMode,
	UserSettingUniquePtr audioProcessingBounds, UserSettingUniquePtr audioProcessingExponent,
	UserSettingUniquePtr audioProcessingFrequencyStart, UserSettingUniquePtr audioProcessingFrequencyEnd
    ) :
	scale (std::move (scale)), speedMin (std::move (speedMin)), speedMax (std::move (speedMax)),
	timeScale (std::move (timeScale)), mask (std::move (mask)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)), audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)) { }
    UserSettingUniquePtr scale;
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    UserSettingUniquePtr timeScale;
    UserSettingUniquePtr mask;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
};

class VortexOperator : public ParticleOperatorBase {
public:
    VortexOperator (
	int controlPoint, int flags, UserSettingUniquePtr axis, UserSettingUniquePtr offset,
	UserSettingUniquePtr distanceInner, UserSettingUniquePtr distanceOuter, UserSettingUniquePtr speedInner,
	UserSettingUniquePtr speedOuter, UserSettingUniquePtr centerForce, UserSettingUniquePtr ringRadius,
	UserSettingUniquePtr ringWidth, UserSettingUniquePtr ringPullDistance, UserSettingUniquePtr ringPullForce,
	UserSettingUniquePtr audioProcessingMode, UserSettingUniquePtr audioProcessingBounds
    ) :
	controlPoint (controlPoint), flags (flags), axis (std::move (axis)), offset (std::move (offset)),
	distanceInner (std::move (distanceInner)), distanceOuter (std::move (distanceOuter)),
	speedInner (std::move (speedInner)), speedOuter (std::move (speedOuter)), centerForce (std::move (centerForce)),
	ringRadius (std::move (ringRadius)), ringWidth (std::move (ringWidth)),
	ringPullDistance (std::move (ringPullDistance)), ringPullForce (std::move (ringPullForce)),
	audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)) { }
    int controlPoint;
    /** `vortex_v2` (ring, centre force); plain `vortex` reads only bit 1 of the flags (GH 0x1401fb56f) */
    bool v2 = false;
    int flags; // 1 = infinite axis; v2 only: 2 = maintain distance to center, 4 = ring shape
    UserSettingUniquePtr axis;
    UserSettingUniquePtr offset;
    UserSettingUniquePtr distanceInner; // Standard vortex inner radius
    UserSettingUniquePtr distanceOuter; // Standard vortex outer radius
    UserSettingUniquePtr speedInner;
    UserSettingUniquePtr speedOuter;
    UserSettingUniquePtr centerForce; // Strength to pull particles toward center
    UserSettingUniquePtr ringRadius; // Ring mode: radius of the ring
    UserSettingUniquePtr ringWidth; // Ring mode: width of the ring
    UserSettingUniquePtr ringPullDistance; // Ring mode: distance at which ring attracts particles
    UserSettingUniquePtr ringPullForce; // Ring mode: strength of ring attraction
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
};

class ControlPointAttractOperator : public ParticleOperatorBase {
public:
    ControlPointAttractOperator (
	int controlPoint, int flags, UserSettingUniquePtr offset, UserSettingUniquePtr scale,
	UserSettingUniquePtr threshold, UserSettingUniquePtr deleteThreshold
    ) :
	controlPoint (controlPoint), flags (flags), offset (std::move (offset)), scale (std::move (scale)),
	threshold (std::move (threshold)), deleteThreshold (std::move (deleteThreshold)) { }
    int controlPoint;
    /** 1: particles within `deletethreshold` of the point die; 2: a step never passes the point (GH 0x140265800) */
    int flags;
    /** WE reads `offset` (GH 0x1401eb290); the `origin` many systems write is not read */
    UserSettingUniquePtr offset;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr threshold;
    UserSettingUniquePtr deleteThreshold;
};

class OscillateAlphaOperator : public ParticleOperatorBase {
public:
    OscillateAlphaOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
};

class OscillateSizeOperator : public ParticleOperatorBase {
public:
    OscillateSizeOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
};

class OscillatePositionOperator : public ParticleOperatorBase {
public:
    OscillatePositionOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax,
	UserSettingUniquePtr mask
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)), mask (std::move (mask)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr mask;
};

using ParticleOperatorUniquePtr = std::unique_ptr<ParticleOperatorBase>;

/**
 * Particle renderer configuration
 */
struct ParticleRenderer {
    std::string name;
    float length;
    float maxLength;
    float minLength;
    float subdivision;
    float segments; // ropetrail: number of history segments per particle
    float uvScale;
    bool uvScrolling;
    bool uvSmoothing; // rope only: reduces flickering when lifetimes are identical
    bool fadeAlpha; // ropetrail: fade alpha along trail
    bool fadeSize; // ropetrail: fade size along trail
    /** sprite: how the quad faces (WE's parser GH 0x1401efc50): 0 `screen` (the default), 1 `upright` (turns about
     *  an up axis to face the camera), 2 `fixed` (a fixed plane, `axis` its normal) */
    int orientation = 0;
    /** sprite: `axis` ("0 0 0" by default: then WE's y axis), normalised */
    glm::vec3 axis = glm::vec3 (0.0f, 1.0f, 0.0f);
    /** sprite: the in-plane up of a `fixed` quad, cross (axis, cross (y, axis)) (the parser builds it) */
    glm::vec3 axisUp = glm::vec3 (0.0f, 0.0f, -1.0f);
    /** sprite: `flags` bit 0: the axes are in world space (the camera's up / world y / `axis` as is), not the
     *  system's own */
    bool worldSpace = false;
};

/**
 * Child particle system
 */
class Particle;

struct ParticleChild {
    /** "static" (always on), "eventfollow" / "eventspawn" (one per spawned particle, following it or left where it
     *  spawned), "eventdeath" (one per particle that dies, where it died); WE's types 1–4 (parser 0x1401feff4) */
    std::string type;
    /** the child system's file (WE's `name`) */
    std::string name;
    /** at most this many instances of an event child at once (WE's default 10, GH 0x1401eec40) */
    int maxCount;
    int controlPointStartIndex;
    float probability;
    glm::vec3 angles;
    glm::vec3 origin;
    glm::vec3 scale;
    std::string particleFile;
    /** the child system, parsed like a particle object (null when its file doesn't load) */
    std::shared_ptr<Particle> particle;
};

/**
 * Instance override values
 */
struct ParticleInstanceOverride {
    UserSettingUniquePtr enabled;
    UserSettingUniquePtr alpha;
    UserSettingUniquePtr size;
    UserSettingUniquePtr lifetime;
    UserSettingUniquePtr rate;
    UserSettingUniquePtr speed;
    UserSettingUniquePtr count;
    UserSettingUniquePtr color; // Replaces particle color
    UserSettingUniquePtr colorn; // Tints the system: an HSV shift against its reference colour (CParticle)
    /** colorn is in the object's instanceoverride (WE tints nothing otherwise) */
    bool colornSet = false;
    /** `controlpoint0`..`controlpoint7`: replace the control points' offsets (WE's instance table +0xf0 + 12i, applied by
     *  GH 0x140251f70 unless the control point's flags have 0x10005); null where the object doesn't set one */
    std::array<UserSettingUniquePtr, 8> controlPoint {};
};

struct ParticleData {
    /** Position and transformation */
    UserSettingUniquePtr scale;
    UserSettingUniquePtr angles;
    UserSettingUniquePtr visible;

    /** Parallax depth */
    UserSettingUniquePtr parallaxDepth;

    /** Reference to particle definition file */
    std::string particleFile;

    /** Particle system configuration */
    std::string animationMode;
    float sequenceMultiplier;
    uint32_t maxCount;
    /** seconds simulated before the first frame (WE parses a float) */
    float startTime;
    uint32_t flags;

    /** Material for rendering */
    ModelUniquePtr material;

    /** Emitters, initializers, operators, renderers */
    std::vector<ParticleEmitter> emitters;
    std::vector<ParticleInitializerUniquePtr> initializers;
    std::vector<ParticleOperatorUniquePtr> operators;
    std::vector<ParticleRenderer> renderers;
    std::vector<ParticleControlPoint> controlPoints;
    std::vector<ParticleChild> children;

    /** Instance override */
    ParticleInstanceOverride instanceOverride;
};

class Particle : public Object, public ParticleData {
public:
    explicit Particle (ObjectData data, ParticleData particleData) noexcept :
	Object (std::move (data)), ParticleData (std::move (particleData)) { };
    ~Particle () override = default;
};

/**
 * Text object data. A text object is an image layer whose picture is its laid-out text (WE's text class extends its
 * image class: the constructor GH 0x14027c1b0 calls the image's, GH 0x140212c50), so it takes effects, a parent,
 * angles, parallax, alpha, colour and a blend mode like any image. These are the fields the text adds (WE's property
 * table GH 0x14027e130 with the defaults its constructor sets)
 */
struct TextData {
    /** The text (a user property, a script, or a literal; `\n` breaks a line) */
    UserSettingUniquePtr text;
    /** Font reference from scene (e.g. "fonts/VCR_OSD_MONO.ttf" or "systemfont_arial"; WE's default systemfont_arial) */
    std::string font;
    /** Font size in points (default 32), rasterised at 300 dpi (FT_Set_Char_Size (0, pt·64, 300), GH 0x1401d94c0) */
    UserSettingUniquePtr pointSize;
    /** "left", "center" (default) or "right": the lines within the block and the origin on the block */
    std::string horizontalAlign;
    /** "top", "center" (default) or "bottom": the origin on the block */
    std::string verticalAlign;
    /** "none" (default) … "topleft": pins the layer to an edge of the visible area (GH 0x14027da50) */
    std::string anchor;
    /** Padding around the text when it renders through a texture (effects): WE's default 32 32, clamped to 512 */
    glm::vec2 padding;
    /** Extra advance per glyph (x) and per line (y) */
    glm::vec2 spacing;
    /** `limitwidth`: lines wrap at `maxwidth` pixels */
    bool limitWidth = false;
    UserSettingUniquePtr maxWidth;
    /** `limitrows`: at most `maxrows` lines */
    bool limitRows = false;
    UserSettingUniquePtr maxRows;
    /** `limituseellipsis`: a line cut by `maxrows` ends in "…" */
    bool limitUseEllipsis = false;
    /** `blockalign`: a wrapped line is justified to `maxwidth` (its spaces widen) */
    bool blockAlign = false;
};

class Text : public Image, public TextData {
public:
    explicit Text (ObjectData data, ImageData imageData, TextData textData) noexcept :
	Image (std::move (data), std::move (imageData)), TextData (std::move (textData)) { };
    ~Text () override = default;
};
} // namespace WallpaperEngine::Data::Model
