#pragma once

#include <map>
#include <optional>

#include <functional>
#include <glm/gtc/type_ptr.hpp>
#include <utility>

#include "../../TextureProvider.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"

namespace WallpaperEngine::Render::Objects {
class CRenderable;
}

namespace WallpaperEngine::Render::Objects::Effects {
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Shaders::Variables;
using namespace WallpaperEngine::Data::Model;

class CPass final : public Helpers::ContextAware {
public:
    CPass (
	CRenderable& renderable, std::shared_ptr<const FBOProvider> fboProvider, const MaterialPass& pass,
	std::optional<std::reference_wrapper<const ImageEffectPassOverride>> override,
	std::optional<std::reference_wrapper<const TextureMap>> binds,
	std::optional<std::reference_wrapper<std::string>> target
    );
    ~CPass ();

    void render ();

    void setDestination (std::shared_ptr<const CFBO> drawTo);
    void setInput (std::shared_ptr<const TextureProvider> input);
    void setPreviousInput (std::shared_ptr<const TextureProvider> input);
    /** Replaces what texture unit `index` samples (a compose layer's base pass reads its group target) */
    void setTexture (int index, std::shared_ptr<const TextureProvider> texture);
    void setTexCoord (GLuint texcoord);
    void setPosition (GLuint position);
    void setModelViewProjectionMatrix (const glm::mat4* projection);
    [[nodiscard]] const glm::mat4* getModelViewProjectionMatrix () const { return this->m_modelViewProjectionMatrix; }
    void setModelViewProjectionMatrixInverse (const glm::mat4* projection);
    void setModelMatrix (const glm::mat4* model);
    /** g_LayerModelMatrix: the layer's own matrix in WE's world (GH 0x1402176a0 copies image +0x384 to ctx +0xa30) */
    void setLayerModelMatrix (const glm::mat4* layerModel);
    void setViewProjectionMatrix (const glm::mat4* viewProjection);
    void setBlendingMode (BlendingMode blendingmode);
    [[nodiscard]] BlendingMode getBlendingMode () const;
    [[nodiscard]] std::shared_ptr<const CFBO> resolveFBO (const std::string& name) const;
    /** A usertexture's texture: the value of the scenetexture property it names (none picked: nullopt), else itself */
    [[nodiscard]] std::optional<std::string> userTextureName (const std::string& name) const;

    [[nodiscard]] std::shared_ptr<const FBOProvider> getFBOProvider () const;
    [[nodiscard]] const CRenderable& getRenderable () const;
    [[nodiscard]] const MaterialPass& getPass () const;
    [[nodiscard]] std::optional<std::reference_wrapper<std::string>> getTarget () const;
    [[nodiscard]] Render::Shaders::Shader* getShader () const;
    [[nodiscard]] GLuint getProgramID () const;

    /** `command: swap` (GH 0x1402131b0 reads it as command 2): render () exchanges the two named FBOs instead */
    void setSwap (std::string first, std::string second);
    /** the WE texture format and .tex flags bound at sampler `index` (TEX<n>FORMAT, components; TextureFormatResolver) */
    [[nodiscard]] Render::Shaders::ResolvedTexture shaderFormatOf (int index, const std::string& defaultName) const;
    [[nodiscard]] bool isSwap () const;

    // Custom geometry rendering support (for particles, etc.)
    using GeometryCallback = std::function<void ()>;
    void
    setGeometryCallback (GeometryCallback setupAttribs, GeometryCallback drawGeometry, GeometryCallback cleanupAttribs);

    // Public uniform setters for external callers (pointer-based, updated per-frame)
    void addUniform (const std::string& name, const float* value, int count = 1);
    void addUniform (const std::string& name, const glm::vec2* value);
    void addUniform (const std::string& name, const glm::vec3* value);
    void addUniform (const std::string& name, const glm::vec4* value);
    void addUniform (const std::string& name, const glm::mat4* value);

private:
    struct TextureChainEntry {
	std::shared_ptr<const TextureProvider> texture;
	std::shared_ptr<TextureChainEntry> next;
    };

    enum UniformType {
	Float = 0,
	Matrix3 = 1,
	Matrix4 = 2,
	Integer = 3,
	Vector2 = 4,
	Vector3 = 5,
	Vector4 = 6,
	Double = 7
    };

    class UniformEntry {
    public:
	UniformEntry (const GLint id, std::string name, UniformType type, const void* value, int count) :
	    id (id), name (std::move (name)), type (type), value (value), count (count) { }

	const GLint id;
	std::string name;
	UniformType type;
	const void* value;
	int count;
    };

    class ReferenceUniformEntry {
    public:
	ReferenceUniformEntry (const GLint id, std::string name, UniformType type, const void** value) :
	    id (id), name (std::move (name)), type (type), value (value) { }

	const GLint id;
	std::string name;
	UniformType type;
	const void** value;
    };

    class AttribEntry {
    public:
	AttribEntry (const GLint id, std::string name, GLint type, GLint elements, const GLuint* value) :
	    id (id), name (std::move (name)), type (type), elements (elements), value (value) { }

	const GLint id;
	std::string name;
	GLint type;
	GLint elements;
	const GLuint* value;
    };

