#include <stdio.h>
#include <vector>
#include <string>
#include <map>

#include "base/logging.h"
#include "math/dataconv.h"
#include "math/lin/matrix4x4.h"
#include "thin3d/thin3d.h"
#include "gfx/gl_common.h"
#include "gfx/GLStateCache.h"
#include "gfx_es2/gpu_features.h"
#include "gfx/gl_lost_manager.h"

#ifdef IOS
extern void bindDefaultFBO();
#endif

namespace Draw {

static const unsigned short compToGL[] = {
	GL_NEVER,
	GL_LESS,
	GL_EQUAL,
	GL_LEQUAL,
	GL_GREATER,
	GL_NOTEQUAL,
	GL_GEQUAL,
	GL_ALWAYS
};

static const unsigned short blendEqToGL[] = {
	GL_FUNC_ADD,
	GL_FUNC_SUBTRACT,
	GL_FUNC_REVERSE_SUBTRACT,
	GL_MIN,
	GL_MAX,
};

static const unsigned short blendFactorToGL[] = {
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA,
	GL_CONSTANT_COLOR,
	GL_ONE_MINUS_CONSTANT_COLOR,
	GL_CONSTANT_ALPHA,
	GL_ONE_MINUS_CONSTANT_ALPHA,
#if !defined(USING_GLES2)   // TODO: Remove when we have better headers
	GL_SRC1_COLOR,
	GL_ONE_MINUS_SRC1_COLOR,
	GL_SRC1_ALPHA,
	GL_ONE_MINUS_SRC1_ALPHA,
#elif !defined(IOS)
	GL_SRC1_COLOR_EXT,
	GL_ONE_MINUS_SRC1_COLOR_EXT,
	GL_SRC1_ALPHA_EXT,
	GL_ONE_MINUS_SRC1_ALPHA_EXT,
#else
	GL_INVALID_ENUM,
	GL_INVALID_ENUM,
	GL_INVALID_ENUM,
	GL_INVALID_ENUM,
#endif
};

static const unsigned short texWrapToGL[] = {
	GL_REPEAT,
	GL_MIRRORED_REPEAT,
	GL_CLAMP_TO_EDGE,
#if !defined(USING_GLES2)
	GL_CLAMP_TO_BORDER,
#else
	GL_REPEAT,
#endif
};

static const unsigned short texFilterToGL[] = {
	GL_NEAREST,
	GL_LINEAR,
};

static const unsigned short texMipFilterToGL[2][2] = {
	// Min nearest:
	{ GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR },
	// Min linear:
	{ GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR },
};

#ifndef USING_GLES2
static const unsigned short logicOpToGL[] = {
	GL_CLEAR,
	GL_SET,
	GL_COPY,
	GL_COPY_INVERTED,
	GL_NOOP,
	GL_INVERT,
	GL_AND,
	GL_NAND,
	GL_OR,
	GL_NOR,
	GL_XOR,
	GL_EQUIV,
	GL_AND_REVERSE,
	GL_AND_INVERTED,
	GL_OR_REVERSE,
	GL_OR_INVERTED,
};
#endif

static const GLuint stencilOpToGL[8] = {
	GL_KEEP,
	GL_ZERO,
	GL_REPLACE,
	GL_INCR,
	GL_DECR,
	GL_INVERT,
	GL_INCR_WRAP,
	GL_DECR_WRAP,
};

static const unsigned short primToGL[] = {
	GL_POINTS,
	GL_LINES,
	GL_LINE_STRIP,
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_TRIANGLE_FAN,
#if !defined(USING_GLES2)   // TODO: Remove when we have better headers
	GL_PATCHES,
	GL_LINES_ADJACENCY,
	GL_LINE_STRIP_ADJACENCY,
	GL_TRIANGLES_ADJACENCY,
	GL_TRIANGLE_STRIP_ADJACENCY,
#elif !defined(IOS)
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
#else
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
#endif
};

class OpenGLBuffer;

static const char *glsl_fragment_prelude =
"#ifdef GL_ES\n"
"precision mediump float;\n"
"#endif\n";

class OpenGLBlendState : public BlendState {
public:
	bool enabled;
	GLuint eqCol, eqAlpha;
	GLuint srcCol, srcAlpha, dstCol, dstAlpha;
	bool logicEnabled;
	GLuint logicOp;
	int colorMask;
	// uint32_t fixedColor;

	void Apply() {
		if (enabled) {
			glEnable(GL_BLEND);
			glBlendEquationSeparate(eqCol, eqAlpha);
			glBlendFuncSeparate(srcCol, dstCol, srcAlpha, dstAlpha);
		} else {
			glDisable(GL_BLEND);
		}
		glColorMask(colorMask & 1, (colorMask >> 1) & 1, (colorMask >> 2) & 1, (colorMask >> 3) & 1);

#if !defined(USING_GLES2)
		if (logicEnabled) {
			glEnable(GL_COLOR_LOGIC_OP);
			glLogicOp(logicOp);
		} else {
			glDisable(GL_COLOR_LOGIC_OP);
		}
#endif
	}
};

class OpenGLSamplerState : public SamplerState {
public:
	// Old school. Should also support using a sampler object.

	GLint wrapS;
	GLint wrapT;
	GLint magFilt;
	GLint minFilt;
	GLint mipMinFilt;

	void Apply(bool hasMips, bool canWrap) {
		if (canWrap) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilt);
		if (hasMips) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipMinFilt);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilt);
		}
	}
};

class OpenGLDepthStencilState : public DepthStencilState {
public:
	bool depthTestEnabled;
	bool depthWriteEnabled;
	GLuint depthComp;
	// TODO: Two-sided
	GLboolean stencilEnabled;
	GLuint stencilFail;
	GLuint stencilZFail;
	GLuint stencilPass;
	GLuint stencilCompareOp;
	uint8_t stencilReference;
	uint8_t stencilCompareMask;
	uint8_t stencilWriteMask;

	void Apply() {
		if (depthTestEnabled) {
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(depthComp);
			glDepthMask(depthWriteEnabled);
		} else {
			glDisable(GL_DEPTH_TEST);
		}
		if (stencilEnabled) {
			glEnable(GL_STENCIL_TEST);
			glStencilOpSeparate(GL_FRONT_AND_BACK, stencilFail, stencilZFail, stencilPass);
			glStencilFuncSeparate(GL_FRONT_AND_BACK, stencilCompareOp, stencilReference, stencilCompareMask);
			glStencilMaskSeparate(GL_FRONT_AND_BACK, stencilWriteMask);
		} else {
			glDisable(GL_STENCIL_TEST);
		}
		glDisable(GL_STENCIL_TEST);
	}
};

