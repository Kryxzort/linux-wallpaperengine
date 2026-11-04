#include "TextLayout.h"

#include <algorithm>
#include <cmath>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
/** WE's whitespace in the layout: tab, CR and space (the mask 0x100002200, GH 0x1401dd330) */
bool isSpace (char32_t c) { return c == 0x20 || c == 0x09 || c == 0x0d; }

constexpr char32_t kEllipsis = 0x2026;

void trimTrailingSpaces (std::u32string& line) {
    while (!line.empty () && isSpace (line.back ())) {
	line.pop_back ();
    }
}

/** WE's FUN_1401dd280: the trailing whitespace off, then "…" unless it already ends in one */
void endLine (std::u32string& line, bool ellipsis) {
    trimTrailingSpaces (line);
    if (ellipsis && (line.empty () || line.back () != kEllipsis)) {
	line.push_back (kEllipsis);
    }
}

/** One glyph of a line: its bitmap, advance, and box relative to the pen (y up) */
struct LineGlyph {
    const TextGlyphBitmap* bitmap;
    char32_t codepoint;
    size_t cluster;
    float advance;
    float x0, y0, x1, y1;
};

const TextGlyphBitmap* render (TextLayout& layout, FT_Face face, uint32_t index) {
    const auto key = std::make_pair (face, index);
    if (const auto it = layout.bitmaps.find (key); it != layout.bitmaps.end ()) {
	return &it->second;
    }

    TextGlyphBitmap bitmap;
    if (FT_Load_Glyph (face, index, FT_LOAD_RENDER) == 0) {
	const auto* slot = face->glyph;
	bitmap.left = slot->bitmap_left;
	bitmap.top = slot->bitmap_top;
	bitmap.width = static_cast<int> (slot->bitmap.width);
	bitmap.rows = static_cast<int> (slot->bitmap.rows);
	bitmap.pitch = bitmap.width;
	bitmap.pixels.resize (static_cast<size_t> (bitmap.width) * bitmap.rows);
	for (int row = 0; row < bitmap.rows; row++) {
	    const unsigned char* src = slot->bitmap.buffer + static_cast<ptrdiff_t> (row) * slot->bitmap.pitch;
	    std::copy (src, src + bitmap.width, bitmap.pixels.begin () + static_cast<ptrdiff_t> (row) * bitmap.width);
	}
    }

    return &layout.bitmaps.emplace (key, std::move (bitmap)).first->second;
}

/** WE keeps tab, LF, CR, space (the mask 0x100002600) and ZWJ in the run before them (GH 0x1401da3c0) */
bool staysInRun (char32_t c) { return c == 0x200d || c == 0x20 || c == 0x09 || c == 0x0a || c == 0x0d; }

/**
 * The glyphs of one line from pen 0, shaped as WE shapes them (GH 0x1401da3c0): the line is cut into runs of one
 * face (each character's face from the font's fallback chain; whitespace and ZWJ stay in the run before them), and
 * each run goes through HarfBuzz on its FreeType face (GPOS / kern kerning, ligatures, marks), advances x_advance >> 6.
 * A control character other than tab draws nothing. Inferred: the hb_ft font with HarfBuzz's default load flags, the
 * runs' script/direction guessed per run (WE's FUN_1401e1c00 / 0x1401e0eb0 weren't read past the run split)
 */
