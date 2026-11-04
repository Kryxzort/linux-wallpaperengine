#include "ShaderUnit.h"

#include "WallpaperEngine/Health.h"
#include "WallpaperEngine/Logging/Log.h"
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <stack>
#include <string>
#include <utility>

#include "GLSLContext.h"
#include "WEShaderRules.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableFloat.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableInteger.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector2.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector3.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector4.h"

#include "WallpaperEngine/Data/Builders/VectorBuilder.h"
#include "WallpaperEngine/FileSystem/Container.h"

#define SHADER_HEADER(filename)                                                                                        \
    "#version 330\n"                                                                                                   \
    "// ======================================================\n"                                                      \
    "// Processed shader "                                                                                             \
	+ filename                                                                                                     \
	+ "\n"                                                                                                         \
	  "// ======================================================\n"                                                \
	  "precision highp float;\n"                                                                                   \
	  "#define mul(x, y) ((y) * (x))\n"                                                                            \
	  "#define max(x, y) max (y, x)\n"                                                                             \
	  "#define lerp mix\n"                                                                                         \
	  "#define frac fract\n"                                                                                       \
	  "#define CAST2(x) (vec2(x))\n"                                                                               \
	  "#define CAST3(x) (vec3(x))\n"                                                                               \
	  "#define CAST4(x) (vec4(x))\n"                                                                               \
	  "#define CAST3X3(x) (mat3(x))\n"                                                                             \
	  WE_HEADER_DEFINES                                                                                            \
	  "#define float2 vec2\n"                                                                                      \
	  "#define float3 vec3\n"                                                                                      \
	  "#define float4 vec4\n"                                                                                      \
	  "#define int2 ivec2\n"                                                                                       \
	  "#define int3 ivec3\n"                                                                                       \
	  "#define int4 ivec4\n"                                                                                       \
	  "#define saturate(x) (clamp(x, 0.0, 1.0))\n"                                                                 \
	  "#define texSample2D texture\n"                                                                              \
	  "#define texSample2DLod textureLod\n"                                                                        \
	  "#define log10(x) (log2(x) * 0.301029995663981)\n"                                                           \
	  "#define atan2 atan\n"                                                                                       \
	  "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"                                                              \
	  "#define ddx dFdx\n"                                                                                         \
	  "#define ddy(x) dFdy(-(x))\n"                                                                                \
	  "#define GLSL 1\n\n";
// the rest of WE's translator header (GH 0x140125e80's #define block) — CASTF/CASTI/CASTU/CAST4U, the sampler
// parameter macros, texLoad2D / texSample2DCompare / texSample3D, the frame buffer mappings — and the HLSL type names
// WE's GLSL dialect lets a shader use. MapNDCToFrameBuffer has WE's y flip undone (D3D's texture rows go down;
// inferred), sampler2DComparison is a shadow sampler, sampler2DBackBuffer a plain one (GH 0x140125e80 picks
// texSample2DBackBuffer per backbuffer kind)
#define WE_HEADER_DEFINES                                                                                              \
    "#define CASTF(x) (float(x))\n"                                                                                    \
    "#define CASTI(x) (int(x))\n"                                                                                      \
    "#define CASTU(x) (uint(x))\n"                                                                                     \
    "#define CAST4U(x) (uvec4(x))\n"                                                                                   \
    "#define DECLARE_SAMPLER2D_PARAMETER(t) sampler2D t\n"                                                             \
    "#define MAKE_SAMPLER2D_ARGUMENT(t) t\n"                                                                           \
    "#define DECLARE_SAMPLER2D_COMPARE_PARAMETER(t) sampler2DShadow t\n"                                               \
    "#define MAKE_SAMPLER2D_COMPARE_ARGUMENT(t) t\n"                                                                   \
    "#define texSample2DCompare(s, u, d) vec4(texture(s, vec3((u), (d))))\n"                                           \
    "#define texLoad2D(s, u, r) texelFetch(s, ivec2((u) * (r)), 0)\n"                                                  \
    "#define texSample3D texture\n"                                                                                    \
    "#define texSample2DBackBuffer(s, u, r) texture(s, (u))\n"                                                         \
    "#define MapTexCoordToFrameBuffer(u) (u)\n"                                                                        \
    "#define MapNDCToFrameBuffer(n) ((n) * 0.5 + 0.5)\n"                                                               \
    "#define MapEffectSpaceToFrameBuffer MapNDCToFrameBuffer\n"                                                        \
    "#define sampler2DComparison sampler2DShadow\n"                                                                    \
    "#define sampler2DBackBuffer sampler2D\n"                                                                          \
    "#define half float\n"                                                                                             \
    "#define half2 vec2\n"                                                                                             \
    "#define half3 vec3\n"                                                                                             \
    "#define half4 vec4\n"                                                                                             \
    "#define uint2 uvec2\n"                                                                                            \
    "#define uint3 uvec3\n"                                                                                            \
    "#define uint4 uvec4\n"                                                                                            \
    "#define bool2 bvec2\n"                                                                                            \
    "#define bool3 bvec3\n"                                                                                            \
    "#define bool4 bvec4\n"                                                                                            \
    "#define float2x2 mat2\n"                                                                                          \
    "#define float3x3 mat3\n"                                                                                          \
    "#define float4x4 mat4\n"                                                                                          \
    "#define float4x3 mat4x3\n"
#define FRAGMENT_SHADER_DEFINES                                                                                        \
    "out vec4 out_FragColor;\n"                                                                                        \
    "#define varying in\n"
#define VERTEX_SHADER_DEFINES                                                                                          \
    "#define attribute in\n"                                                                                           \
    "#define varying out\n"
#define DEFINE_COMBO(name, value) "#define " + name + " " + std::to_string (value) + "\n";

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Render::Shaders;

