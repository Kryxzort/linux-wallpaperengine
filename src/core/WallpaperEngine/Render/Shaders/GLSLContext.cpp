#include "GLSLContext.h"
#include "ShaderCache.h"
#include "WEShaderRules.h"
#include "WallpaperEngine/Health.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cassert>
#include <memory>
#include <regex>
#include <sstream>
#include <algorithm>

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Include/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv_glsl.hpp"

using namespace WallpaperEngine::Render::Shaders;

TBuiltInResource BuiltInResource = { .maxLights = 32,
				     .maxClipPlanes = 6,
				     .maxTextureUnits = 32,
				     .maxTextureCoords = 32,
				     .maxVertexAttribs = 64,
				     .maxVertexUniformComponents = 4096,
				     .maxVaryingFloats = 64,
				     .maxVertexTextureImageUnits = 32,
				     .maxCombinedTextureImageUnits = 80,
				     .maxTextureImageUnits = 32,
				     .maxFragmentUniformComponents = 4096,
				     .maxDrawBuffers = 32,
				     .maxVertexUniformVectors = 128,
				     .maxVaryingVectors = 8,
				     .maxFragmentUniformVectors = 16,
				     .maxVertexOutputVectors = 16,
				     .maxFragmentInputVectors = 15,
				     .minProgramTexelOffset = -8,
				     .maxProgramTexelOffset = 7,
				     .maxClipDistances = 8,
				     .maxComputeWorkGroupCountX = 65535,
				     .maxComputeWorkGroupCountY = 65535,
				     .maxComputeWorkGroupCountZ = 65535,
				     .maxComputeWorkGroupSizeX = 1024,
				     .maxComputeWorkGroupSizeY = 1024,
				     .maxComputeWorkGroupSizeZ = 64,
				     .maxComputeUniformComponents = 1024,
				     .maxComputeTextureImageUnits = 16,
				     .maxComputeImageUniforms = 8,
				     .maxComputeAtomicCounters = 8,
				     .maxComputeAtomicCounterBuffers = 1,
				     .maxVaryingComponents = 60,
				     .maxVertexOutputComponents = 64,
				     .maxGeometryInputComponents = 64,
				     .maxGeometryOutputComponents = 128,
				     .maxFragmentInputComponents = 128,
				     .maxImageUnits = 8,
				     .maxCombinedImageUnitsAndFragmentOutputs = 8,
				     .maxCombinedShaderOutputResources = 8,
				     .maxImageSamples = 0,
				     .maxVertexImageUniforms = 0,
				     .maxTessControlImageUniforms = 0,
				     .maxTessEvaluationImageUniforms = 0,
				     .maxGeometryImageUniforms = 0,
				     .maxFragmentImageUniforms = 8,
				     .maxCombinedImageUniforms = 8,
				     .maxGeometryTextureImageUnits = 16,
				     .maxGeometryOutputVertices = 256,
				     .maxGeometryTotalOutputComponents = 1024,
				     .maxGeometryUniformComponents = 1024,
				     .maxGeometryVaryingComponents = 64,
				     .maxTessControlInputComponents = 128,
				     .maxTessControlOutputComponents = 128,
				     .maxTessControlTextureImageUnits = 16,
				     .maxTessControlUniformComponents = 1024,
				     .maxTessControlTotalOutputComponents = 4096,
				     .maxTessEvaluationInputComponents = 128,
				     .maxTessEvaluationOutputComponents = 128,
				     .maxTessEvaluationTextureImageUnits = 16,
				     .maxTessEvaluationUniformComponents = 1024,
				     .maxTessPatchComponents = 120,
				     .maxPatchVertices = 32,
				     .maxTessGenLevel = 64,
				     .maxViewports = 16,
				     .maxVertexAtomicCounters = 0,
				     .maxTessControlAtomicCounters = 0,
				     .maxTessEvaluationAtomicCounters = 0,
				     .maxGeometryAtomicCounters = 0,
				     .maxFragmentAtomicCounters = 8,
				     .maxCombinedAtomicCounters = 8,
				     .maxAtomicCounterBindings = 1,
				     .maxVertexAtomicCounterBuffers = 0,
				     .maxTessControlAtomicCounterBuffers = 0,
				     .maxTessEvaluationAtomicCounterBuffers = 0,
				     .maxGeometryAtomicCounterBuffers = 0,
				     .maxFragmentAtomicCounterBuffers = 1,
				     .maxCombinedAtomicCounterBuffers = 1,
				     .maxAtomicCounterBufferSize = 16384,
				     .maxTransformFeedbackBuffers = 4,
				     .maxTransformFeedbackInterleavedComponents = 64,
				     .maxCullDistances = 8,
				     .maxCombinedClipAndCullDistances = 8,
				     .maxSamples = 4,
				     .maxMeshOutputVerticesNV = 256,
				     .maxMeshOutputPrimitivesNV = 512,
				     .maxMeshWorkGroupSizeX_NV = 32,
				     .maxMeshWorkGroupSizeY_NV = 1,
				     .maxMeshWorkGroupSizeZ_NV = 1,
				     .maxTaskWorkGroupSizeX_NV = 32,
				     .maxTaskWorkGroupSizeY_NV = 1,
				     .maxTaskWorkGroupSizeZ_NV = 1,
				     .maxMeshViewCountNV = 4,
				     .limits = {
					 .nonInductiveForLoops = true,
					 .whileLoops = true,
					 .doWhileLoops = true,
					 .generalUniformIndexing = true,
					 .generalAttributeMatrixVectorIndexing = true,
					 .generalVaryingIndexing = true,
					 .generalSamplerIndexing = true,
					 .generalVariableIndexing = true,
					 .generalConstantMatrixVectorIndexing = true,
				     } };