std::vector<LineGlyph> shapeLine (TextFonts& fonts, TextLayout& layout, const std::u32string& line, float spacingX) {
    std::vector<LineGlyph> glyphs;

    struct Run {
	FT_Face face;
	std::vector<size_t> at;
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < line.size (); i++) {
	const char32_t cp = line[i];
	if (cp < 0x20 && cp != 0x09) {
	    continue;
	}
	FT_Face face = nullptr;
	if (staysInRun (cp) && !runs.empty ()) {
	    face = runs.back ().face;
	} else {
	    uint32_t index = 0;
	    face = fonts.faceFor (cp, index);
	}
	if (face == nullptr) {
	    continue;
	}
	if (runs.empty () || runs.back ().face != face) {
	    runs.push_back (Run { .face = face, .at = {} });
	}
	runs.back ().at.push_back (i);
    }

    hb_buffer_t* buffer = hb_buffer_create ();
    for (const auto& run : runs) {
	hb_buffer_clear_contents (buffer);
	for (const size_t i : run.at) {
	    hb_buffer_add (buffer, static_cast<hb_codepoint_t> (line[i]), static_cast<unsigned int> (i));
	}
	hb_buffer_set_content_type (buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
	hb_buffer_guess_segment_properties (buffer);
	hb_font_t* font = hb_ft_font_create_referenced (run.face);
	hb_shape (font, buffer, nullptr, 0);

	unsigned int count = 0;
	const hb_glyph_info_t* info = hb_buffer_get_glyph_infos (buffer, &count);
	const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions (buffer, &count);
	for (unsigned int k = 0; k < count; k++) {
	    const TextGlyphBitmap* bitmap = render (layout, run.face, info[k].codepoint);
	    const auto dx = static_cast<float> (pos[k].x_offset >> 6);
	    const auto dy = static_cast<float> (pos[k].y_offset >> 6);
	    glyphs.push_back (LineGlyph {
		.bitmap = bitmap,
		.codepoint = line[info[k].cluster],
		.cluster = info[k].cluster,
		.advance = static_cast<float> (pos[k].x_advance >> 6) + spacingX,
		.x0 = static_cast<float> (bitmap->left) + dx,
		.y0 = static_cast<float> (bitmap->top - bitmap->rows) + dy,
		.x1 = static_cast<float> (bitmap->left + bitmap->width) + dx,
		.y1 = static_cast<float> (bitmap->top) + dy,
	    });
	}
	hb_font_destroy (font);
    }
    hb_buffer_destroy (buffer);

    return glyphs;
}

/** Where a line's ink ends: the pen plus the last glyph's right edge (the ellipsis fit's measure) */
float inkRight (const std::vector<LineGlyph>& glyphs) {
    float pen = 0.0f, right = 0.0f;
    for (const auto& g : glyphs) {
	right = std::max (right, pen + g.x1);
	pen += g.advance;
    }
    return right;
}
} // namespace

std::u32string WallpaperEngine::Render::Objects::decodeUtf8 (const std::string& text) {
    std::u32string out;
    out.reserve (text.size ());

    for (size_t i = 0; i < text.size ();) {
	const auto c = static_cast<unsigned char> (text[i]);
	int extra = 0;
	char32_t cp = 0;

	if (c < 0x80) {
	    cp = c;
	} else if ((c & 0xe0) == 0xc0) {
	    cp = c & 0x1f;
	    extra = 1;
	} else if ((c & 0xf0) == 0xe0) {
	    cp = c & 0x0f;
	    extra = 2;
	} else if ((c & 0xf8) == 0xf0) {
	    cp = c & 0x07;
	    extra = 3;
	} else {
	    out.push_back (0xfffd);
	    i++;
	    continue;
	}

	bool ok = true;
	for (int k = 1; ok && k <= extra; k++) {
	    if (i + k >= text.size () || (static_cast<unsigned char> (text[i + k]) & 0xc0) != 0x80) {
		ok = false;
	    } else {
		cp = (cp << 6) | (static_cast<unsigned char> (text[i + k]) & 0x3f);
	    }
	}

	if (!ok) {
	    out.push_back (0xfffd);
	    i++;
	    continue;
	}

	out.push_back (cp);
	i += extra + 1;
    }

    return out;
}

TextFonts::~TextFonts () {
    for (auto& face : this->m_fallbacks) {
	FT_Done_Face (face.face);
    }
    if (this->m_primary.face != nullptr) {
	FT_Done_Face (this->m_primary.face);
    }
    if (this->m_library != nullptr) {
	FT_Done_FreeType (this->m_library);
    }
}

