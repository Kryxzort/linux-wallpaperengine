#include "CModel.h"
#include "WallpaperEngine/Health.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using JSON = WallpaperEngine::Data::JSON::JSON;

namespace {
// the interleaved vertex: position, normal, tangent (w: the bitangent's sign), texcoord, blend weights
constexpr GLsizei kFloatsPerVertex = 3 + 3 + 4 + 2 + 4;
// generic4's morph uniforms hold the count and up to 11 targets (g_MorphOffsets[12], g_MorphWeights[12])
constexpr size_t kMorphSlots = 11;
} // namespace

CModelMesh::CModelMesh (
    Wallpapers::CScene& scene, const MeshObject& object, const Material& material, const Puppet::Mesh& mesh
) :
    CObject (scene, object), CRenderable (scene, object, material), m_meshMaterial (material), m_mesh (mesh) { }

CModelMesh::~CModelMesh () {
    for (auto* pass : this->m_passes) {
	delete pass;
    }
    this->m_passes.clear ();
    delete this->m_shadowPass;

    if (this->m_vbo != GL_NONE) {
	glDeleteBuffers (1, &this->m_vbo);
    }
    if (this->m_ibo != GL_NONE) {
	glDeleteBuffers (1, &this->m_ibo);
    }
    if (this->m_boneIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_boneIndices);
    }
}

void CModelMesh::setAnimation (const std::vector<glm::mat4x3>* bones, const std::vector<float>* weights) {
    this->m_bones = bones;
    this->m_shapeWeights = weights;
}

void CModelMesh::buildMorphMap () {
    // model_vertex_v1.h (generic4, the casters): the map is a float stream read 3 (6 with MORPHING_NORMALS) floats at
    // (gl_VertexID + g_MorphOffsets[1 + t]) · 3, packed into RGBA texels of a square texture (y = index / height). Per
    // target, per vertex: its point's delta as the snorm value; g_MorphWeights[0] is the position scale (2.8.42's
    // MORPH_POSITION_SCALE). A vertex's point is its vec4 position's w − 1, else its own index (a dense target)
    const auto& mesh = this->m_mesh;
    const size_t vertices = mesh.vertices.size ();
    if (mesh.morphs.empty () || vertices == 0 || mesh.morphScale == 0.0f) {
	return;
    }
    this->m_morphNormals = (mesh.flags & 0x400) != 0;
    const size_t per = this->m_morphNormals ? 6 : 3;
    std::vector<float> stream (mesh.morphs.size () * vertices * per + 8, 0.0f);
    for (size_t t = 0; t < mesh.morphs.size (); t++) {
	const auto& target = mesh.morphs[t];
	for (size_t v = 0; v < vertices; v++) {
	    const uint32_t w = mesh.vertices[v].morph;
	    const size_t point = w > 0 ? w - 1 : v;
	    if (point >= target.positions.size () || (w == 0 && vertices != mesh.morphPoints)) {
		continue;
	    }
	    float* out = &stream[(t * vertices + v) * per];
	    const glm::vec3 p = target.positions[point] / mesh.morphScale;
	    out[0] = p.x, out[1] = p.y, out[2] = p.z;
	    if (this->m_morphNormals && point < target.normals.size ()) {
		out[3] = target.normals[point].x, out[4] = target.normals[point].y, out[5] = target.normals[point].z;
	    }
	}
    }
    const size_t texels = stream.size () / 4 + 1;
    const auto side = static_cast<uint32_t> (std::ceil (std::sqrt (static_cast<double> (texels))));
    stream.resize (static_cast<size_t> (side) * side * 4, 0.0f);
    this->m_morphMap = std::make_shared<CFBO> ("", TextureFormat_RGBA16161616f, TextureFlags_NoFlags, 1, side, side, side, side);
    glBindTexture (GL_TEXTURE_2D, this->m_morphMap->getTextureID (0));
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, side, side, GL_RGBA, GL_FLOAT, stream.data ());
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture (GL_TEXTURE_2D, 0);
    this->m_morphResolution = glm::vec4 (side, side, side, side);
}

