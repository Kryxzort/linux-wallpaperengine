#include "CLightVolumes.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Health.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;

namespace {
constexpr int kSegments = 32; // WE's cone: 32 steps of 2π / 32 (GH 0x140282d40)
constexpr int kStacks = 16;
// the default `volumetrics` quality, medium (GH 0x140048ca0's default "medium" = 2): the light buffer is an eighth
// of the scene (GH 0x1401c56e0: a divisor of 8 up to medium, 4 above) and is blurred
constexpr int kQuality = 2;
constexpr int kDivisor = 8;

bool castsVolumetrics (const Light& light) {
    return (light.kind == LightData::Kind::Point || light.kind == LightData::Kind::Spot) && light.castVolumetrics != nullptr
	&& (light.castVolumetrics->value->getBool () || light.castVolumetrics->property != nullptr
	    || light.castVolumetrics->value->getScriptSource ().has_value ());
}

/** GL's clip z (−w … w) to D3D's (0 … w): the depth WE's volumetrics shader compares (a GL depth texture's value) */
glm::mat4 d3dDepth () {
    glm::mat4 m (1.0f);
    m[2][2] = 0.5f;
    m[3][2] = 0.5f;
    return m;
}

void push (std::vector<float>& out, const glm::vec3& v) {
    out.push_back (v.x);
    out.push_back (v.y);
    out.push_back (v.z);
}

/**
 * The spot's volume in its own clip space: a cylinder of radius 1 (the circle inside the frustum's square is the outer
 * cone) from the near plane (z −1) to the far one (+1), capped; wound so that, through inverse (light VP) and the
 * camera, its outside faces the eye (checked by the depth its back faces leave)
 */
std::vector<float> coneMesh () {
    std::vector<float> out;
    for (int i = 0; i < kSegments; i++) {
	const float a = 6.2831855f * static_cast<float> (i) / kSegments;
	const float b = 6.2831855f * static_cast<float> (i + 1) / kSegments;
	const glm::vec3 na (std::sin (a), -std::cos (a), -1.0f), nb (std::sin (b), -std::cos (b), -1.0f);
	const glm::vec3 fa (na.x, na.y, 1.0f), fb (nb.x, nb.y, 1.0f);
	// the side, the near cap, the far cap
	for (const glm::vec3& v : { na, fb, fa, na, nb, fb }) {
	    push (out, v);
	}
	for (const glm::vec3& v : { glm::vec3 (0.0f, 0.0f, -1.0f), nb, na }) {
	    push (out, v);
	}
	for (const glm::vec3& v : { glm::vec3 (0.0f, 0.0f, 1.0f), fa, fb }) {
	    push (out, v);
	}
    }
    return out;
}

/** A unit sphere (circumscribed: its facets reach the radius), its outside counter-clockwise */
std::vector<float> sphereMesh () {
    std::vector<float> out;
    const float grow = 1.0f / std::cos (3.1415927f / kSegments) / std::cos (3.1415927f / (2 * kStacks));
    const auto at = [grow] (int stack, int slice) {
	const float theta = 3.1415927f * static_cast<float> (stack) / kStacks;
	const float phi = 6.2831855f * static_cast<float> (slice) / kSegments;
	return glm::vec3 (std::sin (theta) * std::cos (phi), std::cos (theta), std::sin (theta) * std::sin (phi)) * grow;
    };
    for (int s = 0; s < kStacks; s++) {
	for (int i = 0; i < kSegments; i++) {
	    const glm::vec3 a = at (s, i), b = at (s, i + 1), c = at (s + 1, i), d = at (s + 1, i + 1);
	    for (const glm::vec3& v : { a, d, b, a, c, d }) {
		push (out, v);
	    }
	}
    }
    return out;
}
} // namespace