bool TextFonts::initLibrary () {
    if (this->m_library == nullptr && FT_Init_FreeType (&this->m_library) != 0) {
	this->m_library = nullptr;
	sLog.error ("CText: FT_Init_FreeType failed");
    }
    return this->m_library != nullptr;
}

bool TextFonts::loadMemory (std::vector<uint8_t> data) {
    if (!this->initLibrary () || data.empty ()) {
	return false;
    }

    this->m_primary.data = std::move (data);
    if (FT_New_Memory_Face (this->m_library, this->m_primary.data.data (),
			    static_cast<FT_Long> (this->m_primary.data.size ()), 0, &this->m_primary.face)
	!= 0) {
	this->m_primary.face = nullptr;
	this->m_primary.data.clear ();
	return false;
    }

    FT_Select_Charmap (this->m_primary.face, FT_ENCODING_UNICODE);
    return true;
}

bool TextFonts::loadFile (const std::string& path, int index) {
    if (!this->initLibrary ()) {
	return false;
    }

    if (FT_New_Face (this->m_library, path.c_str (), index, &this->m_primary.face) != 0) {
	this->m_primary.face = nullptr;
	return false;
    }

    FT_Select_Charmap (this->m_primary.face, FT_ENCODING_UNICODE);
    return true;
}

bool TextFonts::loaded () const { return this->m_primary.face != nullptr; }

FT_Face TextFonts::primary () const { return this->m_primary.face; }

void TextFonts::setPointSize (float points) {
    points = std::clamp (points, 1.0f, 256.0f);

    if (points == this->m_points) {
	return;
    }

    this->m_points = points;
    const auto size = static_cast<FT_F26Dot6> (points * 64.0f);

    if (this->m_primary.face != nullptr) {
	FT_Set_Char_Size (this->m_primary.face, 0, size, 300, 300);
    }
    for (auto& face : this->m_fallbacks) {
	FT_Set_Char_Size (face.face, 0, size, 300, 300);
    }
}

FT_Face TextFonts::faceFor (char32_t codepoint, uint32_t& glyphIndex) {
    if (this->m_primary.face == nullptr) {
	return nullptr;
    }

    glyphIndex = FT_Get_Char_Index (this->m_primary.face, codepoint);
    if (glyphIndex != 0 || codepoint < 0x20) {
	return this->m_primary.face;
    }

    if (const int fallback = this->fallbackFor (codepoint); fallback >= 0) {
	const FT_Face face = this->m_fallbacks[fallback].face;
	glyphIndex = FT_Get_Char_Index (face, codepoint);
	return face;
    }

    return this->m_primary.face;
}