void CModelMesh::bindSkinAttributes (GLuint program) const {
    if (this->m_bones == nullptr) {
	return;
    }
    if (const GLint at = glGetAttribLocation (program, "a_BlendIndices"); at >= 0 && this->m_boneIndices != GL_NONE) {
	glEnableVertexAttribArray (at);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_boneIndices);
	// uvec4 in the shader takes integers; a translator that made it a float vector takes them converted
	GLint count = 0;
	glGetProgramiv (program, GL_ACTIVE_ATTRIBUTES, &count);
	GLenum type = GL_UNSIGNED_INT_VEC4;
	for (GLint i = 0; i < count; i++) {
	    char name[64];
	    GLsizei length = 0;
	    GLint size = 0;
	    GLenum t = 0;
	    glGetActiveAttrib (program, static_cast<GLuint> (i), sizeof (name), &length, &size, &t, name);
	    if (std::string (name, length) == "a_BlendIndices") {
		type = t;
	    }
	}
	if (type == GL_UNSIGNED_INT_VEC4 || type == GL_INT_VEC4) {
	    glVertexAttribIPointer (at, 4, GL_UNSIGNED_INT, 0, nullptr);
	} else {
	    glVertexAttribPointer (at, 4, GL_UNSIGNED_INT, GL_FALSE, 0, nullptr);
	}
    }
    if (const GLint at = glGetAttribLocation (program, "a_BlendWeights"); at >= 0) {
	glEnableVertexAttribArray (at);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
	glVertexAttribPointer (at, 4, GL_FLOAT, GL_FALSE, kFloatsPerVertex * sizeof (GLfloat),
			       reinterpret_cast<const void*> (12 * sizeof (GLfloat)));
    }
}

void CModelMesh::unbindSkinAttributes (GLuint program) const {
    for (const char* name : { "a_BlendIndices", "a_BlendWeights" }) {
	if (const GLint at = glGetAttribLocation (program, name); at >= 0) {
	    glDisableVertexAttribArray (at);
	}
    }
}

void CModelMesh::uploadAnimationUniforms (GLuint program, int morphSlot) const {
    if (this->m_bones != nullptr && !this->m_bones->empty ()) {
	if (const GLint at = glGetUniformLocation (program, "g_Bones"); at >= 0) {
	    glUniformMatrix4x3fv (at, static_cast<GLsizei> (this->m_bones->size ()), GL_FALSE,
				  glm::value_ptr ((*this->m_bones)[0]));
	}
    }
    if (this->m_morphMap == nullptr) {
	return;
    }
    // g_MorphOffsets[0] the targets drawn (≤ 11: the ones weighted ≠ 0, in order; Inferred: WE's pick), [1 + i] the
    // target's first vertex in the map; g_MorphWeights[0] the position scale, [1 + i] the weight
    GLuint offsets[kMorphSlots + 1] = {};
    GLfloat weights[kMorphSlots + 1] = {};
    weights[0] = this->m_mesh.morphScale;
    size_t used = 0;
    if (this->m_shapeWeights != nullptr) {
	for (size_t t = 0; t < this->m_shapeWeights->size () && used < kMorphSlots; t++) {
	    if ((*this->m_shapeWeights)[t] == 0.0f) {
		continue;
	    }
	    offsets[1 + used] = static_cast<GLuint> (t * this->m_mesh.vertices.size ());
	    weights[1 + used] = (*this->m_shapeWeights)[t];
	    used++;
	}
    }
    offsets[0] = static_cast<GLuint> (used);
    if (const GLint at = glGetUniformLocation (program, "g_MorphOffsets"); at >= 0) {
	glUniform1uiv (at, kMorphSlots + 1, offsets);
    }
    if (const GLint at = glGetUniformLocation (program, "g_MorphWeights"); at >= 0) {
	glUniform1fv (at, kMorphSlots + 1, weights);
    }
    const std::string slot = std::to_string (morphSlot);
    const glm::vec4 texel (1.0f / this->m_morphResolution.x, 1.0f / this->m_morphResolution.y, this->m_morphResolution.x,
			   this->m_morphResolution.y);
    if (const GLint at = glGetUniformLocation (program, ("g_Texture" + slot + "Resolution").c_str ()); at >= 0) {
	glUniform4fv (at, 1, glm::value_ptr (this->m_morphResolution));
    }
    if (const GLint at = glGetUniformLocation (program, ("g_Texture" + slot + "Texel").c_str ()); at >= 0) {
	glUniform4fv (at, 1, glm::value_ptr (texel));
    }
}

void CModelMesh::uploadGeometry () {
    std::vector<GLfloat> vertices;
    vertices.reserve (this->m_mesh.vertices.size () * kFloatsPerVertex);
    for (const auto& v : this->m_mesh.vertices) {
	vertices.insert (vertices.end (), { v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
					    v.tangent.x, v.tangent.y, v.tangent.z, v.tangent.w, v.uv.x, v.uv.y,
					    v.weights.x, v.weights.y, v.weights.z, v.weights.w });
    }
    if (this->m_vbo == GL_NONE) {
	glGenBuffers (1, &this->m_vbo);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
    glBufferData (GL_ARRAY_BUFFER, vertices.size () * sizeof (GLfloat), vertices.data (), GL_DYNAMIC_DRAW);
    if (this->m_ibo == GL_NONE) {
	glGenBuffers (1, &this->m_ibo);
    }
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ibo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, this->m_mesh.indices.size () * sizeof (uint32_t), this->m_mesh.indices.data (),
	GL_DYNAMIC_DRAW
    );
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
    this->m_indexCount = static_cast<GLsizei> (this->m_mesh.indices.size ());
}