class OpenGLRasterState : public RasterState {
public:
	void Apply() {
		glEnable(GL_SCISSOR_TEST);
		if (!cullEnable) {
			glDisable(GL_CULL_FACE);
			return;
		}
		glEnable(GL_CULL_FACE);
		glFrontFace(frontFace);
		glCullFace(cullMode);
	}

	GLboolean cullEnable;
	GLenum cullMode;
	GLenum frontFace;
};

GLuint ShaderStageToOpenGL(ShaderStage stage) {
	switch (stage) {
	case ShaderStage::VERTEX: return GL_VERTEX_SHADER;
#ifndef USING_GLES2
	case ShaderStage::COMPUTE: return GL_COMPUTE_SHADER;
	case ShaderStage::EVALUATION: return GL_TESS_EVALUATION_SHADER;
	case ShaderStage::CONTROL: return GL_TESS_CONTROL_SHADER;
	case ShaderStage::GEOMETRY: return GL_GEOMETRY_SHADER;
#endif
	case ShaderStage::FRAGMENT:
	default:
		return GL_FRAGMENT_SHADER;
	}
}

// Not registering this as a resource holder, instead Pipeline is registered. It will
// invoke Compile again to recreate the shader then link them together.
class OpenGLShaderModule : public ShaderModule {
public:
	OpenGLShaderModule(ShaderStage stage) : stage_(stage), shader_(0) {
		glstage_ = ShaderStageToOpenGL(stage);
	}

	~OpenGLShaderModule() {
		glDeleteShader(shader_);
	}

	bool Compile(ShaderLanguage language, const uint8_t *data, size_t dataSize);
	GLuint GetShader() const {
		return shader_;
	}
	const std::string &GetSource() const { return source_; }

	void Unset() {
		shader_ = 0;
	}
	ShaderLanguage GetLanguage() {
		return language_;
	}
	ShaderStage GetStage() const override {
		return stage_;
	}

private:
	ShaderStage stage_;
	ShaderLanguage language_;
	GLuint shader_;
	GLuint glstage_;
	bool ok_;
	std::string source_;  // So we can recompile in case of context loss.
};

bool OpenGLShaderModule::Compile(ShaderLanguage language, const uint8_t *data, size_t dataSize) {
	source_ = std::string((const char *)data);
	shader_ = glCreateShader(glstage_);
	language_ = language;

	std::string temp;
	// Add the prelude on automatically for fragment shaders.
	if (glstage_ == GL_FRAGMENT_SHADER) {
		temp = std::string(glsl_fragment_prelude) + source_;
		source_ = temp.c_str();
	}

	const char *code = source_.c_str();
	glShaderSource(shader_, 1, &code, nullptr);
	glCompileShader(shader_);
	GLint success = 0;
	glGetShaderiv(shader_, GL_COMPILE_STATUS, &success);
	if (!success) {
#define MAX_INFO_LOG_SIZE 2048
		GLchar infoLog[MAX_INFO_LOG_SIZE];
		GLsizei len = 0;
		glGetShaderInfoLog(shader_, MAX_INFO_LOG_SIZE, &len, infoLog);
		infoLog[len] = '\0';
		glDeleteShader(shader_);
		shader_ = 0;
		ILOG("%s Shader compile error:\n%s", glstage_ == GL_FRAGMENT_SHADER ? "Fragment" : "Vertex", infoLog);
	}
	ok_ = success != 0;
	return ok_;
}

class OpenGLInputLayout : public InputLayout, GfxResourceHolder {
public:
	~OpenGLInputLayout();

	void Apply(const void *base = nullptr);
	void Unapply();
	void Compile();
	void GLRestore() override;
	void GLLost() override;
	bool RequiresBuffer() {
		return id_ != 0;
	}

	InputLayoutDesc desc;

	int semanticsMask_;  // Fast way to check what semantics to enable/disable.
	int stride_;
	GLuint id_;
	bool needsEnable_;
	intptr_t lastBase_;
};

struct UniformInfo {
	int loc_;
};

// TODO: Add Uniform Buffer support.
class OpenGLPipeline : public Pipeline, GfxResourceHolder {
public:
	OpenGLPipeline() {
		program_ = 0;
		register_gl_resource_holder(this);
	}
	~OpenGLPipeline() {
		unregister_gl_resource_holder(this);
		for (auto iter : shaders) {
			iter->Release();
		}
		glDeleteProgram(program_);
		if (depthStencil) depthStencil->Release();
		if (blend) blend->Release();
		if (raster) raster->Release();
		if (inputLayout) inputLayout->Release();
	}
	bool RequiresBuffer() override {
		return inputLayout->RequiresBuffer();
	}

	bool LinkShaders();

	void Apply();
	void Unapply();

	int GetUniformLoc(const char *name);

	void SetVector(const char *name, float *value, int n) override;
	void SetMatrix4x4(const char *name, const float value[16]) override;

	void GLLost() override {
		program_ = 0;
		for (auto iter : shaders) {
			iter->Unset();
		}
	}

	void GLRestore() override {
		for (auto iter : shaders) {
			iter->Compile(iter->GetLanguage(), (const uint8_t *)iter->GetSource().c_str(), iter->GetSource().size());
		}
		LinkShaders();
	}

	GLuint prim;
	std::vector<OpenGLShaderModule *> shaders;
	OpenGLInputLayout *inputLayout = nullptr;
	OpenGLDepthStencilState *depthStencil = nullptr;
	OpenGLBlendState *blend = nullptr;
	OpenGLRasterState *raster = nullptr;

private:
	GLuint program_;
	std::map<std::string, UniformInfo> uniforms_;
};

class OpenGLFramebuffer;

class OpenGLContext : public DrawContext {
public:
	OpenGLContext();
	virtual ~OpenGLContext();

	const DeviceCaps &GetDeviceCaps() const override {
		return caps_;
	}
	uint32_t GetSupportedShaderLanguages() const override {
#if defined(USING_GLES2)
		return (uint32_t)ShaderLanguage::GLSL_ES_200 | (uint32_t)ShaderLanguage::GLSL_ES_300;
#else
		return (uint32_t)ShaderLanguage::GLSL_ES_200 | (uint32_t)ShaderLanguage::GLSL_410;
#endif
	}
	uint32_t GetDataFormatSupport(DataFormat fmt) const override;

