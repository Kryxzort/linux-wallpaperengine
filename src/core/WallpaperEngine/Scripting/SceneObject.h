#pragma once
#include "quickjs.h"

namespace WallpaperEngine::Data::Model {
class DynamicValue;
}

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class SceneObject {
public:
    SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~SceneObject ();

    Render::Wallpapers::CScene& getScene () const { return m_scene; }
    /** the scene value a property setter writes (by the setter's magic), or nullptr (read-only) */
    Data::Model::DynamicValue* settableValue (int index) const;
    JSValue getInstance () const { return m_instance; }
    ScriptEngine& getEngine () const { return m_engine; }

private:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    JSClassID m_classId;
    JSClassDef m_definition;
    JSValue m_instance;
};
}