GLSLContext::GLSLContext () {
    assert (this->sInstance == nullptr);

    glslang::InitializeProcess ();
}

GLSLContext::~GLSLContext () { glslang::FinalizeProcess (); }

GLSLContext& GLSLContext::get () {
    if (sInstance == nullptr) {
	sInstance = std::make_unique<GLSLContext> ();
    }

    return *sInstance;
}

namespace {
using WallpaperEngine::Render::Shaders::WERules::dropUnmatchedEndifs;
using WallpaperEngine::Render::Shaders::WERules::spliceLines;

/*
 * D3DCompile's HLSL rules, emulated from glslang's errors. WE hands its translated units to FXC (GH 0x140106780:
 * vs_5_0 / ps_5_0, flags 0x8800), which accepts what GLSL refuses; the shaders were written against that. Each fixer
 * reads one glslang error, rewrites the line it names the way HLSL reads it and returns true; parseUnit retries.
 * The rules are HLSL's (inferred from its documented conversions and the library's shaders, which WE builds — a
 * unit FXC refuses renders as WE's red fallback, dx11fallback.frag).
 */
struct Line {
    size_t begin = 0, end = 0;
    std::string text;
};

bool lineAt (const std::string& source, int number, Line& line) {
    if (number <= 0) {
	return false;
    }
    size_t begin = 0;
    for (int i = 1; i < number && begin != std::string::npos; i++) {
	begin = source.find ('\n', begin);
	begin = begin == std::string::npos ? begin : begin + 1;
    }
    if (begin == std::string::npos || begin > source.size ()) {
	return false;
    }
    line.begin = begin;
    line.end = std::min (source.find ('\n', begin), source.size ());
    line.text = source.substr (begin, line.end - begin);
    return true;
}

void replaceLine (std::string& source, const Line& line, const std::string& text) {
    source.replace (line.begin, line.end - line.begin, text);
}

/** glslang's type text (' temp 4-component vector of float', ' const int') → base and width; false for others */
bool readType (const std::string& text, std::string& base, int& width) {
    static const std::regex vector (R"((\d)-component vector of (float|int|uint|bool)\s*$)");
    static const std::regex scalar (R"(\b(float|int|uint|bool)\s*$)");
    std::smatch m;
    if (std::regex_search (text, m, vector)) {
	base = m[2].str ();
	width = std::stoi (m[1].str ());
	return true;
    }
    if (text.find ("matrix") == std::string::npos && text.find ("structure") == std::string::npos
	&& std::regex_search (text, m, scalar)) {
	base = m[1].str ();
	width = 1;
	return true;
    }
    return false;
}

std::string typeName (const std::string& base, int width) {
    if (width == 1) {
	return base;
    }
    const std::string prefix = base == "float" ? "" : base == "int" ? "i" : base == "uint" ? "u" : "b";
    return prefix + "vec" + std::to_string (width);
}

/** The first `[lhs] = rhs;` on the line from `from` (not ==, <=, >=, !=): the rhs's bounds; false when there's none */
bool assignment (const std::string& text, size_t& rhsBegin, size_t& rhsEnd, size_t from = 0) {
    for (size_t i = from; i < text.size (); i++) {
	if (text[i] != '=') {
	    continue;
	}
	const char before = i > 0 ? text[i - 1] : ' ';
	const char after = i + 1 < text.size () ? text[i + 1] : ' ';
	if (after == '=' || before == '=' || before == '<' || before == '>' || before == '!') {
	    if (after == '=') {
		i++;
	    }
	    continue;
	}
	int depth = 0;
	size_t j = i + 1;
	for (; j < text.size (); j++) {
	    if (text[j] == '(' || text[j] == '[') {
		depth++;
	    } else if (text[j] == ')' || text[j] == ']') {
		depth--;
	    } else if ((text[j] == ';' || text[j] == ',') && depth <= 0) {
		break;
	    }
	}
	if (j >= text.size ()) {
	    return false;
	}
	rhsBegin = i + 1;
	rhsEnd = j;
	return true;
    }
    return false;
}

/**
 * HLSL's implicit conversions on an assignment or initializer: a wider vector truncated (`vec2 p = v_TexCoord;`
 * with a vec4 v_TexCoord), a scalar promoted (`vec3 c = 0.5;`), bool / int / uint / float converted
 * (`int hit = a < b;`). From glslang's "cannot convert from 'A' to 'B'", the right-hand side becomes a B.
 */
bool convertAssignment (std::string& source, const std::string& log) {
    static const std::regex conversion (R"(ERROR: 0:(\d+): '[^']*' :\s+cannot convert from '([^']*)' to '([^']*)')");
    std::smatch m;
    if (!std::regex_search (log, m, conversion)) {
	return false;
    }
    std::string fromBase, toBase;
    int fromWidth = 0, toWidth = 0;
    Line line;
    if (!readType (m[2].str (), fromBase, fromWidth) || !readType (m[3].str (), toBase, toWidth)
	|| !lineAt (source, std::stoi (m[1].str ()), line)) {
	return false;
    }
    static const char* swizzles[] = { "", ".x", ".xy", ".xyz" };
    const std::string ctor = typeName (toBase, toWidth) + "(";
    // the first assignment on the line not converted yet (a line may hold several: `a *= x; b *= y;`)
    size_t rb = 0, re = 0, from = 0;
    while (assignment (line.text, rb, re, from)) {
	std::string rhs = line.text.substr (rb, re - rb);
	const size_t lead = rhs.find_first_not_of (" \t");
	if (lead != std::string::npos && (rhs.compare (lead, ctor.size (), ctor) == 0 || rhs.compare (lead, 1, "(") == 0)) {
	    from = re;
	    continue;
	}
	std::string wrapped;
	if (fromWidth > toWidth) {
	    // truncation keeps the first components; then the base, if it differs too
	    wrapped = "(" + rhs + ")" + swizzles[toWidth];
	    if (fromBase != toBase) {
		wrapped = ctor + wrapped + ")";
	    }
	} else if (fromWidth == 1 || fromWidth == toWidth) {
	    // a scalar fills every component; a same-width vector converts its base
	    wrapped = ctor + rhs + ")";
	} else {
	    return false;
	}
	sLog.out ("HLSL conversion on line ", m[1].str (), ": ", rhs, " -> ", wrapped);
	WPENGINE_HEALTH ("compat", { { "what", "HLSL: an assignment converts implicitly" } });
	replaceLine (source, line, line.text.substr (0, rb) + " " + wrapped + line.text.substr (re));
	return true;
    }
    return false;
}

/** The arguments of the call whose `(` is at `open`: their bounds; false when the parentheses don't close */
bool callArguments (const std::string& text, size_t open, std::vector<std::pair<size_t, size_t>>& args, size_t& close) {
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = open; i < text.size (); i++) {
	const char c = text[i];
	if (c == '(' || c == '[') {
	    depth++;
	} else if (c == ')' || c == ']') {
	    depth--;
	    if (depth == 0) {
		if (text.find_first_not_of (" \t", start) < i) {
		    args.emplace_back (start, i);
		}
		close = i;
		return true;
	    }
	} else if (c == ',' && depth == 1) {
	    args.emplace_back (start, i);
	    start = i + 1;
	}
    }
    return false;
}