	DepthStencilState *CreateDepthStencilState(const DepthStencilStateDesc &desc) override;
	BlendState *CreateBlendState(const BlendStateDesc &desc) override;
	SamplerState *CreateSamplerState(const SamplerStateDesc &desc) override;
	RasterState *CreateRasterState(const RasterStateDesc &desc) override;
	Pipeline *CreateGraphicsPipeline(const PipelineDesc &desc) override;
	InputLayout *CreateInputLayout(const InputLayoutDesc &desc) override;
	ShaderModule *CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize) override;

	Texture *CreateTexture(const TextureDesc &desc) override;
	Buffer *CreateBuffer(size_t size, uint32_t usageFlags) override;
	Framebuffer *CreateFramebuffer(const FramebufferDesc &desc) override;

	void UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) override;

	void CopyFramebufferImage(Framebuffer *src, int level, int x, int y, int z, Framebuffer *dst, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth) override;
	bool BlitFramebuffer(Framebuffer *src, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *dst, int dstX1, int dstY1, int dstX2, int dstY2, int channelBits, FBBlitFilter filter) override;

	// These functions should be self explanatory.
	void BindFramebufferAsRenderTarget(Framebuffer *fbo) override;
	// color must be 0, for now.
	void BindFramebufferAsTexture(Framebuffer *fbo, int binding, FBChannel channelBit, int attachment) override;
	void BindFramebufferForRead(Framebuffer *fbo) override;

	void BindBackbufferAsRenderTarget() override;
	uintptr_t GetFramebufferAPITexture(Framebuffer *fbo, int channelBits, int attachment) override;

	void GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) override;

	void BindSamplerStates(int start, int count, SamplerState **states) override {
		if (samplerStates_.size() < (size_t)(start + count)) {
			samplerStates_.resize(start + count);
		}
		for (int i = 0; i < count; ++i) {
			int index = i + start;
			OpenGLSamplerState *s = static_cast<OpenGLSamplerState *>(states[index]);

			if (samplerStates_[index]) {
				samplerStates_[index]->Release();
			}
			samplerStates_[index] = s;
			samplerStates_[index]->AddRef();

			// TODO: Ideally, get these from the texture and apply on the right stage?
			if (index == 0) {
				s->Apply(false, true);
			}
		}
	}

	void SetScissorRect(int left, int top, int width, int height) override {
		glScissor(left, targetHeight_ - (top + height), width, height);
	}

	void SetViewports(int count, Viewport *viewports) override {
		// TODO: Use glViewportArrayv.
		glViewport(viewports[0].TopLeftX, viewports[0].TopLeftY, viewports[0].Width, viewports[0].Height);
#if defined(USING_GLES2)
		glDepthRangef(viewports[0].MinDepth, viewports[0].MaxDepth);
#else
		glDepthRange(viewports[0].MinDepth, viewports[0].MaxDepth);
#endif
	}

	void SetBlendFactor(float color[4]) override {
		glBlendColor(color[0], color[1], color[2], color[3]);
	}

	void BindTextures(int start, int count, Texture **textures) override;
	void BindPipeline(Pipeline *pipeline) override;
	void BindVertexBuffers(int start, int count, Buffer **buffers, int *offsets) override {
		for (int i = 0; i < count; i++) {
			curVBuffers_[i + start] = (OpenGLBuffer  *)buffers[i];
			curVBufferOffsets_[i + start] = offsets ? offsets[i] : 0;
		}
	}
	void BindIndexBuffer(Buffer *indexBuffer, int offset) override {
		curIBuffer_ = (OpenGLBuffer  *)indexBuffer;
		curIBufferOffset_ = offset;
	}

	// TODO: Add more sophisticated draws.
	void Draw(int vertexCount, int offset) override;
	void DrawIndexed(int vertexCount, int offset) override;
	void DrawUP(const void *vdata, int vertexCount) override;

	void Clear(int mask, uint32_t colorval, float depthVal, int stencilVal) override;

	std::string GetInfoString(InfoField info) const override {
		// TODO: Make these actually query the right information
		switch (info) {
			case APINAME:
				if (gl_extensions.IsGLES) {
					return "OpenGL ES";
				} else {
					return "OpenGL";
				}
			case VENDORSTRING: return (const char *)glGetString(GL_VENDOR);
			case VENDOR:
				switch (gl_extensions.gpuVendor) {
				case GPU_VENDOR_AMD: return "VENDOR_AMD";
				case GPU_VENDOR_POWERVR: return "VENDOR_POWERVR";
				case GPU_VENDOR_NVIDIA: return "VENDOR_NVIDIA";
				case GPU_VENDOR_INTEL: return "VENDOR_INTEL";
				case GPU_VENDOR_ADRENO: return "VENDOR_ADRENO";
				case GPU_VENDOR_ARM: return "VENDOR_ARM";
				case GPU_VENDOR_BROADCOM: return "VENDOR_BROADCOM";
				case GPU_VENDOR_UNKNOWN:
				default:
					return "VENDOR_UNKNOWN";
				}
				break;
			case RENDERER: return (const char *)glGetString(GL_RENDERER);
			case SHADELANGVERSION: return (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
			case APIVERSION: return (const char *)glGetString(GL_VERSION);
			default: return "?";
		}
	}

	uintptr_t GetNativeObject(NativeObject obj) const override {
		return 0;
	}

	void HandleEvent(Event ev) override {}

	OpenGLFramebuffer *fbo_ext_create(const FramebufferDesc &desc);
	void fbo_bind_fb_target(bool read, GLuint name);
	GLenum fbo_get_fb_target(bool read, GLuint **cached);
	void fbo_unbind();

private:
	std::vector<OpenGLSamplerState *> samplerStates_;
	DeviceCaps caps_;
	
	// Bound state
	OpenGLPipeline *curPipeline_;
	OpenGLBuffer *curVBuffers_[4];
	int curVBufferOffsets_[4];
	OpenGLBuffer *curIBuffer_;
	int curIBufferOffset_;

	// Framebuffer state
	GLuint currentDrawHandle_ = 0;
	GLuint currentReadHandle_ = 0;
};

OpenGLContext::OpenGLContext() {
	CreatePresets();

	// TODO: Detect more caps
	if (gl_extensions.IsGLES) {
		if (gl_extensions.OES_packed_depth_stencil || gl_extensions.OES_depth24) {
			caps_.preferredDepthBufferFormat = DataFormat::D24_S8;
		} else {
			caps_.preferredDepthBufferFormat = DataFormat::D16;
		}
	} else {
		caps_.preferredDepthBufferFormat = DataFormat::D24_S8;
	}
}

OpenGLContext::~OpenGLContext() {
	for (OpenGLSamplerState *s : samplerStates_) {
		if (s) {
			s->Release();
		}
	}
	samplerStates_.clear();
}