void CModelMesh::setup () {
    this->detectTexture ();

    // a material without a texture of its own samples white (generic4's albedo default, "util/white")
    if (this->m_texture == nullptr) {
	try {
	    this->m_texture = this->getContext ().resolveTexture ("util/white");
	} catch (const std::exception&) {
	    this->m_texture = nullptr;
	}
    }
    if (this->m_texture == nullptr) {
	auto white = std::make_shared<CFBO> ("", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1, 1, 1, 1, 1);
	GLint previous = 0;
	glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previous);
	glBindFramebuffer (GL_FRAMEBUFFER, white->getFramebuffer ());
	GLfloat clear[4] = {};
	glGetFloatv (GL_COLOR_CLEAR_VALUE, clear);
	glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glClearColor (clear[0], clear[1], clear[2], clear[3]);
	glBindFramebuffer (GL_FRAMEBUFFER, previous);
	this->m_texture = white;
    }

    CRenderable::setup ();

    this->uploadGeometry ();
    if (this->m_bones != nullptr) {
	std::vector<GLuint> bones;
	bones.reserve (this->m_mesh.vertices.size () * 4);
	for (const auto& v : this->m_mesh.vertices) {
	    for (int c = 0; c < 4; c++) {
		// an index past the skeleton reads bone 0 (its weight is 0 in a well-formed file)
		bones.push_back (v.bones[c] < this->m_bones->size () ? v.bones[c] : 0);
	    }
	}
	glGenBuffers (1, &this->m_boneIndices);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_boneIndices);
	glBufferData (GL_ARRAY_BUFFER, bones.size () * sizeof (GLuint), bones.data (), GL_STATIC_DRAW);
    }
    if (this->m_shapeWeights != nullptr) {
	this->buildMorphMap ();
    }


    for (const auto& pass : this->m_meshMaterial.passes) {
	auto* cpass = new CPass (*this, std::make_shared<FBOProvider> (this), *pass, std::nullopt, std::nullopt, std::nullopt);
	const GLuint program = cpass->getProgramID ();

	cpass->setModelMatrix (&this->m_modelMatrix);
	cpass->setViewProjectionMatrix (&this->m_viewProjection);
	cpass->setModelViewProjectionMatrix (&this->m_modelViewProjection);
	cpass->setModelViewProjectionMatrixInverse (&this->m_modelViewProjectionInverse);
	cpass->addUniform ("g_EyePosition", &this->m_eye);
	cpass->setInput (this->m_texture);
	// the morph map is generic4's g_Texture5 ({"material":"morph","hidden":true})
	if (this->m_morphMap != nullptr) {
	    cpass->setTexture (5, this->m_morphMap);
	}

	// the scene's light cookie where the material samples _alias_lightCookie (generic4's g_Texture7 under
	// LIGHTS_COOKIE; WE points the alias at the cookie, GH 0x1401bf270 → scene+0x1310)
	if (const auto cookie = this->getScene ().getLightCookie (); cookie != nullptr) {
	    for (const auto& [index, name] : cpass->getShader ()->getFragment ().getTextures ()) {
		if (name == "_alias_lightCookie") {
		    cpass->setTexture (index, cookie);
		}
	    }
	}

	const GLint position = glGetAttribLocation (program, "a_Position");
	const GLint normal = glGetAttribLocation (program, "a_Normal");
	const GLint tangent = glGetAttribLocation (program, "a_Tangent4");
	const GLint texcoord = glGetAttribLocation (program, "a_TexCoord");

	cpass->setGeometryCallback (
	    [this, program, position, normal, tangent, texcoord] () {
		this->bindSkinAttributes (program);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ibo);
		const GLsizei stride = kFloatsPerVertex * sizeof (GLfloat);
		const std::pair<GLint, std::pair<GLint, size_t>> attributes[] = {
		    { position, { 3, 0 } }, { normal, { 3, 3 } }, { tangent, { 4, 6 } }, { texcoord, { 2, 10 } }
		};
		for (const auto& [location, layout] : attributes) {
		    if (location < 0) {
			continue;
		    }
		    glEnableVertexAttribArray (location);
		    glVertexAttribPointer (
			location, layout.first, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<const void*> (layout.second * sizeof (GLfloat))
		    );
		}
	    },
	    [this, program] () {
		this->uploadSceneUniforms (program);
		this->uploadAnimationUniforms (program, 5);
		glDrawElements (GL_TRIANGLES, this->m_indexCount, GL_UNSIGNED_INT, nullptr);
	    },
	    [this, program, position, normal, tangent, texcoord] () {
		this->unbindSkinAttributes (program);
		for (const GLint location : { position, normal, tangent, texcoord }) {
		    if (location >= 0) {
			glDisableVertexAttribArray (location);
		    }
		}
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
	    }
	);

	this->m_passes.push_back (cpass);
    }

    this->m_ready = !this->m_passes.empty () && this->m_indexCount > 0;
}

