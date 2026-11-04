#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera) :
    m_width (0), m_height (0), m_camera (camera), m_scene (scene) {
    // get the lookat position
    // TODO: ENSURE THIS IS ONLY USED WHEN NOT DOING AN ORTOGRAPHIC CAMERA AS IT THROWS OFF POINTS
    this->m_lookat = glm::lookAt (this->getEye (), this->getCenter (), this->getUp ());
    this->m_view = this->m_lookat;
}

Camera::~Camera () = default;

const glm::vec3& Camera::getCenter () const { return this->m_camera.configuration.center; }

const glm::vec3& Camera::getEye () const { return this->m_camera.configuration.eye; }

const glm::vec3& Camera::getUp () const { return this->m_camera.configuration.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_view; }

void Camera::setViewAdjust (const glm::mat4& adjust) {
    this->m_adjust = adjust;
    this->m_view = this->m_lookat * adjust;
}

float Camera::getOverrideFov () const {
    const auto& fov = this->m_camera.projection.overrideFov;
    return glm::clamp (fov == nullptr ? 95.0f : fov->value->getFloat (), 0.1f, 179.9f);
}

float Camera::getPerspective2DDistance () const {
    // GH 0x1401b4fd0: view.z = −1 / (tan (fov / 2) · proj[1][1]), the orthographic projection's m11 being 2 / height
    return (this->m_height / 2.0f) / std::tan (glm::radians (this->getOverrideFov ()) / 2.0f);
}

glm::mat4 Camera::getPerspective2DViewProjection () const {
    const float d = this->getPerspective2DDistance ();
    // near 5 (DAT_1404d43e0), far max (15000, d + 1000) (DAT_1404d44ec, DAT_1404d44d4). WE keeps the orthographic
    // view's x/y and centres the axis on the visible area (view.xy −= size · crop fraction); LWE's 2D scene space is
    // centred already, and its orthographic projection is translated by the eye, so the eye's x/y go the same way
    const glm::mat4 projection = glm::perspective (
	glm::radians (this->getOverrideFov ()), this->m_width / this->m_height, 5.0f, std::max (15000.0f, d + 1000.0f)
    );
    const glm::vec3 eye = this->getEye ();
    return projection * glm::translate (glm::mat4 (1.0f), glm::vec3 (eye.x, eye.y, -d)) * this->m_adjust;
}

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getFov () const { return this->m_camera.projection.fov->value->getFloat (); }

bool Camera::isPerspective () const { return this->m_isPerspective; }

const glm::mat4& Camera::getWorldViewProjection () const { return this->m_worldViewProjection; }

const glm::vec3& Camera::getWorldEye () const { return this->m_worldEye; }

glm::vec3 Camera::getSceneEye () const {
    return { this->m_worldEye.x - this->m_width / 2.0f, this->m_height / 2.0f - this->m_worldEye.y, this->m_worldEye.z };
}

const glm::mat4& Camera::getScreenProjection () const { return this->m_screenProjection; }

void Camera::setPerspectiveProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;
    this->m_isOrthogonal = false;
    this->m_isPerspective = true;

    this->setPerspectiveFov (this->getFov ());
    // full-screen layers keep the 2D layout: LWE's scene space at z 0 (x centred, y down) over the whole target
    this->m_screenProjection = glm::ortho<float> (-width / 2.0f, width / 2.0f, -height / 2.0f, height / 2.0f, -1.0f, 1.0f);
    this->setPerspectiveView (this->getEye (), this->getCenter (), this->getUp ());
}

void Camera::setPerspectiveFov (float fov) {
    fov = glm::clamp (fov, 0.1f, 179.9f);

    if (fov == this->m_perspectiveFov) {
	return;
    }

    this->m_perspectiveFov = fov;
    const float nearz = std::max (this->getNearZ (), 1e-4f);
    const float farz = std::max (this->getFarZ (), nearz * 2.0f);

    // the scene FBO is read top-down (row 0 is the top, as a texture's): flip clip y
    this->m_worldProjection = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	* glm::perspective (glm::radians (fov), this->m_width / this->m_height, nearz, farz);
    this->m_projection = this->m_worldProjection;
}

void Camera::setPerspectiveView (const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up) {
    const glm::mat4 view = glm::lookAt (eye, center, up);

    this->m_worldEye = eye;
    this->m_worldViewProjection = this->m_worldProjection * view;
    // LWE's scene space (x − w/2, h/2 − y, z) back to WE's world, then the view: what getProjection () · getLookAt ()
    // gives the objects that place themselves in scene space (particles)
    const glm::mat4 fromScene = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f))
	* glm::translate (glm::mat4 (1.0f), glm::vec3 (this->m_width / 2.0f, -this->m_height / 2.0f, 0.0f));
    this->m_lookat = view * fromScene;
    this->m_view = this->m_lookat;
}

float Camera::getNearZ () const { return this->m_camera.projection.nearz->value->getFloat (); }

float Camera::getFarZ () const { return this->m_camera.projection.farz->value->getFloat (); }

void Camera::setOrthogonalProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;

    // the 2D projection keeps the depth range it always had: general.nearz (0.1 by default) would clip the layers,
    // which sit at z 0 (nearz/farz/fov are for perspective use, getNearZ/getFarZ/getFov)
    constexpr float nearz = 0.0f;
    constexpr float farz = 1000.0f;

    this->m_projection = glm::ortho<float> (-width / 2.0, width / 2.0, -height / 2.0, height / 2.0, nearz, farz);
    this->m_projection = glm::translate (this->m_projection, this->getEye ());
    this->m_screenProjection = this->m_projection;
    this->m_isOrthogonal = true;
}