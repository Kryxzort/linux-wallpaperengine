#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Forward-declare FreeType types to avoid leaking the header into users.
struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace WallpaperEngine::Render::Objects {
/**
 * What a text layout depends on besides the text (WE's layout descriptor, filled at GH 0x14027c600)
 */
struct TextLayoutParams {
    /** Points, clamped to 1..256 and rasterised at 300 dpi (GH 0x1401dd330, 0x1401d94c0) */
    float pointSize = 32.0f;
    /** Added to every glyph's advance (x) and to the line height (y) */
    float spacingX = 0.0f;
    float spacingY = 0.0f;
    /** Lines wrap where a glyph would end past this many pixels; 0: no limit (`limitwidth` off) */
    float maxWidth = 0.0f;
    /** At most this many lines; 0: no limit (`limitrows` off) */
    int maxRows = 0;
    /** A line cut short by maxRows ends in "…" (`limituseellipsis`) */
    bool ellipsis = false;
    /** A wrapped line is justified to maxWidth (`blockalign`) */
    bool blockAlign = false;
    /** 0 left, 1 centre, 2 right (WE's enum, GH 0x14027e130) */
    int horizontalAlign = 1;

    bool operator== (const TextLayoutParams&) const = default;
};

/**
 * The fonts a text layer draws with: its own font, and — for a character it lacks — a system font that has it
 */
class TextFonts {
public:
    TextFonts () = default;
    TextFonts (const TextFonts&) = delete;
    TextFonts& operator= (const TextFonts&) = delete;
    ~TextFonts ();

    /** The font from its bytes (a scene's or WE's `fonts/…`), kept alive with the face */
    bool loadMemory (std::vector<uint8_t> data);
    /** A font file (one fontconfig found) */
    bool loadFile (const std::string& path, int index);
    [[nodiscard]] bool loaded () const;
    /** Every face to `points` at 300 dpi, as WE (FT_Set_Char_Size (0, pt·64, 300), GH 0x1401d94c0) */
    void setPointSize (float points);
    [[nodiscard]] FT_Face primary () const;
    /** The face that has `codepoint` (the font, else a fallback; else the font's missing glyph) and its glyph */
    FT_Face faceFor (char32_t codepoint, uint32_t& glyphIndex);

    /**
     * A system font for WE's `systemfont_*` names (WE takes arial.ttf, calibri.ttf, … from the Windows fonts
     * directory, GH 0x1401dd330's table; an unknown one fails and falls back to arial.ttf, GH 0x1401d9d70) through
     * fontconfig, the Windows family first, then metric-compatible and generic ones. Path and face index
     */
    static std::optional<std::pair<std::string, int>> systemFont (const std::string& name);

private:
    struct Face {
	FT_Face face = nullptr;
	std::vector<uint8_t> data;
    };

    bool initLibrary ();
    /** A fallback face for the codepoint (fontconfig), or -1 */
    int fallbackFor (char32_t codepoint);

    FT_Library m_library = nullptr;
    Face m_primary;
    std::vector<Face> m_fallbacks;
    std::map<char32_t, int> m_fallbackFor;
    float m_points = 0.0f;
};

/** A rendered glyph: 8-bit coverage, its box relative to the pen and baseline (y up) */
struct TextGlyphBitmap {
    int left = 0;
    int top = 0;
    int width = 0;
    int rows = 0;
    int pitch = 0;
    std::vector<uint8_t> pixels;
};

/** A glyph placed in the block: x, y of its bitmap's top-left corner in layout space (y up) */
struct TextPlacedGlyph {
    const TextGlyphBitmap* bitmap;
    float x;
    float y;
};

/**
 * Wallpaper Engine's text layout (GH 0x1401dd330): lines split at `\n`, glyphs advance by their advance plus
 * spacing.x, a line wraps at the last space before the glyph that ends past maxWidth, maxRows cuts the rest (with "…"
 * when asked), a wrapped line can be justified, and each line is aligned within the block (the widest line). Layout
 * space has x right and y up; line n's baseline is at y = −n · lineHeight
 */
struct TextLayout {
    std::vector<TextPlacedGlyph> glyphs;
    /** The block: the glyphs' ink and every line's 0..ascender / the last line's descent (GH 0x1401dd330) */
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    /** The font's ascender and descender at this size, and the line height (its height + spacing.y) */
    float ascender = 0.0f;
    float descender = 0.0f;
    float lineHeight = 0.0f;
    int rows = 0;
    /** No text: WE lays nothing out (the layer is 2 × 2, GH 0x14027dd90) */
    bool empty = true;
    /** The rendered glyphs the placed ones point at */
    std::map<std::pair<FT_Face, uint32_t>, TextGlyphBitmap> bitmaps;
};

/** UTF-8 to code points (a malformed sequence is U+FFFD) */
std::u32string decodeUtf8 (const std::string& text);

TextLayout layoutText (TextFonts& fonts, const std::u32string& text, const TextLayoutParams& params);
} // namespace WallpaperEngine::Render::Objects
