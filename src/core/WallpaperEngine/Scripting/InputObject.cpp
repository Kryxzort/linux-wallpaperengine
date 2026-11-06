#include "InputObject.h"

#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <glm/glm.hpp>

using namespace WallpaperEngine::Scripting;

JSValue get_cursor_world_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));

    // WE (wallpaper64 GH 0x1401bcb00, the script host's vtable +0xa8 that scenescript64's getter calls with flag 0):
    // glm-style unProject of (x, 1 − y, 0.5) through the frame's view and projection, viewport (0, 0, 1, 1), so NDC z 0.
    // Its 3D projection is DirectXMath's PerspectiveFovRH with nearz and farz swapped (GH 0x14009afd0: a reversed
    // depth), where NDC z 0 is the far plane: in a 3D scene the cursor is the point on its ray at the view depth farz
    // (3477054430's head turns toward it, 3509243656 divides it by 8000 / 9000 at farz 10000). An orthographic
    // scene's unProject is scene pixels, x right, y up, z 0 (ctx flag 0x400 zeroes z).
    const auto& scene = input->getScene ();
    const glm::vec2 position = *scene.getMousePositionNormalized ();
    const auto& camera = scene.getCamera ();
    if (camera.isPerspective ()) {
	// ours: GL clip space, far at z +1, clip y flipped (the scene FBO is read top-down)
	const glm::dmat4 inverse = glm::inverse (glm::dmat4 (camera.getWorldViewProjection ()));
	const glm::dvec4 world = inverse
	    * glm::dvec4 (2.0 * position.x - 1.0, 2.0 * position.y - 1.0, 1.0, 1.0);
	return scene.getScriptEngine ().newVec (glm::vec3 (glm::dvec3 (world) / world.w));
    }
    return scene.getScriptEngine ().newVec (glm::vec3 (
	position.x * static_cast<float> (scene.getWidth ()), (1.0f - position.y) * static_cast<float> (scene.getHeight ()),
	0.0f
    ));
}

JSValue get_cursor_screen_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));

    // "the cursor on the screen in pixels" (d.ts): in engine.screenResolution's pixels (3509243656 centres it with
    // screenResolution / 2); it was 0..1
    return input->getScene ().getScriptEngine ().newVec (input->getScene ().getCursorScreenPosition ());
}

JSValue get_cursor_left_down (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));
    return JS_NewBool (ctx, input->getScene ().getPointerState ()->x > 0.5f);
}

JSValue input_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

InputObject::InputObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0) {
    this->m_definition = { .class_name = "IInput" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cursorWorldPosition"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_world_position, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cursorScreenPosition"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_screen_position, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "cursorLeftDown"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_left_down, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
}
InputObject::~InputObject () { JS_FreeValue (this->m_engine.getContext (), this->m_instance); }