void CModelMesh::uploadSceneUniforms (GLuint program) const {
    const auto& scene = this->getScene ();

    if (const GLint at = glGetUniformLocation (program, "g_NormalModelMatrix"); at >= 0) {
	glUniformMatrix3fv (at, 1, GL_FALSE, glm::value_ptr (this->m_normalMatrix));
    }

    const auto& lights = scene.getLights ();
    using SceneLights = Wallpapers::CScene::SceneLights;
    if (const GLint at = glGetUniformLocation (program, "g_LCluster_Counts"); at >= 0) {
	glUniform4f (
	    at, static_cast<float> (lights.count), static_cast<float> (lights.tubes),
	    static_cast<float> (lights.projections), 0.0f
	);
    }
    const std::tuple<const char*, const glm::vec4*, int> arrays[] = {
	{ "g_LCluster_Origin", lights.origin, SceneLights::Max },
	{ "g_LCluster_Color", lights.color, SceneLights::Max },
	{ "g_LCluster_Direction", lights.direction, SceneLights::Max },
	{ "g_LCluster_Extra", lights.extra, SceneLights::Max },
	{ "g_LCluster_ProjectionTransform", lights.projectionTransform, SceneLights::MaxProjections },
	{ "g_LCluster_PointProjection", lights.pointProjection, SceneLights::MaxProjections },
	{ "g_LDirectional_Color", lights.directionalColor, scene.directionalLightCount () },
	{ "g_LDirectional_Direction", lights.directionalDirection, scene.directionalLightCount () },
	// the atlas's texel size: generic4's SHADOW_ATLAS_TEXEL (2.8.485's generator names it g_TextureShadowAtlasTexel)
	{ "g_Texture6Texel", &lights.atlasTexel, 1 },
	{ "g_TextureShadowAtlasTexel", &lights.atlasTexel, 1 },
    };
    for (const auto& [name, values, count] : arrays) {
	if (count <= 0) {
	    continue;
	}
	if (const GLint at = glGetUniformLocation (program, name); at >= 0) {
	    glUniform4fv (at, count, glm::value_ptr (values[0]));
	}
    }
    if (const GLint at = glGetUniformLocation (program, "g_LCluster_Projection"); at >= 0) {
	glUniformMatrix4fv (at, SceneLights::MaxProjections, GL_FALSE, glm::value_ptr (lights.projection[0]));
    }

    const auto& fog = scene.getScene ().fog;
    const std::pair<const char*, glm::vec4> fog4[] = {
	{ "g_FogDistanceParams", fog.distanceParams },
	{ "g_FogHeightParams", fog.heightParams },
    };
    for (const auto& [name, value] : fog4) {
	if (const GLint at = glGetUniformLocation (program, name); at >= 0) {
	    glUniform4fv (at, 1, glm::value_ptr (value));
	}
    }
    const std::pair<const char*, glm::vec3> fog3[] = {
	{ "g_FogDistanceColor", fog.distanceColor },
	{ "g_FogHeightColor", fog.heightColor },
    };
    for (const auto& [name, value] : fog3) {
	if (const GLint at = glGetUniformLocation (program, name); at >= 0) {
	    glUniform3fv (at, 1, glm::value_ptr (value));
	}
    }
}

void CModelMesh::draw (const std::shared_ptr<const CFBO>& target, const glm::mat4& world, bool keepAlpha) {
    const auto& camera = this->getScene ().getCamera ();

    this->m_modelMatrix = world;
    this->m_viewProjection = camera.getWorldViewProjection ();
    if (!camera.isPerspective ()) {
	// a 2D scene ("flat", IModelLayer.perspective false; createModelData: "1 unit = 1 pixel"): WE's world pixels
	// (y up) into LWE's scene space (x − w/2, h/2 − y), then the layers' projection. Ours: z is squashed into the
	// 2D projection's depth range (in front of the eye), and the 2D target has no depth buffer
	const float w = camera.getWidth (), h = camera.getHeight ();
	const glm::mat4 toScene = glm::translate (glm::mat4 (1.0f), glm::vec3 (-w / 2.0f, h / 2.0f, -1.0f))
	    * glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1e-4f));
	this->m_viewProjection = camera.getProjection () * camera.getLookAt () * toScene;
    }
    this->m_modelViewProjection = this->m_viewProjection * world;
    this->m_modelViewProjectionInverse = glm::inverse (this->m_modelViewProjection);
    this->m_normalMatrix = glm::transpose (glm::inverse (glm::mat3 (world)));
    this->m_eye = camera.getWorldEye ();

    for (auto* pass : this->m_passes) {
	pass->setDestination (target);
	glColorMask (true, true, true, !keepAlpha);
	pass->render ();
    }

    glColorMask (true, true, true, true);
    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
}