std::unique_ptr<CLightVolumes> CLightVolumes::create (Wallpapers::CScene& scene) {
    std::vector<const Light*> lights;
    for (const auto& object : scene.getScene ().objects) {
	if (object->is<Light> () && castsVolumetrics (*object->as<Light> ())) {
	    lights.push_back (object->as<Light> ());
	}
    }
    if (lights.empty () || !scene.getCamera ().isPerspective ()) {
	return nullptr;
    }

    const auto& project = scene.getScene ().project;
    Materials materials;
    try {
	materials.back = MaterialParser::load (project, "materials/util/volumetrics_back.json");
	materials.front = MaterialParser::load (project, "materials/util/volumetrics_front.json");
	materials.fullscreen = MaterialParser::load (project, "materials/util/volumetrics_fullscreen.json");
	materials.frontPoint = MaterialParser::load (project, "materials/util/volumetrics_front.json");
	materials.fullscreenPoint = MaterialParser::load (project, "materials/util/volumetrics_fullscreen.json");
	materials.blurH = MaterialParser::load (project, "materials/util/volumetrics_blur_h.json");
	materials.blurV = MaterialParser::load (project, "materials/util/volumetrics_blur_v.json");
	materials.combine = MaterialParser::load (project, "materials/util/volumetrics_combine.json");
    } catch (const std::exception& e) {
	sLog.error ("Cannot load the volumetrics materials, the lights draw without volumetrics: ", e.what ());
	WPENGINE_HEALTH ("effect_dropped", { { "object", lights.front ()->id }, { "object_name", "volumetrics" }, { "reason", WallpaperEngine::Health::brief (e.what ()) }, { "takes_layer", false } });
	return nullptr;
    }

    // the back faces' depth: WE's material leaves depth to the defaults (on); LWE's parser defaults it off
    for (auto& pass : materials.back->passes) {
	pass->depthtest = DepthtestMode_Enabled;
	pass->depthwrite = DepthwriteMode_Enabled;
    }
    // the front pass's combos (GH 0x1401c56e0): QUALITY (the volumetrics quality), POINTLIGHT for a point light, SHADOW
    // with a shadow (not done here), COOKIE with usecookie (not done)
    for (auto* material : { materials.front.get (), materials.fullscreen.get (), materials.frontPoint.get (), materials.fullscreenPoint.get () }) {
	for (auto& pass : material->passes) {
	    pass->combos.insert_or_assign ("QUALITY", kQuality);
	    pass->combos.insert_or_assign ("SHADOW", 0);
	    pass->combos.insert_or_assign ("FOG", 0);
	}
    }
    for (auto* material : { materials.frontPoint.get (), materials.fullscreenPoint.get () }) {
	for (auto& pass : material->passes) {
	    pass->combos.insert_or_assign ("POINTLIGHT", 1);
	}
    }

    const Light& first = *lights.front ();
    try {
	auto volumes = std::unique_ptr<CLightVolumes> (new CLightVolumes (scene, first, std::move (lights), std::move (materials)));
	volumes->setup ();
	return volumes;
    } catch (const std::exception& e) {
	sLog.error ("Cannot build the volumetrics passes, the lights draw without volumetrics: ", e.what ());
	WPENGINE_HEALTH ("effect_dropped", { { "object", first.id }, { "object_name", "volumetrics" }, { "reason", WallpaperEngine::Health::brief (e.what ()) }, { "takes_layer", false } });
	return nullptr;
    }
}

CLightVolumes::CLightVolumes (
    Wallpapers::CScene& scene, const Light& first, std::vector<const Light*> lights, Materials materials
) :
    CObject (scene, first), CRenderable (scene, first, *materials.back), m_lights (std::move (lights)),
    m_materials (std::move (materials)) { }

CLightVolumes::~CLightVolumes () {
    for (auto* pass : this->m_passes) {
	delete pass;
    }
    for (GLuint* buffer : { &this->m_cone, &this->m_sphere, &this->m_quad, &this->m_quadUV }) {
	if (*buffer != GL_NONE) {
	    glDeleteBuffers (1, buffer);
	}
    }
}