InputLayout *OpenGLContext::CreateInputLayout(const InputLayoutDesc &desc) {
	OpenGLInputLayout *fmt = new OpenGLInputLayout();
	fmt->desc = desc;
	fmt->Compile();
	return fmt;
}

GLuint TypeToTarget(TextureType type) {
	switch (type) {
#ifndef USING_GLES2
	case TextureType::LINEAR1D: return GL_TEXTURE_1D;
#endif
	case TextureType::LINEAR2D: return GL_TEXTURE_2D;
	case TextureType::LINEAR3D: return GL_TEXTURE_3D;
	case TextureType::CUBE: return GL_TEXTURE_CUBE_MAP;
#ifndef USING_GLES2
	case TextureType::ARRAY1D: return GL_TEXTURE_1D_ARRAY;
#endif
	case TextureType::ARRAY2D: return GL_TEXTURE_2D_ARRAY;
	default: return GL_NONE;
	}
}

inline bool isPowerOf2(int n) {
	return n == 1 || (n & (n - 1)) == 0;
}

class OpenGLTexture : public Texture {
public:
	OpenGLTexture(const TextureDesc &desc) : tex_(0), target_(TypeToTarget(desc.type)), format_(desc.format), mipLevels_(desc.mipLevels) {
		generatedMips_ = false;
		canWrap_ = true;
		width_ = desc.width;
		height_ = desc.height;
		depth_ = desc.depth;
		canWrap_ = !isPowerOf2(width_) || !isPowerOf2(height_);

		glGenTextures(1, &tex_);

		if (!desc.initData.size())
			return;

		int level = 0;
		for (auto data : desc.initData) {
			SetImageData(0, 0, 0, width_, height_, depth_, level, 0, data);
			width_ = (width_ + 1) /2;
			height_ = (height_ + 1) /2;
			level++;
		}
		if (desc.initData.size() < desc.mipLevels)
			AutoGenMipmaps();
	}
	~OpenGLTexture() {
		Destroy();
	}

	void Destroy() {
		if (tex_) {
			glDeleteTextures(1, &tex_);
			tex_ = 0;
			generatedMips_ = false;
		}
	}

	void SetImageData(int x, int y, int z, int width, int height, int depth, int level, int stride, const uint8_t *data) override;
	void AutoGenMipmaps();

	bool HasMips() {
		return mipLevels_ > 1 || generatedMips_;
	}
	bool CanWrap() {
		return canWrap_;
	}

	void Bind() {
		glBindTexture(target_, tex_);
	}

private:
	GLuint tex_;
	GLuint target_;

	DataFormat format_;
	int mipLevels_;
	bool generatedMips_;
	bool canWrap_;
};

Texture *OpenGLContext::CreateTexture(const TextureDesc &desc) {
	return new OpenGLTexture(desc);
}

void OpenGLTexture::AutoGenMipmaps() {
	if (!generatedMips_) {
		Bind();
		glGenerateMipmap(target_);
		// TODO: Really, this should follow the sampler state.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
		generatedMips_ = true;
	}
}

void OpenGLTexture::SetImageData(int x, int y, int z, int width, int height, int depth, int level, int stride, const uint8_t *data) {
	int internalFormat;
	int format;
	int type;

	if (width != width_ || height != height_ || depth != depth_) {
		// When switching to texStorage we need to handle this correctly.
		width_ = width;
		height_ = height;
		depth_ = depth;
	}

	switch (format_) {
	case DataFormat::R8G8B8A8_UNORM:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
		break;
	case DataFormat::R4G4B4A4_UNORM_PACK16:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_SHORT_4_4_4_4;
		break;

#ifndef USING_GLES2
	case DataFormat::A4B4G4R4_UNORM_PACK16:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_SHORT_4_4_4_4_REV;
		break;
#endif

	default:
		ELOG("Thin3d GL: Unsupported texture format %d", (int)format_);
		return;
	}

	Bind();
	switch (target_) {
	case GL_TEXTURE_2D:
		glTexImage2D(GL_TEXTURE_2D, level, internalFormat, width_, height_, 0, format, type, data);
		break;
	default:
		ELOG("Thin3D GL: Targets other than GL_TEXTURE_2D not yet supported");
		break;
	}

	GLenum err = glGetError();
	if (err) {
		ELOG("Thin3D GL: Error loading texture: %08x", err);
	}
}

OpenGLInputLayout::~OpenGLInputLayout() {
	if (id_) {
		glDeleteVertexArrays(1, &id_);
	}
}

void OpenGLInputLayout::Compile() {
	int semMask = 0;
	for (int i = 0; i < (int)desc.attributes.size(); i++) {
		semMask |= 1 << desc.attributes[i].location;
	}
	semanticsMask_ = semMask;

	if (gl_extensions.ARB_vertex_array_object && gl_extensions.IsCoreContext) {
		glGenVertexArrays(1, &id_);
	} else {
		id_ = 0;
	}
	needsEnable_ = true;
	lastBase_ = -1;
}

void OpenGLInputLayout::GLLost() {
	id_ = 0;
}

void OpenGLInputLayout::GLRestore() {
	Compile();
}

DepthStencilState *OpenGLContext::CreateDepthStencilState(const DepthStencilStateDesc &desc) {
	OpenGLDepthStencilState *ds = new OpenGLDepthStencilState();
	ds->depthTestEnabled = desc.depthTestEnabled;
	ds->depthWriteEnabled = desc.depthWriteEnabled;
	ds->depthComp = compToGL[(int)desc.depthCompare];
	ds->stencilEnabled = desc.stencilEnabled;
	ds->stencilCompareOp = compToGL[(int)desc.front.compareOp];
	ds->stencilPass = stencilOpToGL[(int)desc.front.passOp];
	ds->stencilFail = stencilOpToGL[(int)desc.front.failOp];
	ds->stencilZFail = stencilOpToGL[(int)desc.front.depthFailOp];
	ds->stencilWriteMask = desc.front.writeMask;
	ds->stencilReference = desc.front.reference;
	ds->stencilCompareMask = desc.front.compareMask;
	return ds;
}

BlendState *OpenGLContext::CreateBlendState(const BlendStateDesc &desc) {
	OpenGLBlendState *bs = new OpenGLBlendState();
	bs->enabled = desc.enabled;
	bs->eqCol = blendEqToGL[(int)desc.eqCol];
	bs->srcCol = blendFactorToGL[(int)desc.srcCol];
	bs->dstCol = blendFactorToGL[(int)desc.dstCol];
	bs->eqAlpha = blendEqToGL[(int)desc.eqAlpha];
	bs->srcAlpha = blendFactorToGL[(int)desc.srcAlpha];
	bs->dstAlpha = blendFactorToGL[(int)desc.dstAlpha];
#ifndef USING_GLES2
	bs->logicEnabled = desc.logicEnabled;
	bs->logicOp = logicOpToGL[(int)desc.logicOp];
#endif
	bs->colorMask = desc.colorMask;
	return bs;
}

