#include "CImage.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include <algorithm>
#include <set>
#include <cmath>

#include "CRenderable.h"
#include "WallpaperEngine/Context.h"

#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Health.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Utils;

namespace {
/**
 * The passes of `effect` that carry a simulation from frame to frame (CImage::m_simulation): the passes that write one
 * of the effect's own FBOs, when some pass reads one of those FBOs before this frame wrote it (last frame's state — a
 * ping-pong across frames, not within one) and none of them reads the layer's picture (`previous`, or texture 0 left
 * unbound, which is the picture so far). cursorripple's apply_force and simulate_force are; its combine, which draws,
 * is not. motionblur's accumulation reads `previous`, so it runs every frame as before
 */
std::set<const WallpaperEngine::Data::Model::EffectPass*> simulationPassesOf (const WallpaperEngine::Data::Model::Effect& effect) {
    std::set<std::string> own;
    for (const auto& fbo : effect.fbos) {
	own.insert (fbo->name);
    }
    std::set<std::string> written;
    std::set<const WallpaperEngine::Data::Model::EffectPass*> writers;
    bool carries = false;
    bool readsPicture = false;

    for (const auto& pass : effect.passes) {
	std::vector<std::string> reads;
	for (const auto& [index, name] : pass->binds) {
	    reads.push_back (name);
	}
	if (!pass->material.has_value () && pass->source.has_value ()) {
	    reads.push_back (*pass->source);
	}
	for (const auto& name : reads) {
	    if (own.contains (name) && !written.contains (name)) {
		carries = true;
	    }
	}
	if (!pass->target.has_value () || !own.contains (*pass->target)) {
	    continue;
	}
	writers.insert (pass.get ());
	written.insert (*pass->target);
	const bool picture = std::ranges::any_of (reads, [] (const std::string& name) { return name == "previous"; })
	    || (pass->material.has_value () && !pass->binds.contains (0));
	readsPicture = readsPicture || picture;
    }

    if (!carries || readsPicture) {
	return {};
    }
    return writers;
}

bool isMagentaNeonTint (const glm::vec3& color) { return color.r > 0.55f && color.g < 0.25f && color.b > 0.45f; }

std::optional<glm::vec3> findMagentaCompositeTint (const Image& image, const std::vector<int>& skippedEffectIds) {
    for (const auto& effect : image.effects) {
	if (std::find (skippedEffectIds.begin (), skippedEffectIds.end (), static_cast<int> (effect->id))
	    != skippedEffectIds.end ()) {
	    continue;
	}
	if (!effect->visible->value->getBool ()) {
	    continue;
	}

	for (const auto& passOverride : effect->passOverrides) {
	    const auto compositeCombo = passOverride->combos.find ("COMPOSITE");
	    if (compositeCombo == passOverride->combos.end () || compositeCombo->second != 2) {
		continue;
	    }

	    const auto compositeColor = passOverride->constants.find ("compositecolor");
	    if (compositeColor == passOverride->constants.end () || compositeColor->second == nullptr
		|| compositeColor->second->value == nullptr) {
		continue;
	    }

	    const auto tint = compositeColor->second->value->getVec3 ();
	    if (isMagentaNeonTint (tint)) {
		return tint;
	    }
	}
    }

    return std::nullopt;
}

}

CImage::CImage (Wallpapers::CScene& scene, const Image& image) :
    CObject (scene, image), CRenderable (scene, image, *image.model->material), ScriptableObject (scene, image),
    m_sceneSpacePosition (GL_NONE), m_copySpacePosition (GL_NONE), m_passSpacePosition (GL_NONE),
    m_texcoordCopy (GL_NONE), m_texcoordPass (GL_NONE), m_modelViewProjectionScreen (),
    m_modelViewProjectionPass (glm::mat4 (1.0)), m_modelViewProjectionCopy (), m_modelViewProjectionScreenInverse (),
    m_modelViewProjectionPassInverse (glm::inverse (m_modelViewProjectionPass)), m_modelViewProjectionCopyInverse (),
    m_modelMatrix (), m_viewProjectionMatrix (), m_image (image), m_pos (), m_initialized (false) {
    // register any properties in use on this object
    this->registerProperty ("origin", *image.origin->value);
    this->registerProperty ("scale", *image.scale->value);
    this->registerProperty ("angles", *image.angles->value);
    this->registerProperty ("visible", *image.visible->value);
    this->registerProperty ("alpha", *image.alpha->value);
    this->registerProperty ("color", *image.color->value);
    this->registerProperty ("parallaxDepth", *image.parallaxDepth->value);
    // IEffectLayer.perspective (G21): read live by isPerspectiveLayer
    this->registerProperty ("perspective", *image.perspective->value);

    // get scene width and height to calculate positions
    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = transform.scale;
    const bool isShape = !this->m_image.shape.empty ();

    // a shape is a square of the scene's (orthographic) height, both sides (GH 0x140285b40 reads the height twice)
    if (isShape) {
	size = { scene_height, scene_height };
    }

    this->detectTexture ();

    // detect texture (if any)
    if (this->m_texture == nullptr) {
	if (this->m_image.model->solidlayer && size.x == 0.0f && size.y == 0.0f) {
	    size.x = scene_width;
	    size.y = scene_height;
	}
	// if (this->m_image->isSolid ()) // layer receives cursor events:
	// https://docs.wallpaperengine.io/en/scene/scenescript/reference/event/cursor.html same applies to effects
	// TODO: create a dummy texture of correct size, fbo constructors should be enough, but this should be properly
	// handled
	// (a shape's picture is never sampled: DIRECTDRAW. 1 x 1, not its square of the scene's height)
	const glm::vec2 dummy = isShape ? glm::vec2 (1.0f) : size;
	this->m_texture = std::make_shared<CFBO> (
	    "", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1, dummy.x, dummy.y, dummy.x, dummy.y
	);
    }

    // If the wallpaper doesn't specify a size, fall back to the texture or model dimensions
    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if (
	(size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	&& this->getImage ().model->height.has_value ()
    ) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    // fullscreen layers should use the whole projection's size
    // TODO: WHAT SHOULD AUTOSIZE DO?
    if (this->getImage ().model->fullscreen) {
	size = { scene_width, scene_height };
	origin = { scene_width / 2, scene_height / 2, 0 };

	// TODO: CHANGE ALIGNMENT TOO?
    }
    // the puppet first: with autosize its canvas grows to hold the posed mesh, which sizes the quad and the FBOs
    this->m_hasPuppetMesh = this->loadPuppetMesh (size);

    // scripts on an animation layer's rate / visible / blend run like a layer property's; thisObject is that
    // IAnimationLayer (3363252053 starts its sway layers at an offset from them)
    for (const auto& state : this->m_puppetLayers) {
	const std::pair<const char*, const UserSettingUniquePtr*> values[] = {
	    { "rate", &state.layer->rate }, { "visible", &state.layer->visible }, { "blend", &state.layer->blend }
	};
	for (const auto& [name, setting] : values) {
	    if (*setting != nullptr && (*setting)->value->getScriptSource ().has_value ()) {
		this->getScene ().getScriptEngine ().queueScript (
		    "animationlayer_" + std::to_string (state.layer->id) + "_" + name + "_" + std::to_string (this->getId ()),
		    *(*setting)->value, *this, nullptr, state.key
		);
	    }
	}
    }
    if (this->m_hasPuppetMesh) {
	size = this->m_puppetSize;
    }
    this->m_size = size;

    glm::vec2 scaledSize = size * glm::vec2 (scale);

    // calculate the center and shift from there
    this->m_pos.x = origin.x - (scaledSize.x / 2);
    this->m_pos.w = origin.y + (scaledSize.y / 2);
    this->m_pos.z = origin.x + (scaledSize.x / 2);
    this->m_pos.y = origin.y - (scaledSize.y / 2);

    if (this->getImage ().alignment.find ("top") != std::string::npos) {
	this->m_pos.y -= scaledSize.y / 2;
	this->m_pos.w -= scaledSize.y / 2;
    } else if (this->getImage ().alignment.find ("bottom") != std::string::npos) {
	this->m_pos.y += scaledSize.y / 2;
	this->m_pos.w += scaledSize.y / 2;
    }

    if (this->getImage ().alignment.find ("left") != std::string::npos) {
	this->m_pos.x += scaledSize.x / 2;
	this->m_pos.z += scaledSize.x / 2;
    } else if (this->getImage ().alignment.find ("right") != std::string::npos) {
	this->m_pos.x -= scaledSize.x / 2;
	this->m_pos.z -= scaledSize.x / 2;
    }

    // wallpaper engine
    this->m_pos.x -= scene_width / 2;
    this->m_pos.y = scene_height / 2 - this->m_pos.y;
    this->m_pos.z -= scene_width / 2;
    this->m_pos.w = scene_height / 2 - this->m_pos.w;

    // register both FBOs into the scene
    std::ostringstream nameA, nameB;

    // which of the two holds what: updateActivePasses (WE's parity)
    nameA << "_rt_imageLayerComposite_" << this->getImage ().id << "_a";
    nameB << "_rt_imageLayerComposite_" << this->getImage ().id << "_b";

    // the composite FBOs' sampling is the layer's, not its texture's (GH 0x140214b50): they clamp unless `clampuvs`
    // is false (WE's default is true, GH 0x140212c50), and sample nearest with `nointerpolation` or a point-filtered
    // texture. Wrapping let UV-shifting effects (waterripple, shake, scroll) pull the opposite edge in
    const uint32_t compositeFlags = (this->m_image.clampUVs->value->getBool () ? TextureFlags_ClampUVs : 0)
	| ((this->m_image.noInterpolation->value->getBool ()
	    || (this->m_texture->getFlags () & TextureFlags_NoInterpolation) != 0)
	       ? TextureFlags_NoInterpolation
	       : 0);

    // WE's composites are half float in an HDR scene (GH 0x140216080: format 0xe when the renderer's flag 0x2000)
    // a shape draws its passes straight into the scene: its composites are never drawn to (1 x 1, not the square)
    const glm::vec2 compositeSize = isShape ? glm::vec2 (1.0f) : size;
    const auto mainFBO = scene.create (
	nameA.str (), scene.getFBO ()->getFormat (), compositeFlags, 1, compositeSize, compositeSize
    );
    const auto subFBO = scene.create (
	nameB.str (), scene.getFBO ()->getFormat (), compositeFlags, 1, compositeSize, compositeSize
    );
    this->m_currentMainFBO = this->m_mainFBO = mainFBO;
    this->m_currentSubFBO = this->m_subFBO = subFBO;
    this->m_sizedTargets.push_back ({ .fbo = mainFBO });
    this->m_sizedTargets.push_back ({ .fbo = subFBO });

    // build a list of vertices, these might need some change later (or maybe invert the camera)
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animated images use different coordinates as they're essentially a texture atlas
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }
    // calculate the correct texCoord limits for the texture based on the texture screen size and real size
    else if (
	this->getTexture () != nullptr
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())
    ) {
	// Account for padding in non-power-of-two textures: clamp UVs to the real content
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    // TODO: RECALCULATE THESE POSITIONS FOR PASSTHROUGH SO THEY TAKE THE RIGHT PART OF THE TEXTURE
    float x = 0.0f;
    float y = 0.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animations should be copied completely
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
    }

    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0;
    GLfloat realY = 0.0;

    if (this->getImage ().model->passthrough) {
	// Passthrough shaders fill the destination FBO from texcoords and sample the scene using positions.
	// Keep the destination quad full-screen in local FBO space, but pass scene-space positions through.
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0;
	    realY = -1.0;
	    realWidth = 1.0;
	    realHeight = 1.0;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };

    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    GLfloat texcoordPass[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    GLfloat passSpacePosition[]
	= { -1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, 1.0, 0.0f, 1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, -1.0, 0.0f };

    // bind vertex list to the openGL buffers
    glGenBuffers (1, &this->m_sceneSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_copySpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_STATIC_DRAW);

    // bind pass' vertex list to the openGL buffers
    glGenBuffers (1, &this->m_passSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordCopy);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordPass);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordPass);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordPass), texcoordPass, GL_STATIC_DRAW);

    // compute the center of the image in scene space for rotation
    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);

    this->m_modelViewProjectionScreen = this->screenViewProjection ();

    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
    } else {
	this->m_modelViewProjectionCopy = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    }
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_viewProjectionMatrix = glm::mat4 (1.0);

    // ensure the input texture is marked as used
    // this makes video playback start if it's not already
    this->m_texture->incrementUsageCount ();
}