void CModelMesh::setupShadowCaster (const Material& caster) {
    if (caster.passes.empty () || this->m_shadowPass != nullptr) {
	return;
    }

    auto* cpass = new CPass (*this, std::make_shared<FBOProvider> (this), *caster.passes[0], std::nullopt, std::nullopt, std::nullopt);
    const GLuint program = cpass->getProgramID ();
    cpass->setModelMatrix (&this->m_modelMatrix);
    cpass->setInput (this->m_texture);
    // the casters read the morph map as g_Texture1 ({"material":"morph"})
    if (this->m_morphMap != nullptr) {
	cpass->setTexture (1, this->m_morphMap);
    }

    const GLint position = glGetAttribLocation (program, "a_Position");
    const GLint texcoord = glGetAttribLocation (program, "a_TexCoord");
    const GLint normal = glGetAttribLocation (program, "a_Normal");
    const GLint viewports = glGetUniformLocation (program, "g_ViewportViewProjectionMatrices");

    cpass->setGeometryCallback (
	[this, program, position, texcoord, normal] () {
	    this->bindSkinAttributes (program);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ibo);
	    const GLsizei stride = kFloatsPerVertex * sizeof (GLfloat);
	    const std::pair<GLint, std::pair<GLint, size_t>> attributes[] = {
		{ position, { 3, 0 } }, { normal, { 3, 3 } }, { texcoord, { 2, 10 } }
	    };
	    for (const auto& [location, layout] : attributes) {
		if (location < 0) {
		    continue;
		}
		glEnableVertexAttribArray (location);
		glVertexAttribPointer (
		    location, layout.first, GL_FLOAT, GL_FALSE, stride,
		    reinterpret_cast<const void*> (layout.second * sizeof (GLfloat))
		);
	    }
	},
	[this, program, viewports] () {
	    this->uploadAnimationUniforms (program, 1);
	    // one draw per viewport: its rect bound and its matrix as g_ViewportViewProjectionMatrices[0] (the caster
	    // reads [gl_InstanceID], 0 for a single instance); WE draws up to six instances, one per viewport
	    if (this->m_shadowViews == nullptr) {
		return;
	    }
	    for (const auto& view : *this->m_shadowViews) {
		if (GLAD_GL_VERSION_4_1) {
		    glViewportIndexedf (0, view.rect.x, view.rect.y, view.rect.z, view.rect.w);
		} else {
		    glViewport (
			static_cast<GLint> (view.rect.x), static_cast<GLint> (view.rect.y), static_cast<GLsizei> (view.rect.z),
			static_cast<GLsizei> (view.rect.w)
		    );
		}
		if (viewports >= 0) {
		    glUniformMatrix4fv (viewports, 1, GL_FALSE, glm::value_ptr (view.clip));
		}
		glDrawElements (GL_TRIANGLES, this->m_indexCount, GL_UNSIGNED_INT, nullptr);
	    }
	},
	[this, program, position, texcoord, normal] () {
	    this->unbindSkinAttributes (program);
	    for (const GLint location : { position, texcoord, normal }) {
		if (location >= 0) {
		    glDisableVertexAttribArray (location);
		}
	    }
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
	}
    );

    this->m_shadowPass = cpass;
}

void CModelMesh::drawShadow (
    const std::shared_ptr<const CFBO>& atlas, const glm::mat4& world, const std::vector<Wallpapers::CScene::ShadowView>& views
) {
    if (this->m_shadowPass == nullptr) {
	return;
    }

    this->m_modelMatrix = world;
    this->m_shadowViews = &views;
    this->m_shadowPass->setDestination (atlas);
    this->m_shadowPass->render ();
    this->m_shadowViews = nullptr;
}

bool CModelMesh::isReady () const { return this->m_ready; }

const float& CModelMesh::getBrightness () const { return this->m_one; }

const float& CModelMesh::getUserAlpha () const { return this->m_one; }

const float& CModelMesh::getAlpha () const { return this->m_one; }

const glm::vec3& CModelMesh::getColor () const { return this->m_white; }