SamplerState *OpenGLContext::CreateSamplerState(const SamplerStateDesc &desc) {
	OpenGLSamplerState *samps = new OpenGLSamplerState();
	samps->wrapS = texWrapToGL[(int)desc.wrapU];
	samps->wrapT = texWrapToGL[(int)desc.wrapV];
	samps->magFilt = texFilterToGL[(int)desc.magFilter];
	samps->minFilt = texFilterToGL[(int)desc.minFilter];
	samps->mipMinFilt = texMipFilterToGL[(int)desc.minFilter][(int)desc.mipFilter];
	return samps;
}

RasterState *OpenGLContext::CreateRasterState(const RasterStateDesc &desc) {
	OpenGLRasterState *rs = new OpenGLRasterState();
	if (desc.cull == CullMode::NONE) {
		rs->cullEnable = GL_FALSE;
		return rs;
	}
	rs->cullEnable = GL_TRUE;
	switch (desc.frontFace) {
	case Facing::CW:
		rs->frontFace = GL_CW;
		break;
	case Facing::CCW:
		rs->frontFace = GL_CCW;
		break;
	}
	switch (desc.cull) {
	case CullMode::FRONT:
		rs->cullMode = GL_FRONT;
		break;
	case CullMode::BACK:
		rs->cullMode = GL_BACK;
		break;
	case CullMode::FRONT_AND_BACK:
		rs->cullMode = GL_FRONT_AND_BACK;
		break;
	case CullMode::NONE:
		// Unsupported
		break;
	}
	return rs;
}

class OpenGLBuffer : public Buffer, GfxResourceHolder {
public:
	OpenGLBuffer(size_t size, uint32_t flags) {
		glGenBuffers(1, &buffer_);
		target_ = (flags & BufferUsageFlag::INDEXDATA) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
		usage_ = 0;
		if (flags & BufferUsageFlag::DYNAMIC)
			usage_ = GL_STREAM_DRAW;
		else
			usage_ = GL_STATIC_DRAW;
		totalSize_ = size;
		glBindBuffer(target_, buffer_);
		glBufferData(target_, size, NULL, usage_);
		register_gl_resource_holder(this);
	}
	~OpenGLBuffer() override {
		unregister_gl_resource_holder(this);
		glDeleteBuffers(1, &buffer_);
	}

	void Bind(int offset) {
		// TODO: Can't support offset using ES 2.0
		glBindBuffer(target_, buffer_);
	}

	void GLLost() override {
		buffer_ = 0;
	}

	void GLRestore() override {
		ILOG("Recreating vertex buffer after gl_restore");
		totalSize_ = 0;  // Will cause a new glBufferData call. Should genBuffers again though?
		glGenBuffers(1, &buffer_);
	}

	GLuint buffer_;
	GLuint target_;
	GLuint usage_;

	size_t totalSize_;
};

Buffer *OpenGLContext::CreateBuffer(size_t size, uint32_t usageFlags) {
	return new OpenGLBuffer(size, usageFlags);
}

void OpenGLContext::UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) {
	OpenGLBuffer *buf = (OpenGLBuffer *)buffer;

	buf->Bind(0);
	if (size + offset > buf->totalSize_) {
		Crash();
	}
	// if (flags & UPDATE_DISCARD) we could try to orphan the buffer using glBufferData.
	glBufferSubData(buf->target_, offset, size, data);
}

Pipeline *OpenGLContext::CreateGraphicsPipeline(const PipelineDesc &desc) {
	if (!desc.shaders.size()) {
		ELOG("Pipeline requires at least one shader");
		return NULL;
	}
	OpenGLPipeline *pipeline = new OpenGLPipeline();
	for (auto iter : desc.shaders) {
		iter->AddRef();
		pipeline->shaders.push_back(static_cast<OpenGLShaderModule *>(iter));
	}
	if (pipeline->LinkShaders()) {
		// Build the rest of the virtual pipeline object.
		pipeline->prim = primToGL[(int)desc.prim];
		pipeline->depthStencil = (OpenGLDepthStencilState *)desc.depthStencil;
		pipeline->blend = (OpenGLBlendState *)desc.blend;
		pipeline->raster = (OpenGLRasterState *)desc.raster;
		pipeline->inputLayout = (OpenGLInputLayout *)desc.inputLayout;
		pipeline->depthStencil->AddRef();
		pipeline->blend->AddRef();
		pipeline->raster->AddRef();
		pipeline->inputLayout->AddRef();
		return pipeline;
	} else {
		delete pipeline;
		return NULL;
	}
}

void OpenGLContext::BindTextures(int start, int count, Texture **textures) {
	for (int i = start; i < start + count; i++) {
		OpenGLTexture *glTex = static_cast<OpenGLTexture *>(textures[i]);
		glActiveTexture(GL_TEXTURE0 + i);
		glTex->Bind();

		if ((int)samplerStates_.size() > i && samplerStates_[i]) {
			samplerStates_[i]->Apply(glTex->HasMips(), glTex->CanWrap());
		}
	}
	glActiveTexture(GL_TEXTURE0);
}


ShaderModule *OpenGLContext::CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize) {
	OpenGLShaderModule *shader = new OpenGLShaderModule(stage);
	if (shader->Compile(language, data, dataSize)) {
		return shader;
	} else {
		shader->Release();
		return nullptr;
	}
}

