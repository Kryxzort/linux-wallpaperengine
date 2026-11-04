#include "CText.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
std::string targetName (const Text& text) { return "_rt_textLayer_" + std::to_string (text.id); }

/**
 * The layer's texture, registered before CImage looks its material's texture up (it is what the text is drawn into;
 * sized by the editor's `size` until the text is laid out)
 */
const Text& withTarget (Wallpapers::CScene& scene, const Text& text) {
    const glm::vec2 size = glm::max (text.size, glm::vec2 (2.0f));
    scene.create (targetName (text), TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, size, size);
    return text;
}

/** WE's enums (GH 0x14027e130): horizontal left 0 centre 1 right 2; vertical bottom 0 centre 1 top 2 */
int horizontalAlignOf (const std::string& value) {
    if (value == "left") {
	return 0;
    }
    if (value == "right") {
	return 2;
    }
    return 1;
}

int verticalAlignOf (const std::string& value) {
    if (value == "bottom") {
	return 0;
    }
    if (value == "top") {
	return 2;
    }
    return 1;
}
} // namespace

CText::CText (Wallpapers::CScene& scene, const Text& text) :
    CObject (scene, text), CImage (scene, withTarget (scene, text)), m_text (text) {
    this->m_target = scene.find (targetName (text));
    this->m_horizontalAlign = text.horizontalAlign;
    this->m_verticalAlign = text.verticalAlign;
    this->registerProperty ("pointSize", *text.pointSize->value);
    // ITextLayer.maxwidth / maxrows (a script on maxwidth sizes song titles to their box)
    this->registerProperty ("maxwidth", *text.maxWidth->value);
    this->registerProperty ("maxrows", *text.maxRows->value);
    // the text's script runs as a layer script (initScriptLayer), not a second time as a property script
    this->registerProperty ("text", *text.text->value, false);
}

CText::~CText () {
    if (this->m_layerHandle != Scripting::kInvalidLayerHandle) {
	this->getScene ().getScriptEngine ().destroyLayer (this->m_layerHandle);
	this->m_layerHandle = Scripting::kInvalidLayerHandle;
    }
}

const Text& CText::getText () const { return this->m_text; }

const std::string& CText::getHorizontalAlign () const { return this->m_horizontalAlign; }

const std::string& CText::getVerticalAlign () const { return this->m_verticalAlign; }

void CText::setHorizontalAlign (const std::string& value) { this->m_horizontalAlign = value; }

void CText::setVerticalAlign (const std::string& value) { this->m_verticalAlign = value; }

void CText::setup () {
    const auto& font = this->m_text.font;
    bool systemFont = font.empty () || font.rfind ("systemfont_", 0) == 0;
    std::string from;

    // the scene's (or WE's) font file; one that can't be read or opened falls back to arial (GH 0x1401d9d70)
    if (!systemFont) {
	try {
	    const auto stream = this->getAssetLocator ().read (font);
	    std::vector<uint8_t> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };
	    if (this->m_fonts.loadMemory (std::move (data))) {
		from = font;
	    } else {
		sLog.error ("CText: cannot open font '", font, "' of '", this->m_text.name, "', using systemfont_arial");
	    }
	} catch (const std::exception& e) {
	    sLog.error ("CText: cannot read font '", font, "' of '", this->m_text.name, "': ", e.what (), ", using systemfont_arial");
	}
    }

    if (!this->m_fonts.loaded ()) {
	const auto found = TextFonts::systemFont (systemFont ? font : std::string ("systemfont_arial"));
	if (found.has_value () && this->m_fonts.loadFile (found->first, found->second)) {
	    from = found->first;
	}
    }

    this->m_hasFont = this->m_fonts.loaded ();
    if (this->m_hasFont) {
	sLog.out ("CText: '", this->m_text.name, "' (", this->m_text.id, ") font ", font.empty () ? "(none)" : font, " -> ", from);
    } else {
	sLog.error ("CText: no usable font for '", this->m_text.name, "' (", font, "): fontconfig found none");
    }

    // laid out before the passes are built: the effects' FBOs take the layer's size
    this->relayout (this->m_text.text->value->getString ());
    this->rasterise ();

    // the editor saves the layer's size (laid out from the same text): WPENGINE_TEXT_DEBUG compares the two
    if (getenv ("WPENGINE_TEXT_DEBUG") != nullptr) {
	sLog.out ("CText size: ", this->m_text.id, " '", this->m_text.name, "' laid out ", this->getSize ().x, " x ",
		  this->getSize ().y, ", saved ", this->m_text.size.x, " x ", this->m_text.size.y, ", colour ", this->m_laidOut.colour.r, " ",
		  this->m_laidOut.colour.g, " ", this->m_laidOut.colour.b, ", ", this->m_layout.glyphs.size (), " glyphs");
    }

    CImage::setup ();

    if (this->m_text.text->value->getScriptSource ().has_value ()) {
	this->initScriptLayer ();
    }
}