/**
 * HLSL converts a call's arguments to its parameters' types (3370055069's depthparallax passes a float to
 * `getDepthMapDataX (vec2, int)`; `smoothstep (0, …)` with an int edge). From glslang's "no matching overloaded
 * function", the call on that line: each argument for a parameter of a user function's single definition with that
 * many parameters is made that type (`int (x)`, `vec2 (v)` — a GLSL constructor truncates and fills like HLSL),
 * out and inout ones left; for a built-in, integer literals become float ones
 */
bool convertArguments (std::string& source, const std::string& log) {
    static const std::regex error (R"(ERROR: 0:(\d+): '([A-Za-z_][A-Za-z0-9_]*)' : no matching overloaded function found)");
    static const std::regex scalarType (R"(^(?:float|int|uint|bool|[biu]?vec[234]|mat[234])$)");
    std::smatch m;
    Line line;
    if (!std::regex_search (log, m, error) || !lineAt (source, std::stoi (m[1].str ()), line)) {
	return false;
    }
    const std::string name = m[2].str ();
    const std::regex callAt ("\\b" + name + "\\s*\\(");
    std::smatch c;
    if (!std::regex_search (line.text, c, callAt)) {
	return false;
    }
    const size_t open = static_cast<size_t> (c.position () + c.length () - 1);
    std::vector<std::pair<size_t, size_t>> args;
    size_t close = 0;
    if (!callArguments (line.text, open, args, close)) {
	return false;
    }
    // the user's definitions with as many parameters (a parameter list may hold macro calls:
    // DECLARE_SAMPLER2D_PARAMETER (morphTexture) in model_vertex_v1.h's ApplyMorphPosition)
    const std::regex definition ("\\b(?:void|float|int|uint|bool|[biu]?vec[234]|mat[234])\\s+" + name + "\\s*\\(");
    std::vector<std::vector<std::string>> candidates;
    for (auto it = std::sregex_iterator (source.begin (), source.end (), definition); it != std::sregex_iterator (); ++it) {
	const size_t paren = static_cast<size_t> (it->position () + it->length () - 1);
	std::vector<std::pair<size_t, size_t>> bounds;
	size_t end = 0;
	if (!callArguments (source, paren, bounds, end)) {
	    continue;
	}
	const size_t body = source.find_first_not_of (" \t\r\n", end + 1);
	if (body == std::string::npos || source[body] != '{') {
	    continue; // a call or a prototype, not the definition
	}
	std::vector<std::string> params;
	for (const auto& [b, e] : bounds) {
	    params.push_back (source.substr (b, e - b));
	}
	if (params.size () == 1 && params[0].find_first_not_of (" \tvoid") == std::string::npos) {
	    params.clear ();
	}
	if (params.size () == args.size ()) {
	    candidates.push_back (params);
	}
    }
    std::string rebuilt;
    size_t last = 0;
    bool changed = false;
    static const std::regex intLiteral (R"((^|[^A-Za-z0-9_.])(\d+)(?![\d.eEuU]))");
    for (size_t i = 0; i < args.size (); i++) {
	const auto [b, e] = args[i];
	const std::string arg = line.text.substr (b, e - b);
	std::string converted = arg;
	if (candidates.size () == 1) {
	    std::stringstream words (candidates[0][i]);
	    std::vector<std::string> w;
	    for (std::string x; words >> x;) {
		w.push_back (x);
	    }
	    // out, inout and array parameters (`in uint morphOffsets[12]`) are left as they are
	    const bool output = std::find (w.begin (), w.end (), "out") != w.end () || std::find (w.begin (), w.end (), "inout") != w.end ()
		|| candidates[0][i].find ('[') != std::string::npos;
	    std::string type;
	    for (const auto& x : w) {
		if (std::regex_match (x, scalarType)) {
		    type = x;
		    break;
		}
	    }
	    const size_t lead = arg.find_first_not_of (" \t");
	    if (!output && !type.empty () && lead != std::string::npos && arg.compare (lead, type.size () + 1, type + "(") != 0) {
		converted = " " + type + "(" + arg + ")";
	    }
	} else if (candidates.empty ()) {
	    converted = std::regex_replace (arg, intLiteral, "$1$2.0");
	}
	if (converted != arg) {
	    changed = true;
	}
	rebuilt += line.text.substr (last, b - last) + converted;
	last = e;
    }
    if (!changed) {
	return false;
    }
    rebuilt += line.text.substr (last);
    sLog.out ("HLSL argument conversion on line ", m[1].str (), ": ", rebuilt);
    WPENGINE_HEALTH ("compat", { { "what", "HLSL: a call converts its arguments" } });
    replaceLine (source, line, rebuilt);
    return true;
}

