#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "TextureCache.h"
namespace WallpaperEngine {
namespace Media {
    class MediaSource;
}
namespace Assets {
    class AssetLocator;
}
class Context;
namespace Render {
    class AlbumTexture;
    class CTexture;
    class TextureCache;

    class RenderContext {
    public:
	explicit RenderContext (Context& context, Assets::AssetLocator& locator);

	[[nodiscard]] const Context& getContext () const;
	[[nodiscard]] std::shared_ptr<const TextureProvider> resolveTexture (const std::string& name);
	[[nodiscard]] const Assets::AssetLocator& getAssetLocator () const;

	void albumArtUrlChange (std::optional<std::string>& url);

	/**
	 * From now until finishTextureUploads, a new JPEG / PNG / … texture decodes on a pool thread while the caller
	 * goes on building: decoded one at a time on the GL thread, they are the largest part of a big scene's build
	 */
	void deferTextureUploads ();
	/** Uploads every texture deferred since deferTextureUploads, in order, and stops deferring */
	void finishTextureUploads ();

    private:
	/** The previous album thumbnail texture */
	std::shared_ptr<const AlbumTexture> m_previousThumbnail = nullptr;
	/** The current album thumbnail texture */
	std::shared_ptr<const AlbumTexture> m_currentThumbnail = nullptr;
	/** Texture cache for the render */
	TextureCache& m_textureCache;
	/** The context that owns this render context */
	Context& m_context;
	/** The asset locator used for this render context */
	Assets::AssetLocator& m_locator;
	/** New textures defer their upload (deferTextureUploads) */
	bool m_deferTextureUploads = false;
	/** The textures created while deferring */
	std::vector<std::shared_ptr<const CTexture>> m_deferredTextures;
    };
} // namespace Render
} // namespace WallpaperEngine