CImage::~CImage () {
    this->m_texture->decrementUsageCount ();

    // delete passes first as they depend on the image's data
    for (auto* pass : this->m_passes) {
	delete pass;
    }

    this->m_passes.clear ();

    // free any gl resources
    glDeleteBuffers (1, &this->m_sceneSpacePosition);
    glDeleteBuffers (1, &this->m_copySpacePosition);
    glDeleteBuffers (1, &this->m_passSpacePosition);
    glDeleteBuffers (1, &this->m_texcoordCopy);
    glDeleteBuffers (1, &this->m_texcoordPass);
    if (this->m_puppetScenePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetScenePosition);
    }
    if (this->m_shapeScenePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_shapeScenePosition);
    }
    if (this->m_shapeTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_shapeTexCoord);
    }
    if (this->m_puppetSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetColor != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetColor);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
}

bool CImage::loadPuppetMesh (const glm::vec2& size) {
    if (!this->getImage ().model->puppet.has_value ()) {
	return false;
    }
    const std::string& file = *this->getImage ().model->puppet;

    try {
	const auto stream = this->getScene ().getScene ().project.assetLocator->read (file);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

	// WE's loader (GH 0x1402873f0): every MDLV version up to 0023, the vertex layout from the mesh's format word
	std::string error;
	if (!Puppet::parse (data.data (), data.size (), this->m_puppet, error)) {
	    sLog.error ("Could not load puppet ", file, ": ", error);
	    WPENGINE_HEALTH ("missing_asset", { { "kind", "puppet" }, { "path", file }, { "message", error } });
	    return false;
	}
	if (!error.empty ()) {
	    sLog.error ("Puppet ", file, " partly loaded: ", error);
	    WPENGINE_HEALTH ("asset_error", { { "kind", "puppet" }, { "path", file }, { "message", "partly loaded: " + error } });
	}

	std::vector<GLfloat> texcoords;
	std::vector<GLuint> indices;
	this->m_puppetVertices.clear ();
	this->m_puppetVertexMesh.clear ();
	this->m_puppetShapes.clear ();
	this->m_puppetShapeWeights.clear ();
	for (size_t m = 0; m < this->m_puppet.meshes.size (); m++) {
	    const auto& mesh = this->m_puppet.meshes[m];
	    const auto base = static_cast<GLuint> (this->m_puppetVertices.size ());
	    for (size_t t = 0; t < mesh.morphs.size (); t++) {
		this->m_puppetShapes.emplace_back (m, t);
	    }
	    this->m_puppetShapeWeights.emplace_back (mesh.morphs.size (), 0.0f);
	    for (const auto& vertex : mesh.vertices) {
		this->m_puppetVertices.push_back (vertex);
		this->m_puppetVertexMesh.push_back (static_cast<uint16_t> (m));
		texcoords.push_back (vertex.uv.x);
		texcoords.push_back (vertex.uv.y);
	    }
	    for (const uint32_t index : mesh.indices) {
		indices.push_back (base + index);
	    }
	}
	if (this->m_puppetVertices.empty () || indices.empty ()) {
	    sLog.error ("Puppet ", file, " has no triangles");
	    WPENGINE_HEALTH ("asset_error", { { "kind", "puppet" }, { "path", file }, { "message", "no triangles" } });
	    return false;
	}

	this->m_puppetBind = Puppet::bindPose (this->m_puppet);
	this->m_puppetTargetBind = Puppet::targetBindPose (this->m_puppet);
	Puppet::bindWorldMatrices (this->m_puppet, this->m_puppetBindWorldInverse);
	for (auto& m : this->m_puppetBindWorldInverse) {
	    m = glm::inverse (m);
	}
	this->m_puppetLayers.clear ();
	for (const auto& layer : this->getImage ().animationLayers) {
	    this->m_puppetLayers.push_back ({ .blendIn = layer->blendIn, .layer = layer.get (),
					      .key = this->m_nextAnimationLayerKey++, .name = layer->name });
	}
	this->m_puppetSkinned = !this->m_puppet.bones.empty () && !this->m_puppet.animations.empty ();
	this->m_puppetMorphed = !this->m_puppetShapes.empty ();
	// a vertex alpha (genericimage4's SKINNING_ALPHA and MORPHING both multiply v_VertexAlpha in): bone alpha tracks
	// or target alphas below 1
	this->m_puppetVertexAlpha = false;
	for (const auto& animation : this->m_puppet.animations) {
	    this->m_puppetVertexAlpha = this->m_puppetVertexAlpha || !animation.boneAlpha.empty ();
	}
	for (const auto& mesh : this->m_puppet.meshes) {
	    for (const auto& target : mesh.morphs) {
		for (const float a : target.alpha) {
		    this->m_puppetVertexAlpha = this->m_puppetVertexAlpha || a < 0.999f;
		}
	    }
	}
	if (this->m_puppetVertexAlpha) {
	    this->m_puppetColors.assign (this->m_puppetVertices.size () * 4, 1.0f);
	    glGenBuffers (1, &this->m_puppetColor);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetColor);
	    glBufferData (GL_ARRAY_BUFFER, this->m_puppetColors.size () * sizeof (GLfloat), this->m_puppetColors.data (),
			  GL_DYNAMIC_DRAW);
	}

	this->m_puppetRawPositions.clear ();
	this->m_puppetRawPositions.reserve (this->m_puppetVertices.size () * 3);
	for (const auto& vertex : this->m_puppetVertices) {
	    this->m_puppetRawPositions.push_back (vertex.position.x);
	    this->m_puppetRawPositions.push_back (vertex.position.y);
	    this->m_puppetRawPositions.push_back (vertex.position.z);
	}

	this->m_puppetSize = this->getImage ().model->autosize && this->m_puppetSkinned ? this->puppetCanvasSize (size)
											 : size;
	this->updatePuppetPositionBuffer (this->m_puppetSize);

	glGenBuffers (1, &this->m_puppetTexCoord);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
	glBufferData (GL_ARRAY_BUFFER, texcoords.size () * sizeof (GLfloat), texcoords.data (), GL_STATIC_DRAW);

	glGenBuffers (1, &this->m_puppetIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (GLuint), indices.data (), GL_STATIC_DRAW);

	this->m_puppetIndexCount = static_cast<GLsizei> (indices.size ());
	sLog.out (
	    "Loaded puppet ", file, " version=", this->m_puppet.version, " vertices=", this->m_puppetVertices.size (),
	    " indices=", this->m_puppetIndexCount, " bones=", this->m_puppet.bones.size (),
	    " animations=", this->m_puppet.animations.size (), " attachments=", this->m_puppet.attachments.size ()
	);
	return true;
    } catch (const std::exception& ex) {
	sLog.error ("Could not load puppet mesh ", file, ": ", ex.what ());
	WPENGINE_HEALTH ("missing_asset", { { "kind", "puppet" }, { "path", file }, { "message", WallpaperEngine::Health::brief (ex.what ()) } });
	return false;
    }
}

glm::vec2 CImage::puppetCanvasSize (const glm::vec2& size) const {
    // (PR #625's autosize: measure the mesh in every frame of every animation, grow the canvas symmetrically)
    float maxX = size.x / 2.0f, maxY = size.y / 2.0f;
    std::vector<glm::mat4> world, skinning (this->m_puppet.bones.size ());
    for (const auto& animation : this->m_puppet.animations) {
	for (uint32_t frame = 0; frame <= animation.frames; frame++) {
	    std::vector<Puppet::Key> pose = this->m_puppetBind;
	    Puppet::mix (pose, this->m_puppetBind, animation, static_cast<float> (frame) / animation.fps, 1.0f, false);
	    Puppet::worldMatrices (this->m_puppet, pose, world);
	    for (size_t b = 0; b < world.size (); b++) {
		skinning[b] = world[b] * this->m_puppetBindWorldInverse[b];
	    }
	    for (const auto& vertex : this->m_puppetVertices) {
		const glm::vec3 p = Puppet::skin (vertex, skinning);
		maxX = std::max (maxX, std::abs (p.x));
		maxY = std::max (maxY, std::abs (p.y));
	    }
	}
    }
    return { std::ceil (maxX) * 2.0f, std::ceil (maxY) * 2.0f };
}

