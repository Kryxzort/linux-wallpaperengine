#pragma once

#include <map>
#include <string>

#include <glm/vec2.hpp>

#include "CFBO.h"
#include "WallpaperEngine/Data/Model/Effect.h"

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class FBOProvider {
public:
    explicit FBOProvider (const FBOProvider* parent);

    /** An effect's FBO, in the format its `format` names (`hdr`: the scene renders HDR, for the backbuffer formats) */
    std::shared_ptr<CFBO> create (const FBO& base, uint32_t flags, glm::vec2 size, bool hdr);
    std::shared_ptr<CFBO> create (
	const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
	glm::vec2 textureSize
    );
    /**
     * An effect FBO's size for a layer of `size` (GH 0x140216080): `width`/`height` replace the layer's, `fit` caps
     * the longer side keeping the aspect (the other side truncated), then divided by `scale`
     */
    [[nodiscard]] static glm::vec2 sizeFor (float scale, uint32_t fit, uint32_t width, uint32_t height, glm::vec2 size);
    /** WE's FBO format name → its texture format; `rgba_backbuffer`/`rgb_backbuffer` are half float in HDR */
    [[nodiscard]] static TextureFormat formatFromName (const std::string& name, bool hdr);
    std::shared_ptr<CFBO> alias (const std::string& newName, const std::string& original);
    [[nodiscard]] std::shared_ptr<CFBO> find (const std::string& name) const;

private:
    const FBOProvider* m_parent;
    std::map<std::string, std::shared_ptr<CFBO>> m_fbos = {};
};
}
