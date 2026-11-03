#include "CRenderable.h"
#include "WallpaperEngine/Context.h"

#include <algorithm>
#include <cmath>

#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;

CRenderable::CRenderable (Wallpapers::CScene& scene, const Object& object, const Material& material) :
    CObject (scene, object), Render::FBOProvider (&scene), m_material (material) { }

void CRenderable::detectTexture () {
    if (TextureMap* textures = &(*this->m_material.passes.begin ())->textures; !textures->empty ()) {
	std::string textureName = textures->begin ()->second;

	if (textureName.find ("_rt_") == 0 || textureName.find ("_alias_") == 0) {
	    this->m_texture = this->getScene ().findFBO (textureName);
	} else {
	    this->m_texture = this->getContext ().resolveTexture (textureName);
	}
    }
}

void CRenderable::setup () {
    CObject::setup ();

    // calculate full animation time (if any)
    this->m_animationTime = 0.0f;

    for (const auto& cur : this->getTexture ()->getFrames ()) {
	this->m_animationTime += cur->frametime;
    }
}

std::shared_ptr<const TextureProvider> CRenderable::getTexture () const { return this->m_texture; }

double CRenderable::getAnimationTime () const { return this->m_animationTime; }
double CRenderable::getTextureAnimationTime () const {
    if (this->m_textureAnimation.detached) {
	return this->m_textureAnimation.time;
    }

    return static_cast<double> (this->getContext ().getContext ().renderTime);
}

int CRenderable::getTextureFrameCount () const {
    const auto texture = this->getTexture ();

    if (texture == nullptr || !texture->isAnimated ()) {
	return 0;
    }

    return static_cast<int> (texture->getFrames ().size ());
}

int CRenderable::getTextureFrame () const {
    const auto texture = this->getTexture ();

    if (texture == nullptr || !texture->isAnimated () || this->m_animationTime <= 0.0) {
	return 0;
    }

    // the same walk CPass::resolveTextureAnimationState does: the frame whose span holds the time
    double time = std::fmod (this->getTextureAnimationTime (), this->m_animationTime);
    int index = 0;

    for (const auto& frame : texture->getFrames ()) {
	time -= frame->frametime;

	if (time <= 0.0) {
	    return index;
	}

	index++;
    }

    return std::max (0, index - 1);
}

CRenderable::TextureAnimationState& CRenderable::detachTextureAnimation () {
    if (!this->m_textureAnimation.detached) {
	// carry on from where the shared animation is
	this->m_textureAnimation.time = static_cast<double> (this->getContext ().getContext ().renderTime);
	this->m_textureAnimation.detached = true;
    }

    return this->m_textureAnimation;
}

void CRenderable::joinTextureAnimation () { this->m_textureAnimation = {}; }

const CRenderable::TextureAnimationState& CRenderable::getTextureAnimationState () const {
    return this->m_textureAnimation;
}

void CRenderable::setTextureFrame (int frame) {
    auto& state = this->detachTextureAnimation ();
    const auto texture = this->getTexture ();

    if (texture == nullptr || !texture->isAnimated () || this->m_animationTime <= 0.0) {
	return;
    }

    const auto& frames = texture->getFrames ();
    const int count = static_cast<int> (frames.size ());

    frame = ((frame % count) + count) % count;

    // a time inside the frame's span: its start plus a little (resolveTextureAnimationState picks the frame whose
    // running sum first reaches the time; exactly the start would be the previous frame's end)
    double start = 0.0;

    for (int i = 0; i < frame; i++) {
	start += frames[i]->frametime;
    }

    state.time = start + std::min (1e-4, frames[frame]->frametime * 0.5);
}

void CRenderable::advanceTextureAnimation (double dt) {
    auto& state = this->m_textureAnimation;

    if (state.detached && state.playing && dt > 0.0) {
	state.time += dt * state.rate;
	// keep it in one cycle (precision over a long run)
	if (this->m_animationTime > 0.0 && state.time > this->m_animationTime) {
	    state.time = std::fmod (state.time, this->m_animationTime);
	}
    }
}