void CImage::updatePuppetPose () {
    if (!this->m_puppetSkinned && !this->m_puppetMorphed) {
	return;
    }
    const float now = static_cast<float> (this->getScene ().getTime ());
    if (now == this->m_puppetPoseTime && !this->m_puppetPoseDirty) {
	return;
    }
    const float dt = this->m_puppetPoseTime < 0.0f ? 0.0f : std::max (now - this->m_puppetPoseTime, 0.0f);
    this->m_puppetPoseTime = now;
    this->m_puppetPoseDirty = false;

    // GH 0x140228470: from the bind pose, every visible layer in order; its clock runs at dt · rate
    std::vector<Puppet::Key> pose = this->m_puppetBind;
    // the IK targets move with the animations as the bones do (their MDLA v2 tracks)
    std::vector<Puppet::Key> targets = this->m_puppetTargetBind;
    std::vector<float> boneAlpha (this->m_puppet.bones.size (), 1.0f);
    std::vector<std::vector<float>> shapeWeights = this->m_puppetShapeWeights;
    for (auto& weights : shapeWeights) {
	std::fill (weights.begin (), weights.end (), 0.0f);
    }
    for (auto& state : this->m_puppetLayers) {
	const auto& layer = *state.layer;
	const auto* match = this->puppetAnimationOf (state);
	if (match == nullptr) {
	    continue;
	}
	if (!layer.visible->value->getBool ()) {
	    continue;
	}
	if (state.playing) {
	    const float before = state.started ? state.clock.time : -1.0f;
	    const uint32_t endsBefore = state.clock.ends;
	    const bool reversedBefore = state.clock.reversed;
	    Puppet::advance (*match, state.clock, dt * layer.rate->value->getFloat (),
			     state.single ? Puppet::Mode::Single : match->mode);
	    state.started = true;
	    // the animation's own events (MDLA) the clock passed: animationEvent (event, value) to the layer's scripts.
	    // Inferred: WE's runtime for them wasn't traced (the keyframe timelines' events go the same way)
	    if (!match->events.empty ()) {
		const auto passed = Puppet::passedEvents (*match, before, state.clock, endsBefore, reversedBefore);
		if (!passed.empty ()) {
		    this->getScene ().getScriptEngine ().layerAnimationEvents (*this, passed);
		}
	    }
	}
	const float weight = Puppet::layerWeight (*match, state.clock, layer.blend->value->getFloat (), state.blendIn,
						  layer.blendOut, layer.blendTime);
	Puppet::mix (pose, this->m_puppetBind, *match, state.clock.time, weight, layer.additive);
	Puppet::mixTargets (targets, this->m_puppetTargetBind, *match, state.clock.time, weight, layer.additive);
	Puppet::mixExtras (boneAlpha, shapeWeights, *match, state.clock.time, weight, layer.additive);
    }
    for (const auto& [index, weight] : this->m_shapeOverrides) {
	if (index >= 0 && static_cast<size_t> (index) < this->m_puppetShapes.size ()) {
	    const auto [mesh, target] = this->m_puppetShapes[index];
	    shapeWeights[mesh][target] = weight;
	}
    }
    this->m_puppetShapeWeights = shapeWeights;
    // a single animation that played is removed (playSingleAnimation)
    std::erase_if (this->m_puppetLayers, [] (const PuppetLayerState& state) { return state.single && state.clock.finished; });

    // IK chains reach for their targets (3042492564's arm follows its hand's target)
    Puppet::solveIk (this->m_puppet, pose, targets);

    this->m_puppetPose = pose;
    // a script's bone poses (setLocalBoneTransform …) over the animations'
    for (const auto& [bone, local] : this->m_boneOverrides) {
	if (bone < 0 || static_cast<size_t> (bone) >= pose.size ()) {
	    continue;
	}
	glm::vec3 skew;
	glm::vec4 perspective;
	glm::decompose (local, pose[bone].scale, pose[bone].rotation, pose[bone].position, skew, perspective);
    }

    Puppet::worldMatrices (this->m_puppet, pose, this->m_puppetWorld);
    std::vector<glm::mat4> skinning (this->m_puppetWorld.size ());
    for (size_t b = 0; b < skinning.size (); b++) {
	skinning[b] = this->m_puppetWorld[b] * this->m_puppetBindWorldInverse[b];
    }
    for (size_t v = 0; v < this->m_puppetVertices.size (); v++) {
	const auto& vertex = this->m_puppetVertices[v];
	const auto& mesh = this->m_puppet.meshes[this->m_puppetVertexMesh[v]];
	// MORPHING before SKINNING (genericimage4): the blend shapes move the bind-space position
	float alpha = 1.0f;
	const glm::vec3 morphed = this->m_puppetMorphed ? Puppet::morph (mesh, vertex, shapeWeights[this->m_puppetVertexMesh[v]], alpha)
						     : vertex.position;
	const glm::vec3 p = this->m_puppetSkinned ? Puppet::skin (morphed, vertex, skinning) : morphed;
	if (this->m_puppetVertexAlpha) {
	    // SKINNING_ALPHA: saturate (Σ weight · alpha[bone])
	    float bones = 0.0f, total = 0.0f;
	    for (int c = 0; c < 4; c++) {
		if (vertex.weights[c] != 0.0f && vertex.bones[c] < boneAlpha.size ()) {
		    bones += vertex.weights[c] * boneAlpha[vertex.bones[c]];
		    total += vertex.weights[c];
		}
	    }
	    this->m_puppetColors[v * 4 + 3] = alpha * (total == 0.0f ? 1.0f : std::clamp (bones, 0.0f, 1.0f));
	}
	this->m_puppetRawPositions[v * 3] = p.x;
	this->m_puppetRawPositions[v * 3 + 1] = p.y;
	// (inferred, as PR #625) flat: x/y bone turns would push vertices out of the FBO's depth range
	this->m_puppetRawPositions[v * 3 + 2] = 0.0f;
    }
    this->updatePuppetPositionBuffer (this->getSize ());
    if (this->m_puppetVertexAlpha && this->m_puppetColor != GL_NONE) {
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetColor);
	glBufferSubData (GL_ARRAY_BUFFER, 0, this->m_puppetColors.size () * sizeof (GLfloat), this->m_puppetColors.data ());
    }
}

std::vector<std::string> CImage::blendShapeNames () const {
    std::vector<std::string> names;
    for (const auto& [mesh, target] : this->m_puppetShapes) {
	names.push_back (this->m_puppet.meshes[mesh].morphs[target].name);
    }
    return names;
}

float CImage::blendShapeWeight (int index) {
    if (index < 0 || static_cast<size_t> (index) >= this->m_puppetShapes.size ()) {
	return 0.0f;
    }
    this->updatePuppetPose ();
    const auto [mesh, target] = this->m_puppetShapes[index];
    return this->m_puppetShapeWeights[mesh][target];
}

void CImage::setBlendShapeWeight (int index, const std::optional<float>& weight) {
    if (index < 0 || static_cast<size_t> (index) >= this->m_puppetShapes.size ()) {
	return;
    }
    if (weight.has_value ()) {
	this->m_shapeOverrides[index] = *weight;
    } else {
	this->m_shapeOverrides.erase (index);
    }
    this->m_puppetPoseDirty = true;
}

bool CImage::attachmentMatrix (const std::string& name, glm::mat4& matrix) {
    for (const auto& attachment : this->m_puppet.attachments) {
	if (attachment.name != name) {
	    continue;
	}
	this->updatePuppetPose ();
	glm::mat4 bone (1.0f);
	if (this->m_puppetSkinned && static_cast<size_t> (attachment.bone) < this->m_puppetWorld.size ()) {
	    bone = this->m_puppetWorld[attachment.bone];
	} else {
	    std::vector<glm::mat4> bind;
	    Puppet::bindWorldMatrices (this->m_puppet, bind);
	    bone = bind[attachment.bone];
	}
	matrix = bone * attachment.matrix;
	return true;
    }
    return false;
}

CImage::PuppetLayerState* CImage::findPuppetLayer (int key) {
    const auto it = std::ranges::find_if (this->m_puppetLayers, [key] (const PuppetLayerState& s) { return s.key == key; });
    return it == this->m_puppetLayers.end () ? nullptr : &*it;
}

const Puppet::Animation* CImage::puppetAnimationOf (const PuppetLayerState& state) const {
    const auto id = static_cast<uint32_t> (state.layer->animation->value->getInt ());
    const auto match = std::ranges::find_if (this->m_puppet.animations, [id] (const Puppet::Animation& a) { return a.id == id; });
    return match == this->m_puppet.animations.end () ? nullptr : &*match;
}

int CImage::createPuppetLayer (const std::string& name, int id, bool single) {
    const auto match = std::ranges::find_if (this->m_puppet.animations, [&] (const Puppet::Animation& a) {
	return name.empty () ? static_cast<int> (a.id) == id : a.name == name;
    });
    if (!this->m_puppetSkinned || match == this->m_puppet.animations.end ()) {
	return -1;
    }
    // its settings as a scene's layer would have them: rate 1, visible, blend 1
    auto layer = std::make_unique<ImageAnimationLayer> (ImageAnimationLayer {
	.id = -1,
	.name = match->name,
	.rate = std::make_unique<UserSetting> (UserSetting { .value = std::make_unique<DynamicValue> (1.0f) }),
	.visible = std::make_unique<UserSetting> (UserSetting { .value = std::make_unique<DynamicValue> (true) }),
	.blend = std::make_unique<UserSetting> (UserSetting { .value = std::make_unique<DynamicValue> (1.0f) }),
	.animation = std::make_unique<UserSetting> (
	    UserSetting { .value = std::make_unique<DynamicValue> (static_cast<int> (match->id)) }
	),
    });
    const int key = this->m_nextAnimationLayerKey++;
    this->m_puppetLayers.push_back ({ .layer = layer.get (), .key = key, .name = match->name, .single = single });
    this->m_createdAnimationLayers.push_back (std::move (layer));
    return key;
}

bool CImage::destroyPuppetLayer (int key) {
    return std::erase_if (this->m_puppetLayers, [key] (const PuppetLayerState& s) { return s.key == key; }) > 0;
}

void CImage::puppetBonePose (std::vector<glm::mat4>& local, std::vector<glm::mat4>& world) {
    this->updatePuppetPose ();
    const auto& bones = this->m_puppet.bones;
    local.assign (bones.size (), glm::mat4 (1.0f));
    for (size_t b = 0; b < bones.size (); b++) {
	if (const auto it = this->m_boneOverrides.find (static_cast<int> (b)); it != this->m_boneOverrides.end ()) {
	    local[b] = it->second;
	} else if (b < this->m_puppetPose.size ()) {
	    const auto& key = this->m_puppetPose[b];
	    local[b] = glm::translate (glm::mat4 (1.0f), key.position) * glm::mat4_cast (key.rotation)
		* glm::scale (glm::mat4 (1.0f), key.scale);
	} else {
	    local[b] = bones[b].local;
	}
    }
    world.assign (bones.size (), glm::mat4 (1.0f));
    for (size_t b = 0; b < bones.size (); b++) {
	const int parent = bones[b].parent;
	world[b] = parent >= 0 && static_cast<size_t> (parent) < b ? world[parent] * local[b] : local[b];
    }
}

void CImage::setBoneOverride (int bone, const std::optional<glm::mat4>& local) {
    if (local.has_value ()) {
	this->m_boneOverrides[bone] = *local;
    } else {
	this->m_boneOverrides.erase (bone);
    }
    this->m_puppetPoseDirty = true;
}

std::vector<std::string> CImage::attachmentNames () const {
    std::vector<std::string> names;
    for (const auto& attachment : this->m_puppet.attachments) {
	names.push_back (attachment.name);
    }
    return names;
}

void CImage::setAlignment (const std::string& alignment) {
    // the scene's data is the layer's live state (as its DynamicValues are)
    const_cast<Image&> (this->m_image).alignment = alignment;
}