CPass* CLightVolumes::makePass (const Material& material) {
    auto* pass = new CPass (*this, std::make_shared<FBOProvider> (this), *material.passes.front (), std::nullopt, std::nullopt, std::nullopt);
    this->m_passes.push_back (pass);
    pass->setModelMatrix (&this->m_inverseViewProjection);
    pass->setViewProjectionMatrix (&this->m_viewProjection);
    pass->setModelViewProjectionMatrix (&this->m_identity);
    pass->setModelViewProjectionMatrixInverse (&this->m_identity);
    pass->addUniform ("g_AltViewProjectionMatrix", &this->m_volume);
    pass->addUniform ("g_AltModelMatrix", &this->m_lightProjection);
    pass->addUniform ("g_RenderVar0", &this->m_renderVar[0]);
    pass->addUniform ("g_RenderVar1", &this->m_renderVar[1]);
    pass->addUniform ("g_RenderVar2", &this->m_renderVar[2]);
    pass->addUniform ("g_RenderVar3", &this->m_renderVar[3]);
    pass->addUniform ("g_RenderVar4", &this->m_renderVar[4]);
    pass->addUniform ("g_EyePosition", &this->m_eye);
    pass->setPosition (this->m_quad);
    pass->setTexCoord (this->m_quadUV);
    return pass;
}

void CLightVolumes::meshGeometry (CPass* pass, const GLuint& vbo, const GLsizei& count) {
    const GLuint program = pass->getProgramID ();
    const GLint position = glGetAttribLocation (program, "a_Position");
    pass->setGeometryCallback (
	[position, &vbo] () {
	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, vbo);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	},
	[&count] () { glDrawArrays (GL_TRIANGLES, 0, count); },
	[position] () {
	    if (position >= 0) {
		glDisableVertexAttribArray (position);
	    }
	}
    );
}

void CLightVolumes::setup () {
    auto& scene = this->getScene ();
    const auto width = static_cast<float> (scene.getFBO ()->getRealWidth ());
    const auto height = static_cast<float> (scene.getFBO ()->getRealHeight ());
    const glm::vec2 small (std::max (1.0f, std::floor (width / kDivisor)), std::max (1.0f, std::floor (height / kDivisor)));

    // WE's targets (GH 0x1401c56e0): the scene's depth, the volume's back faces' depth, the light buffer and its blur
    // partner. Ours: half float light buffers; the depth copies are textures LWE blits / draws into
    this->m_sceneDepth = scene.create ("_rt_volumetricsBack", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { width, height }, { width, height });
    this->m_sceneDepth->makeDepthTexture (true);
    this->m_volumeDepth = scene.create ("_rt_volumetricsSingle", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, small, small);
    this->m_volumeDepth->makeDepthTexture (false);
    this->m_lightBuffer = scene.create ("_rt_volumetricsLightBuffer", TextureFormat_RGBA16161616f, TextureFlags_ClampUVs, 1.0, small, small);
    this->m_lightBufferB = scene.create ("_rt_volumetricsLightBufferB", TextureFormat_RGBA16161616f, TextureFlags_ClampUVs, 1.0, small, small);
    this->m_texture = this->m_lightBuffer;

    const auto upload = [] (GLuint& vbo, const std::vector<float>& data) {
	glGenBuffers (1, &vbo);
	glBindBuffer (GL_ARRAY_BUFFER, vbo);
	glBufferData (GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (data.size () * sizeof (float)), data.data (), GL_STATIC_DRAW);
    };
    const auto cone = coneMesh ();
    const auto sphere = sphereMesh ();
    upload (this->m_cone, cone);
    upload (this->m_sphere, sphere);
    this->m_coneCount = static_cast<GLsizei> (cone.size () / 3);
    this->m_sphereCount = static_cast<GLsizei> (sphere.size () / 3);
    // the full-screen quad in clip space; its uv (0, 0) at the bottom left, as the buffers are drawn
    upload (this->m_quad, { -1, -1, 0, -1, 1, 0, 1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0 });
    upload (this->m_quadUV, { 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1 });

    CRenderable::setup ();

    this->m_back = this->makePass (*this->m_materials.back);
    this->m_front = this->makePass (*this->m_materials.front);
    this->m_frontPoint = this->makePass (*this->m_materials.frontPoint);
    this->m_fullscreen = this->makePass (*this->m_materials.fullscreen);
    this->m_fullscreenPoint = this->makePass (*this->m_materials.fullscreenPoint);
    this->m_blurH = this->makePass (*this->m_materials.blurH);
    this->m_blurV = this->makePass (*this->m_materials.blurV);
    this->m_combine = this->makePass (*this->m_materials.combine);

    // the back pass draws with the y-flipped view-projection: its back faces end up front-facing for the material's
    // culling, and its depth lands where the front pass's D3D-style screen uv (y down) reads it
    this->m_back->setViewProjectionMatrix (&this->m_backViewProjection);
    for (auto* pass : { this->m_back, this->m_front, this->m_frontPoint }) {
	this->meshGeometry (pass, this->m_meshVbo, this->m_meshCount);
    }
    for (auto* pass : { this->m_front, this->m_frontPoint, this->m_fullscreen, this->m_fullscreenPoint }) {
	pass->setTexture (1, this->m_sceneDepth);
	pass->setTexture (3, this->m_volumeDepth);
	pass->setInput (this->m_sceneDepth);
	pass->setDestination (this->m_lightBuffer);
    }
    this->m_back->setInput (this->m_lightBuffer);
    this->m_back->setDestination (this->m_volumeDepth);
    this->m_blurH->setInput (this->m_lightBuffer);
    this->m_blurH->setDestination (this->m_lightBufferB);
    this->m_blurV->setInput (this->m_lightBufferB);
    this->m_blurV->setDestination (this->m_lightBuffer);
    this->m_combine->setInput (this->m_lightBuffer);
    this->m_combine->setDestination (scene.getFBO ());
}

