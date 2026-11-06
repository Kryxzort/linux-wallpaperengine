#ifndef __WP_LIB_PROJECT_H__
#define __WP_LIB_PROJECT_H__

#include "context.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Background project instance
 */
typedef void wp_project;

enum wp_mouse_input_button {
    WP_MOUSE_INPUT_BUTTON_LEFT = 1,
    WP_MOUSE_INPUT_BUTTON_RIGHT = 2,
    WP_MOUSE_INPUT_BUTTON_MIDDLE = 4,
};

/**
 * Property type
 */
enum wp_property_type {
    WP_PROPERTY_TYPE_UNKNOWN = 0,
    WP_PROPERTY_TYPE_TEXT = 1,
    WP_PROPERTY_TYPE_SLIDER = 2,
    WP_PROPERTY_TYPE_COLOR = 3,
    WP_PROPERTY_TYPE_BOOLEAN = 4,
    WP_PROPERTY_TYPE_FILE = 5,
    WP_PROPERTY_TYPE_SCENE_TEXTURE = 6,
    WP_PROPERTY_TYPE_COMBO = 7,
    WP_PROPERTY_TYPE_TEXT_INPUT = 8,
};

/**
 * Provides callbacks to get mouse position
 */
struct wp_mouse_input {
    /**
     * Pointer to user-defined data that will be passed to the callbacks
     */
    void* user_parameter;
    /**
     * Callback that returns the current mouse's x position on the given background
     */
    double (*get_x) (void* user_parameter);
    /**
     * Callback that returns the current mouse's y position on the given background
     */
    double (*get_y) (void* user_parameter);
    /**
     * Callback that returns the current mouse's button state
     */
    int (*is_pressed) (void* user_parameter, int button);
};

/**
 * Provides callbacks to describe the project's contents for debugging purposes
 */
struct wp_describe_callback {
    /**
     * Pointer to user-defined data that will be passed to the callbacks
     */
    void* user_parameter;

    /**
     * Called by the describe function with data to write to the output buffer.
     */
    void (*write) (void* user_parameter, const char* buffer, unsigned long size);
};

/**
 * Project's property
 */
struct wp_project_property {
    wp_property_type type;
    const char* name;
    const char* text;
};

struct wp_project_property_slider {
    wp_project_property base;
    float min;
    float max;
    float step;
    float value;
};

struct wp_project_property_text {
    wp_project_property base;
    const char* value;
};

/** A `textinput` property: the text the user typed, or its default */
struct wp_project_property_text_input {
    wp_project_property base;
    const char* value;
};

struct wp_project_property_file {
    wp_project_property base;
    const char* path;
};

struct wp_project_property_scene_texture {
    wp_project_property base;
    const char* value;
};

struct wp_project_property_combo_value {
    const char* key;
    const char* value;
};

struct wp_project_property_combo {
    wp_project_property base;
    int value_count;
    wp_project_property_combo_value* values;
};

struct wp_project_property_color {
    wp_project_property base;
    float r;
    float g;
    float b;
    float a;
};

struct wp_project_property_boolean {
    wp_project_property base;
    bool value;
};

#define WP_TYPE_LOWER_BOOLEAN boolean
#define WP_TYPE_LOWER_COLOR color
#define WP_TYPE_LOWER_SLIDER slider
#define WP_TYPE_LOWER_TEXT text
#define WP_TYPE_LOWER_FILE file
#define WP_TYPE_LOWER_SCENE_TEXTURE scene_texture
#define WP_TYPE_LOWER_COMBO combo
#define WP_TYPE_LOWER_TEXT_INPUT text_input

#define WP_TYPE_EXPAND(x) x
#define WP_TYPE_CONCAT(a, b) a##b
#define WP_TYPE_CONCAT_EXPAND(a, b) WP_TYPE_CONCAT (a, b)

#define WP_PROPERTY_TYPE_NAME(type)                                                                                    \
    WP_TYPE_CONCAT_EXPAND (wp_project_property_, WP_TYPE_EXPAND (WP_TYPE_CONCAT_EXPAND (WP_TYPE_LOWER_, type)))