const glm::vec4& CModelMesh::getColor4 () const { return this->m_white4; }

const glm::vec3& CModelMesh::getCompositeColor () const { return this->m_white; }

// a script reaches a model layer (thisScene.getLayer, createLayer's result) as any layer: its object's transform
CModel::CModel (Wallpapers::CScene& scene, const MeshObject& object) :
    CObject (scene, object), Scripting::ScriptableObject (scene, object), m_object (object) {
    // a model's scale / angles / visible are the values it draws with (CScene::localMatrixOf, isEffectivelyVisible):
    // scripts on them run as an image's do (3477054430's head turns by an angles script, 3509243656's stars spin and
    // size by angles / scale scripts; both stood still)
    this->registerProperty ("scale", *object.groupScale->value);
    this->registerProperty ("angles", *object.groupAngles->value);
    this->registerProperty ("visible", *object.groupVisible->value);
}

CModel::~CModel () = default;

void CModel::setup () {
    const auto& project = this->getScene ().getScene ().project;
    const std::string& file = this->m_object.file;

    // IScene.createModelData's geometry (createLayer ({ model: data })): the scene holds it
    if (file.rfind ("__modeldata/", 0) == 0) {
	this->m_scriptModelId = std::atoi (file.c_str () + 12);
	const auto* data = this->getScene ().scriptModel (this->m_scriptModelId);
	if (data == nullptr) {
	    sLog.error ("createLayer: no model data ", file);
	    return;
	}
	this->m_model = data->model;
	this->m_scriptVersion = data->version;
	this->m_scriptRebuilt = data->rebuilt;
	this->buildMeshes ();
	return;
    }

    try {
	const auto stream = project.assetLocator->read (file);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };
	std::string error;

	if (!Puppet::parse (data.data (), data.size (), this->m_model, error)) {
	    sLog.error ("Could not load model ", file, ": ", error);
WPENGINE_HEALTH ("missing_asset", { { "kind", "model" }, { "path", file }, { "message", error } });
	    return;
	}
	if (!error.empty ()) {
	    sLog.error ("Model ", file, " partly loaded: ", error);
	    WPENGINE_HEALTH ("asset_error", { { "kind", "model" }, { "path", file }, { "message", "partly loaded: " + error } });
	}
    } catch (const std::exception& ex) {
	sLog.error ("Could not load model ", file, ": ", ex.what ());
	WPENGINE_HEALTH ("missing_asset", { { "kind", "model" }, { "path", file }, { "message", WallpaperEngine::Health::brief (ex.what ()) } });
	return;
    }

    // F4: a skeleton with animations (MDLS + MDLA) is posed by the object's animation layers, as a puppet's (GH
    // 0x140228470), and skinned on the GPU (generic4's SKINNING, g_Bones = world · bindWorld⁻¹ as mat4x3, BONECOUNT the
    // bone count: GH 0x140230ec0 sets them for every skinned mesh); blend shapes (MDMP) through MORPHING
    this->m_animated = !this->m_model.bones.empty () && !this->m_model.animations.empty ();
    if (this->m_animated) {
	this->m_bind = Puppet::bindPose (this->m_model);
	Puppet::bindWorldMatrices (this->m_model, this->m_bindWorldInverse);
	for (auto& m : this->m_bindWorldInverse) {
	    m = glm::inverse (m);
	}
	this->m_skinning.assign (this->m_model.bones.size (), glm::mat4x3 (1.0f));
	for (const auto& layer : this->m_object.animationLayers) {
	    this->m_layers.push_back ({ .blendIn = layer->blendIn, .layer = layer.get () });
	}
    }
    this->buildMeshes ();
}

