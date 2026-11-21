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
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace {
using namespace WallpaperEngine::Data::Assets;

/**
 * The largest image file read for a texture name that is an image path, the largest side it may decode to, and the most
 * pixels it may decode to (8192², 256 MiB as RGBA8: an 8K image fits; a flat 16384² PNG of under 1 MiB would be 1 GiB,
 * and a preset can fill every image slot of its scene).
 */
constexpr std::uintmax_t MAX_IMAGE_FILE_BYTES = std::uintmax_t (64) << 20;
constexpr int MAX_IMAGE_SIDE = 16384;
constexpr std::uint64_t MAX_IMAGE_PIXELS = std::uint64_t (8192) * 8192;

/**
 * The image format of a texture name the host set to an image file's absolute path (a scenetexture property set to a
 * preset's `files/<name>`, or to a picked file), FIF_UNKNOWN for any other name. WE's own names are relative
 * (`materials/<name>.tex` inside the scene's folders, which refuse an absolute path); a scene's files can still name an
 * absolute image path, so `imageFileTexture` caps what it reads and decodes.
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
 * AssetLoadException for a file that can't be read, isn't an image stb_image knows, or is past the caps (no more than
 * MAX_IMAGE_FILE_BYTES is ever read, an image over MAX_IMAGE_SIDE a side or MAX_IMAGE_PIXELS is never decoded: any
 * absolute image path takes this route, so a size is no promise), which every caller reports and survives by keeping
 * the slot's own texture.
 */
TextureUniquePtr imageFileTexture (const std::string& path, FIF format) {
    std::ifstream file (path, std::ios::binary);
    if (!std::filesystem::is_regular_file (path) || !file) {
	throw WallpaperEngine::Assets::AssetLoadException (
	    "Cannot read image file", path, std::make_error_code (std::errc::no_such_file_or_directory)
	);
    }
    const auto tooLarge = [&path] () {
	return WallpaperEngine::Assets::AssetLoadException (
	    "Image file over 64 MiB", path, std::make_error_code (std::errc::file_too_large)
	);
    };
    std::error_code sizeError;
    const auto size = std::filesystem::file_size (path, sizeError);
    if (sizeError) {
	throw WallpaperEngine::Assets::AssetLoadException ("Cannot size image file", path, sizeError);
    }
    if (size > MAX_IMAGE_FILE_BYTES) {
	throw tooLarge ();
    }
    // read from the open file, never past the cap: it can still grow after the size check (a download landing)
    std::vector<char> bytes;
    bytes.reserve (static_cast<size_t> (size));
    std::array<char, 1 << 16> chunk {};
    while (file.read (chunk.data (), chunk.size ()) || file.gcount () > 0) {
	bytes.insert (bytes.end (), chunk.data (), chunk.data () + file.gcount ());
	if (bytes.size () > MAX_IMAGE_FILE_BYTES) {
	    throw tooLarge ();
	}
    }
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
    if (width > MAX_IMAGE_SIDE || height > MAX_IMAGE_SIDE) {
	throw WallpaperEngine::Assets::AssetLoadException (
	    "Image over 16384 px a side", path, std::make_error_code (std::errc::value_too_large)
	);
    }
    if (static_cast<std::uint64_t> (width) * static_cast<std::uint64_t> (height) > MAX_IMAGE_PIXELS) {
	throw WallpaperEngine::Assets::AssetLoadException (
	    "Image over 64 Mpx", path, std::make_error_code (std::errc::value_too_large)
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
