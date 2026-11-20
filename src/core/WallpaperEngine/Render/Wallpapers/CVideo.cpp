#include "CVideo.h"

#include "WallpaperEngine/Context.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/VideoPlayback/GLPlayer.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Wallpapers;
using namespace WallpaperEngine::VideoPlayback;

CVideo::CVideo (const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext) :
    CWallpaper (wallpaper, context, audioContext, nullptr) {
    // setup framebuffers
    this->setupFramebuffers ();

    const std::filesystem::path videopath
	= this->getVideo ().project.assetLocator->physicalPath (this->getVideo ().filename);

    // create a player with a small framebuffer, resized to the video's at its first frame. The core's video player
    // decodes no sound: a video wallpaper plays silent
    this->m_player = std::make_unique<GLPlayer> (
	this->getContext (), this->CWallpaper::getWallpaperTexture (), videopath, 64, 64,
	this->CWallpaper::getWallpaperFramebuffer ()
    );
    // make sure the video has at least one usage marked, this ensures the video plays
    this->m_player->incrementUsageCount ();
}

CVideo::~CVideo () { this->m_player->decrementUsageCount (); }

void CVideo::renderFrame () {
    this->m_player->render ();
}

const Data::Model::Video& CVideo::getVideo () const { return *this->getWallpaperData ().as<Data::Model::Video> (); }

void CVideo::setPause (bool newState) {
    if (newState) {
	this->m_player->setPaused ();
    } else {
	this->m_player->clearPaused ();
    }
}

int CVideo::getWidth () const { return this->m_player ? this->m_player->getWidth () : 16; }

int CVideo::getHeight () const { return this->m_player ? this->m_player->getHeight () : 16; }
