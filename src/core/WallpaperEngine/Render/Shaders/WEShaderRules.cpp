#include "WEShaderRules.h"

#include "WallpaperEngine/Health.h"
#include "WallpaperEngine/Logging/Log.h"

#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WallpaperEngine::Render::Shaders::WERules {
namespace {
std::vector<std::string> splitLines (const std::string& source) {
    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin <= source.size ()) {
	const size_t end = source.find ('\n', begin);
	if (end == std::string::npos) {
	    lines.push_back (source.substr (begin));
	    break;
	}
	lines.push_back (source.substr (begin, end - begin));
	begin = end + 1;
    }
    return lines;
}

std::string joinLines (const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size (); i++) {
	out += lines[i];
	if (i + 1 < lines.size ()) {
	    out += '\n';
	}
    }
    return out;
}

/** The line without its `//` comment (block comments are left: the callers only look at directive lines) */
std::string withoutLineComment (const std::string& line) {
    const size_t at = line.find ("//");
    return at == std::string::npos ? line : line.substr (0, at);
}

const std::regex& directive () {
    static const std::regex re (R"(^\s*#\s*([a-z]+)\b\s*([^\n]*)$)");
    return re;
}
} // namespace

std::string expandIncludes (const std::string& source, const IncludeLoader& load, std::set<std::string>& seen) {
    std::string out;
    out.reserve (source.size ());
    for (const auto& line : splitLines (source)) {
	// GH 0x140191050: only a line that begins with the 8 bytes "#include"
	if (line.compare (0, 8, "#include") != 0) {
	    out += line;
	    out += '\n';
	    continue;
	}
	const size_t open = line.find ('"');
	const size_t close = open == std::string::npos ? std::string::npos : line.find ('"', open + 1);
	if (close == std::string::npos) {
	    out += "\n";
	    continue;
	}
	const std::string name = line.substr (open + 1, close - open - 1);
	if (seen.contains (name)) {
	    out += "// already included from file " + name + "\n";
	    continue;
	}
	const auto content = load (name);
	if (!content.has_value ()) {
	    out += "// tried including file " + name + " but was not found\n";
	    continue;
	}
	seen.insert (name);
	out += "// begin of include from file " + name + "\n";
	out += expandIncludes (*content, load, seen);
	out += "// end of included from file " + name + "\n";
    }
    if (!out.empty () && (source.empty () || source.back () != '\n')) {
	out.pop_back ();
    }
    return out;
}

