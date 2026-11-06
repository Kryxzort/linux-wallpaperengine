#include "JSON.h"

#include "Parsers/DynamicValueParser.h"
#include "Parsers/UserSettingParser.h"

using namespace WallpaperEngine::Data::JSON;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;

UserSettingUniquePtr JsonExtensions::user (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");
    const DynamicValueParser::PropertyScope scope (this, key);

    return UserSettingParser::parse (value, properties);
}

UserSettingUniquePtr JsonExtensions::text (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");
    const DynamicValueParser::PropertyScope scope (this, key);

    return UserSettingParser::parse (value, properties, false, true);
}

UserSettingUniquePtr JsonExtensions::color (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");
    const DynamicValueParser::PropertyScope scope (this, key);

    return UserSettingParser::parse (value, properties, true);
}

UserSettingUniquePtr JsonExtensions::colorBytes (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");
    const DynamicValueParser::PropertyScope scope (this, key);

    return UserSettingParser::parse (value, properties, true, false, true);
}
