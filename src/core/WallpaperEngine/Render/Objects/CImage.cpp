#include "CImage.h"

#include <algorithm>
#include <cmath>

#include "CRenderable.h"
#include "WallpaperEngine/Context.h"

#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Utils;

namespace {
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

struct PuppetMeshBlock {
    size_t headerOffset = 0;
    uint32_t vertexBytes = 0;
    uint32_t indexBytes = 0;
};

std::optional<PuppetMeshBlock> findPuppetMeshBlock (
    const BinaryReader& reader, size_t markerSize, size_t mdlsOffset, size_t meshHeaderSize, size_t vertexStride
) {
    for (size_t offset = markerSize; offset + meshHeaderSize + sizeof (uint32_t) < mdlsOffset; offset++) {
	reader.base ().seekg (static_cast<std::streamoff> (offset + sizeof (uint32_t)), std::ios::beg);
	const uint32_t candidateVertexBytes = reader.nextUInt32 ();
	const size_t verticesOffset = offset + meshHeaderSize;
	const size_t indexLengthOffset = verticesOffset + candidateVertexBytes;

	if (candidateVertexBytes == 0 || candidateVertexBytes % vertexStride != 0
	    || indexLengthOffset + sizeof (uint32_t) > mdlsOffset) {
	    continue;
	}

	reader.base ().seekg (static_cast<std::streamoff> (indexLengthOffset), std::ios::beg);
	const uint32_t candidateIndexBytes = reader.nextUInt32 ();
	const size_t indicesOffset = indexLengthOffset + sizeof (uint32_t);
	if (candidateIndexBytes == 0 || candidateIndexBytes % (sizeof (uint16_t) * 3) != 0
	    || indicesOffset + candidateIndexBytes > mdlsOffset) {
	    continue;
	}

	return PuppetMeshBlock { .headerOffset = offset,
				 .vertexBytes = candidateVertexBytes,
				 .indexBytes = candidateIndexBytes };
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

    // get scene width and height to calculate positions
    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = transform.scale;

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
	this->m_texture = std::make_shared<CFBO> (
	    "", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1, size.x, size.y, size.x, size.y
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

    const auto mainFBO = scene.create (
	nameA.str (), TextureFormat_ARGB8888, compositeFlags, 1, { size.x, size.y }, { size.x, size.y }
    );
    const auto subFBO = scene.create (
	nameB.str (), TextureFormat_ARGB8888, compositeFlags, 1, { size.x, size.y }, { size.x, size.y }
    );
    this->m_currentMainFBO = this->m_mainFBO = mainFBO;
    this->m_currentSubFBO = this->m_subFBO = subFBO;
    this->m_sizedTargets.emplace_back (mainFBO, 1.0f);
    this->m_sizedTargets.emplace_back (subFBO, 1.0f);

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

    this->m_hasPuppetMesh = this->loadPuppetMesh (size);

    // compute the center of the image in scene space for rotation
    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);

    this->m_modelViewProjectionScreen
	= this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getLookAt ();

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
    if (this->m_puppetSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
}

bool CImage::loadPuppetMesh (const glm::vec2& size) {
    if (!this->getImage ().model->puppet.has_value ()) {
	return false;
    }

    try {
	const auto stream = this->getScene ().getScene ().project.assetLocator->read (*this->getImage ().model->puppet);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

	constexpr size_t markerSize = 9;
	constexpr size_t meshHeaderSize = sizeof (uint32_t) * 2;
	constexpr size_t vertexStride = 80;
	constexpr size_t positionOffset = 0;
	constexpr size_t uvOffset = 72;

	const std::string puppetVersion
	    = data.size () >= markerSize ? std::string (data.data (), strlen ("MDLV0021")) : "";
	if (puppetVersion != "MDLV0021" && puppetVersion != "MDLV0023") {
	    sLog.error ("Unsupported puppet model header ", puppetVersion, " in ", *this->getImage ().model->puppet);
	    return false;
	}

	const size_t mdlsOffset = [&data] () -> size_t {
	    for (size_t offset = markerSize; offset + strlen ("MDLS") < data.size (); offset++) {
		if (std::memcmp (data.data () + offset, "MDLS", strlen ("MDLS")) == 0) {
		    return offset;
		}
	    }
	    return data.size ();
	}();

	auto meshBuffer = std::make_unique<char[]> (data.size ());
	std::copy (data.begin (), data.end (), meshBuffer.get ());
	const BinaryReader reader (std::make_shared<MemoryStream> (std::move (meshBuffer), data.size ()));
	const auto meshBlock = findPuppetMeshBlock (reader, markerSize, mdlsOffset, meshHeaderSize, vertexStride);
	if (!meshBlock.has_value ()) {
	    sLog.error ("Could not find a usable MDLV mesh block in ", *this->getImage ().model->puppet);
	    return false;
	}

	const size_t vertexCount = meshBlock->vertexBytes / vertexStride;
	const size_t verticesOffset = meshBlock->headerOffset + meshHeaderSize;
	const size_t indicesOffset = verticesOffset + meshBlock->vertexBytes + sizeof (uint32_t);
	const size_t indexCount = meshBlock->indexBytes / sizeof (uint16_t);
	std::vector<GLfloat> texcoords;
	std::vector<GLushort> indices;

	this->m_puppetRawPositions.clear ();
	this->m_puppetRawPositions.reserve (vertexCount * 3);
	texcoords.reserve (vertexCount * 2);
	indices.reserve (indexCount);

	for (size_t index = 0; index < vertexCount; index++) {
	    const size_t vertexOffset = verticesOffset + index * vertexStride;
	    reader.base ().seekg (static_cast<std::streamoff> (vertexOffset + positionOffset), std::ios::beg);
	    const float x = reader.nextFloat ();
	    const float y = reader.nextFloat ();
	    const float z = reader.nextFloat ();
	    reader.base ().seekg (static_cast<std::streamoff> (vertexOffset + uvOffset), std::ios::beg);
	    const float u = reader.nextFloat ();
	    const float v = reader.nextFloat ();

	    this->m_puppetRawPositions.push_back (x);
	    this->m_puppetRawPositions.push_back (y);
	    this->m_puppetRawPositions.push_back (z);
	    texcoords.push_back (u);
	    texcoords.push_back (v);
	}

	reader.base ().seekg (static_cast<std::streamoff> (indicesOffset), std::ios::beg);
	for (size_t index = 0; index < indexCount; index++) {
	    uint16_t value = 0;
	    reader.next (reinterpret_cast<char*> (&value), sizeof (value));
	    if (value >= vertexCount) {
		sLog.error ("Invalid puppet mesh index ", value, " in ", *this->getImage ().model->puppet);
		return false;
	    }
	    indices.push_back (value);
	}

	this->updatePuppetPositionBuffer (size);

	glGenBuffers (1, &this->m_puppetTexCoord);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
	glBufferData (GL_ARRAY_BUFFER, texcoords.size () * sizeof (GLfloat), texcoords.data (), GL_STATIC_DRAW);

	glGenBuffers (1, &this->m_puppetIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (GLushort), indices.data (), GL_STATIC_DRAW);

	this->m_puppetIndexCount = static_cast<GLsizei> (indices.size ());
	sLog.out (
	    "Loaded puppet mesh ", *this->getImage ().model->puppet, " version=", puppetVersion,
	    " vertices=", vertexCount, " indices=", this->m_puppetIndexCount
	);

	return true;
    } catch (const std::exception& ex) {
	sLog.error ("Could not load puppet mesh ", *this->getImage ().model->puppet, ": ", ex.what ());
	return false;
    }
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

void CImage::setupPuppetGeometryCallback (Effects::CPass* pass) const {
    pass->setGeometryCallback (
	[this, pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSpacePosition);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
		glVertexAttribPointer (texCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	},
	[this] () {
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
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	    glDrawElements (GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_SHORT, nullptr);
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

    // copy pass to the composite layer
    for (const auto& cur : this->getImage ().model->material->passes) {
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
		this->m_sizedTargets.emplace_back (
		    fboProvider->create (*fbo, this->m_texture->getFlags (), this->getSize ()), fbo->scale
		);
	    }

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
			continue;
		    }

		    if (!(*curEffect)->source.has_value ()) {
			sLog.error ("Pass without material and source not supported");
			continue;
		    }

		    if (!(*curEffect)->target.has_value ()) {
			sLog.error ("Pass without material and target not supported");
			continue;
		    }

		    if ((*curEffect)->command != Command_Copy) {
			sLog.error ("Only copy command is supported for pass without material");
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

		    // build a pass for a copy shader
		    this->m_passes.push_back (new CPass (
			*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ()
		    ));
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
		    throw;
		}

		sLog.error ("Leaving out effect ", cur->id, " (", cur->name, ") of object ", this->m_image.id, ": ", e.what ());

		while (this->m_passes.size () > passesBefore) {
		    delete this->m_passes.back ();
		    this->m_passes.pop_back ();
		}
	    }

	    // the passes just added are this effect's: they run only while it is visible
	    this->m_passEffects.resize (this->m_passes.size (), cur.get ());
	}
    }

    if (!debug.baseOnly) {
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
    if (!debug.baseOnly && colorBlendMode > 0 && colorBlendMode != 31) {
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

    // if there's more than one pass the blendmode has to be moved from the beginning to the end (of the passes that
    // run: a hidden last effect hands the layer's blending to the pass before it)
    if (this->m_activePasses.size () > 1) {
	const auto first = this->m_activePasses.front ();
	const auto last = this->m_activePasses.back ();

	last->setBlendingMode (first->getBlendingMode ());
	first->setBlendingMode (BlendingMode_Normal);
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

	if (isFirstPass && this->m_hasPuppetMesh) {
	    pass->setBlendingMode (BlendingMode_Translucent);
	    this->setupPuppetGeometryCallback (pass);
	}

	pass->setModelMatrix (&this->m_modelMatrix);
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

    for (const auto& [fbo, scale] : this->m_sizedTargets) {
	const auto width = static_cast<uint32_t> (std::max (1.0f, size.x / scale));
	const auto height = static_cast<uint32_t> (std::max (1.0f, size.y / scale));
	fbo->resize (width, height, width, height);
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

    // Always update screen transform (handles rotation + parallax dynamically)
    this->updateScreenSpacePosition ();

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

    auto cur = this->m_activePasses.begin ();

    // the last draw keeps the scene's alpha (a compose layer that starts transparent takes this layer's as coverage)
    const bool keepAlpha = this->getScene ().targetKeepsAlpha (this->m_image);

    for (const auto end = this->m_activePasses.end (); cur != end; ++cur) {
	if (std::next (cur) == end) {
	    glColorMask (true, true, true, !keepAlpha);
	}

	(*cur)->render ();
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

const float& CImage::getBrightness () const { return this->m_image.brightness->value->getFloat (); }

const float& CImage::getUserAlpha () const { return this->m_image.alpha->value->getFloat (); }

const float& CImage::getAlpha () const { return this->m_image.alpha->value->getFloat (); }

const glm::vec3& CImage::getColor () const { return this->m_image.color->value->getVec3 (); }

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
	this->updatePuppetPositionBuffer (size);
    }

    this->updateScenePosition (origin, size, scale, sceneWidth, sceneHeight);
    this->uploadGeometryBuffers (size);
    return transform;
}

void CImage::updateScreenSpacePosition () {
    const ResolvedTransform transform = this->updateGeometryBuffers ();

    // Build rotation from angles (already in radians from scene.json — see CParticle.cpp:2119)
    // Negate X and Z rotations to account for Y-flipped coordinate system (CParticle.cpp:2120)
    const float angle = transform.angle;
    glm::mat4 rotModel = glm::mat4 (1.0f);
    if (angle != 0.0f) {
	rotModel = glm::translate (rotModel, this->m_scenePivot);
	rotModel = glm::rotate (rotModel, -angle, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::translate (rotModel, -this->m_scenePivot);
    }

    // the mouse parallax moves the object in the world, after its own transform (WE: view · translate(offset), GH
    // 0x1401b9b50); WE's y is up, this world's y is down
    const glm::vec2 parallax = this->getScene ().getParallaxOffset (this->getImage ());
    const glm::mat4 mvp = this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getLookAt ()
	* glm::translate (glm::mat4 (1.0f), glm::vec3 (parallax.x, -parallax.y, 0.0f)) * rotModel;

    this->m_modelViewProjectionScreen = mvp;
    this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
	this->m_modelViewProjectionCopyInverse = this->m_modelViewProjectionScreenInverse;
    }
}

const Image& CImage::getImage () const { return this->m_image; }

bool CImage::hitTest (const glm::vec2& world, glm::vec2& local) const {
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
