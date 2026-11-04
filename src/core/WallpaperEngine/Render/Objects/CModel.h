#pragma once

#include <memory>
#include <vector>

#include "CRenderable.h"
#include "Puppet.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

namespace WallpaperEngine::Render::Objects {
/**
 * One mesh of a model object: its material's passes (WE's `generic4` and the like) over the mesh's vertices, in the
 * model's world matrix through the 3D scene's camera. It isn't one of the scene's objects: its model draws it
 */
class CModelMesh final : public CRenderable {
public:
    CModelMesh (
	Wallpapers::CScene& scene, const MeshObject& object, const Material& material, const Puppet::Mesh& mesh
    );
    ~CModelMesh () override;

    void setup () override;
    /** Draws the mesh into `target` with this frame's matrices */
    void draw (const std::shared_ptr<const CFBO>& target, const glm::mat4& world, bool keepAlpha);
    [[nodiscard]] bool isReady () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

private:
    /** the engine values CPass doesn't feed: the normal matrix, the lights, the fog */
    void uploadSceneUniforms (GLuint program) const;

    const Material& m_meshMaterial;
    const Puppet::Mesh& m_mesh;
    std::vector<Effects::CPass*> m_passes = {};
    GLuint m_vbo = GL_NONE;
    GLuint m_ibo = GL_NONE;
    GLsizei m_indexCount = 0;
    bool m_ready = false;

    glm::mat4 m_modelMatrix = glm::mat4 (1.0f);
    glm::mat4 m_viewProjection = glm::mat4 (1.0f);
    glm::mat4 m_modelViewProjection = glm::mat4 (1.0f);
    glm::mat4 m_modelViewProjectionInverse = glm::mat4 (1.0f);
    glm::mat3 m_normalMatrix = glm::mat3 (1.0f);
    glm::vec3 m_eye = glm::vec3 (0.0f);

    float m_one = 1.0f;
    glm::vec3 m_white = glm::vec3 (1.0f);
    glm::vec4 m_white4 = glm::vec4 (1.0f);
};

/**
 * A model object (`"model": "models/x.mdl"`, G25): WE's MDL file read by the puppet loader (GH 0x1402873f0) for
 * its meshes only (static: no skinning, morphs or animation), each mesh drawn with the material it names. A 3D
 * scene's models depth test against the scene's depth buffer. A mesh whose material can't be built is left out
 */
class CModel final : public CObject {
public:
    CModel (Wallpapers::CScene& scene, const MeshObject& object);
    ~CModel () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const MeshObject& getMeshObject () const;
    /** The meshes that could be built (tests) */
    [[nodiscard]] size_t getMeshCount () const;

private:
    const MeshObject& m_object;
    Puppet::Model m_model = {};
    std::vector<MaterialUniquePtr> m_materials = {};
    std::vector<std::unique_ptr<CModelMesh>> m_meshes = {};
};
} // namespace WallpaperEngine::Render::Objects