ShaderUnit::ShaderUnit (
    const GLSLContext::UnitType type, std::string file, std::string content, const AssetLocator& assetLocator,
    const ShaderConstantMap& constants, const TextureMap& passTextures, const TextureMap& overrideTextures,
    const ComboMap& combos, const ComboMap& overrideCombos, TextureFormatResolver formatOf
) :
    m_type (type), m_file (std::move (file)), m_content (std::move (content)), m_combos (combos),
    m_overrideCombos (overrideCombos), m_constants (constants), m_passTextures (passTextures),
    m_overrideTextures (overrideTextures), m_formatOf (std::move (formatOf)), m_link (nullptr),
    m_assetLocator (assetLocator) {
    // pre-process the shader so the units are clear
    this->preprocess ();
}

void ShaderUnit::preprocess () {
    this->m_preprocessed = this->m_content;

    this->preprocessIncludes ();
    this->preprocessRequires ();
    // GH 0x140125e80: declarations are WE's cbuffers / structs, ahead of all code (the includes pasted in place too)
    this->m_preprocessed = WERules::hoistDeclarations (this->m_preprocessed);
    this->preprocessVariables ();

    // replace gl_FragColor with the equivalent
    const std::string from = "gl_FragColor";
    const std::string to = "out_FragColor";

    size_t start_pos = 0;
    while ((start_pos = this->m_preprocessed.find (from, start_pos)) != std::string::npos) {
	this->m_preprocessed.replace (start_pos, from.length (), to);
	start_pos += to.length (); // Handles case where 'to' is a substring of 'from'
    }
}

void ShaderUnit::preprocessVariables () {
    size_t start = 0, end = 0;
    while ((end = this->m_preprocessed.find ('\n', start)) != std::string::npos) {
	// Extract a line from the string
	std::string line = this->m_preprocessed.substr (start, end - start);
	const size_t combo = line.find ("// [COMBO] ");
	const size_t uniform = line.find ("uniform ");
	const size_t comment = line.find ("// ");
	const size_t semicolon = line.find (';');

	if (combo != std::string::npos) {
	    this->parseComboConfiguration (line.substr (combo + strlen ("// [COMBO] ")), 0);
	} else if (
	    uniform != std::string::npos && comment != std::string::npos && semicolon != std::string::npos &&
	    // this check ensures that the comment is after the semicolon (so it's not a commented-out line)
	    // this needs further refining as it's not taking into account block comments
	    semicolon < comment
	) {
	    // uniforms with comments should never have a value assigned, use this fact to detect the required parts
	    const size_t last_space = line.find_last_of (' ', semicolon);

	    if (last_space != std::string::npos) {
		const size_t previous_space = line.find_last_of (' ', last_space - 1);

		if (previous_space != std::string::npos) {
		    // extract type and name
		    std::string type = line.substr (previous_space + 1, last_space - previous_space - 1);
		    std::string name = line.substr (last_space + 1, semicolon - last_space - 1);
		    std::string json = line.substr (comment + 2);

		    this->parseParameterConfiguration (type, name, json);
		}
	    }
	}

	// Move to the next line
	start = end + 1;
    }
}

void ShaderUnit::preprocessIncludes () {
    // WE's rule (GH 0x140191050 / 0x1401917f0): each include in place, where its line was — LWE pasted them all
    // before main, after the unit's last declaration, so a function above main using an include's #define
    // (3235948233's auto_sway: M_PI from common.h) didn't compile. hoistDeclarations keeps what that order gave: an
    // include's code seeing the unit's uniforms (common_blur.h samples g_Texture0, declared below its #include)
    std::set<std::string> seen;
    const auto load = [this] (const std::string& name) -> std::optional<std::string> {
	try {
	    return std::filesystem::path (name).extension () == ".h"
		? this->m_assetLocator.includeShader (name)
		: this->m_assetLocator.readString (std::filesystem::path ("shaders") / name);
	} catch (AssetLoadException&) {
	    return std::nullopt;
	}
    };
    this->m_preprocessed = WERules::expandIncludes (this->m_preprocessed, load, seen);
}

void ShaderUnit::preprocessRequires () {
    size_t start = 0, end = 0;

    while ((start = this->m_preprocessed.find ("#require", end)) != std::string::npos) {
	const size_t lineEnd = this->m_preprocessed.find_first_of ('\n', start);

	const size_t nameStart = start + std::string ("#require ").length ();

	if (nameStart >= lineEnd) {
	    sLog.error ("Malformed #require directive (no module name) in shader ", this->m_file);
	    end = lineEnd;
	    continue;
	}

	std::string moduleName = this->m_preprocessed.substr (nameStart, lineEnd - nameStart);

	while (!moduleName.empty () && (moduleName.back () == ' ' || moduleName.back () == '\r')) {
	    moduleName.pop_back ();
	}

	if (moduleName.empty ()) {
	    sLog.error ("Malformed #require directive (empty module name) in shader ", this->m_file);
	    end = lineEnd;
	    continue;
	}

	sLog.out ("Resolving require module: ", moduleName, " in shader ", this->m_file);

	std::string moduleCode = this->resolveRequireModule (moduleName);

	// comment out the #require directive
	this->m_preprocessed = this->m_preprocessed.replace (start, 2, "//");

	if (!moduleCode.empty ()) {
	    // insert the generated code directly into m_preprocessed at the #require location
	    this->m_preprocessed.insert (start, moduleCode);
	    end = start + moduleCode.length ();
	} else {
	    end = lineEnd;
	}
    }
}

std::string ShaderUnit::resolveRequireModule (const std::string& moduleName) const {
    if (moduleName == "LightingV1") {
	return this->generateLightingV1 ();
    }

    sLog.error ("Unknown #require module: ", moduleName, " in shader ", this->m_file);
    WPENGINE_HEALTH ("missing_asset", { { "kind", "shader_module" }, { "path", moduleName }, { "file", this->m_file } });
    return "";
}