/** Convenient macro to check for a property type */
#define WP_PROPERTY_IS(property, type)                                                                                 \
    (property != NULL && ((wp_project_property*)property)->ty##pe == WP_PROPERTY_TYPE_##type)
/** Convenient macro to cast a property to the given type safely */
#define WP_PROPERTY_AS(property, type)                                                                                 \
    (WP_PROPERTY_IS (property, type) ? (WP_PROPERTY_TYPE_NAME (type)*)property : NULL)

#define WP_PROPERTY_IS_COLOR(property) WP_PROPERTY_IS (property, COLOR)
#define WP_PROPERTY_AS_COLOR(property) WP_PROPERTY_AS (property, COLOR)
#define WP_PROPERTY_IS_BOOLEAN(property) WP_PROPERTY_IS (property, BOOLEAN)
#define WP_PROPERTY_AS_BOOLEAN(property) WP_PROPERTY_AS (property, BOOLEAN)
#define WP_PROPERTY_IS_SLIDER(property) WP_PROPERTY_IS (property, SLIDER)
#define WP_PROPERTY_AS_SLIDER(property) WP_PROPERTY_AS (property, SLIDER)
#define WP_PROPERTY_IS_TEXT(property) WP_PROPERTY_IS (property, TEXT)
#define WP_PROPERTY_AS_TEXT(property) WP_PROPERTY_AS (property, TEXT)
#define WP_PROPERTY_IS_FILE(property) WP_PROPERTY_IS (property, FILE)
#define WP_PROPERTY_AS_FILE(property) WP_PROPERTY_AS (property, FILE)
#define WP_PROPERTY_IS_SCENE_TEXTURE(property) WP_PROPERTY_IS (property, SCENE_TEXTURE)
#define WP_PROPERTY_AS_SCENE_TEXTURE(property) WP_PROPERTY_AS (property, SCENE_TEXTURE)
#define WP_PROPERTY_IS_COMBO(property) WP_PROPERTY_IS (property, COMBO)
#define WP_PROPERTY_AS_COMBO(property) WP_PROPERTY_AS (property, COMBO)
#define WP_PROPERTY_IS_TEXT_INPUT(property) WP_PROPERTY_IS (property, TEXT_INPUT)
#define WP_PROPERTY_AS_TEXT_INPUT(property) WP_PROPERTY_AS (property, TEXT_INPUT)

/**
 * Loads the given background ID from the backgrounds folder
 *
 * @param context
 * @param mouse_input The mouse input handler if available
 * @param id The background id to load
 *
 * @return The project if was loaded successfully, NULL otherwise
 */
WPENGINE_API wp_project* wp_project_load_id (wp_context* context, wp_mouse_input* mouse_input, unsigned long int id);

/**
 * Loads the given background ID from the backgrounds folder
 *
 * @param context
 * @param mouse_input The mouse input handler if available
 * @param id The background id to load
 *
 * @return The project if it was loaded successfully, NULL otherwise
 */
WPENGINE_API wp_project* wp_project_load_id_str (wp_context* context, wp_mouse_input* mouse_input, const char* id);

/**
 * Loads the project.json from the given folder
 *
 * @param context
 * @param mouse_input The mouse input handler if available
 * @param folder Full, absolute path to the background's project.json
 *
 * @return The project if was loaded successfully, NULL otherwise
 */
WPENGINE_API wp_project* wp_project_load_folder (wp_context* context, wp_mouse_input* mouse_input, const char* folder);

/**
 * WARNING: pointers returned by this function live as long as you do not reset, go to the next element or modify it.
 * if you want to keep this info for a UI or something, make copies of the data
 *
 * @param project The project to read properties off
 *
 * @return The current property entry with the available information
 */
WPENGINE_API wp_project_property* wp_project_property_list_next (wp_project* project);

/**
 * @param project Regardless of the current progress, restarts the property listing
 */
WPENGINE_API void wp_project_property_list_reset (wp_project* project);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_value over this one
 *
 * @param project
 * @param name
 * @param value
 */
WPENGINE_API bool wp_project_property_set_value_by_name (wp_project* project, const char* name, const char* value);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_text over this one
 *
 * @param project
 * @param name
 * @param value
 */
WPENGINE_API bool wp_project_property_set_text_by_name (wp_project* project, const char* name, const char* value);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_boolean over this one
 *
 * @param project
 * @param name
 * @param value
 */
WPENGINE_API bool wp_project_property_set_boolean_by_name (wp_project* project, const char* name, bool value);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_slider over this one
 *
 * @param project
 * @param name
 * @param value
 */
WPENGINE_API bool wp_project_property_set_slider_by_name (wp_project* project, const char* name, float value);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_color over this one
 *
 * @param project
 * @param name
 * @param r
 * @param g
 * @param b
 * @param a
 */
WPENGINE_API bool
wp_project_property_set_color_by_name (wp_project* project, const char* name, float r, float g, float b, float a);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_value over this one
 *
 * @param project
 * @param name
 * @param path
 */
WPENGINE_API bool wp_project_property_set_file_by_name (wp_project* project, const char* name, const char* path);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_value over this one
 *
 * @param project
 * @param name
 * @param path
 */
WPENGINE_API bool
wp_project_property_set_scene_texture_by_name (wp_project* project, const char* name, const char* path);

/**
 * WARNING: if the property being updated is the same as the last one returned from @see wp_project_property_list_next
 * the pointer will be freed and invalid.
 *
 * Prefer @see wp_project_property_set_value over this one
 *
 * @param project
 * @param name
 * @param key
 */
WPENGINE_API bool wp_project_property_set_combo_by_name (wp_project* project, const char* name, const char* key);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param value The value to set for the property
 */
WPENGINE_API bool wp_project_property_set_value (wp_project* project, wp_project_property* property, const char* value);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param value The value to set for the property
 */
WPENGINE_API bool
wp_project_property_set_text (wp_project* project, wp_project_property_text* property, const char* value);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param value The value to set for the property
 */
WPENGINE_API bool
wp_project_property_set_boolean (wp_project* project, wp_project_property_boolean* property, bool value);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param value The value to set for the property
 */
WPENGINE_API bool
wp_project_property_set_slider (wp_project* project, wp_project_property_slider* property, float value);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param r
 * @param g
 * @param b
 * @param a
 */
WPENGINE_API bool wp_project_property_set_color (
    wp_project* project, wp_project_property_color* property, float r, float g, float b, float a
);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param path The value to set for the property
 */
WPENGINE_API bool
wp_project_property_set_file (wp_project* project, wp_project_property_file* property, const char* path);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param path The value to set for the property
 */
WPENGINE_API bool wp_project_property_set_scene_texture (
    wp_project* project, wp_project_property_scene_texture* property, const char* path
);

/**
 * @param project The project to set the property for
 * @param property The property to update
 * @param key The value to set for the property
 */
WPENGINE_API bool
wp_project_property_set_combo (wp_project* project, wp_project_property_combo* property, const char* key);

/**
 * Frees any allocated memory by the project, stops the renderer (if setup)
 * and cleans up any used resources
 *
 * @param project The project to free
 */
WPENGINE_API void wp_project_destroy (wp_project* project);

/**
 * @param project The project to get render width for
 * @return The width of the project's render viewport. This can change at any point
 */
WPENGINE_API int wp_project_get_width (wp_project* project);

/**
 * @param project The project to get render height for
 * @return The height of the project's render viewport. This can change at any point
 */
WPENGINE_API int wp_project_get_height (wp_project* project);

/**
 * Gives the project a hint for the size it will render at
 *
 * @param project The project to hint the size for
 * @param width The width to hint for the project
 * @param height The height to hint for the project
 */
WPENGINE_API void wp_project_hint_size (wp_project* project, int width, int height);

/**
 * Updates the output framebuffer for the project
 *
 * @param project The project to update the output framebuffer for
 * @param framebuffer The framebuffer to use for rendering
 */
WPENGINE_API void wp_project_set_output_framebuffer (wp_project* project, unsigned int framebuffer);

/**
 * Updates the mouse input used for the project
 *
 * @param project The project to update mouse input handler for
 * @param mouse_input The mouse input handler to use for the project
 */
WPENGINE_API void wp_project_set_mouse_input (wp_project* project, wp_mouse_input* mouse_input);

/**
 * The frame rate the host means to draw the project at (Wallpaper Engine's fps setting; the rate before any
 * throttling). A simulation effect — passes that carry state in the effect's own FBOs from frame to frame, as
 * cursorripple's — steps at it: a frame that comes late runs several steps, one that comes early none, so the
 * simulation behaves as in WE at that fps whatever rate the frames actually arrive at. 0 (the default) steps once
 * per frame drawn. Can be called before or after the first frame, and again when the rate changes
 *
 * @param project The project
 * @param hz The rate in frames per second, 0 for none
 */
WPENGINE_API void wp_project_set_frame_rate (wp_project* project, float hz);

/**
 * The pointer's next position (what the mouse input's callbacks return on the next frame) is a jump, not a move: the
 * pointer entered the output, or came back from behind a window. Wallpaper Engine polls the cursor on the whole
 * desktop, so it never jumps there; a host that only sees the pointer over its own surface says so here, and the
 * frame that reads the new position takes it as g_PointerPositionLast too (cursorripple strokes no line along the
 * jump). Before the first frame: nothing to do
 *
 * @param project The project
 */
WPENGINE_API void wp_project_pointer_jump (wp_project* project);

/**
 * Provides a textual representation of the project and it's elements, useful for debugging
 *
 * @param project
 * @param callback
 */
WPENGINE_API void wp_project_describe (wp_project* project, wp_describe_callback* callback);

/**
 * Notifies the project about a new track being played in the system
 *
 * @param title
 * @param artist
 * @param album
 */
WPENGINE_API void
wp_project_notify_track_metadata_change (wp_project* project, const char* title, const char* artist, const char* album);

/**
 * Notifies the project about a new album art url being available
 *
 * @param project
 * @param url
 */
WPENGINE_API void wp_project_notify_album_art_url_change (wp_project* project, const char* url);

/**
 * Notifies the project about playback state change
 *
 * @param project
 * @param state
 */
WPENGINE_API void wp_project_notify_playback_state_change (wp_project* project, wp_media_playback_state state);

/**
 * Notifies the project about changes in the playback position
 *
 * @param project
 * @param position
 */
WPENGINE_API void
wp_project_notify_playback_position_and_duration_change (wp_project* project, double position, double duration);

/**
 * Notifies the project about the current track's metadata beyond title/artist/album (SceneScript's
 * MediaPropertiesEvent): the album artist, a sub-title, the genres (comma-separated) and the content type
 * ("music", "video" or "image"). NULL or empty leaves a field empty. Sends a mediaPropertiesChanged with every field
 *
 * @param project
 */
WPENGINE_API void wp_project_notify_track_metadata_extra (
    wp_project* project, const char* album_artist, const char* sub_title, const char* genres, const char* content_type
);

/**
 * Notifies the project whether media integration is on (SceneScript's mediaStatusChanged, MediaStatusEvent.enabled):
 * a host that feeds the media events above says so once, and again when that changes
 *
 * @param project
 * @param enabled
 */
WPENGINE_API void wp_project_notify_media_status (wp_project* project, bool enabled);

/**
 * @param project
 * @return Whether the project's scripts or materials use the media events or the media thumbnail (a host may skip
 * reading the system's media sessions for one that doesn't)
 */
WPENGINE_API bool wp_project_uses_media (wp_project* project);

/**
 * The shader sweep (a diagnostic; draws nothing): builds shader `shader` (relative to the project's shaders/, no
 * extension — "genericimage2", "workshop/<id>/effects/<name>") through the same preprocessing, glslang pass and GL
 * compile and link a material pass uses, on the GL context current on the calling thread.
 *
 * @param project
 * @param shader
 * @param request JSON or NULL: {"combos": {NAME: int…}, "textures": [slot…], "gl": bool (default true)}
 * @param out Receives JSON {"shader", "ok", "stage" (load | glslang | gl | link), "message", "unit", "compat": [the
 * HLSL-compatible rewrites applied], "combos": {the effective combos}}, NUL-terminated and cut to `out_size`
 * @param out_size
 * @return The length of the whole JSON (more than `out_size - 1` when it was cut), 0 on an error before it
 */
WPENGINE_API size_t wp_project_shader_check (
    wp_project* project, const char* shader, const char* request, char* out, size_t out_size
);

#ifdef __cplusplus
}
#endif

#endif