    struct TextureAnimationState {
	uint32_t currentTexture = 0;
	glm::vec2 translation = { 0.0f, 0.0f };
	glm::vec4 rotation = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    static GLuint compileShader (const char* shader, GLuint type);
    void setupShaders ();
    void setupShaderVariables ();
    void setupUniforms ();
    void setupTextureUniforms ();
    void setupAttributes ();
    void addAttribute (const std::string& name, GLint type, GLint elements, const GLuint* value);
    void addUniform (ShaderVariable* value);
    void addUniform (const ShaderVariable* value, const DynamicValue* setting);
    void addUniform (const std::string& name, int value);
    void addUniform (const std::string& name, double value);
    void addUniform (const std::string& name, float value);
    void addUniform (const std::string& name, glm::vec2 value);
    void addUniform (const std::string& name, glm::vec3 value);
    void addUniform (const std::string& name, glm::vec4 value);
    void addUniform (const std::string& name, const glm::mat3& value);
    void addUniform (const std::string& name, glm::mat4 value);
    void addUniform (const std::string& name, const int* value, int count = 1);
    void addUniform (const std::string& name, const double* value, int count = 1);
    void addUniform (const std::string& name, const glm::mat3* value);
    void addUniform (const std::string& name, const int** value);
    void addUniform (const std::string& name, const double** value);
    void addUniform (const std::string& name, const float** value);
    void addUniform (const std::string& name, const glm::vec2** value);
    void addUniform (const std::string& name, const glm::vec3** value);
    void addUniform (const std::string& name, const glm::vec4** value);
    void addUniform (const std::string& name, const glm::mat3** value);
    void addUniform (const std::string& name, const glm::mat4** value);
    template <typename T> void addUniform (const std::string& name, UniformType type, T value);
    template <typename T> void addUniform (const std::string& name, UniformType type, T* value, int count = 1);
    template <typename T> void addUniform (const std::string& name, UniformType type, T** value);

    void setupRenderFramebuffer () const;
    void setupRenderTexture ();
    [[nodiscard]] std::shared_ptr<const TextureProvider> resolveTexture0 ();
    [[nodiscard]] TextureAnimationState
    resolveTextureAnimationState (const std::shared_ptr<const TextureProvider>& texture) const;
    void bindTextureUnit (int index, const std::shared_ptr<const TextureProvider>& texture, uint32_t frame) const;
    /** The program samples g_Texture<index> (it's active: not compiled out by a combo) */
    [[nodiscard]] bool samplerIsUsed (int index) const;
    void bindTextureOverrides (uint32_t currentTexture, std::shared_ptr<const TextureProvider>& texture0) const;
    void setupRenderUniforms ();
    void setupRenderReferenceUniforms ();
    void setupRenderAttributes () const;
    void renderGeometry () const;
    void cleanupRenderSetup ();

    std::shared_ptr<const TextureProvider> resolveTexture (
	std::shared_ptr<const TextureProvider> expected, int index,
	std::shared_ptr<const TextureProvider> previous = nullptr
    );

    CRenderable& m_renderable;
    std::shared_ptr<const FBOProvider> m_fboProvider;
    const MaterialPass& m_pass;
    const TextureMap& m_binds;
    const ImageEffectPassOverride& m_override;
    std::optional<std::reference_wrapper<std::string>> m_target;
    std::map<int, std::shared_ptr<const CFBO>> m_fbos = {};
    std::map<std::string, int> m_combos = {};
    std::vector<AttribEntry*> m_attribs = {};
    std::map<std::string, UniformEntry*> m_uniforms = {};
    std::map<std::string, ReferenceUniformEntry*> m_referenceUniforms = {};
    BlendingMode m_blendingmode = BlendingMode_Normal;
    const glm::mat4* m_modelViewProjectionMatrix;
    const glm::mat4* m_modelViewProjectionMatrixInverse;
    const glm::mat4* m_modelMatrix;
    const glm::mat4* m_layerModelMatrix = &kIdentityMatrix;
    static inline const glm::mat4 kIdentityMatrix = glm::mat4 (1.0f);
    const glm::mat4* m_viewProjectionMatrix;

    /**
     * Contains the final map of textures to be used
     */
    std::map<int, std::shared_ptr<TextureChainEntry>> m_textures = {};

    Render::Shaders::Shader* m_shader = nullptr;

    std::shared_ptr<const CFBO> m_drawTo = nullptr;
    /** this render's copy of m_drawTo, for a texture unit that samples it (taken once per render) */
    mutable std::shared_ptr<const CFBO> m_drawToCopy = nullptr;
    /** a swap command's FBO names (empty: a draw) */
    std::optional<std::pair<std::string, std::string>> m_swap = std::nullopt;
    /** samplerIsUsed, by unit */
    mutable std::map<int, bool> m_samplerUsed = {};
    std::shared_ptr<const TextureProvider> m_input = nullptr;
    std::shared_ptr<const TextureProvider> m_previousInput = nullptr;
    glm::vec4 m_texture0Resolution = {};
    /** g_Texture<n>Texel per bound texture (std::map: the uniforms point at its nodes) */
    std::map<int, glm::vec4> m_textureTexels = {};

    GLuint m_programID;

    // shader variables used temporary
    GLint g_Texture0Rotation;
    GLint g_Texture0Translation;
    GLuint a_TexCoord;
    GLuint a_Position;
    GLuint m_vao;

    // Custom geometry callbacks (for particles, etc.)
    GeometryCallback m_setupAttribsCallback;
    GeometryCallback m_drawGeometryCallback;
    GeometryCallback m_cleanupAttribsCallback;
};
} // namespace WallpaperEngine::Render::Objects::Effects
