#pragma once

#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>

/**
 * Wallpaper Engine's own shader preprocessing, ported. WE's shaders are a GLSL dialect that WE turns into HLSL and
 * hands to D3DCompile (wallpaper64.exe 2.8.485; addresses are its Ghidra functions, as in the WE/LWE gap audit):
 *
 * - GH 0x140191050 reads a unit line by line; a line that *starts* with `#include` (no indent; a commented-out one
 *   is just a comment) is blanked and the file named between its quotes is recorded with the offset it was at, and
 *   GH 0x1401917f0 pastes each include (`shaders/<name>`, itself expanded the same way) at that offset — in place,
 *   not before main. A missing include is skipped silently.
 * - GH 0x140125e80 (the translator) takes every `attribute`, `varying`, `in`, `out` and `uniform` declaration line out
 *   of the text: uniforms go to cbuffers / Texture2D + SamplerState declarations ahead of all code, attributes and
 *   varyings to the VS_INPUT / VS_OUTPUT structs, and every use becomes `IN.x` / `OUT.x`. So code anywhere — an
 *   include pasted at the top included — sees every declaration of the unit. The fragment unit's inputs are the
 *   vertex unit's outputs (its VS_OUTPUT list, types and all); `gl_FragColor` is `OUT.gl_FragColor`; a global line
 *   starting `const` gets `static ` in front. Then the text goes to D3DCompile (vs_5_0 / ps_5_0, flags 0x8800 =
 *   optimization level 3 | strictness), whose HLSL semantics are what the shaders were written against: implicit
 *   vector truncation, scalar promotion, bool/int/float conversions in conditions and assignments, non-constant
 *   initializers for `static const` globals and for `const` locals, uncalled functions not type-checked to the end.
 *   A unit that still fails is replaced by shaders/HLSL/dx11fallback (solid 0.5 0 0 red) — so a shader a published
 *   scene uses compiles there.
 * - GH 0x140199cf0 is WE's own preprocessor pass over the result (#define, #undef, #if/#ifdef/#ifndef/#elif/#else/
 *   #endif, #require); an `#endif` with no open `#if` is ignored there.
 *
 * LWE hands GLSL 3.30 to glslang, so these functions rewrite a unit's text so that glslang and the GL driver accept
 * what WE's path accepts, with the same meaning. Each names its rule; "inferred" marks the parts read from HLSL's
 * rules and the library's shaders rather than from WE's code.
 */
