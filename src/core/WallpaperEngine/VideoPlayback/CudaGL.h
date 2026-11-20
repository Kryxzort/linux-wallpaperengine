#pragma once

#include <cstddef>

namespace WallpaperEngine::VideoPlayback {
/**
 * The few libcuda calls that copy a decoded CUDA frame into GL textures (CUDA–GL interop), looked up in the process's
 * libcuda: FFmpeg's CUDA device already loaded it by soname (and a host that wants the interop loads it before, from
 * wherever its driver lives). Declared here rather than from cuda.h, which no build dependency ships.
 */
struct CudaGL {
    using CUresult = int;
    using CUcontext = void*;
    using CUstream = void*;
    using CUgraphicsResource = void*;
    using CUarray = void*;
    using CUdeviceptr = unsigned long long;

    static constexpr unsigned kRegisterWriteDiscard = 2; // CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD
    static constexpr int kMemoryDevice = 2;		 // CU_MEMORYTYPE_DEVICE
    static constexpr int kMemoryArray = 3;		 // CU_MEMORYTYPE_ARRAY

    /** CUDA_MEMCPY2D (cuda.h, the _v2 layout) */
    struct Memcpy2D {
	size_t srcXInBytes;
	size_t srcY;
	int srcMemoryType;
	const void* srcHost;
	CUdeviceptr srcDevice;
	CUarray srcArray;
	size_t srcPitch;
	size_t dstXInBytes;
	size_t dstY;
	int dstMemoryType;
	void* dstHost;
	CUdeviceptr dstDevice;
	CUarray dstArray;
	size_t dstPitch;
	size_t WidthInBytes;
	size_t Height;
    };

    CUresult (*ctxPushCurrent) (CUcontext);
    CUresult (*ctxPopCurrent) (CUcontext*);
    CUresult (*graphicsGLRegisterImage) (CUgraphicsResource*, unsigned image, unsigned target, unsigned flags);
    CUresult (*graphicsUnregisterResource) (CUgraphicsResource);
    CUresult (*graphicsMapResources) (unsigned count, CUgraphicsResource*, CUstream);
    CUresult (*graphicsUnmapResources) (unsigned count, CUgraphicsResource*, CUstream);
    CUresult (*graphicsSubResourceGetMappedArray) (CUarray*, CUgraphicsResource, unsigned index, unsigned level);
    CUresult (*memcpy2DAsync) (const Memcpy2D*, CUstream);

    /** the calls, or null when the process has no libcuda or it lacks one of them */
    static const CudaGL* get ();
};
} // namespace WallpaperEngine::VideoPlayback
