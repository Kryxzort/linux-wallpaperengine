#pragma once

#include <utility>
#include <glad/glad.h>

#include <map>
#include <tuple>

#include "linux-wallpaperengine/project.h"

#include "WallpaperEngine/Audio/AudioContext.h"

#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"

#include "FBOProvider.h"
#include "WallpaperEngine/Assets/AssetLocator.h"

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser {
class WebBrowserContext;
}

namespace WallpaperEngine::Render {
namespace Helpers {
    class ContextAware;
}

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Audio;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::FileSystem;

class CWallpaper : public Helpers::ContextAware, public FBOProvider, public TypeCaster {
    friend class WallpaperEngine::Application::WallpaperApplication;

public:
    virtual ~CWallpaper () override;

    /**
     * Performs a render pass of the wallpaper
     */
    void render ();

    /**
     * Pause the renderer
     */
    virtual void setPause (bool newState);

    /**
     * @return The container to resolve files for this wallpaper
     */
    [[nodiscard]] const Assets::AssetLocator& getAssetLocator () const;

    /**
     * @return The current audio context for this wallpaper
     */
    AudioContext& getAudioContext () const;

    /**
     * @return The scene's framebuffer
     */
    [[nodiscard]] virtual GLuint getWallpaperFramebuffer () const;
    /**
     * @return The scene's texture
     */
    [[nodiscard]] virtual GLuint getWallpaperTexture () const;
    /**
     * Searches the FBO list for the given FBO
     *
     * @param name
     * @return
     */
    [[nodiscard]] std::shared_ptr<const CFBO> findFBO (const std::string& name) const;

    /**
     * @return The main FBO of this wallpaper
     */
    [[nodiscard]] std::shared_ptr<const CFBO> getFBO () const;

    /**
     * A copy of `source` taken now, for a pass that samples the FBO it draws into. WE copies the framebuffer before
     * a pass samples it (GH 0x140214b50: `_rt_FullFrameBuffer`'s copy runs before an image whose material or blend
     * mode reads it); sampling the target being drawn is a GL feedback loop. One copy per size and flags, shared.
     * Leaves the copy bound as the draw framebuffer.
     */
    [[nodiscard]] std::shared_ptr<const CFBO> copyForSampling (const CFBO& source) const;

    /**
     * Updates the mouse input handler for this wallpaper
     *
     * @param newMouseInput The new mouse input to use for this wallpaper
     */
    void setMouseInputHandler (wp_mouse_input* newMouseInput);
    /** The rate the host means to draw at (wp_project_set_frame_rate; 0: unknown): a simulation effect steps at it */
    void setFrameRate (float hz) { this->m_frameRate = hz; }
    [[nodiscard]] float getFrameRate () const { return this->m_frameRate; }
    /** The pointer's next position is a jump (wp_project_pointer_jump): the frame that reads it takes it as the last too */
    void markPointerJump () { this->m_pointerJump = true; }
    /** Whether a jump was marked since the last call */
    bool takePointerJump () { return std::exchange (this->m_pointerJump, false); }

    /**
     * Updates the destination framebuffer for this wallpaper
     *
     * @param framebuffer
     */
    void setDestinationFramebuffer (GLuint framebuffer) const;

    /**
     * @return The current destination framebuffer
     */
    GLuint getDestinationFramebuffer () const;

    /**
     * @return The width of this wallpaper
     */
    [[nodiscard]] virtual int getWidth () const = 0;

    /**
     * @return The height of this wallpaper
     */
    [[nodiscard]] virtual int getHeight () const = 0;

    /**
     * Creates a new instance of CWallpaper based on the information provided by the read backgrounds
     *
     * @param wallpaper
     * @param context
     * @param audioContext
     * @param mouseInput
     *
     * @return
     */
    static std::unique_ptr<CWallpaper> fromWallpaper (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext, wp_mouse_input* mouseInput
    );

protected:
    CWallpaper (
	const Wallpaper& wallpaperData, RenderContext& context, AudioContext& audioContext, wp_mouse_input* input
    );

    /**
     * Renders a frame of the wallpaper
     */
    virtual void renderFrame () = 0;

    /**
     * Setups OpenGL's framebuffers for ping-pong and scene rendering
     */
    void setupFramebuffers ();

    glm::dvec2 getLiveMousePosition () const;
    wp_mouse_input* getMouseInputHandler () const;

    const Wallpaper& m_wallpaperData;

    [[nodiscard]] const Wallpaper& getWallpaperData () const;

    /** The FBO used for scene output */
    std::shared_ptr<const CFBO> m_sceneFBO = nullptr;
    /**
     * The scene FBO's format (setupFramebuffers): half float for a scene WE renders HDR (WE: RGB161616f, GH 0x1401af5b0;
     * LWE keeps alpha), else RGBA8. The output copy to the host's framebuffer converts (and clamps) to its format
     */
    TextureFormat m_sceneFormat = TextureFormat_ARGB8888;

    GLuint m_vaoBuffer = GL_NONE;

private:
    /** copyForSampling's copies, by width, height and flags */
    mutable std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::shared_ptr<CFBO>> m_samplingCopies = {};

    /** The texture used for the scene output */
    GLuint m_texCoordBuffer = GL_NONE;
    GLuint m_positionBuffer = GL_NONE;
    GLuint m_shader = GL_NONE;
    // shader variables
    GLint g_Texture0 = GL_NONE;
    GLint a_Position = GL_NONE;
    GLint a_TexCoord = GL_NONE;
    /** The framebuffer to draw the background to */
    mutable GLuint m_destFramebuffer = GL_NONE;
    /** Setups OpenGL's shaders for this wallpaper backbuffer */
    void setupShaders ();
    /** List of FBOs registered for this wallpaper */
    std::map<std::string, std::shared_ptr<const CFBO>> m_fbos = {};
    /** Audio context that is using this wallpaper */
    AudioContext& m_audioContext;
    /** Current Wallpaper state */
    wp_mouse_input* m_mouseInput;
    float m_frameRate = 0.0f;
    bool m_pointerJump = false;
};
} // namespace WallpaperEngine::Render