/**
 * HLSL's implicit truncation in an operation between a wider and a narrower float vector: the wider one's first
 * components, with a warning (3414858021's iris_movement__: `vec2 da = transformedCursorPosition * g_CursorScale`, a
 * vec4 times a vec2). From glslang's error, the line's variables declared with the wider type get the narrower
 * swizzle — not one being declared on the line, and when glslang names the wider operand's storage (in, uniform,
 * temp), one of that storage first (2800594362's clipping_mask: `vec4 clip = texSample2D(…, v_TexCoord + …)` had
 * `clip` itself swizzled)
 */
bool truncateWiderOperands (std::string& source, const std::string& log) {
    static const std::regex operands (
	R"(ERROR: 0:(\d+): '[^']*' :\s+wrong operand types: no operation '[^']*' exists that takes a left-hand operand of type ' ((?:[a-z]+ )*)(\d)-component vector of float' and a right operand of type ' ((?:[a-z]+ )*)(\d)-component vector of float')"
    );
    std::smatch match;
    if (!std::regex_search (log, match, operands)) {
	return false;
    }
    const int line = std::stoi (match[1].str ());
    const int a = std::stoi (match[3].str ()), b = std::stoi (match[5].str ());
    const int wider = std::max (a, b), narrower = std::min (a, b);
    const std::string storage = a > b ? match[2].str () : match[4].str ();
    if (wider == narrower) {
	return false;
    }
    Line at;
    if (!lineAt (source, line, at)) {
	return false;
    }
    const std::string& text = at.text;
    static const char* swizzles[] = { "", ".x", ".xy", ".xyz" };
    static const std::regex identifier (R"(\b([A-Za-z_][A-Za-z0-9_]*)\b(?!\s*[.(\[])(?![A-Za-z0-9_]))");
    static const std::regex typeBefore (R"((?:\b(?:vec[234]|float[234]|float|int|bool|mat[234]|const)\s+)$)");
    // the wider operand's storage: varyings are `in` (fragment), uniforms `uniform`, the rest temporaries
    const bool wantsInput = storage.find ("in ") != std::string::npos || storage.find ("smooth") != std::string::npos;
    const bool wantsUniform = storage.find ("uniform") != std::string::npos;

    for (int pass = 0; pass < 2; pass++) {
	std::string rebuilt;
	size_t last = 0;
	bool any = false;
	for (auto it = std::sregex_iterator (text.begin (), text.end (), identifier); it != std::sregex_iterator (); ++it) {
	    const std::string name = (*it)[1].str ();
	    const size_t pos = static_cast<size_t> (it->position ());
	    if ((pos > 0 && text[pos - 1] == '.') || std::regex_search (text.substr (0, pos), typeBefore)) {
		continue;
	    }
	    const std::string wide = "\\bvec" + std::to_string (wider) + "\\s+" + name + "\\b";
	    if (!std::regex_search (source, std::regex (wide))) {
		continue;
	    }
	    if (pass == 0 && (wantsInput || wantsUniform)) {
		const std::string q = wantsInput ? "(?:varying|in)" : "uniform";
		if (!std::regex_search (source, std::regex ("\\b" + q + "\\s+(?:[A-Za-z_]+\\s+)*vec" + std::to_string (wider) + "\\s+" + name + "\\b"))) {
		    continue;
		}
	    }
	    rebuilt += text.substr (last, pos + name.size () - last) + swizzles[narrower];
	    last = pos + name.size ();
	    any = true;
	}
	if (!any) {
	    continue;
	}
	rebuilt += text.substr (last);
	sLog.out ("Truncating to vec", narrower, " as HLSL does, line ", line, ": ", rebuilt);
	WPENGINE_HEALTH ("compat", { { "what", "glslang: wider vector truncated as HLSL does" } });
	replaceLine (source, at, rebuilt);
	return true;
    }
    return false;
}

