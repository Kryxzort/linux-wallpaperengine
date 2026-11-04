#pragma once

#include "ObjectAdapter.h"
#include "WallpaperEngine/Data/Model/Types.h"

#include <map>
#include <string>
#include <unordered_map>

namespace WallpaperEngine::Scripting::Adapters {
/**
 * SceneScript's ILayer (and its IImageLayer / ITextLayer / ISoundLayer faces): one JS object per layer, so
 * `getLayer ('a') === getLayer ('a')` and what a script stores on it stays. Its registered properties read and write
 * the live values; `getEffect`, `getTextureAnimation`, `getParent`, `play`, … are its methods. IEffect and
 * ITextureAnimation are classes of their own
 */
class ScriptableObjectAdapter final : public ObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine, std::string name);
    ~ScriptableObjectAdapter () override;

    /** The layer's one JS object (a new reference) */
    JSValue instantiate (ScriptableObject& object) override;
    JSValue instantiate (Data::Model::DynamicValue& value) override;

    /** An IEffect for one effect of an image layer (a new object) */
    [[nodiscard]] JSValue effect (const Data::Model::ImageEffect& effect);

    /** The layer behind a JS value this adapter made, or nullptr */
    [[nodiscard]] ScriptableObject* fromValue (JSValueConst value) const;

    /** A method of ILayer by name, or JS_UNDEFINED (a borrowed reference) */
    [[nodiscard]] JSValueConst method (const char* name) const;

    /** An ILayer method written in JS (builtins.js `__layerMethods`) by name, or JS_UNDEFINED (a new reference) */
    [[nodiscard]] JSValue jsMethod (const char* name);

    [[nodiscard]] JSClassID getEffectClassId () const { return m_effectClassId; }
    [[nodiscard]] JSClassID getTextureAnimationClassId () const { return m_textureAnimationClassId; }
    [[nodiscard]] JSClassID getAnimationClassId () const { return m_animationClassId; }

private:
    JSClassExoticMethods m_exoticMethods;
    JSClassExoticMethods m_effectExoticMethods;
    JSClassExoticMethods m_textureAnimationExoticMethods;
    JSClassDef m_effectDefinition;
    JSClassDef m_textureAnimationDefinition;
    JSClassID m_effectClassId = 0;
    JSClassID m_textureAnimationClassId = 0;
    JSClassExoticMethods m_animationExoticMethods {};
    JSClassDef m_animationDefinition {};
    JSClassID m_animationClassId = 0;
    std::string m_name;
    std::unordered_map<ScriptableObject*, JSValue> m_instances;
    std::map<std::string, JSValue> m_methods;
    /** builtins.js `__layerMethods`, once installed */
    JSValue m_jsMethods = JS_UNDEFINED;
};
} // namespace WallpaperEngine::Scripting::Adapters