bool OpenGLPipeline::LinkShaders() {
	program_ = glCreateProgram();
	for (auto iter : shaders) {
		glAttachShader(program_, iter->GetShader());
	}

	// Bind all the common vertex data points. Mismatching ones will be ignored.
	glBindAttribLocation(program_, SEM_POSITION, "Position");
	glBindAttribLocation(program_, SEM_COLOR0, "Color0");
	glBindAttribLocation(program_, SEM_TEXCOORD0, "TexCoord0");
	glBindAttribLocation(program_, SEM_NORMAL, "Normal");
	glBindAttribLocation(program_, SEM_TANGENT, "Tangent");
	glBindAttribLocation(program_, SEM_BINORMAL, "Binormal");
	glLinkProgram(program_);

	GLint linkStatus = GL_FALSE;
	glGetProgramiv(program_, GL_LINK_STATUS, &linkStatus);
	if (linkStatus != GL_TRUE) {
		GLint bufLength = 0;
		glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &bufLength);
		if (bufLength) {
			char* buf = new char[bufLength];
			glGetProgramInfoLog(program_, bufLength, NULL, buf);
			ELOG("Could not link program:\n %s", buf);
			// We've thrown out the source at this point. Might want to do something about that.
#ifdef _WIN32
			OutputDebugStringUTF8(buf);
#endif
			delete[] buf;
		}
		return false;
	}

	// Auto-initialize samplers.
	glUseProgram(program_);
	for (int i = 0; i < 4; i++) {
		char temp[256];
		sprintf(temp, "Sampler%i", i);
		int samplerLoc = GetUniformLoc(temp);
		if (samplerLoc != -1) {
			glUniform1i(samplerLoc, i);
		}
	}

	// Here we could (using glGetAttribLocation) save a bitmask about which pieces of vertex data are used in the shader
	// and then AND it with the vertex format bitmask later...
	return true;
}

int OpenGLPipeline::GetUniformLoc(const char *name) {
	auto iter = uniforms_.find(name);
	int loc = -1;
	if (iter != uniforms_.end()) {
		loc = iter->second.loc_;
	} else {
		loc = glGetUniformLocation(program_, name);
		UniformInfo info;
		info.loc_ = loc;
		uniforms_[name] = info;
	}
	return loc;
}

void OpenGLPipeline::SetVector(const char *name, float *value, int n) {
	glUseProgram(program_);
	int loc = GetUniformLoc(name);
	if (loc != -1) {
		switch (n) {
		case 1: glUniform1fv(loc, 1, value); break;
		case 2: glUniform1fv(loc, 2, value); break;
		case 3: glUniform1fv(loc, 3, value); break;
		case 4: glUniform1fv(loc, 4, value); break;
		}
	}
}

void OpenGLPipeline::SetMatrix4x4(const char *name, const float value[16]) {
	glUseProgram(program_);
	int loc = GetUniformLoc(name);
	if (loc != -1) {
		glUniformMatrix4fv(loc, 1, false, value);
	}
}

void OpenGLPipeline::Apply() {
	glUseProgram(program_);
}

void OpenGLPipeline::Unapply() {
	glUseProgram(0);
}

void OpenGLContext::BindPipeline(Pipeline *pipeline) {
	curPipeline_ = (OpenGLPipeline *)pipeline;
	curPipeline_->blend->Apply();
	curPipeline_->depthStencil->Apply();
	curPipeline_->raster->Apply();
}

void OpenGLContext::Draw(int vertexCount, int offset) {
	curVBuffers_[0]->Bind(curVBufferOffsets_[0]);
	curPipeline_->inputLayout->Apply();
	curPipeline_->Apply();

	glDrawArrays(curPipeline_->prim, offset, vertexCount);

	curPipeline_->Unapply();
	curPipeline_->inputLayout->Unapply();
}

void OpenGLContext::DrawIndexed(int vertexCount, int offset) {
	curVBuffers_[0]->Bind(curVBufferOffsets_[0]);
	curPipeline_->inputLayout->Apply();
	curPipeline_->Apply();
	// Note: ibuf binding is stored in the VAO, so call this after binding the fmt.
	curIBuffer_->Bind(curIBufferOffset_);

	glDrawElements(curPipeline_->prim, vertexCount, GL_UNSIGNED_INT, (const void *)(size_t)offset);
	
	curPipeline_->Unapply();
	curPipeline_->inputLayout->Unapply();
}

void OpenGLContext::DrawUP(const void *vdata, int vertexCount) {
	curPipeline_->inputLayout->Apply(vdata);
	curPipeline_->Apply();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDrawArrays(curPipeline_->prim, 0, vertexCount);

	curPipeline_->Unapply();
	curPipeline_->inputLayout->Unapply();
}

void OpenGLContext::Clear(int mask, uint32_t colorval, float depthVal, int stencilVal) {
	float col[4];
	Uint8x4ToFloat4(col, colorval);
	GLuint glMask = 0;
	if (mask & ClearFlag::COLOR) {
		glClearColor(col[0], col[1], col[2], col[3]);
		glMask |= GL_COLOR_BUFFER_BIT;
	}
	if (mask & ClearFlag::DEPTH) {
#if defined(USING_GLES2)
		glClearDepthf(depthVal);
#else
		glClearDepth(depthVal);
#endif
		glMask |= GL_DEPTH_BUFFER_BIT;
	}
	if (mask & ClearFlag::STENCIL) {
		glClearStencil(stencilVal);
		glMask |= GL_STENCIL_BUFFER_BIT;
	}
	glClear(glMask);
}

DrawContext *T3DCreateGLContext() {
	return new OpenGLContext();
}

void OpenGLInputLayout::Apply(const void *base) {
	if (id_ != 0) {
		glBindVertexArray(id_);
	}

	if (needsEnable_ || id_ == 0) {
		for (int i = 0; i < SEM_MAX; i++) {
			if (semanticsMask_ & (1 << i)) {
				glEnableVertexAttribArray(i);
			}
		}
		if (id_ != 0) {
			needsEnable_ = false;
		}
	}

	intptr_t b = (intptr_t)base;
	if (b != lastBase_) {
		for (size_t i = 0; i < desc.attributes.size(); i++) {
			GLsizei stride = (GLsizei)desc.bindings[desc.attributes[i].binding].stride;
			switch (desc.attributes[i].format) {
			case DataFormat::R32G32_FLOAT:
				glVertexAttribPointer(desc.attributes[i].location, 2, GL_FLOAT, GL_FALSE, stride, (void *)(b + (intptr_t)desc.attributes[i].offset));
				break;
			case DataFormat::R32G32B32_FLOAT:
				glVertexAttribPointer(desc.attributes[i].location, 3, GL_FLOAT, GL_FALSE, stride, (void *)(b + (intptr_t)desc.attributes[i].offset));
				break;
			case DataFormat::R32G32B32A32_FLOAT:
				glVertexAttribPointer(desc.attributes[i].location, 4, GL_FLOAT, GL_FALSE, stride, (void *)(b + (intptr_t)desc.attributes[i].offset));
				break;
			case DataFormat::R8G8B8A8_UNORM:
				glVertexAttribPointer(desc.attributes[i].location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void *)(b + (intptr_t)desc.attributes[i].offset));
				break;
			case DataFormat::UNDEFINED:
			default:
				ELOG("Thin3DGLVertexFormat: Invalid or unknown component type applied.");
				break;
			}
		}
		if (id_ != 0) {
			lastBase_ = b;
		}
	}
}