namespace {
/** The lines of `source` split at '\n' as splitLines does, as views into it */
std::vector<std::string_view> lineViews (std::string_view source) {
    std::vector<std::string_view> lines;
    size_t begin = 0;
    while (true) {
	const size_t end = source.find ('\n', begin);
	if (end == std::string_view::npos) {
	    lines.push_back (source.substr (begin));
	    return lines;
	}
	lines.push_back (source.substr (begin, end - begin));
	begin = end + 1;
    }
}

/** `[A-Za-z_]`: where `[A-Za-z_][A-Za-z0-9_]*` can start */
bool startsIdentifier (char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/**
 * Calls `fn (identifier)` for each match of `[A-Za-z_][A-Za-z0-9_]*` in `text`, in the order an sregex_iterator lists
 * them (each the longest run from the leftmost place one can start, the next search from its end), until `fn` returns
 * true; returns whether it did
 */
template <typename F> bool anyIdentifier (std::string_view text, F fn) {
    size_t at = 0;
    while (at < text.size ()) {
	if (!startsIdentifier (text[at])) {
	    at++;
	    continue;
	}
	size_t end = at + 1;
	while (end < text.size () && Scan::isWord (text[end])) {
	    end++;
	}
	if (fn (text.substr (at, end - at))) {
	    return true;
	}
	at = end;
    }
    return false;
}

/** A line that `directive ()` matches whole: group 1 (`kind`) and group 2 (`rest`) */
struct DirectiveLine {
    std::string_view kind;
    std::string_view rest;
};

/**
 * Whether `^\s*#\s*([a-z]+)\b\s*([^\n]*)$` matches the whole line (one without '\n'), and its groups when it does: the
 * `[a-z]` run after the `#` (no other word character may follow it), then the line after the run and the `\s` after it
 */
std::optional<DirectiveLine> readDirective (std::string_view line) {
    size_t at = Scan::skipSpace (line, 0);
    if (at >= line.size () || line[at] != '#') {
	return std::nullopt;
    }
    at = Scan::skipSpace (line, at + 1);
    const size_t begin = at;
    while (at < line.size () && line[at] >= 'a' && line[at] <= 'z') {
	at++;
    }
    if (at == begin || (at < line.size () && Scan::isWord (line[at]))) {
	return std::nullopt;
    }
    return DirectiveLine { line.substr (begin, at - begin), line.substr (Scan::skipSpace (line, at)) };
}

bool isStorageWord (std::string_view word) {
    return word == "uniform" || word == "attribute" || word == "varying" || word == "in" || word == "out";
}

bool isQualifierWord (std::string_view word) {
    return word == "lowp" || word == "mediump" || word == "highp" || word == "flat" || word == "smooth"
	|| word == "noperspective" || word == "centroid";
}

bool is234 (char c) {
    return c == '2' || c == '3' || c == '4';
}

/** Whether `rest` is empty or `x[234]` */
bool isEmptyOrColumns (std::string_view rest) {
    return rest.empty () || (rest.size () == 2 && rest[0] == 'x' && is234 (rest[1]));
}

/**
 * Whether the word is one of `float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?|sampler[A-Za-z0-9]*|
 * float[234](?:x[234])?|int[234]` whole
 */
bool isTypeWord (std::string_view word) {
    if (word == "float" || word == "int" || word == "uint" || word == "bool") {
	return true;
    }
    if (word.starts_with ("sampler")) {
	for (const char c : word.substr (7)) {
	    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
		return false;
	    }
	}
	return true;
    }
    std::string_view vec = word;
    if (!vec.empty () && (vec[0] == 'b' || vec[0] == 'i' || vec[0] == 'u')) {
	vec.remove_prefix (1);
    }
    if (vec.size () == 4 && vec.starts_with ("vec") && is234 (vec[3])) {
	return true;
    }
    if (word.size () >= 4 && word.starts_with ("mat") && is234 (word[3])) {
	return isEmptyOrColumns (word.substr (4));
    }
    if (word.size () >= 6 && word.starts_with ("float") && is234 (word[5])) {
	return isEmptyOrColumns (word.substr (6));
    }
    return word.size () == 4 && word.starts_with ("int") && is234 (word[3]);
}

/**
 * Whether the declaration regex
 * `^\s*(?:uniform|attribute|varying|in|out)\s+(?:(?:lowp|mediump|highp|flat|smooth|noperspective|centroid)\s+)*`
 * `(?:<a type of isTypeWord>)\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:\[[^\]]*\])?\s*;\s*(?://[^\n]*)?$` matches the whole line
 * (one without '\n'). Every word of its head is followed by `\s+`, so each is a whole `\w` run between spaces, and the
 * qualifiers are no types: the line's words decide it in order, with no other way for the regex to split them
 */
bool isDeclaration (std::string_view line) {
    size_t at = Scan::skipSpace (line, 0);
    const auto word = [&] {
	const size_t begin = at;
	while (at < line.size () && Scan::isWord (line[at])) {
	    at++;
	}
	return line.substr (begin, at - begin);
    };
    const auto spaces = [&] {
	if (at >= line.size () || !Scan::isSpace (line[at])) {
	    return false;
	}
	at = Scan::skipSpace (line, at);
	return true;
    };
    if (!isStorageWord (word ()) || !spaces ()) {
	return false;
    }
    std::string_view next = word ();
    while (isQualifierWord (next)) {
	if (!spaces ()) {
	    return false;
	}
	next = word ();
    }
    if (!isTypeWord (next) || !spaces ()) {
	return false;
    }
    if (at >= line.size () || !startsIdentifier (line[at])) {
	return false;
    }
    word ();
    at = Scan::skipSpace (line, at);
    if (at < line.size () && line[at] == '[') {
	const size_t close = line.find (']', at + 1);
	if (close == std::string_view::npos) {
	    return false;
	}
	at = Scan::skipSpace (line, close + 1);
    }
    if (at >= line.size () || line[at] != ';') {
	return false;
    }
    at = Scan::skipSpace (line, at + 1);
    return at == line.size () || line.substr (at, 2) == "//";
}

/** What hoistDeclarations returns for a text: `text` is empty when it returns its input */
struct Hoisted {
    std::string text;
    bool moved = false;
};

Hoisted hoist (const std::string& source) {
    const auto lines = lineViews (source);
    std::vector<std::vector<std::string_view>> chains; // the open #if chains, outermost first
    std::vector<size_t> chainStart;                    // the line each chain's #if is on
    int braces = 0, parens = 0;
    bool block = false;
    long hoistAt = -1;
    std::set<std::string, std::less<>> definedSince;
    std::string hoisted;
    std::vector<bool> moved (lines.size (), false);
    bool anyMoved = false;

    for (size_t i = 0; i < lines.size (); i++) {
	const std::string_view line = lines[i];
	const bool topLevel = braces == 0 && parens == 0 && !block;
	if (!block) {
	    if (const auto directive = readDirective (line)) {
		const std::string_view kind = directive->kind;
		if (kind == "if" || kind == "ifdef" || kind == "ifndef") {
		    chains.push_back ({ line });
		    chainStart.push_back (i);
		} else if ((kind == "elif" || kind == "else") && !chains.empty ()) {
		    chains.back ().push_back (line);
		} else if (kind == "endif" && !chains.empty ()) {
		    chains.pop_back ();
		    chainStart.pop_back ();
		} else if ((kind == "define" || kind == "undef") && hoistAt >= 0) {
		    anyIdentifier (directive->rest, [&] (std::string_view name) {
			definedSince.emplace (name);
			return true;
		    });
		}
		continue;
	    }
	}
	if (hoistAt < 0 && topLevel && line.find ("// begin of include from file ") != std::string_view::npos) {
	    // the first include: before the outermost #if it is in, so the whole chain stays below
	    hoistAt = chainStart.empty () ? static_cast<long> (i) : static_cast<long> (chainStart.front ());
	    // the chains open at that point are above the hoisted block: a declaration in them keeps only the inner ones
	} else if (hoistAt >= 0 && topLevel && isDeclaration (line)) {
	    const auto local = [&] (std::string_view name) { return definedSince.contains (name); };
	    // the chains the hoisted copy needs: those opened after the hoist point
	    std::string open, close;
	    bool readsLocalDefine = false;
	    for (size_t c = 0; c < chains.size (); c++) {
		if (static_cast<long> (chainStart[c]) < hoistAt) {
		    continue;
		}
		for (const auto& d : chains[c]) {
		    open += d;
		    open += '\n';
		    readsLocalDefine = readsLocalDefine || anyIdentifier (d.substr (0, d.find ("//")), local);
		}
		close += "#endif\n";
	    }
	    readsLocalDefine = readsLocalDefine || anyIdentifier (line, local);
	    if (!readsLocalDefine) {
		hoisted += open;
		hoisted += line;
		hoisted += '\n';
		hoisted += close;
		moved[i] = true;
		anyMoved = true;
		// the line is now empty: no scope to track in it
		continue;
	    }
	}
	// track the scope for the next line
	for (size_t c = 0; c < line.size (); c++) {
	    if (block) {
		if (line.substr (c, 2) == "*/") {
		    block = false;
		    c++;
		}
		continue;
	    }
	    if (line.substr (c, 2) == "//") {
		break;
	    }
	    if (line.substr (c, 2) == "/*") {
		block = true;
		c++;
		continue;
	    }
	    switch (line[c]) {
		case '{': braces++; break;
		case '}': braces = std::max (0, braces - 1); break;
		case '(': parens++; break;
		case ')': parens = std::max (0, parens - 1); break;
		default: break;
	    }
	}
    }

    if (!anyMoved || hoistAt < 0) {
	return {};
    }
    Hoisted result { .text = {}, .moved = true };
    result.text.reserve (source.size () + hoisted.size () + 96);
    for (size_t i = 0; i < lines.size (); i++) {
	if (i > 0) {
	    result.text += '\n';
	}
	if (static_cast<long> (i) == hoistAt) {
	    result.text += "// declarations hoisted as WE's cbuffers and structs are\n";
	    result.text += hoisted;
	    result.text += "// end of hoisted declarations\n";
	}
	if (!moved[i]) {
	    result.text += lines[i];
	}
    }
    return result;
}
} // namespace