void CModel::buildMeshes () {
    const auto& project = this->getScene ().getScene ().project;
    const std::string& file = this->m_object.file;
    this->m_shapeWeights.clear ();
    for (const auto& mesh : this->m_model.meshes) {
	this->m_shapeWeights.emplace_back (mesh.morphs.size (), 0.0f);
    }

    const auto& fog = this->getScene ().getScene ().fog;
    const auto& scene = this->getScene ();
    const bool casts = scene.castsShadows () && this->m_object.castShadow != nullptr
	&& this->m_object.castShadow->value->getBool ();

    for (size_t meshIndex = 0; meshIndex < this->m_model.meshes.size (); meshIndex++) {
	const auto& mesh = this->m_model.meshes[meshIndex];
	const bool skinned = this->m_animated && std::ranges::any_of (mesh.vertices, [] (const Puppet::Vertex& v) {
	    return v.weights != glm::vec4 (0.0f);
	});
	const bool morphed = !mesh.morphs.empty () && mesh.morphScale != 0.0f;
	try {
	    auto material = MaterialParser::load (project, mesh.material);
	    const auto raw = JSON::parse (project.assetLocator->readString (mesh.material));
	    const auto& passes = raw.require ("passes", "Material must have passes to render");

	    for (size_t i = 0; i < material->passes.size (); i++) {
		auto& pass = *material->passes[i];
		const auto& json = i < passes.size () ? passes[i] : JSON::object ();

		// a model depth tests and writes depth unless its material says otherwise (the parser's 2D default is
		// off; ours). The y flip of the scene's target turns the winding over: culling is left off
		if (!json.contains ("depthtest")) {
		    pass.depthtest = DepthtestMode_Enabled;
		}
		if (!json.contains ("depthwrite")) {
		    pass.depthwrite = DepthwriteMode_Enabled;
		}
		pass.cullmode = CullingMode_Disable;

		// the lights' combos (WE sets them on every model pass it builds, GH 0x1401c56e0 and the scene's light
		// config): the shadow atlas and its quality when a light casts a shadow, the cookie, the directional count
		if (scene.castsShadows ()) {
		    pass.combos.insert_or_assign ("LIGHTS_SHADOW_MAPPING", 1);
		    pass.combos.insert_or_assign ("LIGHTS_SHADOW_MAPPING_QUALITY", Wallpapers::CScene::ShadowQuality);
		}
		if (scene.getLightCookie () != nullptr) {
		    pass.combos.insert_or_assign ("LIGHTS_COOKIE", 1);
		}
		if (scene.directionalLightCount () > 0) {
		    pass.combos.insert_or_assign ("LIGHTS_DIRECTIONAL", scene.directionalLightCount ());
		}

		if (skinned) {
		    pass.combos.insert_or_assign ("SKINNING", 1);
		    pass.combos.insert_or_assign ("BONECOUNT", static_cast<int> (this->m_model.bones.size ()));
		}
		if (morphed) {
		    pass.combos.insert_or_assign ("MORPHING", 1);
		    if (mesh.flags & 0x400) {
			pass.combos.insert_or_assign ("MORPHING_NORMALS", 1);
		    }
		}

		// the scene's fog for a material that keeps generic4's FOG combo (default 1)
		const auto fogCombo = pass.combos.find ("FOG");
		if (fogCombo == pass.combos.end () || fogCombo->second != 0) {
		    if (fog.distance) {
			pass.combos.insert_or_assign ("FOG_DIST", 1);
		    }
		    if (fog.height) {
			pass.combos.insert_or_assign ("FOG_HEIGHT", 1);
		    }
		}
	    }

	    auto renderable = std::make_unique<CModelMesh> (this->getScene (), this->m_object, *material, mesh);
	    renderable->setAnimation (skinned ? &this->m_skinning : nullptr, morphed ? &this->m_shapeWeights[meshIndex] : nullptr);
	    renderable->setup ();

	    if (renderable->isReady () && casts) {
		// its caster: materials/util/shadowcaster.json with the shader for the mesh's (WE maps the material's
		// shader through a table, GH 0x140185230; foliage4 → shadowcasterfoliage4, fur4 → shadowcasterfur4 by
		// their names, inferred) and the mesh material's combos (ALPHATOCOVERAGE, …)
		try {
		    auto caster = MaterialParser::load (project, "materials/util/shadowcaster.json");
		    const auto& first = *material->passes.front ();
		    for (auto& pass : caster->passes) {
			if (first.shader == "foliage4" || first.shader == "fur4") {
			    pass->shader = "shadowcaster" + first.shader;
			}
			for (const auto& [name, value] : first.combos) {
			    pass->combos.insert_or_assign (name, value);
			}
			pass->depthtest = DepthtestMode_Enabled;
			pass->depthwrite = DepthwriteMode_Enabled;
			pass->cullmode = CullingMode_Disable;
			pass->blending = BlendingMode_Normal;
		    }
		    renderable->setupShadowCaster (*caster);
		    this->m_materials.push_back (std::move (caster));
		} catch (const std::exception& ex) {
		    sLog.error ("No shadow caster for mesh ", mesh.material, " of model ", file, ": ", ex.what ());
		    WPENGINE_HEALTH ("asset_error", { { "kind", "material" }, { "path", "materials/util/shadowcaster.json" }, { "message", "no shadow caster: " + WallpaperEngine::Health::brief (ex.what ()) } });
		}
	    }

	    this->m_materials.push_back (std::move (material));

	    if (renderable->isReady ()) {
		this->m_meshes.push_back (std::move (renderable));
	    }
	} catch (const std::exception& ex) {
	    sLog.error ("Leaving out mesh ", mesh.material, " of model ", file, ": ", ex.what ());
	    WPENGINE_HEALTH ("asset_error", { { "kind", "material" }, { "path", mesh.material }, { "message", "mesh left out: " + WallpaperEngine::Health::brief (ex.what ()) } });
	}
    }

    sLog.out (
	"Loaded model ", file, " version=", this->m_model.version, " meshes=", this->m_meshes.size (), "/",
	this->m_model.meshes.size ()
    );
}