void OpenGLInputLayout::Unapply() {
	if (id_ == 0) {
		for (int i = 0; i < (int)SEM_MAX; i++) {
			if (semanticsMask_ & (1 << i)) {
				glDisableVertexAttribArray(i);
			}
		}
	} else {
		glBindVertexArray(0);
	}
}

class OpenGLFramebuffer : public Framebuffer {
public:
	~OpenGLFramebuffer();
	GLuint handle;
	GLuint color_texture;
	GLuint z_stencil_buffer;  // Either this is set, or the two below.
	GLuint z_buffer;
	GLuint stencil_buffer;

	int width;
	int height;
	FBColorDepth colorDepth;
};

// On PC, we always use GL_DEPTH24_STENCIL8. 
// On Android, we try to use what's available.

#ifndef USING_GLES2
OpenGLFramebuffer *OpenGLContext::fbo_ext_create(const FramebufferDesc &desc) {
	OpenGLFramebuffer *fbo = new OpenGLFramebuffer();
	fbo->width = desc.width;
	fbo->height = desc.height;
	fbo->colorDepth = desc.colorDepth;

	// Color texture is same everywhere
	glGenFramebuffersEXT(1, &fbo->handle);
	glGenTextures(1, &fbo->color_texture);

	// Create the surfaces.
	glBindTexture(GL_TEXTURE_2D, fbo->color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// TODO: We could opt to only create 16-bit render targets on slow devices. For later.
	switch (fbo->colorDepth) {
	case FBO_8888:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		break;
	case FBO_4444:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
		break;
	case FBO_5551:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		break;
	case FBO_565:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fbo->width, fbo->height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		break;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	fbo->stencil_buffer = 0;
	fbo->z_buffer = 0;
	// 24-bit Z, 8-bit stencil
	glGenRenderbuffersEXT(1, &fbo->z_stencil_buffer);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_STENCIL_EXT, fbo->width, fbo->height);
	//glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, width, height);

	// Bind it all together
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo->handle);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, fbo->color_texture, 0);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);

	GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE_EXT:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED_EXT:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}
	// Unbind state we don't need
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	currentDrawHandle_ = fbo->handle;
	currentReadHandle_ = fbo->handle;
	return fbo;
}
#endif

Framebuffer *OpenGLContext::CreateFramebuffer(const FramebufferDesc &desc) {
	CheckGLExtensions();

#ifndef USING_GLES2
	if (!gl_extensions.ARB_framebuffer_object && gl_extensions.EXT_framebuffer_object) {
		return fbo_ext_create(desc);
	} else if (!gl_extensions.ARB_framebuffer_object) {
		return nullptr;
	}
	// If GLES2, we have basic FBO support and can just proceed.
#endif

	OpenGLFramebuffer *fbo = new OpenGLFramebuffer();
	fbo->width = desc.width;
	fbo->height = desc.height;
	fbo->colorDepth = desc.colorDepth;

	// Color texture is same everywhere
	glGenFramebuffers(1, &fbo->handle);
	glGenTextures(1, &fbo->color_texture);

	// Create the surfaces.
	glBindTexture(GL_TEXTURE_2D, fbo->color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// TODO: We could opt to only create 16-bit render targets on slow devices. For later.
	switch (fbo->colorDepth) {
	case FBO_8888:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		break;
	case FBO_4444:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
		break;
	case FBO_5551:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		break;
	case FBO_565:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fbo->width, fbo->height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		break;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	if (gl_extensions.IsGLES) {
		if (gl_extensions.OES_packed_depth_stencil) {
			ILOG("Creating %i x %i FBO using DEPTH24_STENCIL8", fbo->width, fbo->height);
			// Standard method
			fbo->stencil_buffer = 0;
			fbo->z_buffer = 0;
			// 24-bit Z, 8-bit stencil combined
			glGenRenderbuffers(1, &fbo->z_stencil_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_stencil_buffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, fbo->width, fbo->height);

			// Bind it all together
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
		} else {
			ILOG("Creating %i x %i FBO using separate stencil", fbo->width, fbo->height);
			// TEGRA
			fbo->z_stencil_buffer = 0;
			// 16/24-bit Z, separate 8-bit stencil
			glGenRenderbuffers(1, &fbo->z_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_buffer);
			// Don't forget to make sure fbo_standard_z_depth() matches.
			glRenderbufferStorage(GL_RENDERBUFFER, gl_extensions.OES_depth24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16, fbo->width, fbo->height);

			// 8-bit stencil buffer
			glGenRenderbuffers(1, &fbo->stencil_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->stencil_buffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, fbo->width, fbo->height);

			// Bind it all together
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_buffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->stencil_buffer);
		}
	} else {
		fbo->stencil_buffer = 0;
		fbo->z_buffer = 0;
		// 24-bit Z, 8-bit stencil
		glGenRenderbuffers(1, &fbo->z_stencil_buffer);
		glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_stencil_buffer);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fbo->width, fbo->height);

		// Bind it all together
		glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
	}

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}
	// Unbind state we don't need
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	currentDrawHandle_ = fbo->handle;
	currentReadHandle_ = fbo->handle;
	return fbo;
}

GLenum OpenGLContext::fbo_get_fb_target(bool read, GLuint **cached) {
	bool supportsBlit = gl_extensions.ARB_framebuffer_object;
	if (gl_extensions.IsGLES) {
		supportsBlit = (gl_extensions.GLES3 || gl_extensions.NV_framebuffer_blit);
	}

	// Note: GL_FRAMEBUFFER_EXT and GL_FRAMEBUFFER have the same value, same with _NV.
	if (supportsBlit) {
		if (read) {
			*cached = &currentReadHandle_;
			return GL_READ_FRAMEBUFFER;
		} else {
			*cached = &currentDrawHandle_;
			return GL_DRAW_FRAMEBUFFER;
		}
	} else {
		*cached = &currentDrawHandle_;
		return GL_FRAMEBUFFER;
	}
}

void OpenGLContext::fbo_bind_fb_target(bool read, GLuint name) {
	GLuint *cached;
	GLenum target = fbo_get_fb_target(read, &cached);
	if (*cached != name) {
		if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
			glBindFramebuffer(target, name);
		} else {
#ifndef USING_GLES2
			glBindFramebufferEXT(target, name);
#endif
		}
		*cached = name;
	}
}

void OpenGLContext::fbo_unbind() {
#ifndef USING_GLES2
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else if (gl_extensions.EXT_framebuffer_object) {
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
	}
#else
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif

#ifdef IOS
	bindDefaultFBO();
#endif

	currentDrawHandle_ = 0;
	currentReadHandle_ = 0;
}

