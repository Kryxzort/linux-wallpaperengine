#include "GLSLContext.h"
#include "WallpaperEngine/Logging/Log.h"

#include <cassert>
#include <memory>
#include <regex>

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
/**
 * The preprocessor's line splicing: a backslash at the end of a line joins it to the next. WE's shaders are
 * HLSL-era sources whose compiler splices (3414858021's depthparallax.frag continues an expression over lines);
 * glslang only does so from GLSL 4.20, so a 3.30 unit failed to parse and its whole layer wasn't built. The lines
 * taken out are put back after the joined one, so the line numbers in errors still match the source.
 */
std::string spliceLines (const std::string& source) {
    std::string out;
    out.reserve (source.size ());
    int pending = 0;

    for (size_t i = 0; i < source.size (); i++) {
	if (source[i] == '\\') {
	    size_t next = i + 1;
	    if (next < source.size () && source[next] == '\r') {
		next++;
	    }
	    if (next < source.size () && source[next] == '\n') {
		pending++;
		i = next;
		continue;
	    }
	}
	out += source[i];
	if (source[i] == '\n' && pending > 0) {
	    out.append (static_cast<size_t> (pending), '\n');
	    pending = 0;
	}
    }

    return out;
}
/**
 * An `#endif` with no open `#if` (2973943998's iris_movement__.vert has a stray one after its FOLLOWCURSOR block)
 * is dropped, as WE's shader compiler builds such effects; glslang stops at it. The line stays (empty) so the
 * line numbers hold.
 */
std::string dropUnmatchedEndifs (const std::string& source) {
    static const std::regex opens (R"(^\s*#\s*if(?:n?def)?\b)");
    static const std::regex closes (R"(^\s*#\s*endif\b)");
    std::string out;
    out.reserve (source.size ());
    int depth = 0;
    size_t begin = 0;

    while (begin < source.size ()) {
	size_t end = source.find ('\n', begin);
	end = end == std::string::npos ? source.size () : end + 1;
	const std::string line = source.substr (begin, end - begin);
	if (std::regex_search (line, opens)) {
	    depth++;
	} else if (std::regex_search (line, closes)) {
	    if (depth == 0) {
		sLog.out ("Dropping an #endif with no #if: ", line);
		out += line.back () == '\n' ? "\n" : "";
		begin = end;
		continue;
	    }
	    depth--;
	}
	out += line;
	begin = end;
    }

    return out;
}

/**
 * HLSL's implicit truncation: an operation between a wider and a narrower float vector (or a narrower variable
 * taking a wider value) uses the wider one's first components, with a warning. GLSL refuses it; WE's effects rely
 * on it (3414858021's iris_movement__: `vec2 da = transformedCursorPosition * g_CursorScale`, a vec4 times a vec2,
 * failed the compose layer holding the girl's head). From glslang's error, the line's variables declared with the
 * wider type get the narrower swizzle; false when the error isn't one of these or nothing changed.
 */
bool truncateWiderOperands (std::string& source, const std::string& log) {
    static const std::regex operands (
	R"(ERROR: 0:(\d+): '[^']*' :\s+wrong operand types: no operation '[^']*' exists that takes a left-hand operand of type ' (?:[a-z]+ )*(\d)-component vector of float' and a right operand of type ' (?:[a-z]+ )*(\d)-component vector of float')"
    );
    static const std::regex conversion (
	R"(ERROR: 0:(\d+): '[^']*' :\s+cannot convert from ' (?:[a-z]+ )*(\d)-component vector of float' to ' (?:[a-z]+ )*(\d)-component vector of float')"
    );
    std::smatch match;
    int line = 0, wider = 0, narrower = 0;

    if (std::regex_search (log, match, operands) || std::regex_search (log, match, conversion)) {
	line = std::stoi (match[1].str ());
	const int a = std::stoi (match[2].str ()), b = std::stoi (match[3].str ());
	wider = std::max (a, b);
	narrower = std::min (a, b);
    }
    if (line <= 0 || wider == narrower) {
	return false;
    }

    size_t begin = 0;
    for (int i = 1; i < line && begin != std::string::npos; i++) {
	begin = source.find ('\n', begin);
	begin = begin == std::string::npos ? begin : begin + 1;
    }
    if (begin == std::string::npos) {
	return false;
    }
    const size_t end = std::min (source.find ('\n', begin), source.size ());
    const std::string text = source.substr (begin, end - begin);
    static const char* swizzles[] = { "", ".x", ".xy", ".xyz" };
    // a bare identifier (not a call, not already swizzled or indexed)
    static const std::regex identifier (R"(\b([A-Za-z_][A-Za-z0-9_]*)\b(?!\s*[.(\[])(?![A-Za-z0-9_]))");
    std::string rebuilt;
    size_t at = 0;

    for (auto it = std::sregex_iterator (text.begin (), text.end (), identifier); it != std::sregex_iterator (); ++it) {
	const std::string name = (*it)[1].str ();
	const std::regex declared ("\\bvec" + std::to_string (wider) + "\\s+" + name + "\\b");
	// a preceding '.' makes it a member or swizzle, not a variable
	const size_t pos = static_cast<size_t> (it->position ());
	const bool member = pos > 0 && text[pos - 1] == '.';
	if (member || !std::regex_search (source, declared)) {
	    continue;
	}
	rebuilt += text.substr (at, pos + name.size () - at) + swizzles[narrower];
	at = pos + name.size ();
    }
    if (at == 0) {
	return false;
    }

    rebuilt += text.substr (at);
    sLog.out ("Truncating to vec", narrower, " as HLSL does, line ", line, ": ", rebuilt);
    source.replace (begin, end - begin, rebuilt);
    return true;
}

/** Parses a unit, retrying after each HLSL-style truncation it needed (`source` must outlive the shader) */
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
	if (attempt >= 16 || !truncateWiderOperands (source, log)) {
	    sLog.error ("GLSL ", what, " unit parsing Failed: ", log);
	    return nullptr;
	}
    }
}
} // namespace

std::pair<std::string, std::string> GLSLContext::toGlsl (const std::string& vertexIn, const std::string& fragmentIn) {
    std::string vertex = dropUnmatchedEndifs (spliceLines (vertexIn));
    std::string fragment = dropUnmatchedEndifs (spliceLines (fragmentIn));
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

std::unique_ptr<GLSLContext> GLSLContext::sInstance = nullptr;