void CText::initScriptLayer () {
    const auto& script = this->m_text.text->value->getScriptSource ();

    this->m_layerHandle = this->getScene ().getScriptEngine ().createLayerScript (
	*script, this->m_text.text->value->getProperties (), this->m_text.text->value->getString (), *this
    );

    if (this->m_layerHandle == Scripting::kInvalidLayerHandle) {
	sLog.error ("CText: createLayerScript failed for '", this->m_text.name, "'");
    }
}

std::string CText::currentText () {
    if (this->m_layerHandle != Scripting::kInvalidLayerHandle) {
	auto& se = this->getScene ().getScriptEngine ();
	se.tickLayer (
	    this->m_layerHandle, static_cast<double> (this->getScene ().getTime ()),
	    static_cast<double> (this->getScene ().getDeltaTime ()), static_cast<double> (this->getScene ().getFps ())
	);
	return se.layerText (this->m_layerHandle);
    }

    // a literal, a user property, or a value another layer's script wrote
    if (this->m_text.text->value->getType () == DynamicValue::String) {
	return this->m_text.text->value->getString ();
    }

    return this->m_laidOut.text;
}

bool CText::relayout (const std::string& text) {
    TextLayoutParams params;
    params.pointSize = this->m_text.pointSize->value->getFloat ();
    params.horizontalAlign = horizontalAlignOf (this->m_horizontalAlign);
    params.verticalAlign = verticalAlignOf (this->m_verticalAlign);
    params.spacingX = this->m_text.spacing.x;
    params.spacingY = this->m_text.spacing.y;
    // the limits count only while switched on (GH 0x14027c600: maxwidth with flag 4, maxrows with flag 8)
    params.maxWidth = this->m_text.limitWidth ? std::max (0.0f, this->m_text.maxWidth->value->getFloat ()) : 0.0f;
    params.maxRows = this->m_text.limitRows ? std::max (0, this->m_text.maxRows->value->getInt ()) : 0;
    params.ellipsis = this->m_text.limitUseEllipsis;
    params.blockAlign = this->m_text.blockAlign;

    // WE pads the text's texture while the layer has effects (GH 0x14027dd90, 0x14027d250)
    const bool padded = !this->getImage ().effects.empty ();

    if (this->m_laidOut.valid && this->m_laidOut.text == text && this->m_laidOut.params == params
	&& this->m_laidOut.padded == padded) {
	return false;
    }

    this->m_laidOut.text = text;
    this->m_laidOut.params = params;
    this->m_laidOut.padded = padded;
    this->m_laidOut.valid = true;
    this->m_layout = layoutText (this->m_fonts, decodeUtf8 (text), params);
    return true;
}