void OpenGLContext::BindFramebufferAsRenderTarget(Framebuffer *fbo) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
	// Without FBO_ARB / GLES3, this will collide with bind_for_read, but there's nothing
	// in ES 2.0 that actually separate them anyway of course, so doesn't matter.
	fbo_bind_fb_target(false, fb->handle);
	// Always restore viewport after render target binding
	glstate.viewport.restore();
}

void OpenGLContext::BindBackbufferAsRenderTarget() {
	fbo_unbind();
}

// For GL_EXT_FRAMEBUFFER_BLIT and similar.
void OpenGLContext::BindFramebufferForRead(Framebuffer *fbo) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
	fbo_bind_fb_target(true, fb->handle);
}

void OpenGLContext::CopyFramebufferImage(Framebuffer *fbsrc, int srcLevel, int srcX, int srcY, int srcZ, Framebuffer *fbdst, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth) {
	OpenGLFramebuffer *src = (OpenGLFramebuffer *)fbsrc;
	OpenGLFramebuffer *dst = (OpenGLFramebuffer *)fbdst;
#if defined(USING_GLES2)
#ifndef IOS
	glCopyImageSubDataOES(
		src->color_texture, GL_TEXTURE_2D, srcLevel, srcX, srcY, srcZ,
		dst->color_texture, GL_TEXTURE_2D, dstLevel, dstX, dstY, dstZ,
		width, height, depth);
	return;
#endif
#else
	if (gl_extensions.ARB_copy_image) {
		glCopyImageSubData(
			src->color_texture, GL_TEXTURE_2D, srcLevel, srcX, srcY, srcZ,
			dst->color_texture, GL_TEXTURE_2D, dstLevel, dstX, dstY, dstZ,
			width, height, depth);
		return;
	} else if (gl_extensions.NV_copy_image) {
		// Older, pre GL 4.x NVIDIA cards.
		glCopyImageSubDataNV(
			src->color_texture, GL_TEXTURE_2D, srcLevel, srcX, srcY, srcZ,
			dst->color_texture, GL_TEXTURE_2D, dstLevel, dstX, dstY, dstZ,
			width, height, depth);
		return;
	}
#endif
}

bool OpenGLContext::BlitFramebuffer(Framebuffer *fbsrc, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *fbdst, int dstX1, int dstY1, int dstX2, int dstY2, int channels, FBBlitFilter linearFilter) {
	OpenGLFramebuffer *src = (OpenGLFramebuffer *)fbsrc;
	OpenGLFramebuffer *dst = (OpenGLFramebuffer *)fbdst;
	GLuint bits = 0;
	if (channels & FB_COLOR_BIT)
		bits |= GL_COLOR_BUFFER_BIT;
	if (channels & FB_DEPTH_BIT)
		bits |= GL_DEPTH_BUFFER_BIT;
	if (channels & FB_STENCIL_BIT)
		bits |= GL_STENCIL_BUFFER_BIT;
	BindFramebufferAsRenderTarget(dst);
	BindFramebufferForRead(src);
	if (gl_extensions.GLES3 || gl_extensions.ARB_framebuffer_object) {
		glBlitFramebuffer(srcX1, srcY1, srcX2, srcY2, dstX1, dstY1, dstX2, dstY2, bits, linearFilter == FB_BLIT_LINEAR ? GL_LINEAR : GL_NEAREST);
#if defined(USING_GLES2) && defined(__ANDROID__)  // We only support this extension on Android, it's not even available on PC.
		return true;
	} else if (gl_extensions.NV_framebuffer_blit) {
		glBlitFramebufferNV(srcX1, srcY1, srcX2, srcY2, dstX1, dstY1, dstX2, dstY2, bits, linearFilter == FB_BLIT_LINEAR ? GL_LINEAR : GL_NEAREST);
#endif // defined(USING_GLES2) && defined(__ANDROID__)
		return true;
	} else {
		return false;
	}
}

uintptr_t OpenGLContext::GetFramebufferAPITexture(Framebuffer *fbo, int channelBits, int attachment) {
	// Unimplemented
	return 0;
}

void OpenGLContext::BindFramebufferAsTexture(Framebuffer *fbo, int binding, FBChannel channelBit, int color) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
	// glActiveTexture(GL_TEXTURE0 + binding);
	switch (channelBit) {
	case FB_COLOR_BIT:
	default:
		if (fbo) {
			glBindTexture(GL_TEXTURE_2D, fb->color_texture);
		}
		break;
	}
}

OpenGLFramebuffer::~OpenGLFramebuffer() {
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		glBindFramebuffer(GL_FRAMEBUFFER, handle);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &handle);
		glDeleteRenderbuffers(1, &z_stencil_buffer);
		glDeleteRenderbuffers(1, &z_buffer);
		glDeleteRenderbuffers(1, &stencil_buffer);
	} else if (gl_extensions.EXT_framebuffer_object) {
#ifndef USING_GLES2
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, handle);
		glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER_EXT, 0);
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
		glDeleteFramebuffersEXT(1, &handle);
		glDeleteRenderbuffersEXT(1, &z_stencil_buffer);
#endif
	}

	glDeleteTextures(1, &color_texture);
}

void OpenGLContext::GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
	*w = fb->width;
	*h = fb->height;
}

uint32_t OpenGLContext::GetDataFormatSupport(DataFormat fmt) const {
	switch (fmt) {
	case DataFormat::B8G8R8A8_UNORM:
		return FMT_RENDERTARGET | FMT_TEXTURE;
	case DataFormat::R4G4B4A4_UNORM_PACK16:
		return FMT_RENDERTARGET | FMT_TEXTURE;
	case DataFormat::B4G4R4A4_UNORM_PACK16:
		return 0;  // native support
	case DataFormat::A4B4G4R4_UNORM_PACK16:
		return 0;  // Can support this if _REV formats are supported.

	case DataFormat::R8G8B8A8_UNORM:
		return FMT_RENDERTARGET | FMT_TEXTURE | FMT_INPUTLAYOUT;

	case DataFormat::R32_FLOAT:
	case DataFormat::R32G32_FLOAT:
	case DataFormat::R32G32B32_FLOAT:
	case DataFormat::R32G32B32A32_FLOAT:
		return FMT_INPUTLAYOUT;

	case DataFormat::R8_UNORM:
		return 0;
	case DataFormat::BC1_RGBA_UNORM_BLOCK:
	case DataFormat::BC2_UNORM_BLOCK:
	case DataFormat::BC3_UNORM_BLOCK:
		return FMT_TEXTURE;
	default:
		return 0;
	}
}

}  // namespace Draw