std::string ShaderUnit::generateLightingV1 () const {
    // WE generates PerformLighting_V1 for the scene's lights (GH 0x140197e20): the point/spot loop over
    // g_LCluster_Origin/Color/Direction/Extra, the tube loop after it (PointSegmentDelta to the segment origin →
    // g_LCluster_Direction), then the directional lights (g_LDirectional_*, ComputePBRLightShadowInfinite). A point or
    // spot light's flags (g_LCluster_Extra.y: 1 shadow, 2 cookie, 4 projected — a spot) pick its projection
    // (g_LCluster_Extra.z): a spot's g_LCluster_Projection for the cookie and its atlas tile, a point light's six cube
    // faces (CalculateProjectedCoordsPoint). WE emits the shadow code when LIGHTS_SHADOW_MAPPING_QUALITY is set; here
    // it's there when the material declares the atlas sampler (generic4: SHADOW_ATLAS_SAMPLER under
    // LIGHTS_SHADOW_MAPPING) and the cookie code when it declares COOKIE_SAMPLER (LIGHTS_COOKIE). Not done: WE's
    // light clusters (the first lights are taken, CScene::updateLights) and the directional cascades.
    // The calls follow the common_pbr_2.h the assets ship: 2.8.42's PerformShadowMapping (coords, transform) and
    // CalculateProjectedCoordsPoint (pos, origin, info, transform); 2.8.485's take the projection scale, the penumbra
    // and the PCSS terms (the generator's own text, GH 0x140197e20)
    const bool v485 = this->m_preprocessed.find ("vec2 projectionScale") != std::string::npos;
    std::string out =
	"// begin of generated module LightingV1\n"
	"uniform vec4 g_LCluster_Counts;\n"
	"uniform vec4 g_LCluster_Origin[16];\n"
	"uniform vec4 g_LCluster_Color[16];\n"
	"uniform vec4 g_LCluster_Direction[16];\n"
	"uniform vec4 g_LCluster_Extra[16];\n"
	"#if defined(SHADOW_ATLAS_SAMPLER) || defined(COOKIE_SAMPLER)\n"
	"uniform mat4 g_LCluster_Projection[16];\n"
	"uniform vec4 g_LCluster_ProjectionTransform[16];\n"
	"uniform vec4 g_LCluster_PointProjection[16];\n"
	"#endif\n"
	"#if defined(LIGHTS_DIRECTIONAL) && LIGHTS_DIRECTIONAL > 0\n"
	"uniform vec4 g_LDirectional_Color[LIGHTS_DIRECTIONAL];\n"
	"uniform vec4 g_LDirectional_Direction[LIGHTS_DIRECTIONAL];\n"
	"#endif\n"
	"vec3 PerformLighting_V1(vec3 worldPos, vec3 color, vec3 normal, vec3 viewVector, vec3 specularTint, vec3 ambient, float roughness, float metallic)\n"
	"{\n"
	"\tvec3 light = CAST3(0.0);\n"
	"\tvec2 shadowNoisePosition = gl_FragCoord.xy;\n"
	"\tuint pointSpotCount = uint(g_LCluster_Counts.x);\n"
	"\tuint tubeCount = uint(g_LCluster_Counts.y);\n"
	"\tfor (uint pointSpotIndex = 0u; pointSpotIndex < pointSpotCount; ++pointSpotIndex)\n"
	"\t{\n"
	"\t\tuint lightIndex = pointSpotIndex;\n"
	"\t\tvec4 lightOrigin = g_LCluster_Origin[lightIndex];\n"
	"\t\tvec4 lightColor = g_LCluster_Color[lightIndex];\n"
	"\t\tvec4 lightDirection = g_LCluster_Direction[lightIndex];\n"
	"\t\tvec4 lightExtra = g_LCluster_Extra[lightIndex];\n"
	"\n"
	"\t\tvec3 lightDelta = lightOrigin.xyz - worldPos;\n"
	"\t\tfloat spotCookie = -dot(normalize(lightDelta), lightDirection.xyz);\n"
	"\t\tspotCookie = smoothstep(0.0, 1.0, spotCookie * lightDirection.w + lightExtra.x);\n"
	"\t\tvec3 clusterColor = lightColor.rgb * spotCookie;\n"
	"\t\tfloat shadowFactor = 1.0;\n"
	"#if defined(SHADOW_ATLAS_SAMPLER) || defined(COOKIE_SAMPLER)\n"
	"\t\tuint lightFlags = uint(lightExtra.y);\n"
	"\t\tif ((lightFlags & 3u) != 0u)\n"
	"\t\t{\n"
	"\t\t\tuint projectionIndex = uint(lightExtra.z);\n"
	"\t\t\tif ((lightFlags & 4u) != 0u)\n"
	"\t\t\t{\n"
	"\t\t\t\tvec3 projectedCoords = CalculateProjectedCoords(worldPos, g_LCluster_Projection[projectionIndex]);\n"
	"#ifdef COOKIE_SAMPLER\n"
	"\t\t\t\tif ((lightFlags & 2u) != 0u)\n"
	"\t\t\t\t{\n"
	"\t\t\t\t\tvec2 cookieInside = step(abs(projectedCoords.xy - CAST2(0.5)), CAST2(0.5));\n"
	"\t\t\t\t\tclusterColor *= texSample2DLod(COOKIE_SAMPLER, projectedCoords.xy, 0.0).rgb * (cookieInside.x * cookieInside.y);\n"
	"\t\t\t\t}\n"
	"#endif\n"
	"#ifdef SHADOW_ATLAS_SAMPLER\n"
	"\t\t\t\tif ((lightFlags & 1u) != 0u)\n"
	"\t\t\t\t{\n";
    out += v485
	? "\t\t\t\t\tshadowFactor = PerformShadowMapping(projectedCoords, g_LCluster_ProjectionTransform[projectionIndex], g_LCluster_PointProjection[projectionIndex], lightExtra.w, shadowNoisePosition, CAST3(0.0), CAST3(0.0));\n"
	: "\t\t\t\t\tshadowFactor = PerformShadowMapping(projectedCoords, g_LCluster_ProjectionTransform[projectionIndex]);\n";
    out +=
	"\t\t\t\t}\n"
	"#endif\n"
	"\t\t\t}\n"
	"#ifdef SHADOW_ATLAS_SAMPLER\n"
	"\t\t\telse\n"
	"\t\t\t{\n";
    out += v485
	? "\t\t\t\tvec4 projectedCoords = CalculateProjectedCoordsPoint(worldPos, lightOrigin.xyz, lightDirection.xy, g_LCluster_PointProjection[projectionIndex], g_LCluster_ProjectionTransform[projectionIndex]);\n"
	  "\t\t\t\tshadowFactor = PerformPointShadowMapping(projectedCoords, g_LCluster_ProjectionTransform[projectionIndex], g_LCluster_PointProjection[projectionIndex], lightExtra.w, shadowNoisePosition, CAST3(0.0), CAST3(0.0));\n"
	: "\t\t\t\tvec4 projectedCoords = CalculateProjectedCoordsPoint(worldPos, lightOrigin.xyz, g_LCluster_PointProjection[projectionIndex], g_LCluster_ProjectionTransform[projectionIndex]);\n"
	  "\t\t\t\tshadowFactor = PerformPointShadowMapping(projectedCoords);\n";
    out +=
	"\t\t\t}\n"
	"#endif\n"
	"\t\t}\n"
	"#endif\n"
	"\n"
	"\t\tlight += ComputePBRLightShadow(normal, lightDelta, viewVector, color, clusterColor, lightOrigin.w, lightColor.w, specularTint, ambient, roughness, metallic, shadowFactor);\n"
	"\t}\n"
	"\n"
	"\tfor (uint tubeIndex = 0u; tubeIndex < tubeCount; ++tubeIndex)\n"
	"\t{\n"
	"\t\tuint lightIndex = pointSpotCount + tubeIndex;\n"
	"\t\tvec4 lightOrigin = g_LCluster_Origin[lightIndex];\n"
	"\t\tvec4 lightColor = g_LCluster_Color[lightIndex];\n"
	"\t\tvec4 lightDirection = g_LCluster_Direction[lightIndex];\n"
	"\n"
	"\t\tvec3 lightDelta = PointSegmentDelta(worldPos, lightOrigin.xyz, lightDirection.xyz);\n"
	"\t\tlight += ComputePBRLightShadow(normal, lightDelta, viewVector, color, lightColor.rgb, lightOrigin.w, lightColor.w, specularTint, ambient, roughness, metallic, 1.0);\n"
	"\t}\n"
	"#if defined(LIGHTS_DIRECTIONAL) && LIGHTS_DIRECTIONAL > 0\n"
	"\tfor (int i = 0; i < LIGHTS_DIRECTIONAL; ++i)\n"
	"\t{\n"
	"\t\tlight += ComputePBRLightShadowInfinite(normal, g_LDirectional_Direction[i].xyz, viewVector, color, g_LDirectional_Color[i].rgb, specularTint, ambient, roughness, metallic, 1.0);\n"
	"\t}\n"
	"#endif\n"
	"\treturn light;\n"
	"}\n"
	"// end of generated module LightingV1\n";
    return out;
}

