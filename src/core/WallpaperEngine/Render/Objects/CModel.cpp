#include "CModel.h"

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
// the interleaved vertex: position, normal, tangent (w: the bitangent's sign), texcoord
constexpr GLsizei kFloatsPerVertex = 3 + 3 + 4 + 2;
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

    if (this->m_vbo != GL_NONE) {
	glDeleteBuffers (1, &this->m_vbo);
    }
    if (this->m_ibo != GL_NONE) {
	glDeleteBuffers (1, &this->m_ibo);
    }
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

    std::vector<GLfloat> vertices;
    vertices.reserve (this->m_mesh.vertices.size () * kFloatsPerVertex);
    for (const auto& v : this->m_mesh.vertices) {
	vertices.insert (vertices.end (), { v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
					    v.tangent.x, v.tangent.y, v.tangent.z, v.tangent.w, v.uv.x, v.uv.y });
    }

    glGenBuffers (1, &this->m_vbo);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_vbo);
    glBufferData (GL_ARRAY_BUFFER, vertices.size () * sizeof (GLfloat), vertices.data (), GL_STATIC_DRAW);
    glGenBuffers (1, &this->m_ibo);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_ibo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, this->m_mesh.indices.size () * sizeof (uint32_t), this->m_mesh.indices.data (),
	GL_STATIC_DRAW
    );
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
    this->m_indexCount = static_cast<GLsizei> (this->m_mesh.indices.size ());

    for (const auto& pass : this->m_meshMaterial.passes) {
	auto* cpass = new CPass (*this, std::make_shared<FBOProvider> (this), *pass, std::nullopt, std::nullopt, std::nullopt);
	const GLuint program = cpass->getProgramID ();

	cpass->setModelMatrix (&this->m_modelMatrix);
	cpass->setViewProjectionMatrix (&this->m_viewProjection);
	cpass->setModelViewProjectionMatrix (&this->m_modelViewProjection);
	cpass->setModelViewProjectionMatrixInverse (&this->m_modelViewProjectionInverse);
	cpass->addUniform ("g_EyePosition", &this->m_eye);
	cpass->setInput (this->m_texture);

	const GLint position = glGetAttribLocation (program, "a_Position");
	const GLint normal = glGetAttribLocation (program, "a_Normal");
	const GLint tangent = glGetAttribLocation (program, "a_Tangent4");
	const GLint texcoord = glGetAttribLocation (program, "a_TexCoord");

	cpass->setGeometryCallback (
	    [this, position, normal, tangent, texcoord] () {
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
		glDrawElements (GL_TRIANGLES, this->m_indexCount, GL_UNSIGNED_INT, nullptr);
	    },
	    [position, normal, tangent, texcoord] () {
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
    if (const GLint at = glGetUniformLocation (program, "g_LCluster_Counts"); at >= 0) {
	glUniform4f (at, static_cast<float> (lights.count), 0.0f, 0.0f, 0.0f);
    }
    const std::pair<const char*, const glm::vec4*> arrays[] = {
	{ "g_LCluster_Origin", lights.origin },
	{ "g_LCluster_Color", lights.color },
	{ "g_LCluster_Direction", lights.direction },
	{ "g_LCluster_Extra", lights.extra },
    };
    for (const auto& [name, values] : arrays) {
	if (const GLint at = glGetUniformLocation (program, name); at >= 0) {
	    glUniform4fv (at, Wallpapers::CScene::SceneLights::Max, glm::value_ptr (values[0]));
	}
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

bool CModelMesh::isReady () const { return this->m_ready; }

const float& CModelMesh::getBrightness () const { return this->m_one; }

const float& CModelMesh::getUserAlpha () const { return this->m_one; }

const float& CModelMesh::getAlpha () const { return this->m_one; }

const glm::vec3& CModelMesh::getColor () const { return this->m_white; }

const glm::vec4& CModelMesh::getColor4 () const { return this->m_white4; }

const glm::vec3& CModelMesh::getCompositeColor () const { return this->m_white; }

CModel::CModel (Wallpapers::CScene& scene, const MeshObject& object) : CObject (scene, object), m_object (object) { }

CModel::~CModel () = default;

void CModel::setup () {
    const auto& project = this->getScene ().getScene ().project;
    const std::string& file = this->m_object.file;

    if (!this->getScene ().getCamera ().isPerspective ()) {
	// (a model in a 2D scene: none in the corpus; there's no depth buffer to draw it against)
	sLog.error ("Model ", file, " is in a 2D scene: not drawn");
	return;
    }

    try {
	const auto stream = project.assetLocator->read (file);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };
	std::string error;

	if (!Puppet::parse (data.data (), data.size (), this->m_model, error)) {
	    sLog.error ("Could not load model ", file, ": ", error);
	    return;
	}
	if (!error.empty ()) {
	    sLog.error ("Model ", file, " partly loaded: ", error);
	}
    } catch (const std::exception& ex) {
	sLog.error ("Could not load model ", file, ": ", ex.what ());
	return;
    }

    const auto& fog = this->getScene ().getScene ().fog;

    for (const auto& mesh : this->m_model.meshes) {
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
	    this->m_materials.push_back (std::move (material));
	    renderable->setup ();

	    if (renderable->isReady ()) {
		this->m_meshes.push_back (std::move (renderable));
	    }
	} catch (const std::exception& ex) {
	    sLog.error ("Leaving out mesh ", mesh.material, " of model ", file, ": ", ex.what ());
	}
    }

    sLog.out (
	"Loaded model ", file, " version=", this->m_model.version, " meshes=", this->m_meshes.size (), "/",
	this->m_model.meshes.size ()
    );
}

void CModel::render () {
    if (this->m_meshes.empty () || !this->getScene ().isEffectivelyVisible (this->m_object)) {
	return;
    }

    const glm::mat4 world = this->getScene ().worldMatrixOf (this->m_object);
    const auto target = this->getScene ().getTargetFor (this->m_object);
    const bool keepAlpha = this->getScene ().targetKeepsAlpha (this->m_object);

    for (const auto& mesh : this->m_meshes) {
	mesh->draw (target, world, keepAlpha);
    }
}

const MeshObject& CModel::getMeshObject () const { return this->m_object; }

size_t CModel::getMeshCount () const { return this->m_meshes.size (); }