/** Brace depth at the start of `offset` (comments skipped) */
int depthAt (const std::string& source, size_t offset) {
    int depth = 0;
    for (size_t i = 0; i < offset && i < source.size (); i++) {
	if (source.compare (i, 2, "//") == 0) {
	    i = std::min (source.find ('\n', i), offset);
	    continue;
	}
	if (source.compare (i, 2, "/*") == 0) {
	    const size_t end = source.find ("*/", i + 2);
	    i = end == std::string::npos ? offset : end + 1;
	    continue;
	}
	depth += source[i] == '{' ? 1 : source[i] == '}' ? -1 : 0;
    }
    return depth;
}

/**
 * HLSL's `const`: a local one is a read-only variable with any initializer (2795521260's color_grading: `const float
 * cosAngle = cos(radians(u_hueShift));`) — the `const` goes. A global one is WE's `static const` (GH 0x140125e80
 * adds `static `), which FXC initialises before main from any expression, uniforms included (`const mat3
 * colorMatrix = mat3(c_red, c_green, c_blue);`) — it becomes a global set at the top of main.
 */
bool constInitializer (std::string& source, const std::string& log) {
    static const std::regex error (
	R"(ERROR: 0:(\d+): '(?:non-constant initializer' :|[^']*' :\s+global const initializers must be constant))"
    );
    static const std::regex constDecl (R"(^(\s*)const\s+(?:(?:lowp|mediump|highp)\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*=\s*(.*);\s*(//.*)?$)");
    std::smatch m;
    Line line;
    if (!std::regex_search (log, m, error) || !lineAt (source, std::stoi (m[1].str ()), line)) {
	return false;
    }
    // inside a function: by brace depth, or by indentation (a unit's #if branches may open a function twice)
    if (depthAt (source, line.begin) > 0 || (!line.text.empty () && (line.text[0] == ' ' || line.text[0] == '\t'))) {
	const size_t at = line.text.find ("const ");
	if (at == std::string::npos) {
	    return false;
	}
	WPENGINE_HEALTH ("compat", { { "what", "HLSL: a local const with a run-time initializer" } });
	replaceLine (source, line, line.text.substr (0, at) + line.text.substr (at + 6));
	return true;
    }
    std::smatch d;
    if (!std::regex_match (line.text, d, constDecl) || d[4].matched) {
	return false;
    }
    static const std::regex mainOpen (R"(\bvoid\s+main\s*\(\s*(?:void)?\s*\)\s*\{)");
    std::smatch open;
    const std::string marker = "/* _wp_static_inits */";
    const std::string init = "\t" + d[3].str () + " = " + d[5].str () + ";\n";
    replaceLine (source, line, d[1].str () + d[2].str () + " " + d[3].str () + "; // static const, set in main");
    // every main (a unit may have one per #if branch): after the inits already there, in declaration order
    if (source.find (marker) != std::string::npos) {
	for (size_t at = source.find (marker); at != std::string::npos; at = source.find (marker, at + init.size () + marker.size ())) {
	    source.insert (at, init);
	}
    } else if (std::regex_search (source, open, mainOpen)) {
	source = std::regex_replace (source, mainOpen, "$&\n" + init + marker);
    } else {
	return false;
    }
    WPENGINE_HEALTH ("compat", { { "what", "HLSL: a static const global initialised in main" } });
    return true;
}

