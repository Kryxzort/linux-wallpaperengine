#include <glad/glad.h>

#include "CWallpaper.h"
#include "RenderContext.h"

#include "AlbumTexture.h"
#include "CTexture.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Data/Parsers/TextureParser.h"
#include "WallpaperEngine/Logging/Log.h"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace {
using namespace WallpaperEngine::Data::Assets;

/**
 * The image format of a texture name the host set to an image file's absolute path (a scenetexture property set to a
 * preset's `files/<name>`, or to a picked file), FIF_UNKNOWN for any other name. WE's own names are relative
 * (`materials/<name>.tex` inside the scene's folders, which refuse an absolute path), so nothing else takes this route.
 */
FIF imageFileFormat (const std::string& name) {
    if (name.empty () || name.front () != '/') {
	return FIF_UNKNOWN;
    }
    std::string extension = std::filesystem::path (name).extension ().string ();
    std::ranges::transform (extension, extension.begin (), [] (unsigned char c) { return std::tolower (c); });
    if (extension == ".png") {
	return FIF_PNG;
    }
    if (extension == ".jpg" || extension == ".jpeg") {
	return FIF_JPEG;
    }
    if (extension == ".gif") {
	return FIF_GIF;
    }
    if (extension == ".bmp") {
	return FIF_BMP;
    }
    if (extension == ".tga") {
	return FIF_TARGA;
    }
    return FIF_UNKNOWN;
}

/**
 * The image file as the texture a .tex wrapping a PNG / JPEG is (TEXB0003 with a FreeImage format): one image, one level
 * holding the file's bytes, decoded to RGBA8 by stb_image when it uploads (a GIF's first frame), its size its own (no
 * power-of-two padding, so an image object's UVs stay whole) and clamped at the edges as WE's image textures are. Throws
 * AssetLoadException for a file that can't be read or isn't an image stb_image knows, which every caller reports and
 * survives by keeping the slot's own texture.
 */
TextureUniquePtr imageFileTexture (const std::string& path, FIF format) {
    std::ifstream file (path, std::ios::binary);
    if (!std::filesystem::is_regular_file (path) || !file) {
	throw WallpaperEngine::Assets::AssetLoadException (
	    "Cannot read image file", path, std::make_error_code (std::errc::no_such_file_or_directory)
	);
    }
    std::vector<char> bytes ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
    int width = 0, height = 0, channels = 0;
    if (bytes.empty () || bytes.size () > static_cast<size_t> (INT32_MAX)
	|| !stbi_info_from_memory (
	    reinterpret_cast<const stbi_uc*> (bytes.data ()), static_cast<int> (bytes.size ()), &width, &height, &channels
	)
	|| width <= 0 || height <= 0) {
	throw WallpaperEngine::Assets::AssetLoadException (
	    "Not an image file", path, std::make_error_code (std::errc::invalid_argument)
	);
    }

    auto mipmap = std::make_shared<Mipmap> ();
    mipmap->width = static_cast<uint32_t> (width);
    mipmap->height = static_cast<uint32_t> (height);
    mipmap->uncompressedSize = static_cast<int> (bytes.size ());
    mipmap->uncompressedData = std::make_unique<char[]> (bytes.size ());
    std::ranges::copy (bytes, mipmap->uncompressedData.get ());

    auto texture = std::make_unique<Texture> ();
    texture->containerVersion = ContainerVersion_TEXB0003;
    texture->format = TextureFormat_ARGB8888;
    texture->flags = TextureFlags_ClampUVs;
    texture->freeImageFormat = format;
    texture->width = texture->textureWidth = mipmap->width;
    texture->height = texture->textureHeight = mipmap->height;
    texture->imageCount = 1;
    texture->images.emplace (0, MipmapList { mipmap });
    return texture;
}
} // namespace

namespace WallpaperEngine::Render {
RenderContext::RenderContext (Context& context, Assets::AssetLocator& locator) :
    m_textureCache (*context.texture_cache), m_context (context), m_locator (locator) {
    // these textures are special cases, so make sure they're created only upon request
    this->m_currentThumbnail = std::make_shared<AlbumTexture> (*this);

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_currentThumbnail->getTextureID (0), -1, "$mediaThumbnail");
#endif

    this->m_previousThumbnail = std::make_shared<AlbumTexture> (*this);

#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_previousThumbnail->getTextureID (0), -1, "$mediaPreviousThumbnail");
#endif

    // load the latest texture (if available)
    this->m_currentThumbnail->load ();

    // add these to the cache and return the right one
    this->m_textureCache.store ("$mediaThumbnail", this->m_currentThumbnail);
    this->m_textureCache.store ("$mediaPreviousThumbnail", this->m_previousThumbnail);
}

const Context& RenderContext::getContext () const { return this->m_context; }

std::shared_ptr<const TextureProvider> RenderContext::resolveTexture (const std::string& name) {
    try {
	return this->m_textureCache.resolve (name);
    } catch (const Assets::AssetLoadException& e) {
	if (const FIF format = imageFileFormat (name); format != FIF_UNKNOWN) {
	    auto texture = std::make_shared<CTexture> (*this, imageFileTexture (name, format), this->m_deferTextureUploads);
	    sLog.debug ("Loaded image file ", name, " as a texture");
	    this->m_textureCache.store (name, texture);
	    if (this->m_deferTextureUploads) {
		this->m_deferredTextures.push_back (texture);
	    }
	    return texture;
	}

	const auto& locator = this->getAssetLocator ();
	const auto contents = locator.texture (name);
	const auto stream = BinaryReader (contents);

	auto metadataLoader = [&locator] (const std::string& filename) -> std::string {
	    return locator.readString (std::filesystem::path ("materials") / filename);
	};

	auto parsedTexture = Data::Parsers::TextureParser::parse (stream, name, metadataLoader);
	auto texture = std::make_shared<CTexture> (*this, std::move (parsedTexture), this->m_deferTextureUploads);

	this->m_textureCache.store (name, texture);
	if (this->m_deferTextureUploads) {
	    this->m_deferredTextures.push_back (texture);
	}

	return texture;
    }
}

void RenderContext::deferTextureUploads () { this->m_deferTextureUploads = true; }

void RenderContext::finishTextureUploads () {
    this->m_deferTextureUploads = false;
    for (const auto& texture : this->m_deferredTextures) {
	texture->finishUpload ();
    }
    this->m_deferredTextures.clear ();
}

const Assets::AssetLocator& RenderContext::getAssetLocator () const { return this->m_locator; }

void RenderContext::albumArtUrlChange (std::optional<std::string>& url) {
    if (this->m_currentThumbnail->isReady ()) {
	// copy over pixel data and setup the new texture with the new data
	this->m_previousThumbnail->copyContents (*this->m_currentThumbnail);
    }

    // load the next image
    this->m_currentThumbnail->load ();
}

} // namespace WallpaperEngine::Render