namespace {
/**
 * WE's fallback chain (GH 0x1401da3c0, the table at 0x1404c5030: name + "in the Windows fonts directory"): arial.ttf,
 * seguiemj.ttf, arialuni.ttf, segoeui.ttf, WE's own fonts/TwemojiMozilla.ttf, seguisym.ttf, msyh.ttc, Malgun.ttf; a
 * character goes to the first that has it. Here each Windows file is its family, then the families fontconfig commonly
 * has in its place (the substitutes are ours)
 */
struct ChainSlot {
    const char* asset;
    std::vector<const char*> families;
};
const std::vector<ChainSlot>& weChain () {
    static const std::vector<ChainSlot> kChain = {
	{ nullptr, { "Arial", "Liberation Sans", "Arimo" } },
	{ nullptr, { "Segoe UI Emoji", "Noto Emoji" } },
	{ nullptr, { "Arial Unicode MS", "Noto Sans", "DejaVu Sans" } },
	{ nullptr, { "Segoe UI", "Selawik", "Noto Sans", "Open Sans" } },
	{ "fonts/TwemojiMozilla.ttf", {} },
	{ nullptr, { "Segoe UI Symbol", "Noto Sans Symbols 2", "Noto Sans Symbols", "Noto Sans Math", "DejaVu Sans" } },
	{ nullptr, { "Microsoft YaHei", "Noto Sans CJK SC", "Source Han Sans SC", "Noto Sans SC", "WenQuanYi Micro Hei",
		     "Droid Sans Fallback" } },
	{ nullptr, { "Malgun Gothic", "Noto Sans CJK KR", "Source Han Sans K", "Noto Sans KR", "NanumGothic" } },
    };
    return kChain;
}

/** The file of the first of `families` fontconfig has under that very family name (not a substitute), or none */
std::optional<std::pair<std::string, int>> exactFamily (const std::vector<const char*>& families) {
    if (!FcInit ()) {
	return std::nullopt;
    }
    for (const char* family : families) {
	FcPattern* pattern = FcPatternCreate ();
	FcPatternAddString (pattern, FC_FAMILY, reinterpret_cast<const FcChar8*> (family));
	FcPatternAddBool (pattern, FC_SCALABLE, FcTrue);
	FcConfigSubstitute (nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute (pattern);
	FcResult res = FcResultNoMatch;
	FcPattern* match = FcFontMatch (nullptr, pattern, &res);
	std::optional<std::pair<std::string, int>> found;
	if (match != nullptr) {
	    FcChar8* got = nullptr;
	    FcChar8* file = nullptr;
	    int index = 0;
	    for (int k = 0; FcPatternGetString (match, FC_FAMILY, k, &got) == FcResultMatch; k++) {
		if (FcStrCmpIgnoreCase (got, reinterpret_cast<const FcChar8*> (family)) == 0
		    && FcPatternGetString (match, FC_FILE, 0, &file) == FcResultMatch) {
		    FcPatternGetInteger (match, FC_INDEX, 0, &index);
		    found = std::make_pair (std::string (reinterpret_cast<const char*> (file)), index);
		    break;
		}
	    }
	    FcPatternDestroy (match);
	}
	FcPatternDestroy (pattern);
	if (found.has_value ()) {
	    return found;
	}
    }
    return std::nullopt;
}
} // namespace

void TextFonts::setAssetReader (AssetReader reader) { this->m_assets = std::move (reader); }

int TextFonts::openFallback (Face face) {
    // a colour-only font (emoji bitmaps) can't be drawn as coverage: skip it
    if (!FT_IS_SCALABLE (face.face)) {
	FT_Done_Face (face.face);
	return -1;
    }
    FT_Select_Charmap (face.face, FT_ENCODING_UNICODE);
    if (this->m_points > 0.0f) {
	FT_Set_Char_Size (face.face, 0, static_cast<FT_F26Dot6> (this->m_points * 64.0f), 300, 300);
    }
    this->m_fallbacks.push_back (std::move (face));
    return static_cast<int> (this->m_fallbacks.size () - 1);
}

int TextFonts::chainSlot (size_t slot) {
    while (this->m_chain.size () <= slot) {
	const auto& entry = weChain ()[this->m_chain.size ()];
	int opened = -1;
	if (this->initLibrary ()) {
	    Face face;
	    if (entry.asset != nullptr) {
		if (this->m_assets) {
		    if (auto data = this->m_assets (entry.asset); data.has_value () && !data->empty ()) {
			face.data = std::move (*data);
			if (FT_New_Memory_Face (this->m_library, face.data.data (), static_cast<FT_Long> (face.data.size ()), 0,
						&face.face)
			    == 0) {
			    opened = this->openFallback (std::move (face));
			}
		    }
		}
	    } else if (const auto file = exactFamily (entry.families); file.has_value ()) {
		if (FT_New_Face (this->m_library, file->first.c_str (), file->second, &face.face) == 0) {
		    opened = this->openFallback (std::move (face));
		}
	    }
	}
	this->m_chain.push_back (opened);
    }
    return this->m_chain[slot];
}

int TextFonts::fallbackFor (char32_t codepoint) {
    if (const auto it = this->m_fallbackFor.find (codepoint); it != this->m_fallbackFor.end ()) {
	return it->second;
    }

    int result = -1;

    // WE's chain, in its order (a slot this system has no font for is skipped)
    for (size_t slot = 0; slot < weChain ().size () && result < 0; slot++) {
	const int face = this->chainSlot (slot);
	if (face >= 0 && FT_Get_Char_Index (this->m_fallbacks[face].face, codepoint) != 0) {
	    result = face;
	}
    }

    // then the faces fontconfig picked for earlier characters
    for (size_t i = 0; i < this->m_fallbacks.size () && result < 0; i++) {
	if (FT_Get_Char_Index (this->m_fallbacks[i].face, codepoint) != 0) {
	    result = static_cast<int> (i);
	}
    }

    // Ours, not WE's (a Windows system has every slot's font; WE would draw the font's missing glyph): fontconfig's
    // best scalable font for the character
    if (result < 0 && FcInit ()) {
	FcPattern* pattern = FcPatternCreate ();
	FcCharSet* charset = FcCharSetCreate ();
	FcCharSetAddChar (charset, codepoint);
	FcPatternAddCharSet (pattern, FC_CHARSET, charset);
	FcPatternAddString (pattern, FC_FAMILY, reinterpret_cast<const FcChar8*> ("sans-serif"));
	FcConfigSubstitute (nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute (pattern);

	FcResult res = FcResultNoMatch;
	FcFontSet* set = FcFontSort (nullptr, pattern, FcTrue, nullptr, &res);

	for (int i = 0; set != nullptr && i < set->nfont && result < 0; i++) {
	    FcCharSet* has = nullptr;
	    FcChar8* file = nullptr;
	    int index = 0;

	    if (FcPatternGetCharSet (set->fonts[i], FC_CHARSET, 0, &has) != FcResultMatch || !FcCharSetHasChar (has, codepoint)
		|| FcPatternGetString (set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) {
		continue;
	    }
	    FcPatternGetInteger (set->fonts[i], FC_INDEX, 0, &index);

	    Face face;
	    if (FT_New_Face (this->m_library, reinterpret_cast<const char*> (file), index, &face.face) != 0) {
		continue;
	    }
	    result = this->openFallback (std::move (face));
	}

	if (set != nullptr) {
	    FcFontSetDestroy (set);
	}
	FcCharSetDestroy (charset);
	FcPatternDestroy (pattern);
    }

    this->m_fallbackFor.emplace (codepoint, result);
    return result;
}

std::optional<std::pair<std::string, int>> TextFonts::systemFont (const std::string& name) {
    // WE's names (GH 0x1401dd330's table: arial.ttf, calibri.ttf, cambria.ttc, comic.ttf, consola.ttf, micross.ttf,
    // segoeui.ttf, verdana.ttf), then the metric-compatible families fontconfig commonly has, then the generic one
    static const std::map<std::string, std::vector<const char*>> kFamilies = {
	{ "systemfont_arial", { "Arial", "Liberation Sans", "Arimo", "sans-serif" } },
	{ "systemfont_calibri", { "Calibri", "Carlito", "sans-serif" } },
	{ "systemfont_cambria", { "Cambria", "Caladea", "serif" } },
	{ "systemfont_comicsans", { "Comic Sans MS", "Comic Neue", "Comic Relief", "sans-serif" } },
	{ "systemfont_consolas", { "Consolas", "Inconsolata", "DejaVu Sans Mono", "monospace" } },
	{ "systemfont_sansserif", { "Microsoft Sans Serif", "Liberation Sans", "Arimo", "sans-serif" } },
	{ "systemfont_segoe", { "Segoe UI", "Selawik", "Noto Sans", "Open Sans", "sans-serif" } },
	{ "systemfont_verdana", { "Verdana", "DejaVu Sans", "sans-serif" } },
    };

    const auto it = kFamilies.find (name);
    // any other name: WE can't open it and takes arial.ttf (GH 0x1401d9d70)
    const auto& families = it != kFamilies.end () ? it->second : kFamilies.at ("systemfont_arial");

    if (!FcInit ()) {
	return std::nullopt;
    }

    FcPattern* pattern = FcPatternCreate ();
    for (const char* family : families) {
	FcPatternAddString (pattern, FC_FAMILY, reinterpret_cast<const FcChar8*> (family));
    }
    FcConfigSubstitute (nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute (pattern);

    FcResult res = FcResultNoMatch;
    FcPattern* match = FcFontMatch (nullptr, pattern, &res);
    std::optional<std::pair<std::string, int>> result = std::nullopt;

    if (match != nullptr) {
	FcChar8* file = nullptr;
	int index = 0;
	if (FcPatternGetString (match, FC_FILE, 0, &file) == FcResultMatch) {
	    FcPatternGetInteger (match, FC_INDEX, 0, &index);
	    result = std::make_pair (std::string (reinterpret_cast<const char*> (file)), index);
	}
	FcPatternDestroy (match);
    }

    FcPatternDestroy (pattern);
    return result;
}

TextLayout WallpaperEngine::Render::Objects::layoutText (
    TextFonts& fonts, const std::u32string& text, const TextLayoutParams& params
) {
    TextLayout layout;

    // WE lays nothing out for an empty string (GH 0x1401dd330 returns null)
    if (text.empty () || !fonts.loaded ()) {
	return layout;
    }

    fonts.setPointSize (params.pointSize);
    const FT_Face primary = fonts.primary ();
    // the size's metrics in whole pixels (26.6 >> 6): ascender, descender, height (+ spacing.y: the line height)
    layout.ascender = static_cast<float> (primary->size->metrics.ascender >> 6);
    layout.descender = static_cast<float> (primary->size->metrics.descender >> 6);
    layout.lineHeight = static_cast<float> (primary->size->metrics.height >> 6) + params.spacingY;

    // the lines: split at '\n' (a trailing one adds none); with maxRows the ones past it are dropped and the last kept
    // one ends ("…" with ellipsis)
    std::vector<std::u32string> lines;
    for (size_t start = 0;;) {
	const size_t end = text.find (U'\n', start);
	const std::u32string segment = text.substr (start, end == std::u32string::npos ? std::u32string::npos : end - start);

	if (params.maxRows > 0 && static_cast<size_t> (params.maxRows) <= lines.size ()) {
	    endLine (lines.back (), params.ellipsis);
	    break;
	}
	lines.push_back (segment);

	if (end == std::u32string::npos || end + 1 >= text.size ()) {
	    break;
	}
	start = end + 1;
    }

    struct FinishedLine {
	std::vector<LineGlyph> glyphs;
	float width;
    };
    std::vector<FinishedLine> finished;
    std::vector<bool> wrapped (lines.size (), false);

    float blockWidth = 0.0f;
    float inkMinY = 0.0f, inkMaxY = 0.0f;
    bool stop = false;

    for (size_t li = 0; li < lines.size () && !stop; li++) {
	std::u32string& line = lines[li];
	std::vector<LineGlyph> all = shapeLine (fonts, layout, line, params.spacingX);
	std::vector<LineGlyph> kept;
	float pen = 0.0f;
	bool redo = false;

	for (const auto& g : all) {
	    // a glyph that would end past maxWidth (not the first on the line, not a space) breaks the line
	    if (params.maxWidth > 0.0f && !kept.empty () && !isSpace (g.codepoint) && params.maxWidth < pen + g.x1) {
		if (params.maxRows < 1 || finished.size () + 1 < static_cast<size_t> (params.maxRows)) {
		    // wrap at the last space before it (or at it, in a word longer than the line); the rest, without
		    // its leading spaces, is the next line
		    size_t at = g.cluster;
		    size_t breakAt = at;
		    for (size_t k = at; k-- > 0;) {
			if (isSpace (line[k])) {
			    breakAt = k;
			    break;
			}
		    }
		    std::u32string rest = line.substr (breakAt);
		    rest.erase (0, std::min (rest.size (), rest.find_first_not_of (U" \t\r")));
		    line.erase (breakAt);
		    trimTrailingSpaces (line);
		    const size_t length = line.size ();
		    // (`line` is not used past the insert: it may move the strings)
		    lines.insert (lines.begin () + static_cast<ptrdiff_t> (li) + 1, rest);
		    wrapped.insert (wrapped.begin () + static_cast<ptrdiff_t> (li) + 1, false);
		    wrapped[li] = true;
		    std::erase_if (kept, [length] (const LineGlyph& k) { return k.cluster >= length; });
		} else if (params.ellipsis) {
		    // the last line allowed: as much as fits with "…" after it (GH 0x1401dd330 shortens it one
		    // character at a time), then this line again
		    size_t keep = g.cluster;
		    for (;;) {
			std::u32string candidate = line.substr (0, keep);
			candidate.push_back (kEllipsis);
			if (keep == 0 || inkRight (shapeLine (fonts, layout, candidate, params.spacingX)) <= params.maxWidth) {
			    break;
			}
			keep--;
		    }
		    line.erase (keep);
		    endLine (line, true);
		    lines.resize (li + 1);
		    wrapped.resize (li + 1);
		    wrapped[li] = false;
		    redo = true;
		} else {
		    // cut: the rest of the line and the lines after it are dropped
		    lines.resize (li + 1);
		    wrapped.resize (li + 1);
		    stop = true;
		}
		break;
	    }

	    kept.push_back (g);
	    pen += g.advance;
	}

	if (redo) {
	    li--;
	    continue;
	}

	const std::u32string& current = lines[li];

	// the line's box from pen 0: its glyphs' ink, and 0
	float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
	pen = 0.0f;
	for (const auto& g : kept) {
	    minX = std::min (minX, pen + g.x0);
	    maxX = std::max (maxX, pen + g.x1);
	    minY = std::min (minY, g.y0);
	    maxY = std::max (maxY, g.y1);
	    pen += g.advance;
	}

	// blockalign: a wrapped line's spaces share what it lacks of maxWidth
	if (params.blockAlign && wrapped[li] && !current.empty ()) {
	    const auto spaces = static_cast<int> (std::count_if (current.begin (), current.end (), isSpace));
	    if (spaces > 0) {
		const float extra = (params.maxWidth - (maxX - minX)) / static_cast<float> (spaces);
		for (auto& g : kept) {
		    if (isSpace (g.codepoint)) {
			g.advance += extra;
		    }
		}
		maxX = minX + params.maxWidth;
	    }
	}

	const float offsetY = -static_cast<float> (finished.size ()) * layout.lineHeight;
	inkMinY = std::min (inkMinY, minY + offsetY);
	inkMaxY = std::max (inkMaxY, maxY + offsetY);
	layout.minX = std::min (layout.minX, minX);
	layout.maxX = std::max (layout.maxX, maxX);
	blockWidth = std::max (blockWidth, maxX - minX);
	finished.push_back (FinishedLine { .glyphs = std::move (kept), .width = maxX - minX });
    }

    layout.rows = static_cast<int> (finished.size ());
    layout.maxY = std::max (layout.ascender, inkMaxY);
    layout.minY = std::min (layout.ascender - static_cast<float> (layout.rows) * layout.lineHeight, inkMinY);

    // each line within the block's width, by the horizontal alignment
    for (size_t li = 0; li < finished.size (); li++) {
	const auto& line = finished[li];
	float alignX = 0.0f;
	if (params.horizontalAlign == 1) {
	    alignX = (blockWidth - line.width) * 0.5f;
	} else if (params.horizontalAlign == 2) {
	    alignX = blockWidth - line.width;
	}

	const float baseline = -static_cast<float> (li) * layout.lineHeight;
	float pen = 0.0f;
	for (const auto& g : line.glyphs) {
	    if (g.bitmap->width > 0 && g.bitmap->rows > 0) {
		layout.glyphs.push_back (TextPlacedGlyph { .bitmap = g.bitmap, .x = pen + g.x0 + alignX, .y = baseline + g.y1 });
	    }
	    pen += g.advance;
	}
    }

    layout.empty = false;
    return layout;
}