std::string hoistDeclarations (const std::string& source) {
    // a unit's text repeats: one effect shader serves many passes (3042492564 builds 1025 units from 68 shader
    // pairs), and the result is a function of the text alone, so each text is worked out once per process
    static std::mutex lock;
    static std::unordered_map<std::string, Hoisted> memo;
    static size_t memoBytes = 0;
    static constexpr size_t kMemoBytes = 64u << 20;

    Hoisted result;
    bool known = false;
    {
	std::scoped_lock guard (lock);
	if (const auto it = memo.find (source); it != memo.end ()) {
	    result = it->second;
	    known = true;
	}
    }
    if (!known) {
	result = hoist (source);
	std::scoped_lock guard (lock);
	const size_t bytes = source.size () + result.text.size ();
	if (memoBytes + bytes > kMemoBytes) {
	    memo.clear ();
	    memoBytes = 0;
	}
	if (memo.emplace (source, result).second) {
	    memoBytes += bytes;
	}
    }
    if (!result.moved) {
	return source;
    }
    WPENGINE_HEALTH ("compat", { { "what", "WE: declarations hoisted above the includes" } });
    return std::move (result.text);
}

namespace {
/** Applies `fn (identifier, position, text)` → replacement to every identifier outside comments */
template <typename F> std::string mapIdentifiers (const std::string& source, F fn) {
    std::string out;
    out.reserve (source.size ());
    size_t i = 0;
    const size_t n = source.size ();
    while (i < n) {
	const char c = source[i];
	if (c == '/' && i + 1 < n && source[i + 1] == '/') {
	    const size_t end = source.find ('\n', i);
	    const size_t stop = end == std::string::npos ? n : end;
	    out.append (source, i, stop - i);
	    i = stop;
	    continue;
	}
	if (c == '/' && i + 1 < n && source[i + 1] == '*') {
	    const size_t end = source.find ("*/", i + 2);
	    const size_t stop = end == std::string::npos ? n : end + 2;
	    out.append (source, i, stop - i);
	    i = stop;
	    continue;
	}
	if (std::isalpha (static_cast<unsigned char> (c)) || c == '_') {
	    size_t j = i;
	    while (j < n && (std::isalnum (static_cast<unsigned char> (source[j])) || source[j] == '_')) {
		j++;
	    }
	    const bool member = i > 0 && source[i - 1] == '.';
	    const std::string word = source.substr (i, j - i);
	    out += member ? word : fn (word, j);
	    i = j;
	    continue;
	}
	if (std::isdigit (static_cast<unsigned char> (c))) {
	    // a number with its suffix (1.0f, 2u, 1e-3) is not an identifier
	    size_t j = i;
	    while (j < n && (std::isalnum (static_cast<unsigned char> (source[j])) || source[j] == '.' || source[j] == '_'
			     || ((source[j] == '-' || source[j] == '+') && (source[j - 1] == 'e' || source[j - 1] == 'E')))) {
		j++;
	    }
	    out.append (source, i, j - i);
	    i = j;
	    continue;
	}
	out += c;
	i++;
    }
    return out;
}
} // namespace

