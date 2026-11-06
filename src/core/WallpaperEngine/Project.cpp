#include "Project.h"
#include "Context.h"
#include "Health.h"
#include "Data/Dumpers/StringPrinter.h"
#include "Data/Model/Property.h"
#include "Data/Parsers/ProjectParser.h"
#include "FileSystem/Adapters/MediaCover.h"
#include "Render/Shaders/GLSLContext.h"
#include "Render/Shaders/Shader.h"
#include "Render/Wallpapers/CScene.h"
#include "Render/Wallpapers/CWeb.h"
#include "Scripting/ScriptEngine.h"

#include <cmath>

using namespace WallpaperEngine;

Context* active_context = nullptr;

namespace {
WallpaperEngine::Scripting::ScriptEngine::MediaProperties scriptMediaProperties (const WallpaperEngine::Project::MediaInfo& media) {
    return {
	.title = media.title.value_or (""),
	.artist = media.artist.value_or (""),
	.albumTitle = media.album.value_or (""),
	.albumArtist = media.albumArtist,
	.subTitle = media.subTitle,
	.genres = media.genres,
	.contentType = media.contentType,
    };
}
} // namespace

void* wp_context_call_gl_proc_address (const char* name) {
    if (active_context == nullptr) {
	return nullptr;
    }

    if (active_context->gl_proc_address == nullptr) {
	return nullptr;
    }

    if (active_context->gl_proc_address->get_proc_address == nullptr) {
	return nullptr;
    }

    return active_context->gl_proc_address->get_proc_address (active_context->gl_proc_address->user_parameter, name);
}

/**
 * Builds an asset locator and sets up all the paths and virtual files that are needed for proper playback
 * TODO: MOVE THIS SOMEWHERE ELSE?
 *
 * @param project
 * @param path
 *
 * @return
 */
