#pragma once

#include <memory>

#include "linux-wallpaperengine/context.h"

#include "Configuration.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

#include "Audio/AudioContext.h"
#include "Render/RenderContext.h"

namespace WallpaperEngine {
class Context {
public:
    std::unique_ptr<TextureCache> texture_cache;
    std::unique_ptr<Audio::AudioContext> audio;
    const Configuration& config;
    wp_gl_proc_address* gl_proc_address;
    wp_time_counter* time_counter;
    wp_audio_input_mix* audio_input_mix;
    std::vector<Data::Model::ProjectSharedPtr> projects;
    bool isRunning;
    float renderTime;
    float renderTimeLast;
    /** `g_Frametime`: this frame's delta, renderTime − renderTimeLast, never negative (WE: the tick's dt, ctx+0x14c) */
    float frameTime;
    /** `g_Time`: renderTime wrapped every 432000 s (5 days) as WE wraps its own (GH 0x1401afbf0, 0x1404d44fc) */
    float shaderTime;
    /**
     * `g_Daytime` and `engine.timeOfDay`: the local time of day in [0, 1), h/24 + m/1440 + s/86400 + ms/86400000
     * (GH 0x14013f370, written every frame by GH 0x140141520); wall-clock time, not the host's scene clock
     */
    float daytime;

    explicit Context (
	const Configuration& config, wp_time_counter* time_counter, wp_gl_proc_address* gl_proc_addresses,
	wp_audio_input_mix* audio_input_mix
    );

    void updateTime ();
};
}