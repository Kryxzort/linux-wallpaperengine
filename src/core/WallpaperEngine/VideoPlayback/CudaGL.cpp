#include "CudaGL.h"

#include <dlfcn.h>
#include <type_traits>

using namespace WallpaperEngine::VideoPlayback;

const CudaGL* CudaGL::get () {
    static const CudaGL* calls = [] () -> const CudaGL* {
	// by soname only: a libcuda already in the process (FFmpeg's, or the host's) is found whatever its path
	void* lib = dlopen ("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
	if (lib == nullptr) {
	    return nullptr;
	}
	static CudaGL c {};
	bool all = true;
	auto bind = [&] (auto& slot, const char* name) {
	    void* f = dlsym (lib, name);
	    all = all && f != nullptr;
	    slot = reinterpret_cast<std::remove_reference_t<decltype (slot)>> (f);
	};
	bind (c.ctxPushCurrent, "cuCtxPushCurrent_v2");
	bind (c.ctxPopCurrent, "cuCtxPopCurrent_v2");
	bind (c.graphicsGLRegisterImage, "cuGraphicsGLRegisterImage");
	bind (c.graphicsUnregisterResource, "cuGraphicsUnregisterResource");
	bind (c.graphicsMapResources, "cuGraphicsMapResources");
	bind (c.graphicsUnmapResources, "cuGraphicsUnmapResources");
	bind (c.graphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray");
	bind (c.memcpy2DAsync, "cuMemcpy2DAsync_v2");
	return all ? &c : nullptr;
    }();
    return calls;
}