AssetLocatorUniquePtr wp_setup_asset_locator (

    const WallpaperEngine::Project& project, const Configuration& config, const std::filesystem::path& path
) {
    auto container = std::make_unique<Container> ();

    container->registerAdapterFactory (std::make_unique<MediaCoverFactory> (project));
    container->mount ("$mediaThumbnail", "$mediaThumbnail");
    container->mount (path, "/");

    // exceptions from these mounts are expected and not a real problem, so ignore them for now
    try {
	container->mount (path / "scene.pkg", "/");
    } catch (std::runtime_error&) { }

    try {
	container->mount (path / "gifscene.pkg", "/");
    } catch (std::runtime_error&) { }

    try {
	container->mount (config.assets_dir, "/");
    } catch (std::runtime_error&) { }

    auto& vfs = container->getVFS ();

    //
    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code
    // these virtual files are loaded by an image in the scene that takes current _rt_FullFrameBuffer and
    // applies the bloom effect to render it out to the screen
    //

    // add the effect file for screen bloom

    // add some model for the image element even if it's going to waste rendering cycles
    vfs.add (
	"effects/wpenginelinux/bloomeffect.json",
	{ { "name", "camerabloom_wpengine_linux" },
	  { "group", "wpengine_linux_camera" },
	  { "dependencies", JSON::array () },
	  {
	      "passes",
	      JSON::array (
		  { { { "material", "materials/util/downsample_quarter_bloom.json" },
		      { "target", "_rt_4FrameBuffer" },
		      { "bind", JSON::array ({ { { "name", "_rt_FullFrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/downsample_eighth_blur_v.json" },
		      { "target", "_rt_8FrameBuffer" },
		      { "bind", JSON::array ({ { { "name", "_rt_4FrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/blur_h_bloom.json" },
		      { "target", "_rt_Bloom" },
		      { "bind", JSON::array ({ { { "name", "_rt_8FrameBuffer" }, { "index", 0 } } }) } },
		    { { "material", "materials/util/combine.json" },
		      { "target", "_rt_FullFrameBuffer" },
		      { "bind",
			JSON::array (
			    { { { "name", "_rt_imageLayerComposite_-1_a" }, { "index", 0 } },
			      { { "name", "_rt_Bloom" }, { "index", 1 } } }
			) } } }
	      ),
	  } }
    );

    // the HDR chain, one effect per level count n (WE's GH 0x1401b3b10 on targets made by GH 0x1401af5b0): the scene
    // into _rt_2FrameBuffer through hdr_downsample_bloom (the threshold), each level into the next half-size one
    // through hdr_downsample, then back up adding each level onto the one above (hdr_upsample, bicubic for the two
    // coarsest steps), and combine_hdr_upsample's linear variant adds _rt_2FrameBuffer onto the scene. Targets are
    // "_rt_" + (2 << level) + "FrameBuffer". CScene feeds g_RenderVar0 (the tap offsets) and the bloom parameters
    for (int levels = 1; levels <= 8; levels++) {
	const auto target = [] (int level) { return "_rt_" + std::to_string (2 << level) + "FrameBuffer"; };
	const auto pass = [] (const std::string& material, const std::string& to, const std::string& from) {
	    return JSON { { "material", material },
			  { "target", to },
			  { "bind", JSON::array ({ { { "name", from }, { "index", 0 } } }) } };
	};
	JSON passes = JSON::array ();

	passes.push_back (pass ("materials/util/hdr_downsample_bloom.json", target (0), "_rt_FullFrameBuffer"));
	for (int level = 1; level < levels; level++) {
	    passes.push_back (pass ("materials/util/hdr_downsample.json", target (level), target (level - 1)));
	}
	for (int level = levels - 1; level > 0; level--) {
	    passes.push_back (pass (
		level < levels - 2 ? "materials/util/hdr_upsample.json" : "materials/util/hdr_upsample_cubic.json",
		target (level - 1), target (level)
	    ));
	}
	passes.push_back (
	    { { "material", "materials/util/combine_hdr_upsample_linear.json" },
	      { "target", "_rt_FullFrameBuffer" },
	      { "bind",
		JSON::array (
		    { { { "name", "_rt_imageLayerComposite_-1_a" }, { "index", 0 } },
		      { { "name", target (0) }, { "index", 1 } } }
		) } }
	);

	vfs.add (
	    "effects/wpenginelinux/bloomhdr" + std::to_string (levels) + ".json",
	    { { "name", "camerabloomhdr_wpengine_linux" },
	      { "group", "wpengine_linux_camera" },
	      { "dependencies", JSON::array () },
	      { "passes", passes } }
	);
    }

    vfs.add ("models/wpenginelinux.json", { { "material", "materials/wpenginelinux.json" } });

    vfs.add (
	"materials/wpenginelinux.json",
	{ { "passes",
	    JSON::array (
		{ { { "blending", "normal" },
		    { "cullmode", "nocull" },
		    { "depthtest", "disabled" },
		    { "depthwrite", "disabled" },
		    { "shader", "genericimage2" },
		    { "textures", JSON::array ({ "_rt_FullFrameBuffer" }) } } }
	    ) } }
    );

    vfs.add (
	"shaders/commands/copy.frag",
	"uniform sampler2D g_Texture0;\n"
	"in vec2 v_TexCoord;\n"
	"void main () {\n"
	"out_FragColor = texture (g_Texture0, v_TexCoord);\n"
	"}"
    );
    vfs.add (
	"shaders/commands/copy.vert",
	"in vec3 a_Position;\n"
	"in vec2 a_TexCoord;\n"
	"out vec2 v_TexCoord;\n"
	"void main () {\n"
	"gl_Position = vec4 (a_Position, 1.0);\n"
	"v_TexCoord = a_TexCoord;\n"
	"}"
    );

    return std::make_unique<AssetLocator> (std::move (container));
}

WallpaperEngine::Project::Project (
    Context* context, wp_mouse_input* mouse_input, const std::filesystem::path& project
) : context (*context), framebuffer (GL_NONE) {
    // WPENGINE_HEALTH is read per project: a host (or a test) can turn it on for one load
    WallpaperEngine::Health::configure ();
    if (!std::filesystem::exists (project) || !std::filesystem::is_directory (project)) {
	sLog.exception ("Cannot find project file");
    }

    auto locator = wp_setup_asset_locator (*this, context->config, project);
    auto json = JSON::parse (locator->readString ("project.json"));
    this->ref = WallpaperEngine::Data::Parsers::ProjectParser::parse (json, std::move (locator));
    context->projects.push_back (this->ref);

    this->current_property = nullptr;
    this->property_it = this->ref->properties.begin ();
    this->mouse_input = mouse_input;
}

WallpaperEngine::Project::~Project () {
    if (this->context.projects.empty ()) {
	active_context = nullptr;
    }

    if (const auto combo = WP_PROPERTY_AS_COMBO (this->current_property)) {
	delete combo->values;
    }

    this->current_property = nullptr;
}

int WallpaperEngine::Project::getHeight () const {
    if (this->wallpaper == nullptr) {
	return 1;
    }

    return this->wallpaper->getHeight ();
}

int WallpaperEngine::Project::getWidth () const {
    if (this->wallpaper == nullptr) {
	return 1;
    }

    return this->wallpaper->getWidth ();
}

void WallpaperEngine::Project::setOutputFramebuffer (const GLuint newFramebuffer) {
    this->framebuffer = newFramebuffer;

    if (this->wallpaper != nullptr) {
	this->wallpaper->setDestinationFramebuffer (newFramebuffer);
    }
}

void WallpaperEngine::Project::setMouseInput (wp_mouse_input* newMouseInput) {
    this->mouse_input = newMouseInput;

    if (this->wallpaper != nullptr) {
	this->wallpaper->setMouseInputHandler (newMouseInput);
    }
}

void WallpaperEngine::Project::setFrameRate (float hz) {
    this->frameRate = std::isfinite (hz) && hz > 0.0f ? hz : 0.0f;

    if (this->wallpaper != nullptr) {
	this->wallpaper->setFrameRate (this->frameRate);
    }
}

void WallpaperEngine::Project::hintSize (int width, int height) {
    this->hintedWidth = width;
    this->hintedHeight = height;

    if (this->wallpaper != nullptr && this->wallpaper->is<Wallpapers::CWeb> ()) {
	this->wallpaper->as<Wallpapers::CWeb> ()->setSize (width, height);
    } else if (this->wallpaper != nullptr && this->wallpaper->is<Wallpapers::CScene> ()) {
	// the screen a scene shows on: engine.screenResolution, and resizeScreen when it changes
	this->wallpaper->as<Wallpapers::CScene> ()->setScreenSize (width, height);
    }
}

void WallpaperEngine::Project::render () {
    if (active_context != &this->context) {
	active_context = &this->context;

	gladLoadGLLoader (wp_context_call_gl_proc_address);
    }

    if (this->renderContext == nullptr) {
	// initialize render if not available
	this->renderContext = std::make_unique<RenderContext> (this->context, *this->ref->assetLocator);
	try {
	    this->wallpaper = CWallpaper::fromWallpaper (
		*this->ref->wallpaper, *this->renderContext, *this->context.audio, this->mouse_input
	    );
	} catch (const std::exception& e) {
	    WPENGINE_HEALTH ("renderer_failed", { { "message", WallpaperEngine::Health::brief (e.what ()) } });
	    throw;
	}
	this->wallpaper->setDestinationFramebuffer (this->framebuffer);
	this->wallpaper->setFrameRate (this->frameRate);
	// give hint about size to the wallpaper only if web
	if (this->wallpaper->is<Wallpapers::CWeb> ()) {
	    this->wallpaper->as<Wallpapers::CWeb> ()->setSize (this->hintedWidth, this->hintedHeight);
	} else if (this->wallpaper->is<Wallpapers::CScene> () && this->hintedWidth > 0 && this->hintedHeight > 0) {
	    // a size hinted before the scene existed is its screen from the start (no resizeScreen: WE sends none on
	    // creation)
	    this->wallpaper->as<Wallpapers::CScene> ()->setScreenSize (this->hintedWidth, this->hintedHeight);
	    this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().dropQueuedEvent ("resizeScreen");
	}
	// notify current media metadata status
	this->renderContext->albumArtUrlChange (this->mediaInfo.url);

	// what the host reported before the scene existed reaches its scripts now (queued for their first tick)
	if (this->wallpaper != nullptr && this->wallpaper->is<Wallpapers::CScene> ()) {
	    auto& scripts = this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ();
	    const auto& media = this->mediaInfo;

	    if (media.enabled.has_value ()) {
		scripts.notifyMediaStatusChange (*media.enabled);
	    }
	    if (media.title.has_value () || media.artist.has_value () || media.album.has_value ()) {
		scripts.notifyTrackMetadataChange (scriptMediaProperties (media));
	    }
	    if (media.url.has_value ()) {
		scripts.notifyAlbumArtUrlChange (media.url);
	    }
	    if (media.state.has_value ()) {
		scripts.notifyPlaybackStateChange (media.state);
	    }
	    if (media.position.has_value () || media.duration.has_value ()) {
		scripts.notifyPlaybackPositionAndDurationChange (media.position, media.duration);
	    }
	}
    }

    // the renderer failed to build on an earlier call (CWallpaper::fromWallpaper threw after the
    // render context was created): there is nothing to render, ever — and wallpaper is null
    if (this->wallpaper == nullptr) {
	return;
    }

    this->wallpaper->render ();
}

wp_project_property* WallpaperEngine::Project::propertyListNext () {
    if (this->property_it == this->ref->properties.end ()) {
	this->property_it = this->ref->properties.begin ();
	return nullptr;
    }

    // get current property's info and copy it over
    const auto& property = this->property_it++;

    if (const auto boolean = property->second->asOrNull<PropertyBoolean> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_boolean {
	    .base = { .type = WP_PROPERTY_TYPE_BOOLEAN, .name = boolean->name.c_str () },
	    .value = boolean->getBool () });
    } else if (const auto color = property->second->asOrNull<PropertyColor> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_color {
	    .base = { .type = WP_PROPERTY_TYPE_COLOR, .name = color->name.c_str () },
	    .r = color->getVec4 ().r,
	    .g = color->getVec4 ().g,
	    .b = color->getVec4 ().b,
	    .a = color->getVec4 ().a,
	});
    } else if (const auto text = property->second->asOrNull<PropertyText> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_text {
	    .base = { .type = WP_PROPERTY_TYPE_TEXT, .name = text->name.c_str () },
	    .value = text->getString ().c_str () });
    } else if (const auto slider = property->second->asOrNull<PropertySlider> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_slider {
	    .base = { .type = WP_PROPERTY_TYPE_SLIDER, .name = slider->name.c_str () },
	    .min = slider->min,
	    .max = slider->max,
	    .step = slider->step,
	    .value = slider->getFloat () });
    } else if (const auto combo = property->second->asOrNull<PropertyCombo> ()) {
	auto result = new wp_project_property_combo {
	    .base = { .type = WP_PROPERTY_TYPE_COMBO, .name = combo->name.c_str () },
	    .value_count = combo->values.size (),
	    .values = new wp_project_property_combo_value[combo->values.size ()],
	};

	int current = 0;

	for (const auto& value : combo->values) {
	    result->values[current++]
		= wp_project_property_combo_value { .key = value.first.c_str (), .value = value.second.c_str () };
	}

	this->current_property = reinterpret_cast<wp_project_property*> (result);
    } else if (const auto file = property->second->asOrNull<PropertyFile> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_file {
	    .base = { .type = WP_PROPERTY_TYPE_FILE, .name = file->name.c_str () },
	    .path = file->getString ().c_str () });
    } else if (const auto sceneTexture = property->second->asOrNull<PropertySceneTexture> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_scene_texture {
	    .base = { .type = WP_PROPERTY_TYPE_SCENE_TEXTURE, .name = sceneTexture->name.c_str () },
	    .value = sceneTexture->getString ().c_str () });
    } else if (const auto textInput = property->second->asOrNull<PropertyTextInput> ()) {
	this->current_property = reinterpret_cast<wp_project_property*> (new wp_project_property_text_input {
	    .base = { .type = WP_PROPERTY_TYPE_TEXT_INPUT, .name = textInput->name.c_str () },
	    .value = textInput->getString ().c_str () });
    } else {
	sLog.exception ("Unknown property type");
    }

    return this->current_property;
}

void WallpaperEngine::Project::propertyListReset () {
    this->property_it = this->ref->properties.begin ();

    if (const auto combo = WP_PROPERTY_AS_COMBO (this->current_property)) {
	delete combo->values;
    }

    delete this->current_property;
    this->current_property = nullptr;
}

void WallpaperEngine::Project::propertySet (const std::string& key, bool value) {
    const auto it = this->ref->properties.find (key);

    if (it == this->ref->properties.end ()) {
	sLog.exception ("Property not found");
    }

    it->second->update (value, DynamicValue::UpdateSource::User);
}

void WallpaperEngine::Project::propertySet (const std::string& key, const std::string& value) {
    const auto it = this->ref->properties.find (key);

    if (it == this->ref->properties.end ()) {
	sLog.exception ("Property not found");
    }

    it->second->update (value, DynamicValue::UpdateSource::User);
}

void WallpaperEngine::Project::propertySet (const std::string& key, float value) {
    const auto it = this->ref->properties.find (key);

    if (it == this->ref->properties.end ()) {
	sLog.exception ("Property not found");
    }

    it->second->update (value, DynamicValue::UpdateSource::User);
}

void WallpaperEngine::Project::propertySet (const std::string& key, glm::vec4 value) {
    const auto it = this->ref->properties.find (key);

    if (it == this->ref->properties.end ()) {
	sLog.exception ("Property not found");
    }

    it->second->update (value, DynamicValue::UpdateSource::User);
}

void WallpaperEngine::Project::propertySet (const wp_project_property* property, float value) {
    if (property != this->current_property) {
	return;
    }

    this->propertySet (property->name, value);
}

void WallpaperEngine::Project::propertySet (const wp_project_property* property, const std::string& value) {
    if (property != this->current_property) {
	sLog.exception ("Property not found");
    }

    this->propertySet (property->name, value);
}

void WallpaperEngine::Project::propertySet (const wp_project_property* property, glm::vec4 value) {
    if (property != this->current_property) {
	sLog.exception ("Property not found");
    }

    this->propertySet (property->name, value);
}

void WallpaperEngine::Project::propertySet (const wp_project_property* property, bool value) {
    if (property != this->current_property) {
	sLog.exception ("Property not found");
    }

    this->propertySet (property->name, value);
}

void WallpaperEngine::Project::describe (wp_describe_callback* callback) {
    // TODO: CHANGE PRETTYPRINTER TO TAKE IN AN OSTREAM THAT WILL CALL THE CALLBACK INSTEAD OF PARSING EVERYTHING
    // UPFRONT
    auto prettyPrinter = Data::Dumpers::StringPrinter ();
    prettyPrinter.printWallpaper (*this->ref->wallpaper);

    const auto result = prettyPrinter.str ();
    constexpr unsigned long chunkSize = 1024;

    for (unsigned long offset = 0; offset < result.size (); offset += chunkSize) {
	callback->write (
	    callback->user_parameter, &result.c_str ()[offset], std::min (chunkSize, result.size () - offset)
	);
    }
}

void WallpaperEngine::Project::trackMetadataChange (const char* title, const char* artist, const char* album) {
    std::string titleStr = title ?: "";
    std::string artistStr = artist ?: "";
    std::string albumStr = album ?: "";

    if (titleStr.empty ()) {
	this->mediaInfo.title = std::nullopt;
    } else {
	this->mediaInfo.title = titleStr;
    }

    if (artistStr.empty ()) {
	this->mediaInfo.artist = std::nullopt;
    } else {
	this->mediaInfo.artist = artistStr;
    }

    if (albumStr.empty ()) {
	this->mediaInfo.album = std::nullopt;
    } else {
	this->mediaInfo.album = albumStr;
    }

    // notify the scene if needed
    if (this->wallpaper == nullptr) {
	return;
    }

    if (!this->wallpaper->is<Wallpapers::CScene> ()) {
	return;
    }

    this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().notifyTrackMetadataChange (scriptMediaProperties (this->mediaInfo));
}

void WallpaperEngine::Project::albumArtUrlChange (const char* url) {
    std::string value = url ?: "";

    if (value.empty ()) {
	this->mediaInfo.url = std::nullopt;
    } else {
	this->mediaInfo.url = url;
    }

    if (this->renderContext == nullptr) {
	return;
    }

    if (this->wallpaper == nullptr) {
	return;
    }

    if (!this->wallpaper->is<Wallpapers::CScene> ()) {
	return;
    }

    this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().notifyAlbumArtUrlChange (this->mediaInfo.url);

    this->renderContext->albumArtUrlChange (this->mediaInfo.url);
}

void WallpaperEngine::Project::playbackPositionAndDurationChange (double position, double duration) {
    this->mediaInfo.position = position;
    this->mediaInfo.duration = duration;

    if (this->wallpaper == nullptr) {
	return;
    }

    if (!this->wallpaper->is<Wallpapers::CScene> ()) {
	return;
    }

    this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().notifyPlaybackPositionAndDurationChange (
	this->mediaInfo.position, this->mediaInfo.duration
    );
}

void WallpaperEngine::Project::playbackStateChange (wp_media_playback_state state) {
    this->mediaInfo.state = state;

    if (this->wallpaper == nullptr) {
	return;
    }

    if (!this->wallpaper->is<Wallpapers::CScene> ()) {
	return;
    }

    this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().notifyPlaybackStateChange (this->mediaInfo.state);
}

const WallpaperEngine::Project::MediaInfo& WallpaperEngine::Project::getMediaInfo () const { return this->mediaInfo; }

void WallpaperEngine::Project::trackMetadataExtraChange (
    const char* albumArtist, const char* subTitle, const char* genres, const char* contentType
) {
    this->mediaInfo.albumArtist = albumArtist ?: "";
    this->mediaInfo.subTitle = subTitle ?: "";
    this->mediaInfo.genres = genres ?: "";
    this->mediaInfo.contentType = contentType != nullptr && *contentType != '\0' ? contentType : "music";

    if (this->wallpaper != nullptr && this->wallpaper->is<Wallpapers::CScene> ()) {
	this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().notifyTrackMetadataChange (scriptMediaProperties (this->mediaInfo));
    }
}

void WallpaperEngine::Project::mediaStatusChange (bool enabled) {
    this->mediaInfo.enabled = enabled;

    if (this->wallpaper != nullptr && this->wallpaper->is<Wallpapers::CScene> ()) {
	this->wallpaper->as<Wallpapers::CScene> ()->getScriptEngine ().notifyMediaStatusChange (enabled);
    }
}

bool WallpaperEngine::Project::usesMedia () const {
    const auto& wallpaper = this->ref->wallpaper;
    return wallpaper != nullptr && wallpaper->is<Data::Model::Scene> () && wallpaper->as<Data::Model::Scene> ()->usesMedia;
}

WallpaperEngine::Project*
WallpaperEngine::Project::loadId (Context* context, wp_mouse_input* mouse_input, const unsigned long int id) {
    return new Project (context, mouse_input, context->config.backgrounds_dir / std::to_string (id));
}

WallpaperEngine::Project*
WallpaperEngine::Project::loadId (Context* context, wp_mouse_input* mouse_input, const std::string& id) {
    return new Project (context, mouse_input, context->config.backgrounds_dir / id);
}

WallpaperEngine::Project*
WallpaperEngine::Project::loadFolder (Context* context, wp_mouse_input* mouse_input, const char* folder) {
    try {
	return new Project (context, mouse_input, folder);
    } catch (const std::exception& e) {
	WPENGINE_HEALTH ("load_failed", { { "message", WallpaperEngine::Health::brief (e.what ()) } });
	throw;
    }
}
namespace {
/** Compiles one GL unit of the sweep; false + the driver's log when it doesn't compile */
bool sweepCompile (GLuint type, const std::string& source, GLuint& id, std::string& log) {
    id = glCreateShader (type);
    const char* text = source.c_str ();
    glShaderSource (id, 1, &text, nullptr);
    glCompileShader (id);
    GLint ok = GL_FALSE, length = 0;
    glGetShaderiv (id, GL_COMPILE_STATUS, &ok);
    glGetShaderiv (id, GL_INFO_LOG_LENGTH, &length);
    if (ok == GL_FALSE && length > 0) {
	std::string buffer (static_cast<size_t> (length), '\0');
	glGetShaderInfoLog (id, length, nullptr, buffer.data ());
	log = buffer.c_str ();
    }
    return ok != GL_FALSE;
}
} // namespace

std::string WallpaperEngine::Project::shaderCheck (const std::string& name, const std::string& request) {
    using namespace WallpaperEngine::Render::Shaders;
    nlohmann::json out = { { "shader", name }, { "ok", false } };
    nlohmann::json compat = nlohmann::json::array ();
    const WallpaperEngine::Health::Capture capture;
    std::string stage = "load", message;

    try {
	const auto req = request.empty () ? nlohmann::json::object () : nlohmann::json::parse (request);
	ComboMap combos;
	TextureMap textures;
	const ComboMap noCombos;
	const TextureMap noTextures;
	const ShaderConstantMap noConstants;
	if (req.contains ("combos")) {
	    for (const auto& [key, value] : req["combos"].items ()) {
		combos[key] = value.is_number () ? value.get<int> () : 0;
	    }
	}
	if (req.contains ("textures")) {
	    for (const auto& slot : req["textures"]) {
		textures[slot.get<int> ()] = "sweep";
	    }
	}
	const bool gl = req.value ("gl", true);

	Shader shader (*this->ref->assetLocator, name, combos, noCombos, textures, noTextures, noConstants);
	const std::string vertexIn = shader.vertex ();
	const std::string fragmentIn = shader.fragment ();
	nlohmann::json effective = nlohmann::json::object ();
	for (const auto* unit : { &shader.getVertex (), &shader.getFragment () }) {
	    for (const auto& [key, value] : unit->getCombos ()) {
		effective[key] = value;
	    }
	    for (const auto& [key, value] : unit->getDiscoveredCombos ()) {
		if (!effective.contains (key)) {
		    effective[key] = value;
		}
	    }
	}
	out["combos"] = effective;
	if (req.value ("source", false)) {
	    out["vertex_source"] = vertexIn;
	    out["fragment_source"] = fragmentIn;
	}
	stage = "glslang";
	const auto [vertex, fragment] = GLSLContext::get ().toGlsl (vertexIn, fragmentIn);
	if (vertex.empty () || fragment.empty ()) {
	    throw std::runtime_error ("");
	}
	if (req.value ("source", false)) {
	    out["vertex_glsl"] = vertex;
	    out["fragment_glsl"] = fragment;
	}
	if (gl) {
	    if (active_context != &this->context) {
		active_context = &this->context;
		gladLoadGLLoader (wp_context_call_gl_proc_address);
	    }
	    GLuint vs = 0, fs = 0;
	    stage = "gl";
	    const bool vok = sweepCompile (GL_VERTEX_SHADER, vertex, vs, message);
	    const bool fok = vok && sweepCompile (GL_FRAGMENT_SHADER, fragment, fs, message);
	    if (vok && fok) {
		stage = "link";
		const GLuint program = glCreateProgram ();
		glAttachShader (program, vs);
		glAttachShader (program, fs);
		glLinkProgram (program);
		GLint ok = GL_FALSE, length = 0;
		glGetProgramiv (program, GL_LINK_STATUS, &ok);
		glGetProgramiv (program, GL_INFO_LOG_LENGTH, &length);
		if (ok == GL_FALSE && length > 0) {
		    std::string buffer (static_cast<size_t> (length), '\0');
		    glGetProgramInfoLog (program, length, nullptr, buffer.data ());
		    message = buffer.c_str ();
		}
		glDeleteProgram (program);
		if (ok != GL_FALSE) {
		    stage = "";
		}
	    } else {
		out["unit"] = vok ? "fragment" : "vertex";
	    }
	    if (vs != 0) {
		glDeleteShader (vs);
	    }
	    if (fs != 0) {
		glDeleteShader (fs);
	    }
	} else {
	    stage = "";
	}
    } catch (const std::exception& e) {
	if (message.empty ()) {
	    message = e.what ();
	}
    }

    for (const auto& record : capture.records ()) {
	const std::string ev = record.value ("ev", "");
	if (ev == "compat") {
	    compat.push_back (record.value ("what", ""));
	} else if (ev == "shader_error" && message.empty ()) {
	    message = record.value ("message", "");
	    if (record.contains ("unit")) {
		out["unit"] = record["unit"];
	    }
	}
    }
    out["ok"] = stage.empty ();
    if (!stage.empty ()) {
	out["stage"] = stage;
	out["message"] = WallpaperEngine::Health::brief (message, 2000);
    }
    out["compat"] = compat;
    return out.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
