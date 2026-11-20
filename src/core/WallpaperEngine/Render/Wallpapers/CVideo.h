#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/VideoPlayback/GLPlayer.h"

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::VideoPlayback;

class CVideo final : public CWallpaper {
public:
    CVideo (const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext);

    ~CVideo () override;

    const Data::Model::Video& getVideo () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;

    void setPause (bool newState) override;

protected:
    void renderFrame () override;

    friend class CWallpaper;

private:
    GLPlayerUniquePtr m_player;

};
} // namespace WallpaperEngine::Render::Wallpapers