std::string ShaderUnit::applyLinkedVaryingCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Vertex || this->m_link == nullptr) {
	return source;
    }

    std::regex fragmentVec4Varying (R"(\bvarying\s+vec4\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    std::smatch varyingMatch;
    std::string linked = this->m_link->m_preprocessed;
    size_t linkedOffset = 0;

    while (std::regex_search (linked.cbegin () + linkedOffset, linked.cend (), varyingMatch, fragmentVec4Varying)) {
	const std::string name = varyingMatch[1].str ();
	linkedOffset += varyingMatch.position () + varyingMatch.length ();

	const std::regex vertexVec2Decl ("\\bvarying\\s+vec2\\s+" + name + "\\s*;");
	if (!std::regex_search (source, vertexVec2Decl)) {
	    continue;
	}

	source = std::regex_replace (source, vertexVec2Decl, "varying vec4 " + name + ";");

	const std::regex assignment ("(^|\\n)([ \\t]*)" + name + "\\s*=\\s*([^;\\n]+);");
	std::smatch assignmentMatch;
	size_t offset = 0;
	while (std::regex_search (source.cbegin () + offset, source.cend (), assignmentMatch, assignment)) {
	    const std::string prefix = assignmentMatch[1].str ();
	    const std::string indent = assignmentMatch[2].str ();
	    const std::string expression = assignmentMatch[3].str ();
	    const std::string replacement = prefix + indent + name + " = vec4(" + expression + ", 0.0, 1.0);";
	    const size_t position = offset + assignmentMatch.position ();
	    source.replace (position, assignmentMatch.length (), replacement);
	    offset = position + replacement.length ();
	}
    }

    return source;
}

