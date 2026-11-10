#pragma once

#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "CImage.h"
#include "TextLayout.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * A text layer: an image layer whose picture is its laid-out text, the way Wallpaper Engine's text class extends its
 * image class (GH 0x14027c1b0). The text is laid out as WE does (TextLayout, decoded from GH 0x1401dd330), rasterised
 * into the layer's texture `_rt_textLayer_<id>`, and drawn through CImage: effects, parent, angles, parallax, alpha,
 * colour and blend modes come with it.
 *
 * The layer's size is the block's (plus the padding while it has effects, GH 0x14027dd90) and its quad sits at the
 * offset from the origin that the alignment gives (GH 0x14027c600). Both follow the text: when it changes the
 * layer's FBOs are resized in place (CImage::resizeTargets)
 */
class CText final : public CImage {
public:
    CText (Wallpapers::CScene& scene, const Text& text);
    ~CText () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const Text& getText () const;
    /** ITextLayer.horizontalalign / verticalalign: "left" "center" "right" / "top" "center" "bottom", live */
    [[nodiscard]] const std::string& getHorizontalAlign () const;
    [[nodiscard]] const std::string& getVerticalAlign () const;
    void setHorizontalAlign (const std::string& value);
    void setVerticalAlign (const std::string& value);
    /** Runs the layer's text script now (a sorted 3D scene runs its scripts in the scene's object order before it
     *  draws in the sorted order); the text is what render () then shows */
    void tickScript ();

private:
    /** Lays the text out again when it, its size or its layout settings changed; true when it did */
    bool relayout (const std::string& text);
    /** Draws m_layout into the layer's texture and gives the layer its size and offset */
    void rasterise ();
    /** The text the layer shows this frame: its script's, a value another script or a property wrote, or its own */
    std::string currentText ();
    void initScriptLayer ();

    const Text& m_text;
    /** The layer's texture (the image's g_Texture0) */
    std::shared_ptr<CFBO> m_target;
    TextFonts m_fonts;
    TextLayout m_layout;
    /** What m_layout was made from */
    struct {
	std::string text;
	TextLayoutParams params;
	bool padded = false;
	bool valid = false;
	glm::vec3 colour = glm::vec3 (-1.0f);
    } m_laidOut;
    Scripting::ScriptLayerHandle m_layerHandle = Scripting::kInvalidLayerHandle;
    /** The text tickScript () got this frame, taken by the next render () */
    std::optional<std::string> m_tickedText;
    bool m_hasFont = false;
    std::string m_horizontalAlign;
    std::string m_verticalAlign;
};
} // namespace WallpaperEngine::Render::Objects
