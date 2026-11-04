#include "FBOProvider.h"

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

FBOProvider::FBOProvider (const FBOProvider* parent) : m_parent (parent) { }

TextureFormat FBOProvider::formatFromName (const std::string& name, const bool hdr) {
    // WE's effect FBO parse (GH 0x140216080 → 0x1402116c0): the backbuffer formats follow the scene's HDR, the rest are
    // the .tex format names
    if (name == "rgba_backbuffer") {
	return hdr ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
    }
    if (name == "rgb_backbuffer") {
	return hdr ? TextureFormat_RGB161616f : TextureFormat_RGB888;
    }

    static const std::map<std::string, TextureFormat> named = {
	{ "rgba8888", TextureFormat_ARGB8888 },	       { "rgb888", TextureFormat_RGB888 },
	{ "rg88", TextureFormat_RG88 },		       { "r8", TextureFormat_R8 },
	{ "rgb565", TextureFormat_RGB565 },	       { "rgba16161616f", TextureFormat_RGBA16161616f },
	{ "rgb161616f", TextureFormat_RGB161616f },     { "rg1616f", TextureFormat_RG1616f },
	{ "r16f", TextureFormat_R16f },		       { "rgba1010102", TextureFormat_RGBa1010102 },
    };

    if (const auto it = named.find (name); it != named.end ()) {
	return it->second;
    }

    // WE's other names are compressed or 32-bit formats no effect renders into; anything else is its default
    return TextureFormat_ARGB8888;
}

std::shared_ptr<CFBO> FBOProvider::create (const FBO& base, uint32_t flags, const glm::vec2 size, const bool hdr) {
    return this->m_fbos[base.name] = std::make_shared<CFBO> (
	       base.name, formatFromName (base.format, hdr), flags, base.scale, size.x / base.scale,
	       size.y / base.scale, size.x / base.scale, size.y / base.scale
	   );
}

std::shared_ptr<CFBO> FBOProvider::create (
    const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
    glm::vec2 textureSize
) {
    return this->m_fbos[name] = std::make_shared<CFBO> (
	       name, format, flags, scale, realSize.x, realSize.y, textureSize.x, textureSize.y
	   );
}

std::shared_ptr<CFBO> FBOProvider::alias (const std::string& newName, const std::string& original) {
    return this->m_fbos[newName] = this->m_fbos[original];
}

std::shared_ptr<CFBO> FBOProvider::find (const std::string& name) const {
    if (const auto it = this->m_fbos.find (name); it != this->m_fbos.end ()) {
	return it->second;
    }

    if (this->m_parent == nullptr) {
	return nullptr;
    }

    return this->m_parent->find (name);
}