void CImage::updatePuppetPositionBuffer (const glm::vec2& size) {
    if (this->m_puppetRawPositions.empty ()) {
	return;
    }

    std::vector<GLfloat> positions;
    positions.reserve (this->m_puppetRawPositions.size ());
    for (size_t index = 0; index + 2 < this->m_puppetRawPositions.size (); index += 3) {
	positions.push_back (size.x / 2.0f + this->m_puppetRawPositions[index]);
	positions.push_back (size.y / 2.0f - this->m_puppetRawPositions[index + 1]);
	positions.push_back (this->m_puppetRawPositions[index + 2]);
    }

    if (this->m_puppetSpacePosition == GL_NONE) {
	glGenBuffers (1, &this->m_puppetSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, positions.size () * sizeof (GLfloat), positions.data (), GL_DYNAMIC_DRAW);
}

void CImage::updatePuppetScenePositionBuffer () {
    // the layer pixel (u, v) of the copy quad to the scene quad (uploadGeometryBuffers pairs copy (0, h) with scene
    // (m_pos.x, m_pos.y) and copy (w, 0) with (m_pos.z, m_pos.w))
    const glm::vec2 size = this->getSize ();
    if (this->m_puppetRawPositions.empty () || size.x <= 0.0f || size.y <= 0.0f) {
	return;
    }
    std::vector<GLfloat> positions;
    positions.reserve (this->m_puppetRawPositions.size ());
    for (size_t i = 0; i + 2 < this->m_puppetRawPositions.size (); i += 3) {
	const float u = size.x / 2.0f + this->m_puppetRawPositions[i];
	const float v = size.y / 2.0f - this->m_puppetRawPositions[i + 1];
	positions.push_back (this->m_pos.x + u / size.x * (this->m_pos.z - this->m_pos.x));
	positions.push_back (this->m_pos.w + v / size.y * (this->m_pos.y - this->m_pos.w));
	positions.push_back (0.0f);
    }
    if (this->m_puppetScenePosition == GL_NONE) {
	glGenBuffers (1, &this->m_puppetScenePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetScenePosition);
    glBufferData (GL_ARRAY_BUFFER, positions.size () * sizeof (GLfloat), positions.data (), GL_DYNAMIC_DRAW);
}

void CImage::updateShapeBuffers () {
    const glm::vec2 size = this->getSize ();
    if (size.x <= 0.0f || size.y <= 0.0f) {
	return;
    }
    // the corners: the first effect's first pass's point0..point3, all four or none (GH 0x1402861b0 falls back to the
    // plain quad, GH 0x140219570)
    glm::vec2 points[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
    if (!this->m_image.effects.empty () && !this->m_image.effects.front ()->passOverrides.empty ()) {
	const auto& constants = this->m_image.effects.front ()->passOverrides.front ()->constants;
	glm::vec2 found[4];
	bool all = true;
	for (int i = 0; i < 4 && all; i++) {
	    const auto it = constants.find ("point" + std::to_string (i));
	    if (it == constants.end () || it->second == nullptr || it->second->value == nullptr) {
		all = false;
	    } else {
		found[i] = it->second->value->getVec2 ();
	    }
	}
	if (all) {
	    std::copy (std::begin (found), std::end (found), std::begin (points));
	}
    }
    // WE's vertices: (S · (p.x − 0.5), S · (0.5 − p.y)) about the origin, y up, uv = p; here the layer pixel (p · S)
    // (y down) on the layer's scene rectangle, as the puppet's mesh (updatePuppetScenePositionBuffer)
    constexpr int order[6] = { 0, 2, 1, 0, 3, 2 };
    GLfloat positions[18];
    GLfloat texcoords[12];
    for (int i = 0; i < 6; i++) {
	const glm::vec2& p = points[order[i]];
	positions[i * 3 + 0] = this->m_pos.x + p.x * (this->m_pos.z - this->m_pos.x);
	positions[i * 3 + 1] = this->m_pos.w + p.y * (this->m_pos.y - this->m_pos.w);
	positions[i * 3 + 2] = 0.0f;
	texcoords[i * 2 + 0] = p.x;
	texcoords[i * 2 + 1] = p.y;
    }
    if (this->m_shapeScenePosition == GL_NONE) {
	glGenBuffers (1, &this->m_shapeScenePosition);
	glGenBuffers (1, &this->m_shapeTexCoord);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_shapeScenePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (positions), positions, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_shapeTexCoord);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoords), texcoords, GL_DYNAMIC_DRAW);
}

void CImage::setupPuppetGeometryCallback (Effects::CPass* pass, bool toScene) const {
    pass->setGeometryCallback (
	[this, pass, toScene] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, toScene ? this->m_puppetScenePosition : this->m_puppetSpacePosition);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
		glVertexAttribPointer (texCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    // the vertex alphas (bone alpha, morph alpha) through genericimage4's VERTEXCOLOR
	    if (const GLint color = glGetAttribLocation (pass->getProgramID (), "a_Color");
		color >= 0 && this->m_puppetColor != GL_NONE) {
		glEnableVertexAttribArray (color);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetColor);
		glVertexAttribPointer (color, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	},
	[this, pass] () {
	    GLint currentFramebuffer = 0;
	    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &currentFramebuffer);
	    if (currentFramebuffer
		!= static_cast<GLint> (this->getScene ().getTargetFor (this->m_image)->getFramebuffer ())) {
		GLfloat previousClearColor[4] = {};
		glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
		glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		glClearColor (
		    previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]
		);
	    }
	    // the mesh's triangles are WE's (front = counter-clockwise, y up); its positions here are y-down layer / scene
	    // pixels, which a flat layer's projection draws upside down into its target, so they reach GL clockwise and a
	    // `cullmode: normal` material (WE keeps it for puppets, GH 0x140230ec0) culled the whole puppet (3490034653,
	    // 3363252053). Front is whichever winding the mesh's own front lands as: clockwise when the matrix keeps the
	    // y-down orientation (det > 0). A perspective layer's camera turns it back (unchanged: counter-clockwise).
	    const glm::mat4* mvp = pass->getModelViewProjectionMatrix ();
	    const bool clockwise = !this->isPerspectiveLayer () && mvp != nullptr
		&& (*mvp)[0][0] * (*mvp)[1][1] - (*mvp)[0][1] * (*mvp)[1][0] > 0.0f;
	    glFrontFace (clockwise ? GL_CW : GL_CCW);
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	    glDrawElements (GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_INT, nullptr);
	    glFrontFace (GL_CCW);
	},
	[pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glDisableVertexAttribArray (position);
	    }

	    if (texCoord >= 0) {
		glDisableVertexAttribArray (texCoord);
	    }
	    if (const GLint color = glGetAttribLocation (pass->getProgramID (), "a_Color"); color >= 0) {
		glDisableVertexAttribArray (color);
	    }
	}
    );
}

