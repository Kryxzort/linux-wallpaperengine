#include "MaterialParser.h"
#include "WallpaperEngine/Health.h"

#include "../Model/Material.h"
#include "../Model/Project.h"
#include "TextureParser.h"
#include "WallpaperEngine/Data/Parsers/ShaderConstantParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

MaterialUniquePtr MaterialParser::load (const Project& project, const std::string& filename) {
    const auto materialJson = JSON::parse (project.assetLocator->readString (filename));

    return parse (materialJson, filename, project);
}

MaterialUniquePtr MaterialParser::parse (const JSON& it, const std::string& filename, const Project& project) {
    return std::make_unique<Material> (Material {
	.filename = filename,
	.passes = parsePasses (it.require ("passes", "Material must have passes to render"), project),
    });
}

std::vector<MaterialPassUniquePtr> MaterialParser::parsePasses (const JSON& it, const Project& project) {
    std::vector<MaterialPassUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (parsePass (cur, project));
    }

    return result;
}

MaterialPassUniquePtr MaterialParser::parsePass (const JSON& it, const Project& project) {
    const auto textures = it.optional ("textures");
    const auto usertextures = it.optional ("usertextures");
    const auto combos = it.optional ("combos");
    const auto constants = it.optional ("constantshadervalues");

    const std::string blending = it.optional ("blending", std::string ("normal"));
    auto pass = std::make_unique<MaterialPass> (MaterialPass {
	.blending = parseBlendMode (blending),
	.cullmode = parseCullMode (it.optional ("cullmode", std::string ("nocull"))),
	.depthtest = parseDepthtestMode (it.optional ("depthtest", std::string ("disabled"))),
	.depthwrite = parseDepthwriteMode (it.optional ("depthwrite", std::string ("disabled"))),
	.shader = it.require<std::string> ("shader", "Material pass must have a shader"),
	.textures = textures.has_value () ? TextureParser::parseTextureMap (*textures) : TextureMap {},
	.usertextures = usertextures.has_value () ? TextureParser::parseTextureMap (*usertextures) : TextureMap {},
	.combos = combos.has_value () ? parseCombos (*combos) : ComboMap {},
	.constants = constants.has_value () ? ShaderConstantParser::parse (*constants, project) : ShaderConstantMap {},
    });

    // `alphawriting` default / enabled / disabled (GH 0x140186aa0 registers the names; an unknown one is default)
    const std::string alphawriting = it.optional ("alphawriting", std::string ("default"));
    pass->alphawriting = alphawriting == "enabled"	   ? AlphawritingMode_Enabled
			 : alphawriting == "disabled" ? AlphawritingMode_Disabled
						      : AlphawritingMode_Default;

    // WE's material pass setup (GH 0x140183570) turns the blending into combos: `alphatocoverage` (3) sets
    // ALPHATOCOVERAGE 1, `additive` (2) ADDITIVE 1. With ALPHATOCOVERAGE the stock shaders sharpen alpha and, on
    // GLSL, discard below 0.5 (generic*.frag, genericimage*.frag, genericparticle.frag): a cutout drawn without
    // blending
    if (blending == "alphatocoverage") {
	pass->combos["ALPHATOCOVERAGE"] = 1;
    } else if (blending == "additive") {
	pass->combos["ADDITIVE"] = 1;
    }

    return pass;
}

std::map<std::string, int> MaterialParser::parseCombos (const JSON& it) {
    std::map<std::string, int> result = {};

    if (!it.is_object ()) {
	return result;
    }

    for (const auto& cur : it.items ()) {
	result.emplace (cur.key (), cur.value ());
    }

    return result;
}

BlendingMode MaterialParser::parseBlendMode (const std::string& mode) {
    if (mode == "normal") {
	return BlendingMode_Normal;
    }

    if (mode == "additive") {
	return BlendingMode_Additive;
    }

    if (mode == "translucent") {
	return BlendingMode_Translucent;
    }

    // alpha to coverage: no blending, the alpha gives the coverage (the pass's ALPHATOCOVERAGE combo also sharpens
    // it, parsePass). Its own mode: a layer with effects hands it to its last pass, whose shader has no combo
    if (mode == "alphatocoverage") {
	return BlendingMode_AlphaToCoverage;
    }

    sLog.error ("Unknown blending mode: ", mode, " defaulting to normal");
    WPENGINE_HEALTH ("unsupported", { { "what", "material blending mode " + std::string (mode) + " (as normal)" } });
    return BlendingMode_Normal;
}

CullingMode MaterialParser::parseCullMode (const std::string& mode) {
    if (mode == "nocull") {
	return CullingMode_Disable;
    }

    if (mode == "normal") {
	return CullingMode_Normal;
    }

    sLog.error ("Unknown culling mode: ", mode, " defaulting to nocull");
    WPENGINE_HEALTH ("unsupported", { { "what", "material culling mode " + std::string (mode) + " (as nocull)" } });
    return CullingMode_Disable;
}

DepthtestMode MaterialParser::parseDepthtestMode (const std::string& mode) {
    if (mode == "disabled") {
	return DepthtestMode_Disabled;
    }

    if (mode == "enabled") {
	return DepthtestMode_Enabled;
    }

    sLog.error ("Unknown depthtest mode: ", mode, " defaulting to disabled");
    WPENGINE_HEALTH ("unsupported", { { "what", "material depthtest mode " + std::string (mode) + " (as disabled)" } });
    return DepthtestMode_Disabled;
}

DepthwriteMode MaterialParser::parseDepthwriteMode (const std::string& mode) {
    if (mode == "disabled") {
	return DepthwriteMode_Disabled;
    }

    if (mode == "enabled") {
	return DepthwriteMode_Enabled;
    }

    sLog.error ("Unknown depthwrite mode: ", mode, " defaulting to disabled");
    WPENGINE_HEALTH ("unsupported", { { "what", "material depthwrite mode " + std::string (mode) + " (as disabled)" } });
    return DepthwriteMode_Disabled;
}