/**
 * A function that doesn't return on the path the combos leave (2821337237's shadow_map: ShadowMask returns only
 * under `#if SHADOWMASK == N`, and SHADOWMASK 0 — "None" — leaves it empty) compiles in FXC when nothing calls it;
 * glslang refuses it. It returns its type's zero at its end
 */
bool missingReturn (std::string& source, const std::string& log) {
    static const std::regex error (R"(ERROR: 0:\d+: '[^']*' : function does not return a value: ([A-Za-z_][A-Za-z0-9_]*))");
    std::smatch m;
    if (!std::regex_search (log, m, error)) {
	return false;
    }
    const std::string name = m[1].str ();
    const std::regex definition ("\\b((?:float|int|uint|bool|[biu]?vec[234]|mat[234]))\\s+" + name + "\\s*\\([^;{]*\\)\\s*\\{");
    std::smatch d;
    std::string::const_iterator from = source.cbegin ();
    while (std::regex_search (from, source.cend (), d, definition)) {
	const size_t open = static_cast<size_t> (d.position () + d.length ()) + static_cast<size_t> (from - source.cbegin ());
	int depth = 1;
	size_t i = open;
	for (; i < source.size () && depth > 0; i++) {
	    depth += source[i] == '{' ? 1 : source[i] == '}' ? -1 : 0;
	}
	if (depth != 0) {
	    return false;
	}
	const std::string ret = "\treturn " + d[1].str () + "(0);\n";
	if (source.compare (i - 1 - ret.size (), ret.size (), ret) != 0) {
	    source.insert (i - 1, ret);
	    WPENGINE_HEALTH ("compat", { { "what", "HLSL: a function missing its return returns zero" } });
	    return true;
	}
	from = source.cbegin () + static_cast<long> (i);
    }
    return false;
}

