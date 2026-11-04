#pragma once

#include "Render/CWallpaper.h"

#include <filesystem>
#include <memory>
#include <vector>

#include "Render/RenderContext.h"

#include "linux-wallpaperengine/project.h"

namespace WallpaperEngine {
class Context;
class Project {
public:
    struct MediaInfo {
	std::optional<std::string> title;
	std::optional<std::string> artist;
	std::optional<std::string> album;
	std::optional<std::string> url;
	std::optional<wp_media_playback_state> state;
	std::optional<double> duration;
	std::optional<double> position;
	/** MediaPropertiesEvent's other fields (wp_project_notify_track_metadata_extra) */
	std::string albumArtist;
	std::string subTitle;
	std::string genres;
	std::string contentType = "music";
	/** mediaStatusChanged's `enabled`, once the host said */
	std::optional<bool> enabled;
    };

    ~Project ();

    int getWidth () const;
    int getHeight () const;
    void setOutputFramebuffer (GLuint newFramebuffer);
    void setMouseInput (wp_mouse_input* newMouseInput);
    void hintSize (int width, int height);
    void render ();

    wp_project_property* propertyListNext ();
    void propertyListReset ();

    void propertySet (const wp_project_property* property, float value);
    void propertySet (const wp_project_property* property, const std::string& value);
    void propertySet (const wp_project_property* property, glm::vec4 value);
    void propertySet (const wp_project_property* property, bool value);
    void propertySet (const std::string& key, const std::string& value);
    void propertySet (const std::string& key, float value);
    void propertySet (const std::string& key, glm::vec4 value);
    void propertySet (const std::string& key, bool value);

    void describe (wp_describe_callback* callback);

    void trackMetadataChange (const char* title, const char* artist, const char* album);
    void albumArtUrlChange (const char* url);
    void playbackStateChange (wp_media_playback_state state);
    void playbackPositionAndDurationChange (double position, double duration);

    const MediaInfo& getMediaInfo () const;
    void trackMetadataExtraChange (const char* albumArtist, const char* subTitle, const char* genres, const char* contentType);
    void mediaStatusChange (bool enabled);
    [[nodiscard]] bool usesMedia () const;

    /**
     * The shader sweep: builds shader `name` (relative to shaders/, no extension) from this project's files the way a
     * material pass does — the same preprocessing, glslang and SPIRV-Cross pass and the GL compile and link, on the
     * GL context current on this thread — and draws nothing. `request` is JSON: `{"combos": {NAME: value…},
     * "textures": [slot…], "gl": bool, "source": bool}` (gl defaults to true; source adds the units as handed to glslang). Returns JSON `{"ok", "stage" (load | glslang | gl |
     * link), "message", "compat": [what…], "combos": {the effective combos}}`
     */
    std::string shaderCheck (const std::string& name, const std::string& request);

    static Project* loadId (Context* context, wp_mouse_input* mouse_input, const unsigned long int id);
    static Project* loadId (Context* context, wp_mouse_input* mouse_input, const std::string& id);
    static Project* loadFolder (Context* context, wp_mouse_input* mouse_input, const char* folder);

protected:
    ProjectSharedPtr ref;
    std::unique_ptr<CWallpaper> wallpaper;
    std::unique_ptr<RenderContext> renderContext;
    wp_mouse_input* mouse_input;
    Context& context;
    Properties::const_iterator property_it;
    wp_project_property* current_property;
    GLuint framebuffer;
    int hintedWidth;
    int hintedHeight;
    MediaInfo mediaInfo;

    Project (Context* context, wp_mouse_input* mouse_input, const std::filesystem::path& project);
};
};