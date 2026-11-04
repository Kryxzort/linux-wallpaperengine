#pragma once

#include <memory>
#include <vector>

#include "CRenderable.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

namespace WallpaperEngine::Render::Objects {
/**
 * A 3D scene's volumetric lights (`castvolumetrics`, light flag bit 2): WE's volumetrics pass (GH 0x1401c56e0), run
 * after the scene's objects. Per light, its volume's back faces' depth into `_rt_volumetricsSingle`
 * (materials/util/volumetrics_back.json), then its front faces — or a full-screen quad while the eye is inside it —
 * march the light (volumetrics_front / volumetrics_fullscreen, WE's volumetricsfront shader) from the front face to
 * the nearer of the back face and the scene's depth (`_rt_volumetricsBack`) into `_rt_volumetricsLightBuffer`
 * (additive, an eighth of the scene at the default medium quality); then the buffer is blurred (blur_k3 across and
 * down, through `_rt_volumetricsLightBufferB`) and added to the scene (volumetrics_combine)
 */
class CLightVolumes final : public CRenderable {
public:
    /** Null when the scene has no point or spot light that can cast volumetrics, or a material doesn't build */
    static std::unique_ptr<CLightVolumes> create (Wallpapers::CScene& scene);
    ~CLightVolumes () override;

    void setup () override;
    /** The pass, for the lights casting volumetrics now (nothing when none does) */
    void render () override;

    [[nodiscard]] const float& getBrightness () const override { return this->m_one; }
    [[nodiscard]] const float& getUserAlpha () const override { return this->m_one; }
    [[nodiscard]] const float& getAlpha () const override { return this->m_one; }
    [[nodiscard]] const glm::vec3& getColor () const override { return this->m_white; }
    [[nodiscard]] const glm::vec4& getColor4 () const override { return this->m_white4; }
    [[nodiscard]] const glm::vec3& getCompositeColor () const override { return this->m_white; }

private:
    struct Materials {
	MaterialUniquePtr back, front, fullscreen, frontPoint, fullscreenPoint, blurH, blurV, combine;
    };
    CLightVolumes (Wallpapers::CScene& scene, const Light& first, std::vector<const Light*> lights, Materials materials);

    Effects::CPass* makePass (const Material& material);
    /** a pass drawing `vbo` (positions, 3 floats) as `count` triangles' vertices */
    void meshGeometry (Effects::CPass* pass, const GLuint& vbo, const GLsizei& count);

    std::vector<const Light*> m_lights;
    Materials m_materials;
    std::vector<Effects::CPass*> m_passes = {};
    Effects::CPass* m_back = nullptr;
    Effects::CPass* m_front = nullptr;
    Effects::CPass* m_fullscreen = nullptr;
    Effects::CPass* m_frontPoint = nullptr;
    Effects::CPass* m_fullscreenPoint = nullptr;
    Effects::CPass* m_blurH = nullptr;
    Effects::CPass* m_blurV = nullptr;
    Effects::CPass* m_combine = nullptr;

    std::shared_ptr<CFBO> m_sceneDepth = nullptr;
    std::shared_ptr<CFBO> m_volumeDepth = nullptr;
    std::shared_ptr<CFBO> m_lightBuffer = nullptr;
    std::shared_ptr<CFBO> m_lightBufferB = nullptr;

    GLuint m_cone = GL_NONE;
    GLsizei m_coneCount = 0;
    GLuint m_sphere = GL_NONE;
    GLsizei m_sphereCount = 0;
    GLuint m_quad = GL_NONE;
    GLuint m_quadUV = GL_NONE;
    /** what the passes of the light being drawn read */
    GLuint m_meshVbo = GL_NONE;
    GLsizei m_meshCount = 0;

    glm::mat4 m_viewProjection = glm::mat4 (1.0f);
    glm::mat4 m_backViewProjection = glm::mat4 (1.0f);
    glm::mat4 m_inverseViewProjection = glm::mat4 (1.0f);
    glm::mat4 m_volume = glm::mat4 (1.0f);
    glm::mat4 m_lightProjection = glm::mat4 (1.0f);
    glm::mat4 m_identity = glm::mat4 (1.0f);
    glm::vec4 m_renderVar[5] = {};
    glm::vec3 m_eye = glm::vec3 (0.0f);

    float m_one = 1.0f;
    glm::vec3 m_white = glm::vec3 (1.0f);
    glm::vec4 m_white4 = glm::vec4 (1.0f);
};
} // namespace WallpaperEngine::Render::Objects