void CImage::setup () {
    // do not double-init stuff, that's bad!
    if (this->m_initialized) {
	return;
    }

    // TODO: CHECK ORDER OF THINGS, 2419444134'S ID 27 DEPENDS ON 104'S COMPOSITE_A WHEN OUR LAST RENDER IS ON
    // COMPOSITE_B
    // TODO: SUPPORT PASSTHROUGH (IT'S A SHADER)
    // a passthrough layer that draws objects of its own, or starts transparent, renders through a group target
    // (GH 0x1402321d0: the background copy or a clear, then its subtree, GH 0x140218260), effects or none
    const bool composes = this->m_image.model->passthrough
	&& (this->getScene ().composesChildren (this->m_image) || !this->m_image.copyBackground->value->getBool ());

    if (this->m_image.model->passthrough && !composes) {
	// passthrough images without effects are bad, do not draw them
	if (this->m_image.effects.empty ()) {
	    return;
	}

	// Some have attempted to declare effects with visible set to false (for good: one a property or a script can
	// turn on is built, and render () draws the layer only while one is on)
	bool allEffectsInvisible = true;
	for (const auto& cur : this->m_image.effects) {
	    if (effectCanBeVisible (*cur)) {
		allEffectsInvisible = false;
		break;
	    }
	}

	if (allEffectsInvisible) {
	    return;
	}
    }

    const auto& debug = this->getScene ().getContext ().getContext ().config;

    // a puppet's vertex alpha (bone alpha tracks, blend-shape alphas) reaches genericimage4's fragment through its
    // VERTEXCOLOR (a_Color = 1 1 1 alpha; color *= v_Color right after WE's color.a *= v_VertexAlpha). Ours: WE skins
    // and morphs on the GPU (SKINNING_ALPHA, MORPHING), LWE's puppets on the CPU. genericimage2 has neither, in WE too
    if (this->m_hasPuppetMesh && this->m_puppetVertexAlpha && !this->getImage ().model->material->passes.empty ()) {
	auto& first = const_cast<MaterialPass&> (*this->getImage ().model->material->passes.front ());
	if (first.shader == "genericimage4") {
	    first.combos.insert_or_assign ("VERTEXCOLOR", 1);
	}
    }

    const bool isShape = !this->m_image.shape.empty ();

    // copy pass to the composite layer (a shape has none: only its effects draw)
    for (const auto& cur : this->getImage ().model->material->passes) {
	if (isShape) {
	    break;
	}
	this->m_passes.push_back (
	    new CPass (*this, std::make_shared<FBOProvider> (this), *cur, std::nullopt, std::nullopt, std::nullopt)
	);
    }

    if (composes && !this->m_passes.empty ()) {
	this->m_composeTarget = this->getScene ().getComposeTarget (this->getId ());
	this->m_passes.front ()->setTexture (0, this->m_composeTarget);
    }

    // prepare the passes list
    if (!debug.baseOnly && !this->getImage ().effects.empty ()) {
	// generate the effects used by this material
	this->m_passEffects.resize (this->m_passes.size (), nullptr);
	int effectIndex = -1;
	for (const auto& cur : this->m_image.effects) {
	    effectIndex++;
	    // a shape draws its last effect only (GH 0x140285b70 takes the effect list's back ())
	    if (isShape && cur != this->m_image.effects.back ()) {
		continue;
	    }
	    // WPENGINE_HEALTH: what fails while this effect builds (its shaders, textures) names it
	    const WallpaperEngine::Health::Scope healthEffect ("effect", cur->id);
	    const WallpaperEngine::Health::Scope healthEffectName ("effect_name", cur->effect != nullptr && !cur->effect->name.empty () ? cur->effect->name : cur->name);
	    if (std::find (debug.skipEffects.begin (), debug.skipEffects.end (), static_cast<int> (cur->id))
		!= debug.skipEffects.end ()) {
		continue;
	    }

	    // an effect that is hidden for good is not built; one a user property or a script can show is built
	    // hidden, and render () takes its passes in and out of the chain as its `visible` changes (WE: live)
	    if (!effectCanBeVisible (*cur)) {
		continue;
	    }

	    // a script on the effect's `visible` runs like a layer property's (thisLayer = this image)
	    if (cur->visible->value->getScriptSource ().has_value ()) {
		this->getScene ().getScriptEngine ().queueScript (
		    "effect_" + std::to_string (cur->id) + "_" + std::to_string (effectIndex) + "_visible_"
			+ std::to_string (this->getId ()),
		    *cur->visible->value, *this, cur.get ()
		);
	    }

	    // scripts on the effect's shader constants (a fade, an audio pulse) run like a layer property's; their
	    // thisObject is this image, and getAnimation () reaches the constant's own keyframe animation
	    int passIndex = 0;
	    for (const auto& pass : cur->passOverrides) {
		for (const auto& [name, constant] : pass->constants) {
		    if (constant != nullptr && constant->value != nullptr && constant->value->getScriptSource ().has_value ()) {
			this->getScene ().getScriptEngine ().queueScript (
			    "effect_" + std::to_string (cur->id) + "_" + std::to_string (effectIndex) + "_pass_"
				+ std::to_string (passIndex) + "_" + name + "_" + std::to_string (this->getId ()),
			    *constant->value, *this
			);
		    }
		}
		passIndex++;
	    }

	    const auto fboProvider = std::make_shared<FBOProvider> (this);

	    // create all the fbos for this effect
	    for (const auto& fbo : cur->effect->fbos) {
		this->m_sizedTargets.push_back ({
		    .fbo = fboProvider->create (*fbo, this->m_texture->getFlags (), this->getSize (), this->getScene ().rendersHDR ()),
		    .scale = fbo->scale,
		    .fit = fbo->fit,
		    .width = fbo->width,
		    .height = fbo->height,
		});
	    }

	    // a simulation (cursorripple): the effect's passes that write its own FBOs, when one of them reads an FBO of
	    // the effect before this frame writes it (last frame's state) and none reads the layer's picture, run on
	    // WE's frame rate (m_simulation)
	    const auto simulation = simulationPassesOf (*cur->effect);
	    std::vector<const CPass*> simulationPasses;

	    // TODO: MAKE USE OF ZIP OPERATOR IN BOOST? WAY OVERKILL JUST FOR THIS...

	    auto curEffect = cur->effect->passes.begin ();
	    auto endEffect = cur->effect->passes.end ();
	    auto curOverride = cur->passOverrides.begin ();
	    auto endOverride = cur->passOverrides.end ();
	    // an effect whose pass can't be built (a packaged shader that doesn't compile, a missing material) is
	    // left out on its own: the image still renders, without it — not dropped with the whole object
	    // (except a passthrough/fullscreen layer, below)
	    const auto passesBefore = this->m_passes.size ();

	    try {
	    for (; curEffect != endEffect; ++curEffect) {
		if (!(*curEffect)->material.has_value ()) {
		    if (!(*curEffect)->command.has_value ()) {
			sLog.error ("Pass without material and command not supported");
			WPENGINE_HEALTH ("unsupported", { { "what", "effect pass without material and command" } });
			continue;
		    }

		    if (!(*curEffect)->source.has_value ()) {
			sLog.error ("Pass without material and source not supported");
			WPENGINE_HEALTH ("unsupported", { { "what", "effect pass without material and source" } });
			continue;
		    }

		    if (!(*curEffect)->target.has_value ()) {
			sLog.error ("Pass without material and target not supported");
			WPENGINE_HEALTH ("unsupported", { { "what", "effect pass without material and target" } });
			continue;
		    }


		    auto virtualPass
			= std::make_unique<MaterialPass> (MaterialPass { .blending = BlendingMode_Normal,
									 .cullmode = CullingMode_Disable,
									 .depthtest = DepthtestMode_Disabled,
									 .depthwrite = DepthwriteMode_Disabled,
									 .shader = "commands/copy",
									 .textures = { { 0, *(*curEffect)->source } },
									 .combos = {},
									 .constants = {} });

		    const auto& config = *this->m_virtualPassess.emplace_back (std::move (virtualPass));

		    // build a pass for a copy shader (a swap is that pass exchanging its source and target instead)
		    this->m_passes.push_back (new CPass (
			*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ()
		    ));
		    if ((*curEffect)->command == Command_Swap) {
			this->m_passes.back ()->setSwap (*(*curEffect)->source, (*curEffect)->target.value ());
		    }
		    if (simulation.contains (curEffect->get ())) {
			simulationPasses.push_back (this->m_passes.back ());
		    }
		} else {
		    for (auto& pass : (*curEffect)->material.value ()->passes) {
			const auto override = curOverride != endOverride
			    ? **curOverride
			    : std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
			const auto target = (*curEffect)->target.has_value ()
			    ? *(*curEffect)->target
			    : std::optional<std::reference_wrapper<std::string>> (std::nullopt);

			this->m_passes.push_back (
			    new CPass (*this, fboProvider, *pass, override, (*curEffect)->binds, target)
			);
			if (simulation.contains (curEffect->get ())) {
			    simulationPasses.push_back (this->m_passes.back ());
			}
		    }

		    if (curOverride != endOverride) {
			++curOverride;
		    }
		}
	    }
	    } catch (const std::exception& e) {
		// a passthrough/fullscreen layer exists only to post-process what's below it: part of its effect
		// chain is an arbitrary picture (measured: a black screen over 3432999904), so it still goes whole
		if (this->m_image.model->passthrough || this->m_image.model->fullscreen) {
		    WPENGINE_HEALTH ("effect_dropped", { { "reason", WallpaperEngine::Health::brief (e.what ()) }, { "takes_layer", true } });
		    throw;
		}

		sLog.error ("Leaving out effect ", cur->id, " (", cur->name, ") of object ", this->m_image.id, ": ", e.what ());
		WPENGINE_HEALTH ("effect_dropped", { { "reason", WallpaperEngine::Health::brief (e.what ()) }, { "takes_layer", false } });

		while (this->m_passes.size () > passesBefore) {
		    delete this->m_passes.back ();
		    this->m_passes.pop_back ();
		}
		simulationPasses.clear ();
	    }

	    // the simulation's passes read their clock's frame time and pointer (WE's, sampled when it steps)
	    for (const auto* pass : simulationPasses) {
		auto* writable = const_cast<CPass*> (pass);
		writable->addUniform ("g_Frametime", &this->m_simulation.frametime);
		writable->addUniform ("g_PointerPosition", &this->m_simulation.pointer);
		writable->addUniform ("g_PointerPositionLast", &this->m_simulation.pointerLast);
		writable->addUniform ("g_PointerState", &this->m_simulation.pointerState);
		this->m_simulationPasses.push_back (pass);
	    }

	    // the passes just added are this effect's: they run only while it is visible
	    this->m_passEffects.resize (this->m_passes.size (), cur.get ());
	}
    }

    if (!debug.baseOnly && !isShape) {
	const auto magentaCompositeTint = findMagentaCompositeTint (this->m_image, debug.skipEffects);
	if (magentaCompositeTint.has_value ()) {
	    auto tintOverride = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
		.id = -1,
		.combos = {
		    { "BLENDMODE", 30 },
		},
		.constants = {},
		.textures = {},
	    });
	    tintOverride->constants.emplace ("color", UserSettingBuilder::fromValue (magentaCompositeTint.value ()));
	    tintOverride->constants.emplace ("alpha", UserSettingBuilder::fromValue (1.0f));

	    this->m_materials.compatibilityMaterials.emplace_back (
		MaterialParser::load (this->getScene ().getScene ().project, "materials/effects/tint.json")
	    );
	    this->m_materials.compatibilityOverrides.emplace_back (std::move (tintOverride));

	    this->m_passes.push_back (new CPass (
		*this, std::make_shared<FBOProvider> (this),
		**this->m_materials.compatibilityMaterials.back ()->passes.begin (),
		*this->m_materials.compatibilityOverrides.back (), std::nullopt, std::nullopt
	    ));
	}
    }

    // extra render pass if there's any blending to be done. Mode 31 (A + B·opacity) is plain additive blending in WE:
    // no BLENDMODE pass, and the layer's last draw blends additively (GH 0x140214b50, 0x1402321d0; the BLENDMODE combo
    // is 0 for modes 0 and 31, GH 0x140211f90)
    const int colorBlendMode = this->m_image.colorBlendMode->value->getInt ();
    if (!debug.baseOnly && !isShape && colorBlendMode > 0 && colorBlendMode != 31) {
	this->m_materials.colorBlending.material
	    = MaterialParser::load (this->getScene ().getScene ().project, "materials/util/effectpassthrough.json");
	this->m_materials.colorBlending.override = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
            .id = -1,
            .combos = {
                {"BLENDMODE", this->m_image.colorBlendMode->value->getInt()},
            },
            .constants = {},
            .textures = {},
        });

	this->m_passes.push_back (new CPass (
	    *this, std::make_shared<FBOProvider> (this), **this->m_materials.colorBlending.material->passes.begin (),
	    *this->m_materials.colorBlending.override, std::nullopt, std::nullopt
	));
    }

    // passes not added by an effect (the base material, the tint and colour-blend passes) always run
    this->m_passEffects.resize (this->m_passes.size (), nullptr);
    for (const auto* pass : this->m_passes) {
	this->m_passBlending.push_back (pass->getBlendingMode ());
    }

    CRenderable::setup ();

    // a shape's quad buffers exist before its passes take them
    if (isShape) {
	this->updateShapeBuffers ();
    }

    this->updateActivePasses ();
    this->m_initialized = true;
}

bool CImage::effectCanBeVisible (const ImageEffect& effect) const {
    // visible now, or bound to a user property, or driven by a script (its own, or a layer script's
    // getEffect (…).visible: built whenever the scene's scripts call getEffect)
    return effect.visible->value->getBool () || effect.visible->property != nullptr
	|| effect.visible->value->getScriptSource ().has_value () || this->getScene ().scriptsReachEffects ();
}

bool CImage::effectVisibilityChanged () const {
    size_t index = 0;

    for (size_t i = 0; i < this->m_passes.size (); i++) {
	const auto* effect = this->m_passEffects[i];
	// one entry per effect: its passes are contiguous
	if (effect == nullptr || (i > 0 && this->m_passEffects[i - 1] == effect)) {
	    continue;
	}
	if (index >= this->m_effectVisibility.size ()
	    || this->m_effectVisibility[index] != effect->visible->value->getBool ()) {
	    return true;
	}
	index++;
    }

    return index != this->m_effectVisibility.size ();
}

void CImage::updateActivePasses () {
    this->m_activePasses.clear ();
    this->m_effectVisibility.clear ();
    this->m_visibleEffects = 0;

    for (size_t i = 0; i < this->m_passes.size (); i++) {
	const auto* effect = this->m_passEffects[i];
	const bool visible = effect == nullptr || effect->visible->value->getBool ();

	if (effect != nullptr && (i == 0 || this->m_passEffects[i - 1] != effect)) {
	    this->m_effectVisibility.push_back (visible);
	    this->m_visibleEffects += visible ? 1 : 0;
	}

	// every pass back to the blending it was built with
	this->m_passes[i]->setBlendingMode (this->m_passBlending[i]);

	if (visible) {
	    this->m_activePasses.push_back (this->m_passes[i]);
	}
    }

    // swaps at the end exchange FBOs after the layer's last draw: they run after it, outside the chain
    this->m_trailingSwaps.clear ();
    while (!this->m_activePasses.empty () && this->m_activePasses.back ()->isSwap ()) {
	this->m_trailingSwaps.insert (this->m_trailingSwaps.begin (), this->m_activePasses.back ());
	this->m_activePasses.pop_back ();
    }

    // if there's more than one pass the blendmode has to be moved from the beginning to the end (of the passes that
    // run: a hidden last effect hands the layer's blending to the pass before it)
    // (a shape's passes each draw into the scene with their own blending, GH 0x1402176a0 per pass)
    if (this->m_activePasses.size () > 1 && this->m_image.shape.empty ()) {
	const auto first = this->m_activePasses.front ();
	const auto last = this->m_activePasses.back ();

	last->setBlendingMode (first->getBlendingMode ());
	first->setBlendingMode (BlendingMode_Normal);
    }

    // a shape's last pass draws additively (GH 0x1402176a0 calls the shape's vtable +0x108, GH 0x1402866d0, which sets
    // the pass's blending to 2 = additive, GH 0x140182410's names): its rays add to the scene
    if (!this->m_image.shape.empty () && !this->m_activePasses.empty ()) {
	this->m_activePasses.back ()->setBlendingMode (BlendingMode_Additive);
    }

    // the layer's last draw: additive for colour blend mode 31; translucent for a passthrough layer that doesn't copy
    // the background (it starts transparent, GH 0x1402321d0)
    if (!this->m_activePasses.empty ()) {
	if (this->m_image.colorBlendMode->value->getInt () == 31) {
	    this->m_activePasses.back ()->setBlendingMode (BlendingMode_Additive);
	} else if (this->m_image.model->passthrough && !this->m_image.copyBackground->value->getBool ()) {
	    this->m_activePasses.back ()->setBlendingMode (BlendingMode_Translucent);
	}
    }

    // the chain starts again from the image's main FBO: which FBO each pass reads and writes, and which pass draws
    // to the scene, follow the passes that run
    this->m_currentMainFBO = this->m_mainFBO;
    this->m_currentSubFBO = this->m_subFBO;

    // WE starts the ping-pong so that the last composite written before the final draw is _a (GH 0x140214b50: the
    // start index is the parity of the passes that write the composite), so `_rt_imageLayerComposite_<id>_a` is what
    // the last pass reads and _b the stage before it. Another layer binding one of them gets that stage
    size_t compositeWrites = 0;
    for (size_t i = 0; i + 1 < this->m_activePasses.size (); i++) {
	compositeWrites += this->m_activePasses[i]->getTarget ().has_value () ? 0 : 1;
    }
    if (compositeWrites % 2 == 0) {
	std::swap (this->m_currentMainFBO, this->m_currentSubFBO);
    }

    this->setupPasses ();
}