std::string ShaderUnit::applyFragmentVaryingDeclarations (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment || this->m_link == nullptr) {
	return source;
    }

    // GH 0x140125e80: the fragment unit's inputs are the vertex unit's VS_OUTPUT list, so a varying the fragment
    // doesn't declare is still there, and one it declares narrower (`varying vec2 v_TexCoord` against the vertex's
    // vec4) is the vertex's type, its uses truncated as HLSL does. LWE linked them with their own types: glslang let
    // the vec2 input read a vec4 output and SPIRV-Cross's GLSL then had `vec2 = vec4` (NVIDIA C7011; 2846673187's
    // audio ring and 2844906964 / 2342779250's test shaders dropped their layers)
    static const std::regex vertexVarying (
	R"(\bvarying\s+(?:(?:lowp|mediump|highp|flat|smooth|noperspective)\s+)*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)"
    );
    static const std::map<std::string, int> width = { { "float", 1 }, { "vec2", 2 }, { "vec3", 3 }, { "vec4", 4 } };
    const std::string& vertex = this->m_link->m_preprocessed;
    std::string missing;
    bool retyped = false;

    for (auto it = std::sregex_iterator (vertex.begin (), vertex.end (), vertexVarying); it != std::sregex_iterator ();
	 ++it) {
	const std::string type = (*it)[1].str ();
	const std::string name = (*it)[2].str ();
	const std::regex declared ("\\bvarying\\s+((?:[A-Za-z_][A-Za-z0-9_]*\\s+)*?)([A-Za-z_][A-Za-z0-9_]*)\\s+" + name + "\\s*;");
	const std::regex used ("\\b" + name + "\\b");
	std::smatch own;

	if (std::regex_search (source, own, declared)) {
	    const std::string ownType = own[2].str ();
	    const auto v = width.find (type), f = width.find (ownType);
	    if (v != width.end () && f != width.end () && v->second > f->second) {
		source = std::regex_replace (source, declared, "varying $1" + type + " " + name + ";");
		retyped = true;
	    }
	    continue;
	}
	if (!std::regex_search (source, used) || missing.find (" " + name + ";") != std::string::npos) {
	    continue;
	}

	missing += "varying " + type + " " + name + ";\n";
    }

    if (retyped) {
	WPENGINE_HEALTH ("compat", { { "what", "WE: a fragment varying takes the vertex unit's type" } });
    }
    if (missing.empty ()) {
	return source;
    }

    sLog.out ("Declaring in the fragment unit of ", this->m_file, " the varyings only its vertex unit has: ", missing);
    WPENGINE_HEALTH ("compat", { { "what", "shader: vertex-only varyings declared in the fragment" } });
    return missing + source;
}

std::string ShaderUnit::applyFragmentTexCoordCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment) {
	return source;
    }

    const std::regex texCoordBeforeCast2 (R"(\bv_TexCoord\b(\s*[-+*/]\s*CAST2\s*\())");
    const std::regex cast2BeforeTexCoord (R"((CAST2\s*\([^)]+\)\s*[-+*/]\s*)\bv_TexCoord\b)");

    const std::regex wideTexCoordDecl (R"(\bvarying\s+vec[34]\s+v_TexCoord\s*;)");
    if (!std::regex_search (source, wideTexCoordDecl)
	|| (!std::regex_search (source, texCoordBeforeCast2) && !std::regex_search (source, cast2BeforeTexCoord))) {
	return source;
    }

    const std::string original = source;
    source = std::regex_replace (source, texCoordBeforeCast2, "v_TexCoord.xy$1");
    source = std::regex_replace (source, cast2BeforeTexCoord, "$1v_TexCoord.xy");

    if (source != original) {
	sLog.out ("Applied fragment TexCoord vec2 compatibility in ", this->m_file);
	WPENGINE_HEALTH ("compat", { { "what", "shader: fragment TexCoord vec2 compatibility" } });
    }

    return source;
}

std::string ShaderUnit::applyWritableFragmentVaryings (std::string source) const {
    // the fragment's varyings and the vertex's attributes are WE's IN struct (GH 0x140125e80), which HLSL lets
    // main write to (3088030303's shadow does `a_TexCoord = …` in its vertex unit)
    const bool fragment = this->m_type == GLSLContext::UnitType_Fragment;
    static const std::regex fragmentDeclaration (
	R"(\bvarying\s+((?:(?:lowp|mediump|highp|flat|smooth|noperspective)\s+)*[A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)"
    );
    static const std::regex vertexDeclaration (
	R"(\battribute\s+((?:(?:lowp|mediump|highp)\s+)*[A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)"
    );
    const std::regex& declaration = fragment ? fragmentDeclaration : vertexDeclaration;
    static const std::regex mainOpen (R"(\bvoid\s+main\s*\(\s*(?:void)?\s*\)\s*\{)");
    std::vector<std::pair<std::string, std::string>> written; // (type, name)

    for (auto it = std::sregex_iterator (source.begin (), source.end (), declaration); it != std::sregex_iterator ();
	 ++it) {
	const std::string name = (*it)[2].str ();
	// `name = …`, `name += …`, `name.xy *= …` (not `==`), or `name++` / `++name`
	const std::regex assigned ("\\b" + name + "\\b(?:\\s*\\.\\s*[xyzwrgba]+)?\\s*(?:[-+*/]?=(?!=)|\\+\\+|--)|(?:\\+\\+|--)\\s*" + name + "\\b");
	const bool listed = std::any_of (written.begin (), written.end (), [&] (const auto& w) { return w.second == name; });
	if (!listed && std::regex_search (source, assigned)) {
	    // the type without its qualifiers
	    std::string type = (*it)[1].str ();
	    type = type.substr (type.find_last_of (" \t") == std::string::npos ? 0 : type.find_last_of (" \t") + 1);
	    written.emplace_back (type, name);
	}
    }

    std::smatch open;
    if (written.empty () || !std::regex_search (source, open, mainOpen)) {
	return source;
    }

    // every use after the declarations reads the copy; each main starts by filling it (a unit may have several
    // under #if — 3235948233's auto_sway has one per AA_VERSION — and only filling the first left the copy of the
    // active one zero: its v_TexCoord read 0 and 3414858021's 左白巾 drew nothing)
    const size_t bodyAt = static_cast<size_t> (open.position () + open.length ());
    std::string head = source.substr (0, bodyAt);
    std::string body = source.substr (bodyAt);
    std::string copies, fill;

    for (const auto& [type, name] : written) {
	const std::regex use ("\\b" + name + "\\b");
	const std::regex decl (std::string (fragment ? "\\bvarying" : "\\battribute") + "\\s+(?:[A-Za-z_][A-Za-z0-9_]*\\s+)+" + name + "\\s*;");
	const std::string copy = "_wp_writable_" + name;
	// keep the declaration (the vertex unit's output links by its name), the other uses go to the copy
	std::string rebuilt;
	size_t at = 0;
	for (auto it = std::sregex_iterator (head.begin (), head.end (), decl); it != std::sregex_iterator (); ++it) {
	    const size_t end = static_cast<size_t> (it->position () + it->length ());
	    // the copy (and a mark that it exists) where the input is declared: inside the same #if, when it's in one
	    rebuilt += std::regex_replace (head.substr (at, it->position () - at), use, copy) + it->str ()
		+ "\n" + type + " " + copy + ";\n#define _WP_HAS" + copy;
	    at = end;
	}
	head = rebuilt + std::regex_replace (head.substr (at), use, copy);
	body = std::regex_replace (body, use, copy);
	fill += "\n#ifdef _WP_HAS" + copy + "\n\t" + copy + " = " + name + ";\n#endif";
    }

    body = std::regex_replace (body, mainOpen, "$&" + fill);
    sLog.out ("Copying the inputs the ", fragment ? "fragment" : "vertex", " unit of ", this->m_file, " writes to:", fill);
    WPENGINE_HEALTH ("compat", { { "what", fragment ? "shader: fragment writes to varyings (copied to globals)" : "WE: vertex writes to attributes (copied to globals)" } });
    return head + fill + body;
}