std::string renameReservedWords (const std::string& source) {
    // GLSL 3.30's keywords and reserved words that HLSL leaves free for a name (HLSL's own keywords — static,
    // inline, extern, volatile, shared, sample, … — would not compile in WE either, so they are not here)
    static const std::set<std::string> reserved = {
	"input",	 "output",    "filter",	    "common",	 "partition", "active",   "asm",       "class",
	"union",	 "enum",      "typedef",    "template",	 "this",      "packed",	  "goto",      "noinline",
	"public",	 "external",  "interface",  "superp",	 "sizeof",    "cast",	  "namespace", "using",
	"resource",	 "patch",     "subroutine", "fvec2",	 "fvec3",     "fvec4",	  "hvec2",     "hvec3",
	"hvec4",	 "smooth",    "flat",	    "invariant", "layout",    "lowp",	  "mediump",   "highp",
	"precision",	 "coherent",  "restrict",   "readonly",	 "writeonly", "buffer",	  "atomic_uint",
	"noperspective", "centroid", "sampler3DRect", "image1D", "image2D",  "image3D", "iimage2D",  "uimage2D",
    };
    // a qualifier the source itself writes as GLSL (`flat in`, `precision highp float`, `layout(…)`) stays
    static const std::set<std::string> qualifiers = { "smooth",	   "flat",     "invariant", "layout",
						      "lowp",	   "mediump",  "highp",	    "precision",
						      "noperspective", "centroid" };
    static const std::regex textureVariable (
	R"(\b(?:float|int|uint|bool|[biu]?vec[234]|mat[234]|sampler2D|float[234])\s+texture\s*[;=,\[)])"
    );
    // the regex needs `<type>\s+texture\s*<one of ;=,[)>`
    const bool textureIsAVariable = Scan::anyWord (source, "texture", [&] (size_t at, size_t end) {
	const size_t k = Scan::skipSpace (source, end);
	return !Scan::wordBefore (source, at).empty () && k < source.size ()
	    && std::string_view (";=,[)").find (source[k]) != std::string_view::npos;
    }) && std::regex_search (source, textureVariable);
    bool renamed = false;

    std::string out = mapIdentifiers (source, [&] (const std::string& word, size_t after) -> std::string {
	if (reserved.contains (word)) {
	    if (qualifiers.contains (word)) {
		// a qualifier is followed by another word (a type or a qualifier) or `(` for layout; a name isn't
		size_t k = after;
		while (k < source.size () && (source[k] == ' ' || source[k] == '\t')) {
		    k++;
		}
		const bool qualifies
		    = k < source.size () && (std::isalpha (static_cast<unsigned char> (source[k])) || source[k] == '(');
		if (qualifies) {
		    return word;
		}
	    }
	    renamed = true;
	    return "_wp_" + word;
	}
	if (textureIsAVariable && word == "texture") {
	    size_t k = after;
	    while (k < source.size () && std::isspace (static_cast<unsigned char> (source[k]))) {
		k++;
	    }
	    if (k >= source.size () || source[k] != '(') {
		renamed = true;
		return "_wp_texture";
	    }
	}
	return word;
    });
    if (renamed) {
	WPENGINE_HEALTH ("compat", { { "what", "WE: a GLSL-reserved name renamed" } });
    }
    return renamed ? out : source;
}