void CImage::setupPasses () {
    // do a pass on everything and setup proper inputs and values
    std::shared_ptr<const CFBO> drawTo = this->m_currentMainFBO;
    std::shared_ptr<const TextureProvider> asInput = this->getTexture ();
    GLuint texcoord = this->getTexCoordCopy ();

    auto cur = this->m_activePasses.begin ();
    auto end = this->m_activePasses.end ();
    bool first = true;
    bool inTargetEffectSequence = false;
    std::shared_ptr<const TextureProvider> effectInput = nullptr;

    for (; cur != end; ++cur) {
	Effects::CPass* pass = *cur;
	std::shared_ptr<const CFBO> prevDrawTo = drawTo;
	bool writesToTarget = false;
	const bool isFirstPass = first;
	GLuint spacePosition = (isFirstPass)
	    ? (this->m_hasPuppetMesh ? this->m_puppetSpacePosition : this->getCopySpacePosition ())
	    : this->getPassSpacePosition ();
	const glm::mat4* projection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopy : &this->m_modelViewProjectionPass;
	const glm::mat4* inverseProjection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopyInverse : &this->m_modelViewProjectionPassInverse;
	first = false;


	pass->setModelMatrix (&this->m_modelMatrix);
	pass->setLayerModelMatrix (&this->m_layerModelMatrix);
	pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);

	writesToTarget = this->configurePassTarget (pass, drawTo, asInput, effectInput, inTargetEffectSequence);
	// determine if it's the last element in the list as this is a screen-copy-like process
	// TODO: PROPERLY CHECK IF THIS IS ALL THAT'S NEEDED
	if (!writesToTarget && this->shouldRenderFinalPass (std::next (cur) == end)) {
	    spacePosition = this->getSceneSpacePosition ();
	    drawTo = this->getScene ().getTargetFor (this->m_image);
	    projection = &this->m_modelViewProjectionScreen;
	    inverseProjection = &this->m_modelViewProjectionScreenInverse;
	}

	// a shape: every pass draws its quad straight into the scene (GH 0x140285b70), reading no layer picture
	if (!this->m_image.shape.empty ()) {
	    writesToTarget = false;
	    spacePosition = this->m_shapeScenePosition;
	    drawTo = this->getScene ().getTargetFor (this->m_image);
	    projection = &this->m_modelViewProjectionScreen;
	    inverseProjection = &this->m_modelViewProjectionScreenInverse;
	    pass->setDestination (drawTo);
	    pass->setInput (this->getTexture ());
	    pass->setPreviousInput (nullptr);
	    pass->setPosition (spacePosition);
	    pass->setTexCoord (this->m_shapeTexCoord);
	    pass->setModelViewProjectionMatrix (projection);
	    pass->setModelViewProjectionMatrixInverse (inverseProjection);
	    drawTo = prevDrawTo;
	    continue;
	}

	// the puppet mesh replaces the first pass's quad; a puppet without effects draws it straight to the scene, in
	// the layer's scene-space rectangle (before, its layer pixels were drawn with the scene's projection)
	if (isFirstPass && this->m_hasPuppetMesh) {
	    pass->setBlendingMode (BlendingMode_Translucent);
	    this->setupPuppetGeometryCallback (pass, spacePosition == this->getSceneSpacePosition ());
	}

	pass->setDestination (drawTo);
	pass->setInput (asInput);
	pass->setPreviousInput (inTargetEffectSequence ? effectInput : nullptr);
	pass->setPosition (spacePosition);
	pass->setTexCoord (texcoord);
	pass->setModelViewProjectionMatrix (projection);
	pass->setModelViewProjectionMatrixInverse (inverseProjection);

	texcoord = this->getTexCoordPass ();

	if (writesToTarget) {
	    asInput = drawTo;
	    drawTo = prevDrawTo;
	} else {
	    drawTo = prevDrawTo;
	    this->pinpongFramebuffer (&drawTo, &asInput);
	    inTargetEffectSequence = false;
	    effectInput = nullptr;
	}
    }
}

bool CImage::shouldRenderFinalPass (bool isLastPass) const {
    // not the visibility: this is decided once, while the passes are built, and render () skips a hidden image every
    // frame. Asking it here sent the last pass of an image hidden at load (by itself or a parent) into an FBO for
    // good, so a property or script showing it later never brought it to the screen
    if (!isLastPass) {
	return false;
    }

    const auto& debug = this->getScene ().getContext ().getContext ().config;
    return !(debug.noSolidFinal && this->getImage ().model->solidlayer);
}

bool CImage::configurePassTarget (
    Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo, const std::shared_ptr<const TextureProvider>& asInput,
    std::shared_ptr<const TextureProvider>& effectInput, bool& inTargetEffectSequence
) {
    if (!pass->getTarget ().has_value ()) {
	return false;
    }

    const std::string target = pass->getTarget ().value ();
    std::shared_ptr<const CFBO> resolved = pass->getFBOProvider ()->find (target);
    if (resolved == nullptr) {
	resolved = this->getScene ().findFBO (target);
    }
    if (resolved == nullptr) {
	sLog.error (
	    "Pass target FBO '", target, "' could not be resolved for object ", pass->getRenderable ().getId (),
	    " shader=", pass->getPass ().shader
	);
	WPENGINE_HEALTH (
	    "pass_error", { { "object", pass->getRenderable ().getId () }, { "shader", pass->getPass ().shader },
			    { "message", "target FBO '" + target + "' not resolved" } }
	);
	return false;
    }

    if (!inTargetEffectSequence) {
	effectInput = asInput;
	inTargetEffectSequence = true;
    }
    drawTo = resolved;
    return true;
}

void CImage::pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput) {
    // temporarily store FBOs used
    std::shared_ptr<const CFBO> currentMainFBO = this->m_currentMainFBO;
    std::shared_ptr<const CFBO> currentSubFBO = this->m_currentSubFBO;

    if (drawTo != nullptr) {
	*drawTo = currentSubFBO;
    }
    if (asInput != nullptr) {
	*asInput = currentMainFBO;
    }

    // swap the FBOs
    this->m_currentMainFBO = currentSubFBO;
    this->m_currentSubFBO = currentMainFBO;
}

void CImage::resizeTargets (const glm::vec2& size) {
    this->m_size = size;

    for (const auto& target : this->m_sizedTargets) {
	const glm::vec2 sized = FBOProvider::sizeFor (target.scale, target.fit, target.width, target.height, size);
	const auto width = static_cast<uint32_t> (sized.x);
	const auto height = static_cast<uint32_t> (sized.y);
	target.fbo->resize (width, height, width, height);
    }
}

void CImage::setLocalOffset (const glm::vec2& offset) { this->m_localOffset = offset; }

bool CImage::drawsSubtree () const { return this->m_initialized && this->m_composeTarget != nullptr; }

