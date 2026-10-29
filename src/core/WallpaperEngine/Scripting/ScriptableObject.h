#pragma once
#include "../Data/Model/Types.h"
#include "WallpaperEngine/Render/CObject.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Scripting {
class ScriptableObject : virtual public CObject {
public:
    struct PropertyEntry {
	std::string key;
	DynamicValue& value;
    };

    ScriptableObject (Wallpapers::CScene& scene, const Object& object);
    virtual ~ScriptableObject () = default;

    DynamicValue& getProperty (const std::string& name);

    const std::map<std::string, PropertyEntry>& getProperties () const;

protected:
    /**
     * Makes `name` readable by scripts (thisLayer, getLayer). Its property script, if any, is queued unless
     * `runScript` is false (an object that runs that script itself)
     */
    void registerProperty (const std::string& name, DynamicValue& value, bool runScript = true);

private:
    std::map<std::string, PropertyEntry> m_properties;
};
}