std::string ShaderUnit::applyBuiltinDeclarations (std::string source) {
    // WE's translator turns a declared `gl_…` input or output into a member of its IN / OUT struct with that system
    // value's semantic (gl_Position SV_POSITION: GH 0x140125e80 adds it; the stock shadow casters declare
    // `in uint gl_InstanceID;` and `varying uint gl_ViewportIndex;` — inferred to be SV_InstanceID and
    // SV_ViewportArrayIndex). GLSL has them built in and refuses a declaration: it's dropped. A vertex unit's
    // gl_ViewportIndex write (GLSL 4.10 or GL_ARB_shader_viewport_layer_array) is dropped too: LWE draws the
    // casters one viewport at a time (CScene::renderShadows), so every instance goes to the viewport bound
    static const std::regex declaration (
	R"((^|\n)[ \t]*(?:in|out|varying|attribute)\s+(?:[A-Za-z_][A-Za-z0-9_]*\s+)+(gl_[A-Za-z0-9_]+)\s*;[^\n]*)"
    );
    if (!std::regex_search (source, declaration)) {
	return source;
    }
    source = std::regex_replace (source, declaration, "$1// built in: $2");
    static const std::regex viewportWrite (R"(\bgl_ViewportIndex\s*=[^;]*;)");
    if (this->m_type == GLSLContext::UnitType_Vertex && std::regex_search (source, viewportWrite)) {
	source = std::regex_replace (source, viewportWrite, "/* one draw per viewport: gl_ViewportIndex */");
	WPENGINE_HEALTH ("compat", { { "what", "WE: a vertex gl_ViewportIndex write dropped (one draw per viewport)" } });
    }
    WPENGINE_HEALTH ("compat", { { "what", "WE: a declared gl_ built-in dropped" } });
    return source;
}

void ShaderUnit::parseComboConfiguration (const std::string& content, const int defaultValue) {
    // TODO: SUPPORT REQUIRES SO WE PROPERLY FOLLOW THE REQUIRED CHAIN
    JSON data;
    try {
	data = JSON::parse (content);
    } catch (const std::exception& e) {
	sLog.error ("Cannot parse combo metadata in shader ", this->m_file, ": ", e.what ());
	WPENGINE_HEALTH ("shader_warning", { { "file", this->m_file }, { "message", std::string ("combo metadata unparsable: ") + e.what () } });
	return;
    }
    const auto combo = data.require<std::string> ("combo", "cannot parse combo information");
    // ignore type as it seems to be used only on the editor
    // const auto type = data.find ("type");
    const auto defvalue = data.find ("default");

    // check the combos
    const auto entry = this->m_combos.find (combo);
    const auto entryOverride = this->m_overrideCombos.find (combo);

    // add the combo to the found list
    this->m_usedCombos.emplace (combo, true);

    // if the combo was not found in the predefined values this means that the default value in the JSON data can be
    // used so only define the ones that are not already defined
    if (entry == this->m_combos.end () && entryOverride == this->m_overrideCombos.end ()) {
	// if no combo is defined just load the default settings
	if (defvalue == data.end ()) {
	    // TODO: PROPERLY SUPPORT EMPTY COMBOS
	    this->m_discoveredCombos.emplace (combo, defaultValue);
	} else if (defvalue->is_number_float ()) {
	    sLog.exception ("float combos are not supported in shader ", this->m_file, ". ", combo);
	} else if (defvalue->is_number_integer ()) {
	    this->m_discoveredCombos.emplace (combo, defvalue->get<int> ());
	} else if (defvalue->is_string ()) {
	    sLog.exception ("string combos are not supported in shader ", this->m_file, ". ", combo);
	} else {
	    sLog.exception ("cannot parse combo information ", combo, ". unknown type for ", defvalue->dump ());
	}
    }
}

namespace {
/**
 * A uniform annotation's vector `default`: an "x y z" string, or a number for every component (a workshop shader
 * writes `"default": 0` for a vec2). It threw a JSON type error, which dropped the whole effect
 */
template <typename V> V vectorDefault (const JSON& value) {
    if (value.is_number ()) {
	return V (value.get<float> ());
    }
    if (value.is_string ()) {
	// as many components as are written, the rest 0, extras ignored (the stock editorpaintbrush gives a vec4
	// "0 0"; VectorBuilder threw and the shader didn't build). Inferred: WE's own reader isn't located
	std::istringstream in (value.get<std::string> ());
	V out (0.0f);
	for (int i = 0; i < V::length (); i++) {
	    float f = 0.0f;
	    if (!(in >> f)) {
		break;
	    }
	    out[i] = f;
	}
	return out;
    }
    return V (0.0f);
}

/** A float `default` written as a string ("0.1") is that float (std::stoi read 0) */
float floatDefault (const JSON& value) {
    if (value.is_number ()) {
	return value.get<float> ();
    }
    if (value.is_string ()) {
	return std::strtof (value.get<std::string> ().c_str (), nullptr);
    }
    return 0.0f;
}

/** An int `default` written as a string: its leading number, 0 when there is none (std::stoi threw) */
int intDefault (const JSON& value) {
    if (value.is_number ()) {
	return value.get<int> ();
    }
    if (value.is_string ()) {
	return static_cast<int> (std::strtol (value.get<std::string> ().c_str (), nullptr, 10));
    }
    return 0;
}
} // namespace