namespace WallpaperEngine::Render::Shaders::WERules {
/** Reads `shaders/<name>`; nullopt when it doesn't exist */
using IncludeLoader = std::function<std::optional<std::string> (const std::string& name)>;

/**
 * GH 0x140191050 / 0x1401917f0: every line starting `#include "name"` replaced by that file, expanded the same way,
 * between `// begin of include from file name` / `// end of included from file name` markers. A file already pasted
 * in this unit is not pasted again (inferred: HLSL would reject the second copy's functions)
 */
std::string expandIncludes (const std::string& source, const IncludeLoader& load, std::set<std::string>& seen);

/**
 * GH 0x140125e80: the unit's `uniform` / `attribute` / `varying` / `in` / `out` declaration lines (at file scope,
 * after the first include) moved up to where the first include starts, each inside copies of the `#if` chain it was
 * in, so every include and function sees them as in WE's cbuffers and structs. A declaration whose `#if` reads a
 * name the unit `#define`s between there and here stays where it is
 */
std::string hoistDeclarations (const std::string& source);

/**
 * Identifiers HLSL leaves free but GLSL reserves (`input`, `output`, `filter`, `common`, …) renamed `_wp_<name>`; a
 * variable named `texture` too (it hides GLSL's texture(), which texSample2D is). Inferred (HLSL's keyword list)
 */
std::string renameReservedWords (const std::string& source);

/**
 * An `#if` / `#elif` whose expression isn't one a preprocessor can read (`#if g_Texture0Resolution.y <= 640`, 3042492564's
 * aa_gaussian) is false, as WE's preprocessor (GH 0x140199cf0, its expression reader GH 0x140196ce0) treats it: the
 * block it guards there is never compiled (it has a missing semicolon). Inferred from that shader
 */
std::string falsifyUnreadableIfs (const std::string& source);

/**
 * The header's HLSL names that are macros (log10, saturate, frac, fmod, lerp, atan2, ddx, ddy) clash with a unit that
 * defines a function of that name itself (2812601803's tone_mapping defines `float log10 (float)`; HLSL lets a
 * user function overload an intrinsic): the macro is #undef'd and replaced by real overloads for the other types.
 * Returns the text to put before the unit's code (empty when nothing clashes)
 */
std::string intrinsicOverloads (const std::string& source);

/**
 * HLSL's clip(x) (discard when any component is below zero; the stock volumetricsfront): overloads for a fragment
 * unit that calls it and has no variable of that name (2800594362's clipping_mask has `vec4 clip`). Empty otherwise
 */
std::string clipFunctions (const std::string& source);

/** An `#endif` with no open `#if` dropped (GH 0x140199cf0 ignores it); the line stays, empty */
std::string dropUnmatchedEndifs (const std::string& source);

/** A backslash at the end of a line joins it to the next (D3DCompile's preprocessor); the line count is kept */
std::string spliceLines (const std::string& source);

/**
 * Cheap pre-checks for the text passes' regexes. A libstdc++ std::regex search is slow (a backtracking executor that
 * copies its sub-matches at every start position: about 1 ms over a 20 kB particle unit), and a unit goes through some 30
 * whole-text searches (one per varying, per intrinsic, …), so a scene with dozens of particle systems would spend
 * seconds of its build in them. Each check below is a necessary condition for its regex to match (never stricter), so
 * a caller that skips the regex when the check fails gets the result the regex would have given; the regex still
 * decides when the check passes. eachMatch runs a regex only where its match can start, with the same result.
 */
namespace Scan {
/** `\w`: the characters a regex word boundary separates */
inline bool isWord (char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/** `\s` in the C locale */
inline bool isSpace (char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/** The first index at or after `at` that isn't `\s` (or the size) */
inline size_t skipSpace (std::string_view text, size_t at) {
    while (at < text.size () && isSpace (text[at])) {
	at++;
    }
    return at;
}

/**
 * Calls `fn (begin, end)` for each occurrence of `word` (an identifier) with no word character on either side —
 * where `\bword\b` matches — in order, until `fn` returns true; returns whether it did
 */
template <typename F> bool anyWord (std::string_view text, std::string_view word, F fn) {
    if (word.empty ()) {
	return false;
    }
    for (size_t at = text.find (word); at != std::string_view::npos; at = text.find (word, at + 1)) {
	const size_t end = at + word.size ();
	if ((at > 0 && isWord (text[at - 1])) || (end < text.size () && isWord (text[end]))) {
	    continue;
	}
	if (fn (at, end)) {
	    return true;
	}
    }
    return false;
}

/** Whether `\bword\b` matches in `text` (exactly, for an identifier `word`) */
inline bool hasWord (std::string_view text, std::string_view word) {
    return anyWord (text, word, [] (size_t, size_t) { return true; });
}

/**
 * The word that ends where the `\s+` run just before `at` starts (empty when no `\s` is just before `at`, or no word
 * ends there): the `T` of `T\s+name` with `name` at `at`
 */
inline std::string_view wordBefore (std::string_view text, size_t at) {
    size_t end = at;
    while (end > 0 && isSpace (text[end - 1])) {
	end--;
    }
    if (end == at) {
	return {};
    }
    size_t begin = end;
    while (begin > 0 && isWord (text[begin - 1])) {
	begin--;
    }
    return text.substr (begin, end - begin);
}

/** Whether a declaration `… name\s*;` can be at the whole word `name` at [at, end): `\s` before it, `\s*;` after */
inline bool declaresAt (std::string_view text, size_t at, size_t end) {
    if (at == 0 || !isSpace (text[at - 1])) {
	return false;
    }
    const size_t k = skipSpace (text, end);
    return k < text.size () && text[k] == ';';
}

/**
 * The matches `std::sregex_iterator (text, re)` lists, in order, for a `re` whose every match starts with the whole word
 * `lead` (`\blead\s…`): the regex is tried only where `lead` is, anchored there (match_continuous, with the character
 * before it for `\b`), which is what the iterator's search finds at that position. Calls `fn (match, position)` until
 * it returns true; returns whether it did
 */
template <typename F> bool eachMatch (const std::string& text, const std::regex& re, std::string_view lead, F fn) {
    size_t from = 0;
    return anyWord (text, lead, [&] (size_t at, size_t) {
	if (at < from) {
	    return false;
	}
	std::smatch match;
	auto flags = std::regex_constants::match_continuous;
	if (at > 0) {
	    flags |= std::regex_constants::match_prev_avail;
	}
	if (!std::regex_search (text.cbegin () + static_cast<std::ptrdiff_t> (at), text.cend (), match, re, flags)) {
	    return false;
	}
	from = at + static_cast<size_t> (match.length (0));
	return static_cast<bool> (fn (match, at));
    });
}

/** Whether `^\s*#` (a directive) can match the line: its first non-`\s` character is `#` */
inline bool directiveLine (std::string_view line) {
    const size_t k = skipSpace (line, 0);
    return k < line.size () && line[k] == '#';
}
} // namespace Scan
} // namespace WallpaperEngine::Render::Shaders::WERules
