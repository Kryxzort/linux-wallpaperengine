#pragma once

#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Data/Model/Wallpaper.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class Camera {
public:
    Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera);
    ~Camera ();

    void setOrthogonalProjection (const float width, const float height);
    /**
     * A 3D scene (no `orthogonalprojection`): WE's perspective (GH 0x1401b3f70) of the general fov (vertical, degrees,
     * clamped to 0.1–179.9 by the camera update GH 0x1401b8af0), width / height, nearz, farz. The scene's FBO rows run
     * top-down (textures do), so clip y is flipped
     */
    void setPerspectiveProjection (const float width, const float height);
    /** A 3D scene's view this frame: lookAt (eye, center, up) in WE's world (x right, y up), the shake applied */
    void setPerspectiveView (const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up);
    /** A 3D scene's view angle this frame (vertical, degrees): general.fov, or the active camera object's */
    void setPerspectiveFov (float fov);
    [[nodiscard]] bool isPerspective () const;
    /**
     * A 3D scene's view-projection for WE's world (x right, y up, z as written): what models and 3D image layers
     * draw with (their model matrix in WE's world)
     */
    [[nodiscard]] const glm::mat4& getWorldViewProjection () const;
    /** The eye in WE's world (3D: with the shake) */
    [[nodiscard]] const glm::vec3& getWorldEye () const;
    /** The 3D view's direction in WE's world (normalised centre − eye) */
    [[nodiscard]] const glm::vec3& getWorldForward () const;
    /**
     * The eye in LWE's scene space (x − width/2, height/2 − y, z), which particles use. In a 3D scene
     * getProjection () · getLookAt () takes LWE's scene space too: the world view after the y flip and the centring
     */
    [[nodiscard]] glm::vec3 getSceneEye () const;
    /** The 2D projection of the scene's rectangle (LWE's scene space at z 0): a 3D scene's full-screen layers */
    [[nodiscard]] const glm::mat4& getScreenProjection () const;

    [[nodiscard]] const glm::vec3& getCenter () const;
    [[nodiscard]] const glm::vec3& getEye () const;
    [[nodiscard]] const glm::vec3& getUp () const;
    [[nodiscard]] const glm::mat4& getProjection () const;
    /** The view: the lookAt with the frame's adjustment (zoom, camera shake) applied in world space */
    [[nodiscard]] const glm::mat4& getLookAt () const;
    /** The view without the frame's adjustment: where a full-screen layer draws (it covers the screen whatever the
     *  zoom, shake or camera position) */
    [[nodiscard]] const glm::mat4& getUnadjustedLookAt () const;
    /** Sets the frame's world-space view adjustment (CScene::updateCamera) */
    void setViewAdjust (const glm::mat4& adjust);
    /**
     * An orthographic scene's perspective camera (WE GH 0x1401b4fd0), in LWE's 2D scene space with the frame's view
     * adjustment: `perspectiveoverridefov` (vertical), width / height, near 5, far max (15000, d + 1000), the eye on
     * the view's axis at d = (height / 2) / tan (fov / 2), so that z 0 lands where the orthographic camera puts it.
     * What `perspective` layers and perspective particles (flags bit 2) draw with
     */
    [[nodiscard]] glm::mat4 getPerspective2DViewProjection () const;
    /** That camera's eye distance d (scene units) */
    [[nodiscard]] float getPerspective2DDistance () const;
    [[nodiscard]] float getOverrideFov () const;
    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] bool isOrthogonal () const;
    [[nodiscard]] float getWidth () const;
    [[nodiscard]] float getHeight () const;
    [[nodiscard]] float getFov () const;
    [[nodiscard]] float getNearZ () const;
    [[nodiscard]] float getFarZ () const;

private:
    float m_width;
    float m_height;
    bool m_isOrthogonal = false;
    bool m_isPerspective = false;
    glm::mat4 m_screenProjection = glm::mat4 (1.0f);
    glm::mat4 m_worldViewProjection = glm::mat4 (1.0f);
    glm::mat4 m_worldProjection = glm::mat4 (1.0f);
    glm::vec3 m_worldEye = glm::vec3 (0.0f);
    glm::vec3 m_worldForward = glm::vec3 (0.0f, 0.0f, -1.0f);
    float m_perspectiveFov = -1.0f;
    glm::mat4 m_projection = {};
    glm::mat4 m_lookat = {};
    glm::mat4 m_view = {};
    glm::mat4 m_adjust = glm::mat4 (1.0f);
    const SceneData::Camera& m_camera;
    Wallpapers::CScene& m_scene;
};
} // namespace WallpaperEngine::Render