void CLightVolumes::render () {
    auto& scene = this->getScene ();
    const auto& camera = scene.getCamera ();

    std::vector<const Light*> lit;
    for (const auto* light : this->m_lights) {
	if (light->castVolumetrics->value->getBool () && scene.isEffectivelyVisible (*light)) {
	    lit.push_back (light);
	}
    }
    if (lit.empty ()) {
	return;
    }

    GLint previous = 0;
    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previous);
    GLfloat clear[4] = {};
    glGetFloatv (GL_COLOR_CLEAR_VALUE, clear);

    const auto& source = *scene.getFBO ();
    const auto W = static_cast<GLint> (source.getRealWidth ()), H = static_cast<GLint> (source.getRealHeight ());
    // the scene's depth, y flipped (the shader's screen uv grows down)
    glBindFramebuffer (GL_READ_FRAMEBUFFER, source.getFramebuffer ());
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, this->m_sceneDepth->getFramebuffer ());
    glBlitFramebuffer (0, 0, W, H, 0, H, W, 0, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer (GL_FRAMEBUFFER, this->m_lightBuffer->getFramebuffer ());
    glColorMask (true, true, true, true);
    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    const glm::mat4 viewProjection = camera.getWorldViewProjection ();
    this->m_viewProjection = d3dDepth () * viewProjection;
    this->m_inverseViewProjection = glm::inverse (this->m_viewProjection);
    this->m_backViewProjection = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)) * viewProjection;
    this->m_eye = camera.getWorldEye ();
    // the eye's point the inside test takes: 0.2 along the view (GH 0x1401c56e0: eye + forward · 0.2)
    const glm::mat4 inverse = glm::inverse (viewProjection);
    const glm::vec4 nearAt = inverse * glm::vec4 (0.0f, 0.0f, -1.0f, 1.0f);
    const glm::vec4 farAt = inverse * glm::vec4 (0.0f, 0.0f, 1.0f, 1.0f);
    const glm::vec3 forward = glm::normalize (glm::vec3 (farAt) / farAt.w - glm::vec3 (nearAt) / nearAt.w);
    const glm::vec3 probe = this->m_eye + forward * 0.2f;

    for (const auto* light : lit) {
	const glm::mat4 world = scene.worldMatrixOf (*light);
	const glm::vec3 origin (world[3]);
	const bool point = light->kind == LightData::Kind::Point;
	const float radius = std::max (light->radius, 0.06f);
	bool inside;

	if (point) {
	    this->m_volume = glm::translate (glm::mat4 (1.0f), origin) * glm::scale (glm::mat4 (1.0f), glm::vec3 (radius));
	    this->m_lightProjection = glm::mat4 (1.0f);
	    this->m_renderVar[3] = glm::vec4 (0.0f);
	    this->m_meshVbo = this->m_sphere;
	    this->m_meshCount = this->m_sphereCount;
	    inside = glm::length (probe - origin) < radius;
	} else {
	    // the spot's projection (as F1's: looking down its x axis, y up, fov 2 · outercone, near 0.05, far = radius),
	    // GL's here: the cone mesh is in its clip space
	    const glm::vec3 x = glm::length (glm::vec3 (world[0])) > 0.0f ? glm::normalize (glm::vec3 (world[0])) : glm::vec3 (1.0f, 0.0f, 0.0f);
	    const glm::vec3 y = glm::length (glm::vec3 (world[1])) > 0.0f ? glm::normalize (glm::vec3 (world[1])) : glm::vec3 (0.0f, 1.0f, 0.0f);
	    const float fov = 2.0f * glm::radians (std::clamp (light->outerCone, 0.5f, 89.5f));
	    this->m_lightProjection = glm::perspective (fov, 1.0f, 0.05f, std::max (radius, 0.06f)) * glm::lookAt (origin, origin + x, y);
	    this->m_volume = glm::inverse (this->m_lightProjection);
	    this->m_renderVar[3] = glm::vec4 (x, 0.0f);
	    this->m_meshVbo = this->m_cone;
	    this->m_meshCount = this->m_coneCount;
	    const glm::vec4 clip = this->m_lightProjection * glm::vec4 (probe, 1.0f);
	    inside = clip.w > 0.0f && std::abs (clip.z) < clip.w && glm::length (glm::vec2 (clip) / clip.w) < 1.0f;
	}
	// GH 0x1401c56e0's packing: RenderVar1 = (radius · 0.99, cos inner, cos outer, intensity), 2 = (origin, density),
	// 3 = the spot's forward (its x axis), 4 = (colour · scattering, volumetricsexponent). RenderVar0 (the shadow
	// map's transform) only matters with SHADOW
	this->m_renderVar[0] = glm::vec4 (0.0f);
	this->m_renderVar[1] = glm::vec4 (
	    radius * 0.99f, std::cos (glm::radians (light->innerCone)), std::cos (glm::radians (light->outerCone)),
	    light->intensity->value->getFloat ()
	);
	this->m_renderVar[2] = glm::vec4 (origin, light->density);
	this->m_renderVar[4] = glm::vec4 (light->color->value->getVec3 () * light->scattering, light->volumetricsExponent);

	// the back faces' depth (cleared to the far plane)
	glBindFramebuffer (GL_FRAMEBUFFER, this->m_volumeDepth->getFramebuffer ());
	glDepthMask (GL_TRUE);
	glClearDepth (1.0);
	glClear (GL_DEPTH_BUFFER_BIT);
	this->m_back->render ();
	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_FALSE);

	// the march: its front faces, or the whole buffer from inside (GH 0x1401c56e0 picks volumetrics_fullscreen)
	if (inside) {
	    (point ? this->m_fullscreenPoint : this->m_fullscreen)->render ();
	} else {
	    (point ? this->m_frontPoint : this->m_front)->render ();
	}
    }
    glDisable (GL_CULL_FACE);

    // the blur (medium quality: blur_k3 across, then down) and the buffer added to the scene
    this->m_blurH->render ();
    this->m_blurV->render ();
    this->m_combine->render ();

    glClearColor (clear[0], clear[1], clear[2], clear[3]);
    glBindFramebuffer (GL_FRAMEBUFFER, previous);
}