void CText::rasterise () {
    const auto& layout = this->m_layout;
    const glm::vec2 padding = this->m_laidOut.padded ? glm::min (this->m_text.padding, glm::vec2 (512.0f)) : glm::vec2 (0.0f);

    // the layer's size: the block (2 × 2 without one) and the padding on each side (GH 0x14027dd90)
    const glm::vec2 block = layout.empty ? glm::vec2 (2.0f) : glm::vec2 (layout.maxX - layout.minX, layout.maxY - layout.minY);
    const auto width = static_cast<int> (std::max (1.0f, std::ceil (block.x + 2.0f * padding.x)));
    const auto height = static_cast<int> (std::max (1.0f, std::ceil (block.y + 2.0f * padding.y)));

    // the text's colour with the coverage as alpha (the base pass multiplies alpha in). Row 0 is the top
    const glm::vec3 colour = glm::clamp (this->m_text.color->value->getVec3 (), glm::vec3 (0.0f), glm::vec3 (1.0f));
    this->m_laidOut.colour = colour;
    const uint8_t rgb[3] = { static_cast<uint8_t> (std::lround (colour.r * 255.0f)), static_cast<uint8_t> (std::lround (colour.g * 255.0f)),
			     static_cast<uint8_t> (std::lround (colour.b * 255.0f)) };
    std::vector<uint8_t> pixels (static_cast<size_t> (width) * height * 4, 0);
    for (size_t i = 0; i < pixels.size (); i += 4) {
	pixels[i] = rgb[0];
	pixels[i + 1] = rgb[1];
	pixels[i + 2] = rgb[2];
    }

    for (const auto& glyph : layout.glyphs) {
	const auto* bitmap = glyph.bitmap;
	const int x0 = static_cast<int> (std::lround (padding.x + glyph.x - layout.minX));
	const int y0 = static_cast<int> (std::lround (padding.y + layout.maxY - glyph.y));

	for (int row = 0; row < bitmap->rows; row++) {
	    const int y = y0 + row;
	    if (y < 0 || y >= height) {
		continue;
	    }
	    for (int col = 0; col < bitmap->width; col++) {
		const int x = x0 + col;
		if (x < 0 || x >= width) {
		    continue;
		}
		uint8_t& alpha = pixels[(static_cast<size_t> (y) * width + x) * 4 + 3];
		alpha = std::max (alpha, bitmap->pixels[static_cast<size_t> (row) * bitmap->pitch + col]);
	    }
	}
    }

    this->m_target->resize (width, height, width, height);
    glBindTexture (GL_TEXTURE_2D, this->m_target->getTextureID (0));
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    // the text is usually drawn smaller than it is rasterised (300 dpi): mipmaps keep it from aliasing
    glGenerateMipmap (GL_TEXTURE_2D);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

    this->resizeTargets ({ static_cast<float> (width), static_cast<float> (height) });

    // where the block sits on the origin (GH 0x14027c600): horizontally its left edge, centre or right edge; vertically
    // the first line's ascender (top), half of it less the lines below (centre) or the last line's descender (bottom)
    glm::vec2 offset (0.0f);
    if (!layout.empty) {
	const float halfWidth = (layout.maxX - layout.minX) * 0.5f;
	const int horizontal = this->m_laidOut.params.horizontalAlign;
	offset.x = horizontal == 0 ? halfWidth : horizontal == 2 ? -halfWidth : 0.0f;

	const float below = static_cast<float> (layout.rows - 1) * layout.lineHeight;
	const int vertical = this->m_laidOut.params.verticalAlign;
	const float reference = vertical == 0 ? layout.descender - below
	    : vertical == 1		      ? (layout.ascender - below) * 0.5f
					      : layout.ascender;
	offset.y = (layout.maxY - (layout.maxY - layout.minY) * 0.5f) - reference;
    }
    this->setLocalOffset (offset);
}

void CText::render () {
    // the script runs while the layer is hidden too (it may be what shows it again)
    const std::string text = this->currentText ();

    // a new text or layout, or a new colour (a script, a user property): drawn again
    if (this->relayout (text) || this->m_text.color->value->getVec3 () != this->m_laidOut.colour) {
	this->rasterise ();
    }

    CImage::render ();
}
