#pragma once
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render::Objects {
class CRenderable : virtual public CObject, public FBOProvider {
    friend CObject;

public:
    CRenderable (Wallpapers::CScene& scene, const Object& object, const Material& material);

    [[nodiscard]] std::shared_ptr<const TextureProvider> getTexture () const;

    [[nodiscard]] double getAnimationTime () const;

    /**
     * SceneScript's ITextureAnimation. By default a texture animation runs on the scene's clock (WE: "all instances of
     * an animated texture share an animation state"); a script call that changes it detaches this object onto its own
     * clock (`join ()` returns to the shared one)
     */
    struct TextureAnimationState {
	bool detached = false;
	bool playing = true;
	double rate = 1.0;
	/** the object's own animation time in seconds (only while detached) */
	double time = 0.0;
    };

    /** @return The time the texture animation is at: the scene's clock, or the object's own clock once detached */
    [[nodiscard]] double getTextureAnimationTime () const;
    /** @return The number of frames of the texture animation (0: not animated) */
    [[nodiscard]] int getTextureFrameCount () const;
    /** @return The frame the texture animation shows now */
    [[nodiscard]] int getTextureFrame () const;
    /** Detaches the animation (from now on the object's own clock) and puts it at the start of `frame` */
    void setTextureFrame (int frame);
    /** play/pause/stop/rate/join: detach and change the object's own clock */
    TextureAnimationState& detachTextureAnimation ();
    void joinTextureAnimation ();
    [[nodiscard]] const TextureAnimationState& getTextureAnimationState () const;
    /** Advances the object's own animation clock by dt (scene seconds) when detached and playing */
    void advanceTextureAnimation (double dt);

    void setup () override;

    [[nodiscard]] virtual const float& getBrightness () const = 0;
    [[nodiscard]] virtual const float& getUserAlpha () const = 0;
    [[nodiscard]] virtual const float& getAlpha () const = 0;
    [[nodiscard]] virtual const glm::vec3& getColor () const = 0;
    [[nodiscard]] virtual const glm::vec4& getColor4 () const = 0;
    [[nodiscard]] virtual const glm::vec3& getCompositeColor () const = 0;

protected:
    void detectTexture ();

    double m_animationTime = 0.0;
    TextureAnimationState m_textureAnimation = {};

    std::shared_ptr<const TextureProvider> m_texture = nullptr;
    const Material& m_material;
};
}
