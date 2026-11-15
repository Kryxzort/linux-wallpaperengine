#pragma once

#include <glad/glad.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include "linux-wallpaperengine/context.h"

namespace WallpaperEngine {
class Context;
}

namespace WallpaperEngine::Render {
/**
 * The thread deferred JPEG / PNG textures upload on, current on the host's upload context
 * (wp_context_set_gl_upload_context). NVIDIA converts a texture's pixels on the CPU of the thread that calls
 * glTexImage2D (about 1.6 GB/s, pixel buffer objects slower still), so on the render thread a scene's large images
 * cost their whole conversion before its first frame; here it runs while the wallpaper builds. One per Context,
 * started at its first texture and stopped in its destructor, after every texture of the cache is gone
 */
class TextureUploads {
public:
    /**
     * The thread starts once a context's deferred images reach this many RGBA8 bytes (~40 ms of conversion); the
     * ones before it upload on the render thread. Below it the thread costs more than it saves (starting it, its
     * context, its uploads beside the render thread's own GL calls): measured, a scene whose images convert in ~3 ms
     * built ~20 ms slower with a thread from the first image, and scenes with 30-60 MB of images 0-50 ms slower
     */
    static constexpr size_t kStartBytes = 64u << 20;

    /**
     * The context's upload thread for an image of `bytes` (RGBA8), started on the render thread once the context's
     * deferred images reach kStartBytes; null: not yet, or the host gave no upload context
     */
    static TextureUploads* of (const Context& context, size_t bytes);

    explicit TextureUploads (wp_gl_upload_context& host);
    ~TextureUploads ();

    /**
     * From any thread: uploads `w` × `h` RGBA8 `pixels` into `level` of `texture` and waits for it. Returns the fence
     * after the upload (the render thread waits on it with glWaitSync, then re-binds the texture), or null: no upload
     * context (make_current failed), and the caller uploads on the render thread
     */
    GLsync upload (GLuint texture, GLint level, int w, int h, const unsigned char* pixels);

private:
    void run ();

    wp_gl_upload_context& m_host;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::packaged_task<GLsync ()>> m_jobs;
    /** make_current failed: every upload is the render thread's */
    bool m_failed = false;
    bool m_stop = false;
    std::thread m_thread;
};
} // namespace WallpaperEngine::Render
