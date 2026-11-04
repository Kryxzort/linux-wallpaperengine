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
    /** Builds the mesh's shadow caster pass from `caster` (materials/util/shadowcaster.json with its shader picked) */
    void setupShadowCaster (const Material& caster);
    /** Draws the mesh's caster into every shadow viewport of `atlas` (the scene's shadow pass) */
    void drawShadow (
	const std::shared_ptr<const CFBO>& atlas, const glm::mat4& world,
	const std::vector<Wallpapers::CScene::ShadowView>& views
    );
    [[nodiscard]] bool isReady () const;
    /**
     * The model's animation for this mesh (before setup): `bones` the skinning matrices (g_Bones, SKINNING with
     * BONECOUNT), `weights` its blend-shape weights (MORPHING over g_Texture5's morph map); either may be null
     */
    void setAnimation (const std::vector<glm::mat4x3>* bones, const std::vector<float>* weights);

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

private:
    /** the engine values CPass doesn't feed: the normal matrix, the lights, the fog */
    void uploadSceneUniforms (GLuint program) const;
    /** g_Bones, g_MorphOffsets / g_MorphWeights and the morph map's resolution in slot `morphSlot` */
    void uploadAnimationUniforms (GLuint program, int morphSlot) const;
    /** builds the morph map (model_vertex_v1.h's layout: per target, per vertex 3 floats, 6 with normals) */
    void buildMorphMap ();
    /** binds a_BlendIndices / a_BlendWeights (SKINNING) */
    void bindSkinAttributes (GLuint program) const;
    void unbindSkinAttributes (GLuint program) const;

    const Material& m_meshMaterial;
    const Puppet::Mesh& m_mesh;
    std::vector<Effects::CPass*> m_passes = {};
    Effects::CPass* m_shadowPass = nullptr;
    const std::vector<Wallpapers::CScene::ShadowView>* m_shadowViews = nullptr;
    GLuint m_vbo = GL_NONE;
    GLuint m_ibo = GL_NONE;
    /** per vertex: the 4 bone indices (u32) */
    GLuint m_boneIndices = GL_NONE;
    const std::vector<glm::mat4x3>* m_bones = nullptr;
    const std::vector<float>* m_shapeWeights = nullptr;
    std::shared_ptr<CFBO> m_morphMap = nullptr;
    glm::vec4 m_morphResolution = glm::vec4 (0.0f);
    bool m_morphNormals = false;
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
    /** The scene's shadow pass: a model with castshadow draws its meshes' casters into the atlas's viewports */
    void renderShadow (const std::shared_ptr<const CFBO>& atlas, const std::vector<Wallpapers::CScene::ShadowView>& views);

    [[nodiscard]] const MeshObject& getMeshObject () const;
    /** The meshes that could be built (tests) */
    [[nodiscard]] size_t getMeshCount () const;

private:
    const MeshObject& m_object;
    Puppet::Model m_model = {};
    std::vector<MaterialUniquePtr> m_materials = {};
    std::vector<std::unique_ptr<CModelMesh>> m_meshes = {};

    /** the animation layers as they play (the puppet's clocks, weights and mixing, GH 0x1401d4d00 / 0x140228470) */
    struct LayerState {
	Puppet::Clock clock;
	bool blendIn = false;
	const ImageAnimationLayer* layer = nullptr;
    };
    /** poses the skeleton for the scene's time (once per time) */
    void updatePose ();
    std::vector<LayerState> m_layers = {};
    std::vector<Puppet::Key> m_bind = {};
    std::vector<glm::mat4> m_bindWorldInverse = {};
    /** world (pose) · bindWorld⁻¹ per bone, as the shaders' mat4x3 */
    std::vector<glm::mat4x3> m_skinning = {};
    /** per mesh, per target */
    std::vector<std::vector<float>> m_shapeWeights = {};
    bool m_animated = false;
    float m_poseTime = -1.0f;
};
} // namespace WallpaperEngine::Render::Objects
