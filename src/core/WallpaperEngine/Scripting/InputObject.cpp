#include "InputObject.h"

#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Scripting;

JSValue get_cursor_world_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));

    // the cursor in WE's world: scene pixels, x right, y up
    const auto& scene = input->getScene ();
    const glm::vec2 position = *scene.getMousePositionNormalized ();
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