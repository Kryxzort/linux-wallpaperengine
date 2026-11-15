#include "TextureUploads.h"

#include "WallpaperEngine/Context.h"

#include <pthread.h>

using namespace WallpaperEngine::Render;

TextureUploads* TextureUploads::of (const Context& context, size_t bytes) {
    if (context.gl_upload_context == nullptr || context.gl_upload_context->make_current == nullptr) {
	return nullptr;
    }
    context.deferred_upload_bytes += bytes;
    if (context.texture_uploads == nullptr) {
	if (context.deferred_upload_bytes < kStartBytes) {
	    return nullptr;
	}
	context.texture_uploads = std::make_unique<TextureUploads> (*context.gl_upload_context);
    }
    return context.texture_uploads.get ();
}

TextureUploads::TextureUploads (wp_gl_upload_context& host) : m_host (host) {
    this->m_thread = std::thread (&TextureUploads::run, this);
}

TextureUploads::~TextureUploads () {
    {
	std::lock_guard lock (this->m_mutex);
	this->m_stop = true;
    }
    this->m_wake.notify_one ();
    this->m_thread.join ();
}

GLsync TextureUploads::upload (GLuint texture, GLint level, int w, int h, const unsigned char* pixels) {
    std::packaged_task<GLsync ()> job ([texture, level, w, h, pixels] {
	glBindTexture (GL_TEXTURE_2D, texture);
	glTexImage2D (GL_TEXTURE_2D, level, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture (GL_TEXTURE_2D, 0);
	GLsync fence = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	// the render thread's glWaitSync needs the fence submitted
	glFlush ();
	return fence;
    });
    auto done = job.get_future ();
    {
	std::lock_guard lock (this->m_mutex);
	if (this->m_failed || this->m_stop) {
	    return nullptr;
	}
	this->m_jobs.push_back (std::move (job));
    }
    this->m_wake.notify_one ();
    try {
	return done.get ();
    } catch (const std::future_error&) {
	// queued before the thread found it has no context: dropped unrun
	return nullptr;
    }
}

void TextureUploads::run () {
    pthread_setname_np (pthread_self (), "wp-tex-upload");
    const bool current = this->m_host.make_current (this->m_host.user_parameter);
    for (;;) {
	std::packaged_task<GLsync ()> job;
	{
	    std::unique_lock lock (this->m_mutex);
	    if (!current) {
		this->m_failed = true;
	    }
	    this->m_wake.wait (lock, [this] { return this->m_stop || !this->m_jobs.empty (); });
	    if (this->m_jobs.empty ()) {
		break;
	    }
	    job = std::move (this->m_jobs.front ());
	    this->m_jobs.pop_front ();
	}
	if (current) {
	    job ();
	}
	// else: dropped unrun, its upload () answers null
    }
    if (current && this->m_host.release != nullptr) {
	this->m_host.release (this->m_host.user_parameter);
    }
}