/**
 * HLSL takes any scalar as a condition (`vec4 final = outside ? inSmooth : outSmooth;` with a float `outside`,
 * 3573886911's frame_builder; `if (mask)`): it is compared with zero
 */
bool booleanCondition (std::string& source, const std::string& log) {
    static const std::regex error (R"(ERROR: 0:(\d+): '[^']*' : boolean expression expected)");
    std::smatch m;
    Line line;
    if (!std::regex_search (log, m, error) || !lineAt (source, std::stoi (m[1].str ()), line)) {
	return false;
    }
    const std::string& text = line.text;
    // a ternary: the operand before `?` (back to `=`, `(`, `,`, `return` or the line's start)
    const size_t q = text.find ('?');
    if (q != std::string::npos) {
	size_t b = q;
	int depth = 0;
	while (b > 0) {
	    const char c = text[b - 1];
	    if (c == ')') {
		depth++;
	    } else if (c == '(') {
		if (depth == 0) {
		    break;
		}
		depth--;
	    } else if (depth == 0 && (c == '=' || c == ',' || c == ':' || c == '?')) {
		break;
	    }
	    b--;
	}
	std::string cond = text.substr (b, q - b);
	if (cond.find ("return ") != std::string::npos) {
	    b += cond.find ("return ") + 7;
	    cond = text.substr (b, q - b);
	}
	if (cond.find_first_not_of (" \t") == std::string::npos || cond.find ("bool(") != std::string::npos) {
	    return false;
	}
	replaceLine (source, line, text.substr (0, b) + " bool(" + cond + ") " + text.substr (q));
	WPENGINE_HEALTH ("compat", { { "what", "HLSL: a scalar condition" } });
	return true;
    }
    // if / while: the parenthesised condition
    static const std::regex statement (R"(\b(if|while)\s*\()");
    std::smatch s;
    if (std::regex_search (text, s, statement)) {
	const size_t open = static_cast<size_t> (s.position () + s.length ());
	int depth = 1;
	size_t i = open;
	for (; i < text.size () && depth > 0; i++) {
	    depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
	}
	if (depth != 0) {
	    return false;
	}
	const std::string cond = text.substr (open, i - 1 - open);
	if (cond.starts_with ("bool(")) {
	    return false;
	}
	replaceLine (source, line, text.substr (0, open) + "bool(" + cond + ")" + text.substr (i - 1));
	WPENGINE_HEALTH ("compat", { { "what", "HLSL: a scalar condition" } });
	return true;
    }
    return false;
}

/**
 * WE's engine uniforms g_Texture0Texel … g_Texture9Texel (the name table GH 0x140002a20, ids 0x49–0x52) exist
 * whether a shader declares them or not: the stock shadowcasterfoliage4 reads g_Texture1Texel under MORPHING and
 * declares only g_Texture1Resolution. From glslang's "undeclared identifier", the uniform is declared at the top of
 * the unit; CPass gives it (1 / width, 1 / height, width, height), the form model_vertex_v1.h's morph lookups read
 * (Inferred: the setter wasn't read)
 */
bool declareTexelUniform (std::string& source, const std::string& log) {
    static const std::regex error (R"(ERROR: 0:\d+: '(g_Texture\d+Texel)' : undeclared identifier)");
    std::smatch m;
    if (!std::regex_search (log, m, error)) {
	return false;
    }
    const std::string declaration = "uniform vec4 " + m[1].str () + ";\n";
    size_t at = 0;
    if (source.rfind ("#version", 0) == 0) {
	at = source.find ('\n');
	at = at == std::string::npos ? source.size () : at + 1;
    }
    source.insert (at, declaration);
    WPENGINE_HEALTH ("compat", { { "what", "WE: an engine texel uniform declared" } });
    return true;
}