std::string falsifyUnreadableIfs (const std::string& source) {
    auto lines = splitLines (source);
    static const std::regex member (R"([A-Za-z_][A-Za-z0-9_]*\s*(?:\.|\[))");
    bool changed = false;
    for (auto& line : lines) {
	std::smatch m;
	if (!Scan::directiveLine (line) || !std::regex_match (line, m, directive ())
	    || (m[1].str () != "if" && m[1].str () != "elif")) {
	    continue;
	}
	const std::string expression = withoutLineComment (m[2].str ());
	if (std::regex_search (expression, member)) {
	    sLog.out ("An #if WE's preprocessor can't read is false: ", line);
	    line = "#" + m[1].str () + " 0 // was: " + expression;
	    changed = true;
	}
    }
    if (changed) {
	WPENGINE_HEALTH ("compat", { { "what", "WE: an unreadable #if is false" } });
    }
    return changed ? joinLines (lines) : source;
}

std::string intrinsicOverloads (const std::string& source) {
    struct Intrinsic {
	const char* name;
	const char* body; // T is the type
    };
    static const Intrinsic intrinsics[] = {
	{ "log10", "T log10 (T x) { return log2 (x) * 0.301029995663981; }" },
	{ "saturate", "T saturate (T x) { return clamp (x, 0.0, 1.0); }" },
	{ "frac", "T frac (T x) { return fract (x); }" },
	{ "fmod", "T fmod (T x, T y) { return x - y * trunc (x / y); }" },
	{ "lerp", "T lerp (T a, T b, T t) { return mix (a, b, t); }" },
	{ "atan2", "T atan2 (T y, T x) { return atan (y, x); }" },
	{ "ddx", "T ddx (T x) { return dFdx (x); }" },
	{ "ddy", "T ddy (T x) { return dFdy (-x); }" },
    };
    static const char* types[] = { "float", "vec2", "vec3", "vec4" };
    static const std::regex typeParameter ("\\bT\\b");
    std::string out;
    for (const auto& intrinsic : intrinsics) {
	// the regex needs `<return type>\s+name\s*(`
	const bool mayDefine = Scan::anyWord (source, intrinsic.name, [&] (size_t at, size_t end) {
	    const std::string_view type = Scan::wordBefore (source, at);
	    const size_t k = Scan::skipSpace (source, end);
	    return (type == "float" || type == "vec2" || type == "vec3" || type == "vec4" || type == "float2"
		    || type == "float3" || type == "float4" || type == "int" || type == "void")
		&& k < source.size () && source[k] == '(';
	});
	if (!mayDefine) {
	    continue;
	}
	const std::regex definition (
	    std::string (R"(\b(?:float|vec[234]|float[234]|int|void)\s+)") + intrinsic.name
	    + R"(\s*\(\s*(?:(?:in|const)\s+)*([A-Za-z_][A-Za-z0-9_]*))"
	);
	std::set<std::string> own;
	for (auto it = std::sregex_iterator (source.begin (), source.end (), definition); it != std::sregex_iterator (); ++it) {
	    std::string type = (*it)[1].str ();
	    if (type.starts_with ("float") && type.size () == 6) {
		type = std::string ("vec") + type.back ();
	    }
	    own.insert (type);
	}
	if (own.empty ()) {
	    continue;
	}
	out += std::string ("#undef ") + intrinsic.name + "\n";
	for (const char* type : types) {
	    if (own.contains (type)) {
		continue;
	    }
	    out += std::regex_replace (intrinsic.body, typeParameter, type) + "\n";
	}
    }
    if (!out.empty ()) {
	WPENGINE_HEALTH ("compat", { { "what", "WE: a user function overloads an HLSL intrinsic" } });
    }
    return out;
}

