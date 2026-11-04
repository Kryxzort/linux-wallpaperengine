#include "ShaderConstantParser.h"

#include "../Model/Project.h"
#include "WallpaperEngine/Data/Parsers/DynamicValueParser.h"
#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

ShaderConstantMap ShaderConstantParser::parse (const JSON& it, const Project& project) {
    if (!it.is_object ()) {
	return {};
    }

    ShaderConstantMap result = {};

    for (const auto& cur : it.items ()) {
	const std::string key = cur.key ();
	const DynamicValueParser::PropertyScope scope (&it, key);
	result.emplace (key, UserSettingParser::parse (cur.value (), project.properties));
    }

    return result;
}