void CModel::updatePose () {
    if (!this->m_animated) {
	return;
    }
    const float now = static_cast<float> (this->getScene ().getTime ());
    if (now == this->m_poseTime) {
	return;
    }
    const float dt = this->m_poseTime < 0.0f ? 0.0f : std::max (now - this->m_poseTime, 0.0f);
    this->m_poseTime = now;

    std::vector<Puppet::Key> pose = this->m_bind;
    std::vector<float> alpha (this->m_model.bones.size (), 1.0f);
    for (auto& weights : this->m_shapeWeights) {
	std::fill (weights.begin (), weights.end (), 0.0f);
    }
    for (auto& state : this->m_layers) {
	const auto& layer = *state.layer;
	const auto id = static_cast<uint32_t> (layer.animation->value->getInt ());
	const auto match = std::ranges::find_if (this->m_model.animations, [id] (const Puppet::Animation& a) { return a.id == id; });
	if (match == this->m_model.animations.end () || !layer.visible->value->getBool ()) {
	    continue;
	}
	Puppet::advance (*match, state.clock, dt * layer.rate->value->getFloat ());
	const float weight = Puppet::layerWeight (*match, state.clock, layer.blend->value->getFloat (), state.blendIn,
						  layer.blendOut, layer.blendTime);
	Puppet::mix (pose, this->m_bind, *match, state.clock.time, weight, layer.additive);
	Puppet::mixExtras (alpha, this->m_shapeWeights, *match, state.clock.time, weight, layer.additive);
    }
    std::vector<glm::mat4> world;
    Puppet::worldMatrices (this->m_model, pose, world);
    for (size_t b = 0; b < world.size () && b < this->m_skinning.size (); b++) {
	this->m_skinning[b] = glm::mat4x3 (world[b] * this->m_bindWorldInverse[b]);
    }
}

void CModel::followScriptModel () {
    if (this->m_scriptModelId <= 0) {
	return;
    }
    const auto* data = this->getScene ().scriptModel (this->m_scriptModelId);
    if (data == nullptr || data->version == this->m_scriptVersion) {
	return;
    }
    this->m_scriptVersion = data->version;
    if (data->rebuilt != this->m_scriptRebuilt || data->model.meshes.size () != this->m_model.meshes.size ()) {
	// replaceData: the shapes (and their materials) built again
	this->m_scriptRebuilt = data->rebuilt;
	this->m_meshes.clear ();
	this->m_materials.clear ();
	this->m_model = data->model;
	this->buildMeshes ();
	return;
    }
    // applyData: the same shapes, new buffers ("a high performance method that you may call every frame")
    for (size_t m = 0; m < this->m_model.meshes.size (); m++) {
	this->m_model.meshes[m].vertices = data->model.meshes[m].vertices;
	this->m_model.meshes[m].indices = data->model.meshes[m].indices;
    }
    for (const auto& mesh : this->m_meshes) {
	mesh->uploadGeometry ();
    }
}

void CModel::render () {
    this->followScriptModel ();
    if (this->m_meshes.empty () || !this->getScene ().isEffectivelyVisible (this->m_object)) {
	return;
    }
    this->updatePose ();

    const glm::mat4 world = this->getScene ().worldMatrixOf (this->m_object);
    const auto target = this->getScene ().getTargetFor (this->m_object);
    const bool keepAlpha = this->getScene ().targetKeepsAlpha (this->m_object);

    for (const auto& mesh : this->m_meshes) {
	mesh->draw (target, world, keepAlpha);
    }
}

void CModel::renderShadow (const std::shared_ptr<const CFBO>& atlas, const std::vector<Wallpapers::CScene::ShadowView>& views) {
    if (this->m_meshes.empty () || this->m_object.castShadow == nullptr || !this->m_object.castShadow->value->getBool ()
	|| !this->getScene ().isEffectivelyVisible (this->m_object)) {
	return;
    }

    this->updatePose ();
    const glm::mat4 world = this->getScene ().worldMatrixOf (this->m_object);
    for (const auto& mesh : this->m_meshes) {
	mesh->drawShadow (atlas, world, views);
    }
}

const MeshObject& CModel::getMeshObject () const { return this->m_object; }

size_t CModel::getMeshCount () const { return this->m_meshes.size (); }