void ShaderUnit::parseParameterConfiguration (
    const std::string& type, const std::string& name, const std::string& content
) {
    JSON data;
    try {
	data = JSON::parse (content);
    } catch (const std::exception& e) {
	sLog.error ("Cannot parse parameter metadata for ", name, " in shader ", this->m_file, ": ", e.what ());
	WPENGINE_HEALTH ("shader_warning", { { "file", this->m_file }, { "message", std::string ("parameter metadata unparsable: ") + e.what () } });
	return;
    }
    const auto material = data.optional ("material");
    const auto defvalue = data.optional ("default");
    // auto range = data.find ("range");
    const auto combo = data.find ("combo");

    // this is not a real parameter
    auto constant = this->m_constants.end ();

    if (material.has_value ()) {
	constant = this->m_constants.find (*material);
    }

    // a parameter with neither a material constant nor a default (the stock shadowcasterfoliage4's g_SpeedLeaves,
    // set by the material it's used with) starts at zero: it threw, and the shader didn't build. Inferred
    if (constant == this->m_constants.end () && !defvalue.has_value () && type != "sampler2D") {
	sLog.out ("No default for parameter ", name, " in shader ", this->m_file, ", using zero");
    }

    Variables::ShaderVariable* parameter = nullptr;
    // a parameter bound to a material constant may have no `default` (dereferencing the empty optional was UB)
    const JSON noDefault;
    const JSON& defaultValue = defvalue.has_value () ? *defvalue : noDefault;

    if (type == "vec4") {
	parameter = new Variables::ShaderVariableVector4 (vectorDefault<glm::vec4> (defaultValue));
    } else if (type == "vec3") {
	parameter = new Variables::ShaderVariableVector3 (vectorDefault<glm::vec3> (defaultValue));
    } else if (type == "vec2") {
	parameter = new Variables::ShaderVariableVector2 (vectorDefault<glm::vec2> (defaultValue));
    } else if (type == "float") {
	parameter = new Variables::ShaderVariableFloat (floatDefault (defaultValue));
    } else if (type == "int") {
	parameter = new Variables::ShaderVariableInteger (intDefault (defaultValue));
    } else if (type == "sampler2D" || type == "sampler2DComparison") {
	// samplers can have special requirements, check what sampler we're working with and create definitions
	// if needed
	const auto textureName = data.find ("default");
	// TODO: CREATE TEXTURE WITH THE GIVEN COLOR
	// extract the texture number from the name
	const char value = name.at (std::string ("g_Texture").length ());
	const auto requireany = data.find ("requireany");
	const auto require = data.find ("require");
	// now convert it to integer
	// TODO: BETTER CONVERSION HERE
	size_t index = value - '0';
	// TODO: SUPPORT USER TEXTURES!!

	if (combo != data.end ()) {
	    // TODO: CLEANUP HOW THIS IS DETERMINED FIRST
	    // if the texture exists (and is not null), add to the combo
	    const auto textureSlotUsed
		= this->m_passTextures.contains (index) || this->m_overrideTextures.contains (index);
	    bool isRequired = false;
	    int comboValue = 1;

	    if (textureSlotUsed) {
		// nothing extra to do, the texture exists, the combo must be set
		// these tend to not have default value
		isRequired = true;
	    } else if (require != data.end ()) {
		// this is required based on certain conditions
		if (requireany != data.end () && requireany->get<bool> ()) {
		    // any of the values set are valid, check for them
		    for (const auto& item : require->items ()) {
			const std::string& macro = item.key ();
			const auto it = this->m_combos.find (macro);

			// if any of the values matched, this option is required
			if (it == this->m_combos.end () || this->m_overrideCombos.contains (macro)
			    || it->second != item.value ()) {
			    isRequired = true;
			    break;
			}
		    }
		} else {
		    isRequired = true;

		    // all values must match for it to be required
		    for (const auto& item : require->items ()) {
			const std::string& macro = item.key ();
			const auto it = this->m_combos.find (macro);

			// these can not exist and that'd be fine, we just care about the values
			if ((it != this->m_combos.end () || this->m_overrideCombos.contains (macro))
			    && it->second == item.value ()) {
			    isRequired = false;
			    break;
			}
		    }
		}
	    }

	    if (isRequired && !textureSlotUsed) {
		if (!defvalue.has_value ()) {
		    isRequired = false;
		} else {
		    // is the combo registered already?
		    // if not, add it with the default value
		    // there's already a combo providing this value, so it doesn't need to be added
		    if (this->m_combos.contains (*combo) || this->m_overrideCombos.contains (*combo)) {
			isRequired = false;
			// otherwise a default value must be used
		    } else if (defvalue->is_string ()) {
			comboValue = std::stoi (defvalue->get<std::string> ().c_str ());
		    } else if (defvalue->is_number ()) {
			comboValue = *defvalue;
		    } else {
			sLog.exception (
			    "Cannot determine default value for combo ", combo->get<std::string> (),
			    " because it's not specified by the shader and is not given a default value: ", this->m_file
			);
		    }
		}
	    }

	    if (isRequired) {
		// add the new combo to the list
		this->m_discoveredCombos.emplace (*combo, comboValue);
		// textures linked to combos need to be tracked too
		this->m_usedCombos.emplace (*combo, true);
	    }
	}

	// "formatcombo": WE defines TEX<n>FORMAT for each of slots 0..9 whose sampler has the flag, to the format of the
	// texture bound there or, with none, of the sampler's default (GH 0x1401d2a20: "TEX" + n + "FORMAT" = the
	// texture's format field; common_fragment.h's FORMAT_*, read by ConvertTextureFormat / DecompressNormal; the
	// stock fur4 uses TEX8FORMAT in code). TEX0FORMAT stays CPass's (RG88 / R8 of the layer's texture)
	const auto formatCombo = data.find ("formatcombo");
	if (formatCombo != data.end () && formatCombo->is_boolean () && formatCombo->get<bool> ()) {
	    const std::string name = "TEX" + std::to_string (index) + "FORMAT";
	    if (!this->m_combos.contains (name) && !this->m_overrideCombos.contains (name)) {
		const std::string defaultName
		    = textureName != data.end () && textureName->is_string () ? textureName->get<std::string> () : "";
		const int format = this->m_formatOf && index != 0 ? this->m_formatOf (static_cast<int> (index), defaultName) : 0;
		this->m_discoveredCombos.emplace (name, format);
	    }
	}

	if (textureName != data.end ()) {
	    this->m_defaultTextures.emplace (index, *textureName);
	}

	// samplers are not saved, we can ignore them for now
	return;
    } else {
	sLog.error ("Unknown parameter type: ", type, " for ", name, " in shader ", this->m_file);
	WPENGINE_HEALTH ("unsupported", { { "what", "shader parameter type " + type }, { "file", this->m_file } });
	return;
    }

    if (material.has_value () && parameter != nullptr) {
	parameter->setIdentifierName (*material);
	parameter->setName (name);

	this->m_parameters.push_back (parameter);
    }
}