/** Parses a unit, retrying after each HLSL rule it needed (`source` must outlive the shader) */
std::unique_ptr<glslang::TShader> parseUnit (EShLanguage stage, std::string& source, const char* what) {
    for (int attempt = 0;; attempt++) {
	auto shader = std::make_unique<glslang::TShader> (stage);
	const char* text = source.c_str ();
	shader->setStrings (&text, 1);
	shader->setEntryPoint ("main");
	shader->setEnvInput (glslang::EShSourceGlsl, stage, glslang::EShClientOpenGL, 330);
	shader->setEnvClient (glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
	shader->setEnvTarget (glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
	shader->setAutoMapLocations (true);
	shader->setAutoMapBindings (true);

	if (shader->parse (&BuiltInResource, 100, false, EShMsgDefault)) {
	    return shader;
	}

	const std::string log = shader->getInfoLog ();
	shader.reset ();
	const bool fixed = attempt < 64
	    && (truncateWiderOperands (source, log) || convertAssignment (source, log) || constInitializer (source, log)
		|| missingReturn (source, log) || booleanCondition (source, log) || convertArguments (source, log)
		|| declareTexelUniform (source, log));
	if (!fixed) {
	    sLog.error ("GLSL ", what, " unit parsing Failed: ", log);
	    WPENGINE_HEALTH ("shader_error", { { "stage", "glslang" }, { "unit", what }, { "message", WallpaperEngine::Health::brief (log) } });
	    return nullptr;
	}
    }
}

/** toGlsl without its cache */
std::pair<std::string, std::string> translate (const std::string& vertexIn, const std::string& fragmentIn) {
    std::string vertex = dropUnmatchedEndifs (spliceLines (vertexIn));
    std::string fragment = dropUnmatchedEndifs (spliceLines (fragmentIn));
    if (WallpaperEngine::Health::enabled () && (vertex.size () != vertexIn.size () || fragment.size () != fragmentIn.size ())
	&& (spliceLines (vertexIn) != vertexIn || spliceLines (fragmentIn) != fragmentIn)) {
	WallpaperEngine::Health::report ("compat", { { "what", "glslang: backslash line continuation spliced" } });
    }
    const auto vertexShader = parseUnit (EShLangVertex, vertex, "vertex");

    if (vertexShader == nullptr) {
	return { "", "" };
    }

    const auto fragmentShader = parseUnit (EShLangFragment, fragment, "fragment");

    if (fragmentShader == nullptr) {
	return { "", "" };
    }
    glslang::TProgram program;
    program.addShader (vertexShader.get ());
    program.addShader (fragmentShader.get ());

    if (!program.link (EShMsgDefault)) {
	sLog.error ("Program Linking Failed: ", program.getInfoLog ());
	WPENGINE_HEALTH ("shader_error", { { "stage", "glslang" }, { "unit", "link" }, { "message", WallpaperEngine::Health::brief (program.getInfoLog ()) } });
	return { "", "" };
    }

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv (*program.getIntermediate (EShLangVertex), spirv);

    spirv_cross::CompilerGLSL vertexCompiler (spirv);
    spirv_cross::CompilerGLSL::Options options;
    options.version = 330;
    options.es = false;
    vertexCompiler.set_common_options (options);

    spirv.clear ();
    glslang::GlslangToSpv (*program.getIntermediate (EShLangFragment), spirv);

    spirv_cross::CompilerGLSL fragmentCompiler (spirv);
    options.version = 330;
    options.es = false;
    fragmentCompiler.set_common_options (options);

    return { vertexCompiler.compile () + "#if 0\n" + vertex + "\n#endif",
	     fragmentCompiler.compile () + "#if 0\n" + fragment + "\n#endif" };
}
} // namespace

std::pair<std::string, std::string> GLSLContext::toGlsl (
    const std::string& vertexIn, const std::string& fragmentIn, const std::optional<std::filesystem::path>& cacheDir
) {
    // the health report and the shader sweep's capture read what the compile itself reports (its rewrites, its
    // errors), which a stored result would skip
    if (cacheDir.has_value () && !WallpaperEngine::Health::enabled ()) {
	if (auto stored = ShaderCache::load (*cacheDir, "glsl", { vertexIn, fragmentIn }); stored && stored->size () == 2) {
	    return { std::move ((*stored)[0]), std::move ((*stored)[1]) };
	}
    }
    auto results = translate (vertexIn, fragmentIn);
    // a failed translation ({"", ""}) is done again: its errors are logged each time
    if (cacheDir.has_value () && !results.first.empty () && !results.second.empty ()) {
	ShaderCache::store (*cacheDir, "glsl", { vertexIn, fragmentIn }, { results.first, results.second });
    }
    return results;
}

std::unique_ptr<GLSLContext> GLSLContext::sInstance = nullptr;