std::string clipFunctions (const std::string& source) {
    static const std::regex call (R"(\bclip\s*\()");
    static const std::regex variable (R"(\b(?:float|vec[234]|float[234]|int|bool)\s+clip\b)");
    const bool calls = Scan::anyWord (source, "clip", [&] (size_t, size_t end) {
	const size_t k = Scan::skipSpace (source, end);
	return k < source.size () && source[k] == '(';
    });
    if (!calls || !std::regex_search (source, call) || std::regex_search (source, variable)) {
	return "";
    }
    return "void clip (float x) { if (x < 0.0) discard; }\n"
	   "void clip (vec2 x) { if (any (lessThan (x, vec2 (0.0)))) discard; }\n"
	   "void clip (vec3 x) { if (any (lessThan (x, vec3 (0.0)))) discard; }\n"
	   "void clip (vec4 x) { if (any (lessThan (x, vec4 (0.0)))) discard; }\n";
}

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
	const bool hashLine = Scan::directiveLine (line);
	if (hashLine && std::regex_search (line, opens)) {
	    depth++;
	} else if (hashLine && std::regex_search (line, closes)) {
	    if (depth == 0) {
		sLog.out ("Dropping an #endif with no #if: ", line);
		WPENGINE_HEALTH ("compat", { { "what", "glslang: unmatched #endif dropped" } });
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
} // namespace WallpaperEngine::Render::Shaders::WERules