const ComboMap& ShaderUnit::getCombos () const { return this->m_combos; }

const ComboMap& ShaderUnit::getDiscoveredCombos () const { return this->m_discoveredCombos; }

void ShaderUnit::linkToUnit (const ShaderUnit* unit) { this->m_link = unit; }

const ShaderUnit* ShaderUnit::getLinkedUnit () const { return this->m_link; }

const std::string& ShaderUnit::compile () {
    if (!this->m_final.empty ()) {
	return this->m_final;
    }

    this->m_final = SHADER_HEADER (this->m_file);

    if (this->m_type == GLSLContext::UnitType_Fragment) {
	this->m_final += FRAGMENT_SHADER_DEFINES;
    } else {
	this->m_final += VERTEX_SHADER_DEFINES;
    }

    // SKINNING sizes g_Bones[BONECOUNT]: WE sets BONECOUNT to the skeleton's bone count with it (vtable +0xd8, GH
    // 0x140230ec0), never 0. A SKINNING pass without a count (a caster's defaults) gets 1 rather than a zero-size array
    const auto skinningSet = [this] () {
	for (const ComboMap* map : std::initializer_list<const ComboMap*> { &this->m_overrideCombos, &this->m_combos, &this->m_discoveredCombos }) {
	    for (const auto& [name, value] : *map) {
		std::string upper;
		std::ranges::transform (name, std::back_inserter (upper), ::toupper);
		if (upper == "SKINNING") {
		    return value != 0;
		}
	    }
	}
	return false;
    } ();
    const auto comboValue = [skinningSet] (const std::string& name, int value) {
	return name == "BONECOUNT" && skinningSet && value < 1 ? 1 : value;
    };
    std::map<std::string, bool> addedCombos;

    for (const auto& [name, value] : this->m_overrideCombos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, comboValue (uppercase, value));
	    addedCombos.emplace (uppercase, true);
	}
    }

    // now add all the combos to the source
    for (const auto& [name, value] : this->m_combos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, comboValue (uppercase, value));
	    addedCombos.emplace (uppercase, true);
	}
    }

    for (const auto& [name, value] : this->m_discoveredCombos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, comboValue (uppercase, value));
	    addedCombos.emplace (uppercase, true);
	}
    }

    if (this->m_link != nullptr) {
	for (const auto& [name, value] : this->m_link->getCombos ()) {
	    std::string uppercase;
	    std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	    if (!addedCombos.contains (uppercase)) {
		this->m_final += DEFINE_COMBO (uppercase, comboValue (uppercase, value));
		addedCombos.emplace (uppercase, true);
	    }
	}

	for (const auto& [name, value] : this->m_link->getDiscoveredCombos ()) {
	    std::string uppercase;
	    std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	    if (!addedCombos.contains (uppercase)) {
		this->m_final += DEFINE_COMBO (uppercase, comboValue (uppercase, value));
		addedCombos.emplace (uppercase, true);
	    }
	}
    }

    // this should be the rest of the shader
    std::string body = this->applyWritableFragmentVaryings (this->applyFragmentTexCoordCompatibility (
	this->applyFragmentVaryingDeclarations (this->applyLinkedVaryingCompatibility (this->m_preprocessed))
    ));
    body = WERules::renameReservedWords (WERules::falsifyUnreadableIfs (this->applyBuiltinDeclarations (body)));
    this->m_final += WERules::intrinsicOverloads (body);
    if (this->m_type == GLSLContext::UnitType_Fragment) {
	this->m_final += WERules::clipFunctions (body);
    }
    this->m_final += body;
    if (!this->m_extensions.empty ()) {
	this->m_final.insert (this->m_final.find ('\n') + 1, this->m_extensions);
    }

    // the pass itself handles shader compilation, the unit doesn't have enough information for this step
    return this->m_final;
}

const std::vector<Variables::ShaderVariable*>& ShaderUnit::getParameters () const { return this->m_parameters; }
const TextureMap& ShaderUnit::getTextures () const { return this->m_defaultTextures; }