void CImage::renderComposeTarget (bool copyBackground) {
    const auto& group = *this->m_composeTarget;
    const auto background = this->getScene ().getTargetFor (this->m_image);

    glColorMask (true, true, true, true);

    // the background behind the layer, or transparent (`copybackground: false`)
    if (copyBackground || this->m_image.copyBackground->value->getBool ()) {
	glBindFramebuffer (GL_READ_FRAMEBUFFER, background->getFramebuffer ());
	glBindFramebuffer (GL_DRAW_FRAMEBUFFER, group.getFramebuffer ());
	glBlitFramebuffer (
	    0, 0, static_cast<GLint> (background->getRealWidth ()), static_cast<GLint> (background->getRealHeight ()), 0,
	    0, static_cast<GLint> (group.getRealWidth ()), static_cast<GLint> (group.getRealHeight ()),
	    GL_COLOR_BUFFER_BIT, GL_NEAREST
	);
    } else {
	GLfloat previousClearColor[4] = {};
	glGetFloatv (GL_COLOR_CLEAR_VALUE, previousClearColor);
	glBindFramebuffer (GL_FRAMEBUFFER, group.getFramebuffer ());
	glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glClearColor (previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    }

    // then the objects it owns, in draw order
    this->getScene ().renderComposed (this->getId (), group);
}

void CImage::render () {
    // do not try to render something that did not initialize successfully
    if (!this->m_initialized) {
	return;
    }

    // a hidden ancestor hides it too (WE's "effectively visible", GH 0x1401b50e0)
    if (!this->getScene ().isEffectivelyVisible (this->getImage ())) {
	return;
    }

    // an effect's `visible` changed (a user property, a script): rebuild the chain of the passes that run
    if (this->effectVisibilityChanged ()) {
	this->updateActivePasses ();
    }

    // a passthrough layer is only its effects: with none visible it draws nothing (as it isn't built when none can be),
    // unless it composes: its subtree and background are its picture
    if (this->m_image.model->passthrough && this->m_visibleEffects == 0 && this->m_composeTarget == nullptr) {
	return;
    }

    if (this->m_composeTarget != nullptr) {
	// with no effect running, the layer's picture is its target: the background with its subtree drawn over it.
	// (composelayer's base pass covers its whole destination: it only ever draws into the layer's own FBO)
	const bool bare = this->m_visibleEffects == 0;

	this->renderComposeTarget (bare);

	if (bare) {
	    const auto& group = *this->m_composeTarget;
	    const auto target = this->getScene ().getTargetFor (this->m_image);

	    glBindFramebuffer (GL_READ_FRAMEBUFFER, group.getFramebuffer ());
	    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, target->getFramebuffer ());
	    glBlitFramebuffer (
		0, 0, static_cast<GLint> (group.getRealWidth ()), static_cast<GLint> (group.getRealHeight ()), 0, 0,
		static_cast<GLint> (target->getRealWidth ()), static_cast<GLint> (target->getRealHeight ()),
		GL_COLOR_BUFFER_BIT, GL_NEAREST
	    );
	    glBindFramebuffer (GL_FRAMEBUFFER, target->getFramebuffer ());
	    return;
	}
    }

    glColorMask (true, true, true, true);

    // g_Color4 is bound to m_color4: bring it up to this frame's colour and alpha
    static_cast<void> (this->getColor4 ());

    // the puppet's pose for this frame (G06/G08)
    if (this->m_hasPuppetMesh) {
	this->updatePuppetPose ();
    }

    // Always update screen transform (handles rotation + parallax dynamically)
    this->updateScreenSpacePosition ();
    if (this->m_hasPuppetMesh) {
	this->updatePuppetScenePositionBuffer ();
    }
    if (!this->m_image.shape.empty ()) {
	this->updateShapeBuffers ();
    }

#if !NDEBUG
    std::string str = "Image ";

    if (this->getScene ().getScene ().camera.bloom.enabled->value->getBool () && this->getId () == -1) {
	str += "bloom";
    } else {
	str += this->getImage ().name + " (" + std::to_string (this->getId ()) + ", "
	    + this->getImage ().model->material->filename + ")";
    }

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    // a simulation steps at WE's frame rate: between its steps its passes keep last step's state
    this->advanceSimulationClock ();

    auto cur = this->m_activePasses.begin ();

    // the last draw keeps the scene's alpha (a compose layer that starts transparent takes this layer's as coverage)
    const bool keepAlpha = this->getScene ().targetKeepsAlpha (this->m_image);

    for (const auto end = this->m_activePasses.end (); cur != end; ++cur) {
	if (std::next (cur) == end) {
	    glColorMask (true, true, true, !keepAlpha);
	}

	// a simulation's passes in a row run once per step of its clock (none on a frame between steps)
	if (this->isSimulationPass (*cur)) {
	    auto run = cur;
	    while (std::next (run) != end && this->isSimulationPass (*std::next (run))) {
		++run;
	    }
	    for (int step = 0; step < this->m_simulation.steps; step++) {
		this->enterSimulationStep (step);
		for (auto pass = cur; pass != std::next (run); ++pass) {
		    (*pass)->render ();
		}
	    }
	    cur = run;
	    continue;
	}

	(*cur)->render ();
    }

    for (auto* swap : this->m_trailingSwaps) {
	if (!this->isSimulationPass (swap) || this->m_simulation.steps > 0) {
	    swap->render ();
	}
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

namespace {
/** The most steps a simulation runs in one frame (a frame 1/8 s late at 240 Hz); a longer stall drops the rest */
constexpr int kMaxSimulationSteps = 32;
} // namespace

void CImage::advanceSimulationClock () {
    if (this->m_simulationPasses.empty ()) {
	return;
    }

    auto& clock = this->m_simulation;
    const auto& scene = this->getScene ();
    const auto& context = scene.getContext ().getContext ();
    const float now = context.renderTime;
    const float rate = scene.getFrameRate ();
    const glm::vec2 pointer = *scene.getMousePosition ();
    const glm::vec4 state = *scene.getPointerState ();
    const bool first = clock.seenTime < 0.0f;

    // the same time again (paused, or drawn twice): nothing steps
    if (!first && now == clock.seenTime) {
	clock.steps = 0;
	return;
    }

    const float dt = first ? context.frameTime : std::max (0.0f, now - clock.seenTime);
    clock.seenTime = now;
    clock.pressed = clock.pressed || state.z > 0.5f;

    if (first) {
	// WE's first frame: its last pointer is 0 0 (GH 0x1401acb91)
	clock.from = *scene.getMousePositionLast ();
	clock.jumps = scene.getPointerJumps ();
    }
    // a jump is no move: the next step starts where the pointer landed
    if (scene.getPointerJumps () != clock.jumps) {
	clock.jumps = scene.getPointerJumps ();
	clock.from = pointer;
    }
    clock.to = pointer;

    if (rate <= 0.0f) {
	// no rate: a step per frame drawn, as every other pass
	clock.steps = 1;
	clock.frametime = dt;
	return;
    }

    const float period = 1.0f / rate;
    clock.phase += dt;
    // a little slack: 8 frames of 1/240 s add up to 1/30 s give or take a float's rounding
    clock.steps = std::min (kMaxSimulationSteps, static_cast<int> (std::floor ((clock.phase + 1e-4f) / period)));
    clock.phase = std::max (0.0f, clock.phase - static_cast<float> (clock.steps) * period);
    if (clock.phase >= period) {
	clock.phase = 0.0f;
    }
    // the first frame always steps (WE draws it)
    if (first && clock.steps == 0) {
	clock.steps = 1;
	clock.phase = 0.0f;
    }
    clock.frametime = period;
}

void CImage::enterSimulationStep (int step) {
    auto& clock = this->m_simulation;
    const auto& scene = this->getScene ();
    const glm::vec4 state = *scene.getPointerState ();
    const float n = static_cast<float> (std::max (1, clock.steps));

    // WE samples the pointer once per frame and its g_PointerPositionLast is the pointer of its frame before (the scene
    // tick copies ctx+0x8c to ctx+0x94 after drawing, GH 0x1401b1c0b): steps between this frame's and the last step's
    // pointer move along the line between them
    clock.pointerLast = glm::mix (clock.from, clock.to, static_cast<float> (step) / n);
    clock.pointer = glm::mix (clock.from, clock.to, static_cast<float> (step + 1) / n);
    clock.pointerState = glm::vec4 (state.x, state.y, clock.pressed && step == 0 ? 1.0f : 0.0f, state.w);

    if (step + 1 == clock.steps) {
	clock.from = clock.to;
	clock.pressed = false;
    }
}

bool CImage::isSimulationPass (const CPass* pass) const {
    return std::find (this->m_simulationPasses.begin (), this->m_simulationPasses.end (), pass) != this->m_simulationPasses.end ();
}

const float& CImage::getBrightness () const { return this->m_image.brightness->value->getFloat (); }

const float& CImage::getUserAlpha () const { return this->m_image.alpha->value->getFloat (); }

const float& CImage::getAlpha () const { return this->m_image.alpha->value->getFloat (); }

const glm::vec3& CImage::getColor () const {
    // WE's draw context colour (g_Color, g_Color4.rgb) is the layer's colour × its brightness when the renderer runs
    // HDR (flag 0x2000), else × 1 (GH 0x1402321d0 / 0x1402328e0 / 0x140231dc0: ctx+0x124…0x12c = image+0x330…0x338 ×
    // (ctx flags & 0x2000 ? image+0x340 : 1)). The VERSION shaders have no g_Brightness term, so this is where an HDR
    // scene's `brightness` shows (2542468435's 1.15). Bound by address: refreshed by getColor4 () every render ()
    const float brightness = this->getScene ().rendersHDR () ? this->getBrightness () : 1.0f;
    this->m_drawColor = this->m_image.color->value->getVec3 () * brightness;
    return this->m_drawColor;
}

const glm::vec4& CImage::getColor4 () const {
    // WE's g_Color4 is (g_Color, g_Alpha): the colour and the layer's alpha, not the colour value's own w
    // (ColorBuilder's 1, or 0 once a vec3 was written)
    this->m_color4 = glm::vec4 (this->getColor (), this->getAlpha ());
    return this->m_color4;
}

const glm::vec3& CImage::getCompositeColor () const { return this->m_image.color->value->getVec3 (); }

glm::vec2 CImage::resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const {
    glm::vec2 size = this->getSize ();

    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if (
	(size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	&& this->getImage ().model->height.has_value ()
    ) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    if (this->getImage ().model->fullscreen) {
	size = { sceneWidth, sceneHeight };
	origin = { sceneWidth / 2.0f, sceneHeight / 2.0f, 0.0f };
    }

    return size;
}

void CImage::updateScenePosition (
    const glm::vec3& origin, const glm::vec2& size, const glm::vec3& scale, float sceneWidth, float sceneHeight
) {
    const glm::vec2 scaledSize = size * glm::vec2 (scale);
    // the quad's centre: the origin, or the given offset from it in the layer's own (scaled) pixels
    const glm::vec2 centre = glm::vec2 (origin) + (this->m_localOffset.has_value () ? *this->m_localOffset * glm::vec2 (scale) : glm::vec2 (0.0f));
    this->m_pos.x = centre.x - (scaledSize.x / 2.0f);
    this->m_pos.w = centre.y + (scaledSize.y / 2.0f);
    this->m_pos.z = centre.x + (scaledSize.x / 2.0f);
    this->m_pos.y = centre.y - (scaledSize.y / 2.0f);

    if (this->m_localOffset.has_value ()) {
	// placed by the offset alone
    } else if (this->getImage ().alignment.find ("top") != std::string::npos) {
	this->m_pos.y -= scaledSize.y / 2.0f;
	this->m_pos.w -= scaledSize.y / 2.0f;
    } else if (this->getImage ().alignment.find ("bottom") != std::string::npos) {
	this->m_pos.y += scaledSize.y / 2.0f;
	this->m_pos.w += scaledSize.y / 2.0f;
    }

    if (this->m_localOffset.has_value ()) {
	// placed by the offset alone
    } else if (this->getImage ().alignment.find ("left") != std::string::npos) {
	this->m_pos.x += scaledSize.x / 2.0f;
	this->m_pos.z += scaledSize.x / 2.0f;
    } else if (this->getImage ().alignment.find ("right") != std::string::npos) {
	this->m_pos.x -= scaledSize.x / 2.0f;
	this->m_pos.z -= scaledSize.x / 2.0f;
    }

    this->m_pos.x -= sceneWidth / 2.0f;
    this->m_pos.y = sceneHeight / 2.0f - this->m_pos.y;
    this->m_pos.z -= sceneWidth / 2.0f;
    this->m_pos.w = sceneHeight / 2.0f - this->m_pos.w;
    // the pivot of the rotation is the origin, not the quad's centre: WE's model matrix is the object's transform
    // (T(origin) · R · S) times T(the alignment offset ±size/2), so an aligned quad turns about its anchor point
    // (GH 0x1402277d0; the offset is set from `alignment` at GH 0x140230a80)
    this->m_scenePivot = glm::vec3 (origin.x - sceneWidth / 2.0f, sceneHeight / 2.0f - origin.y, 0.0f);
}

void CImage::uploadGeometryBuffers (const glm::vec2& size) {
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;
    if (this->getTexture () != nullptr && !this->getTexture ()->isAnimated ()
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())) {
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    float x = 0.0f;
    float y = 0.0f;
    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0f;
    GLfloat realY = 0.0f;

    if (this->getImage ().model->passthrough) {
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0f;
	    realY = -1.0f;
	    realWidth = 1.0f;
	    realHeight = 1.0f;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };
    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_DYNAMIC_DRAW);

    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);
    this->m_modelViewProjectionCopy = this->getImage ().model->passthrough
	? this->m_modelViewProjectionScreen
	: glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
}

CImage::ResolvedTransform CImage::updateGeometryBuffers () {
    auto sceneWidth = static_cast<float> (this->getScene ().getWidth ());
    auto sceneHeight = static_cast<float> (this->getScene ().getHeight ());
    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    const glm::vec3 scale = transform.scale;
    const glm::vec2 size = this->resolveGeometrySize (sceneWidth, sceneHeight, origin);
    const glm::vec2 previousSize = this->m_size;
    this->m_size = size;
    if (this->m_hasPuppetMesh && size != previousSize) {
	this->m_puppetSize = size;
	this->updatePuppetPositionBuffer (size);
    }

    this->updateScenePosition (origin, size, scale, sceneWidth, sceneHeight);
    this->uploadGeometryBuffers (size);
    return transform;
}

bool CImage::isWorldPlaced () const {
    // in a 3D scene every layer is in the world but the ones that cover the screen (full-screen and passthrough
    // layers, and LWE's own bloom image, id -1)
    return this->getScene ().getCamera ().isPerspective () && !this->getImage ().model->fullscreen
	&& !this->getImage ().model->passthrough && this->getId () >= 0;
}

bool CImage::isPerspectiveLayer () const {
    // the image draw (GH 0x140214b50) swaps in the perspective camera when the layer's flag 0x80 is set; a 3D scene's
    // layers are world-placed already. Full-screen and compose layers keep covering the screen (ours: no library
    // scene turns one)
    const auto& image = this->getImage ();
    return !this->getScene ().getCamera ().isPerspective () && image.perspective != nullptr
	&& image.perspective->value->getBool () && !image.model->fullscreen && !image.model->passthrough
	&& this->getId () >= 0;
}

glm::mat4 CImage::screenViewProjection () const {
    const auto& camera = this->getScene ().getCamera ();

    // a 3D scene's screen layers keep the 2D layout over the scene's target
    return camera.isPerspective () ? camera.getScreenProjection () : camera.getProjection () * camera.getLookAt ();
}

void CImage::updateWorldPosition () {
    const glm::vec2 size = this->m_size;
    // the quad in the layer's own space (x right, y up, its pixels), its centre at the origin or moved by
    // `alignment` (the anchor is on the origin, GH 0x140230a80) or by a text block's offset; scale, angles, z and
    // the parents are the world matrix's
    glm::vec2 centre (0.0f);

    if (this->m_localOffset.has_value ()) {
	centre = *this->m_localOffset;
    } else {
	const std::string& alignment = this->getImage ().alignment;
	if (alignment.find ("top") != std::string::npos) {
	    centre.y = -size.y / 2.0f;
	} else if (alignment.find ("bottom") != std::string::npos) {
	    centre.y = size.y / 2.0f;
	}
	if (alignment.find ("left") != std::string::npos) {
	    centre.x = size.x / 2.0f;
	} else if (alignment.find ("right") != std::string::npos) {
	    centre.x = -size.x / 2.0f;
	}
    }

    const float left = centre.x - size.x / 2.0f, right = centre.x + size.x / 2.0f;
    const float bottom = centre.y - size.y / 2.0f, top = centre.y + size.y / 2.0f;
    // the order and texture rows of the 2D quad: (left, bottom) takes v 1
    const GLfloat position[] = { left, bottom, 0.0f, left, top, 0.0f, right, bottom, 0.0f,
				 right, bottom, 0.0f, left, top, 0.0f, right, top, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (position), position, GL_DYNAMIC_DRAW);

    this->m_modelViewProjectionScreen
	= this->getScene ().getCamera ().getWorldViewProjection () * this->getScene ().worldMatrixOf (this->getImage ());
    this->m_modelViewProjectionScreenInverse = glm::inverse (this->m_modelViewProjectionScreen);
}

void CImage::updateScreenSpacePosition () {
    const ResolvedTransform transform = this->updateGeometryBuffers ();
    // g_LayerModelMatrix: WE's world matrix of the layer this frame (origin, angles, scale and the parents')
    this->m_layerModelMatrix = this->getScene ().worldMatrixOf (this->getImage ());

    if (this->isWorldPlaced ()) {
	this->updateWorldPosition ();
	return;
    }

    // Build rotation from angles (already in radians from scene.json — see CParticle.cpp:2119)
    // Negate X and Z rotations to account for Y-flipped coordinate system (CParticle.cpp:2120)
    const float angle = transform.angle;
    const bool perspective = this->isPerspectiveLayer ();
    glm::mat4 rotModel = glm::mat4 (1.0f);
    if (perspective) {
	// a `perspective` layer of an orthographic scene: WE's object matrix (GH 0x1401b5170: R = Rz · Ry · Rx, then
	// the origin with its z) about the layer's pivot, in LWE's y-down scene space (x and z turns negated, as above).
	// The parents' x/y angles are not followed (the 2D transform resolves z turns only)
	const glm::vec3 own = this->getImage ().angles->value->getVec3 ();
	rotModel = glm::translate (rotModel, this->m_scenePivot + glm::vec3 (0.0f, 0.0f, transform.origin.z));
	rotModel = glm::rotate (rotModel, -angle, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::rotate (rotModel, own.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	rotModel = glm::rotate (rotModel, -own.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	rotModel = glm::translate (rotModel, -this->m_scenePivot);
    } else if (angle != 0.0f) {
	rotModel = glm::translate (rotModel, this->m_scenePivot);
	rotModel = glm::rotate (rotModel, -angle, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::translate (rotModel, -this->m_scenePivot);
    }

    // the mouse parallax moves the object in the world, after its own transform (WE: view · translate(offset), GH
    // 0x1401b9b50); WE's y is up, this world's y is down
    const glm::vec2 parallax = this->getScene ().getParallaxOffset (this->getImage ());
    const glm::mat4 mvp = (perspective ? this->getScene ().getCamera ().getPerspective2DViewProjection ()
				       : this->screenViewProjection ())
	* glm::translate (glm::mat4 (1.0f), glm::vec3 (parallax.x, -parallax.y, 0.0f)) * rotModel;

    this->m_modelViewProjectionScreen = mvp;
    this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
	this->m_modelViewProjectionCopyInverse = this->m_modelViewProjectionScreenInverse;
    }
}

const Image& CImage::getImage () const { return this->m_image; }

nlohmann::json CImage::healthLayer () const {
    const float width = static_cast<float> (this->getScene ().getWidth ());
    const float height = static_cast<float> (this->getScene ().getHeight ());
    const auto transform = this->resolveTransform (this->getImage ());
    const glm::vec2 size = this->m_size;
    bool finite = std::isfinite (transform.origin.x) && std::isfinite (transform.origin.y)
	&& std::isfinite (transform.origin.z) && std::isfinite (transform.scale.x) && std::isfinite (transform.scale.y)
	&& std::isfinite (transform.angle) && std::isfinite (size.x) && std::isfinite (size.y);
    for (int col = 0; col < 4; col++) {
	for (int row = 0; row < 4; row++) {
	    finite = finite && std::isfinite (this->m_modelViewProjectionScreen[col][row]);
	}
    }

    // the quad the screen MVP takes: the drawing space's corners (2D), or the layer's own pixels (a 3D scene's world)
    glm::vec2 corners[4];
    if (this->isWorldPlaced ()) {
	glm::vec2 centre = this->m_localOffset.value_or (glm::vec2 (0.0f));
	if (!this->m_localOffset.has_value ()) {
	    const std::string& alignment = this->getImage ().alignment;
	    centre.y = alignment.find ("top") != std::string::npos ? -size.y / 2.0f
		: alignment.find ("bottom") != std::string::npos   ? size.y / 2.0f
								   : 0.0f;
	    centre.x = alignment.find ("left") != std::string::npos ? size.x / 2.0f
		: alignment.find ("right") != std::string::npos    ? -size.x / 2.0f
								   : 0.0f;
	}
	corners[0] = centre - size / 2.0f;
	corners[1] = { centre.x - size.x / 2.0f, centre.y + size.y / 2.0f };
	corners[2] = { centre.x + size.x / 2.0f, centre.y - size.y / 2.0f };
	corners[3] = centre + size / 2.0f;
    } else {
	corners[0] = { this->m_pos.x, this->m_pos.y };
	corners[1] = { this->m_pos.x, this->m_pos.w };
	corners[2] = { this->m_pos.z, this->m_pos.y };
	corners[3] = { this->m_pos.z, this->m_pos.w };
    }

    glm::vec4 box (INFINITY, INFINITY, -INFINITY, -INFINITY);
    bool behind = false;
    for (const auto& corner : corners) {
	const glm::vec4 clip = this->m_modelViewProjectionScreen * glm::vec4 (corner, 0.0f, 1.0f);
	if (!(clip.w > 0.0f)) {
	    behind = true;
	    continue;
	}
	// NDC → canvas pixels, y down (the PNG's rows): the scene draws upside down into its target (the host flips
	// it), so NDC +y is the canvas's bottom
	const float x = (clip.x / clip.w + 1.0f) * 0.5f * width;
	const float y = (1.0f + clip.y / clip.w) * 0.5f * height;
	box = { std::min (box.x, x), std::min (box.y, y), std::max (box.z, x), std::max (box.w, y) };
    }
    if (this->getImage ().model->fullscreen) {
	box = { 0.0f, 0.0f, width, height };
    }

    nlohmann::json out = {
	{ "alpha", this->getImage ().alpha->value->getFloat () },
	{ "size", { size.x, size.y } },
	{ "finite", finite },
	{ "passthrough", this->getImage ().model->passthrough },
	{ "fullscreen", this->getImage ().model->fullscreen },
	{ "passes", this->m_passes.size () },
    };
    if (behind) {
	out["behind_camera"] = true;
    }
    if (std::isfinite (box.x) && std::isfinite (box.y) && std::isfinite (box.z) && std::isfinite (box.w)) {
	const float area = (box.z - box.x) * (box.w - box.y);
	const float onW = std::max (0.0f, std::min (box.z, width) - std::max (box.x, 0.0f));
	const float onH = std::max (0.0f, std::min (box.w, height) - std::max (box.y, 0.0f));
	out["bbox"] = { box.x, box.y, box.z, box.w };
	out["area_px"] = area;
	out["on_canvas_px"] = onW * onH;
    } else {
	out["bbox"] = nullptr;
    }
    return out;
}

bool CImage::hitTest (const glm::vec2& world, glm::vec2& local) const {
    // a layer in a 3D scene's world isn't hit-tested (the cursor would have to be cast into the world)
    if (this->isWorldPlaced ()) {
	return false;
    }

    const float sceneWidth = static_cast<float> (this->getScene ().getWidth ());
    const float sceneHeight = static_cast<float> (this->getScene ().getHeight ());
    // the drawing's space (m_pos): x from the centre, y down
    glm::vec2 point (world.x - sceneWidth / 2.0f, sceneHeight / 2.0f - world.y);

    // undo the mouse parallax and the rotation about the pivot (updateScreenSpacePosition: translate (p.x, -p.y),
    // rotate (-angle) about the pivot)
    const glm::vec2 parallax = this->getScene ().getParallaxOffset (this->getImage ());
    point -= glm::vec2 (parallax.x, -parallax.y);

    const float angle = this->resolveTransform (this->getImage ()).angle;

    if (angle != 0.0f) {
	const glm::vec2 pivot (this->m_scenePivot);
	const glm::vec2 d = point - pivot;
	const float c = std::cos (angle), sn = std::sin (angle);
	point = pivot + glm::vec2 (c * d.x - sn * d.y, sn * d.x + c * d.y);
    }

    const float left = std::min (this->m_pos.x, this->m_pos.z), right = std::max (this->m_pos.x, this->m_pos.z);
    const float top = std::min (this->m_pos.y, this->m_pos.w), bottom = std::max (this->m_pos.y, this->m_pos.w);

    if (point.x < left || point.x > right || point.y < top || point.y > bottom || right <= left || bottom <= top) {
	return false;
    }

    const glm::vec2 size = this->getSize ();
    local = glm::vec2 ((point.x - left) / (right - left) * size.x, (bottom - point.y) / (bottom - top) * size.y);
    return true;
}

glm::vec2 CImage::getSize () const {
    if (this->m_size.x > 0.0f && this->m_size.y > 0.0f) {
	return this->m_size;
    }
    if (this->m_texture == nullptr) {
	return this->getImage ().size;
    }

    return { this->m_texture->getRealWidth (), this->m_texture->getRealHeight () };
}

GLuint CImage::getSceneSpacePosition () const { return this->m_sceneSpacePosition; }

GLuint CImage::getCopySpacePosition () const { return this->m_copySpacePosition; }

GLuint CImage::getPassSpacePosition () const { return this->m_passSpacePosition; }

GLuint CImage::getTexCoordCopy () const { return this->m_texcoordCopy; }

GLuint CImage::getTexCoordPass () const { return this->m_texcoordPass; }

const std::vector<Effects::CPass*>& CImage::getPasses () const { return this->m_passes; }
