#include "precompiled.h"
//
// Copyright (c) 2002-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Shader.cpp: Implements the gl::Shader class and its  derived classes
// VertexShader and FragmentShader. Implements GL shader objects and related
// functionality. [OpenGL ES 2.0.24] section 2.10 page 24 and section 3.8 page 84.

#include "libGLESv2/Shader.h"

#include "GLSLANG/ShaderLang.h"
#include "common/utilities.h"
#include "libGLESv2/renderer/Renderer.h"
#include "libGLESv2/Constants.h"
#include "libGLESv2/ResourceManager.h"

namespace gl
{
void *Shader::mFragmentCompiler = NULL;
void *Shader::mVertexCompiler = NULL;

template <typename VarT>
const std::vector<VarT> *GetShaderVariables(const std::vector<VarT> *variableList)
{
    // TODO: handle staticUse. for now, assume all returned variables are active.
    ASSERT(variableList);
    return variableList;
}

Shader::Shader(ResourceManager *manager, const rx::Renderer *renderer, GLuint handle)
    : mHandle(handle), mRenderer(renderer), mResourceManager(manager)
{
    uncompile();
    initializeCompiler();

    mRefCount = 0;
    mDeleteStatus = false;
    mShaderVersion = 100;
}

Shader::~Shader()
{
}

GLuint Shader::getHandle() const
{
    return mHandle;
}

void Shader::setSource(GLsizei count, const char *const *string, const GLint *length)
{
    std::ostringstream stream;

    for (int i = 0; i < count; i++)
    {
        stream << string[i];
    }

    mSource = stream.str();
}

int Shader::getInfoLogLength() const
{
    return mInfoLog.empty() ? 0 : (mInfoLog.length() + 1);
}

void Shader::getInfoLog(GLsizei bufSize, GLsizei *length, char *infoLog) const
{
    int index = 0;

    if (bufSize > 0)
    {
        index = std::min(bufSize - 1, static_cast<GLsizei>(mInfoLog.length()));
        memcpy(infoLog, mInfoLog.c_str(), index);

        infoLog[index] = '\0';
    }

    if (length)
    {
        *length = index;
    }
}

int Shader::getSourceLength() const
{
    return mSource.empty() ? 0 : (mSource.length() + 1);
}

int Shader::getTranslatedSourceLength() const
{
    return mHlsl.empty() ? 0 : (mHlsl.length() + 1);
}

void Shader::getSourceImpl(const std::string &source, GLsizei bufSize, GLsizei *length, char *buffer) const
{
    int index = 0;

    if (bufSize > 0)
    {
        index = std::min(bufSize - 1, static_cast<GLsizei>(source.length()));
        memcpy(buffer, source.c_str(), index);

        buffer[index] = '\0';
    }

    if (length)
    {
        *length = index;
    }
}

void Shader::getSource(GLsizei bufSize, GLsizei *length, char *buffer) const
{
    getSourceImpl(mSource, bufSize, length, buffer);
}

void Shader::getTranslatedSource(GLsizei bufSize, GLsizei *length, char *buffer) const
{
    getSourceImpl(mHlsl, bufSize, length, buffer);
}

unsigned int Shader::getUniformRegister(const std::string &uniformName) const
{
    ASSERT(mUniformRegisterMap.count(uniformName) > 0);
    return mUniformRegisterMap.find(uniformName)->second;
}

unsigned int Shader::getInterfaceBlockRegister(const std::string &blockName) const
{
    ASSERT(mInterfaceBlockRegisterMap.count(blockName) > 0);
    return mInterfaceBlockRegisterMap.find(blockName)->second;
}

const std::vector<sh::Uniform> &Shader::getUniforms() const
{
    return mActiveUniforms;
}

const std::vector<sh::InterfaceBlock> &Shader::getInterfaceBlocks() const
{
    return mActiveInterfaceBlocks;
}

std::vector<PackedVarying> &Shader::getVaryings()
{
    return mVaryings;
}

bool Shader::isCompiled() const
{
    return !mHlsl.empty();
}

const std::string &Shader::getHLSL() const
{
    return mHlsl;
}

void Shader::addRef()
{
    mRefCount++;
}

void Shader::release()
{
    mRefCount--;

    if (mRefCount == 0 && mDeleteStatus)
    {
        mResourceManager->deleteShader(mHandle);
    }
}

unsigned int Shader::getRefCount() const
{
    return mRefCount;
}

bool Shader::isFlaggedForDeletion() const
{
    return mDeleteStatus;
}

void Shader::flagForDeletion()
{
    mDeleteStatus = true;
}

// Perform a one-time initialization of the shader compiler (or after being destructed by releaseCompiler)
void Shader::initializeCompiler()
{
    if (!mFragmentCompiler)
    {
        int result = ShInitialize();

        if (result)
        {
#if defined(ANGLE_PLATFORM_WINRT)
            ShShaderOutput hlslVersion = SH_HLSL11_OUTPUT;
#else
            ShShaderOutput hlslVersion = (mRenderer->getMajorShaderModel() >= 4) ? SH_HLSL11_OUTPUT : SH_HLSL9_OUTPUT;
#endif

            ShBuiltInResources resources;
            ShInitBuiltInResources(&resources);

            // TODO(geofflang): use context's caps
            const gl::Caps &caps = mRenderer->getRendererCaps();
            const gl::Extensions &extensions = mRenderer->getRendererExtensions();

            resources.MaxVertexAttribs = MAX_VERTEX_ATTRIBS;
            resources.MaxVertexUniformVectors = mRenderer->getMaxVertexUniformVectors();
            resources.MaxVaryingVectors = mRenderer->getMaxVaryingVectors();
            resources.MaxVertexTextureImageUnits = mRenderer->getMaxVertexTextureImageUnits();
            resources.MaxCombinedTextureImageUnits = mRenderer->getMaxCombinedTextureImageUnits();
            resources.MaxTextureImageUnits = MAX_TEXTURE_IMAGE_UNITS;
            resources.MaxFragmentUniformVectors = mRenderer->getMaxFragmentUniformVectors();
            resources.MaxDrawBuffers = caps.maxDrawBuffers;
            resources.OES_standard_derivatives = extensions.standardDerivatives;
            resources.EXT_draw_buffers = extensions.drawBuffers;
            resources.EXT_shader_texture_lod = 1;
            // resources.OES_EGL_image_external = mRenderer->getShareHandleSupport() ? 1 : 0; // TODO: commented out until the extension is actually supported.
            resources.FragmentPrecisionHigh = 1;   // Shader Model 2+ always supports FP24 (s16e7) which corresponds to highp
            resources.EXT_frag_depth = 1; // Shader Model 2+ always supports explicit depth output
            // GLSL ES 3.0 constants
            resources.MaxVertexOutputVectors = mRenderer->getMaxVaryingVectors();
            resources.MaxFragmentInputVectors = mRenderer->getMaxVaryingVectors();
            resources.MinProgramTexelOffset = -8;   // D3D10_COMMONSHADER_TEXEL_OFFSET_MAX_NEGATIVE
            resources.MaxProgramTexelOffset = 7;    // D3D10_COMMONSHADER_TEXEL_OFFSET_MAX_POSITIVE

            mFragmentCompiler = ShConstructCompiler(GL_FRAGMENT_SHADER, SH_GLES2_SPEC, hlslVersion, &resources);
            mVertexCompiler = ShConstructCompiler(GL_VERTEX_SHADER, SH_GLES2_SPEC, hlslVersion, &resources);
        }
    }
}

void Shader::releaseCompiler()
{
    ShDestruct(mFragmentCompiler);
    ShDestruct(mVertexCompiler);

    mFragmentCompiler = NULL;
    mVertexCompiler = NULL;

    ShFinalize();
}

void Shader::parseVaryings(void *compiler)
{
    if (!mHlsl.empty())
    {
        const std::vector<sh::Varying> *activeVaryings = ShGetVaryings(compiler);
        ASSERT(activeVaryings);

        for (size_t varyingIndex = 0; varyingIndex < activeVaryings->size(); varyingIndex++)
        {
            mVaryings.push_back(PackedVarying((*activeVaryings)[varyingIndex]));
        }

        mUsesMultipleRenderTargets = mHlsl.find("GL_USES_MRT")          != std::string::npos;
        mUsesFragColor             = mHlsl.find("GL_USES_FRAG_COLOR")   != std::string::npos;
        mUsesFragData              = mHlsl.find("GL_USES_FRAG_DATA")    != std::string::npos;
        mUsesFragCoord             = mHlsl.find("GL_USES_FRAG_COORD")   != std::string::npos;
        mUsesFrontFacing           = mHlsl.find("GL_USES_FRONT_FACING") != std::string::npos;
        mUsesPointSize             = mHlsl.find("GL_USES_POINT_SIZE")   != std::string::npos;
        mUsesPointCoord            = mHlsl.find("GL_USES_POINT_COORD")  != std::string::npos;
        mUsesDepthRange            = mHlsl.find("GL_USES_DEPTH_RANGE")  != std::string::npos;
        mUsesFragDepth             = mHlsl.find("GL_USES_FRAG_DEPTH")   != std::string::npos;
        mUsesDiscardRewriting      = mHlsl.find("ANGLE_USES_DISCARD_REWRITING") != std::string::npos;
        mUsesNestedBreak           = mHlsl.find("ANGLE_USES_NESTED_BREAK") != std::string::npos;
    }
}

void Shader::resetVaryingsRegisterAssignment()
{
    for (unsigned int varyingIndex = 0; varyingIndex < mVaryings.size(); varyingIndex++)
    {
        mVaryings[varyingIndex].resetRegisterAssignment();
    }
}

// initialize/clean up previous state
void Shader::uncompile()
{
    // set by compileToHLSL
    mHlsl.clear();
    mInfoLog.clear();

    // set by parseVaryings
    mVaryings.clear();

    mUsesMultipleRenderTargets = false;
    mUsesFragColor = false;
    mUsesFragData = false;
    mUsesFragCoord = false;
    mUsesFrontFacing = false;
    mUsesPointSize = false;
    mUsesPointCoord = false;
    mUsesDepthRange = false;
    mUsesFragDepth = false;
    mShaderVersion = 100;
    mUsesDiscardRewriting = false;
    mUsesNestedBreak = false;

    mActiveUniforms.clear();
    mActiveInterfaceBlocks.clear();
}

void Shader::compileToHLSL(void *compiler)
{
    // ensure the compiler is loaded
    initializeCompiler();

    int compileOptions = SH_OBJECT_CODE;
    std::string sourcePath;
    if (perfActive())
    {
        sourcePath = getTempPath();
        writeFile(sourcePath.c_str(), mSource.c_str(), mSource.length());
        compileOptions |= SH_LINE_DIRECTIVES;
    }

    int result;
    if (sourcePath.empty())
    {
        const char* sourceStrings[] =
        {
            mSource.c_str(),
        };

        result = ShCompile(compiler, sourceStrings, ArraySize(sourceStrings), compileOptions);
    }
    else
    {
        const char* sourceStrings[] =
        {
            sourcePath.c_str(),
            mSource.c_str(),
        };

        result = ShCompile(compiler, sourceStrings, ArraySize(sourceStrings), compileOptions | SH_SOURCE_PATH);
    }

    size_t shaderVersion = 100;
    ShGetInfo(compiler, SH_SHADER_VERSION, &shaderVersion);

    mShaderVersion = static_cast<int>(shaderVersion);

    if (shaderVersion == 300 && mRenderer->getCurrentClientVersion() < 3)
    {
        mInfoLog = "GLSL ES 3.00 is not supported by OpenGL ES 2.0 contexts";
        TRACE("\n%s", mInfoLog.c_str());
    }
    else if (result)
    {
        size_t objCodeLen = 0;
        ShGetInfo(compiler, SH_OBJECT_CODE_LENGTH, &objCodeLen);

        char* outputHLSL = new char[objCodeLen];
        ShGetObjectCode(compiler, outputHLSL);

#ifdef _DEBUG
        std::ostringstream hlslStream;
        hlslStream << "// GLSL\n";
        hlslStream << "//\n";

        size_t curPos = 0;
        while (curPos != std::string::npos)
        {
            size_t nextLine = mSource.find("\n", curPos);
            size_t len = (nextLine == std::string::npos) ? std::string::npos : (nextLine - curPos + 1);

            hlslStream << "// " << mSource.substr(curPos, len);

            curPos = (nextLine == std::string::npos) ? std::string::npos : (nextLine + 1);
        }
        hlslStream << "\n\n";
        hlslStream << outputHLSL;
        mHlsl = hlslStream.str();
#else
        mHlsl = outputHLSL;
#endif

        SafeDeleteArray(outputHLSL);

        mActiveUniforms = *GetShaderVariables(ShGetUniforms(compiler));

        for (size_t uniformIndex = 0; uniformIndex < mActiveUniforms.size(); uniformIndex++)
        {
            const sh::Uniform &uniform = mActiveUniforms[uniformIndex];

            unsigned int index = -1;
            bool result = ShGetUniformRegister(compiler, uniform.name.c_str(), &index);
            UNUSED_ASSERTION_VARIABLE(result);
            ASSERT(result);

            mUniformRegisterMap[uniform.name] = index;
        }

        mActiveInterfaceBlocks = *GetShaderVariables(ShGetInterfaceBlocks(compiler));

        for (size_t blockIndex = 0; blockIndex < mActiveInterfaceBlocks.size(); blockIndex++)
        {
            const sh::InterfaceBlock &interfaceBlock = mActiveInterfaceBlocks[blockIndex];

            unsigned int index = -1;
            bool result = ShGetInterfaceBlockRegister(compiler, interfaceBlock.name.c_str(), &index);
            UNUSED_ASSERTION_VARIABLE(result);
            ASSERT(result);

            mInterfaceBlockRegisterMap[interfaceBlock.name] = index;
        }
    }
    else
    {
        size_t infoLogLen = 0;
        ShGetInfo(compiler, SH_INFO_LOG_LENGTH, &infoLogLen);

        char* infoLog = new char[infoLogLen];
        ShGetInfoLog(compiler, infoLog);
        mInfoLog = infoLog;

        TRACE("\n%s", mInfoLog.c_str());
    }
}

rx::D3DWorkaroundType Shader::getD3DWorkarounds() const
{
    if (mUsesDiscardRewriting)
    {
        // ANGLE issue 486:
        // Work-around a D3D9 compiler bug that presents itself when using conditional discard, by disabling optimization
        return rx::ANGLE_D3D_WORKAROUND_SKIP_OPTIMIZATION;
    }

    if (mUsesNestedBreak)
    {
        // ANGLE issue 603:
        // Work-around a D3D9 compiler bug that presents itself when using break in a nested loop, by maximizing optimization
        // We want to keep the use of ANGLE_D3D_WORKAROUND_MAX_OPTIMIZATION minimal to prevent hangs, so usesDiscard takes precedence
        return rx::ANGLE_D3D_WORKAROUND_MAX_OPTIMIZATION;
    }

    return rx::ANGLE_D3D_WORKAROUND_NONE;
}

// true if varying x has a higher priority in packing than y
bool Shader::compareVarying(const PackedVarying &x, const PackedVarying &y)
{
    if (x.type == y.type)
    {
        return x.arraySize > y.arraySize;
    }

    // Special case for handling structs: we sort these to the end of the list
    if (x.type == GL_STRUCT_ANGLEX)
    {
        return false;
    }

    if (y.type == GL_STRUCT_ANGLEX)
    {
        return true;
    }

    return gl::VariableSortOrder(x.type) <= gl::VariableSortOrder(y.type);
}

int Shader::getShaderVersion() const
{
    return mShaderVersion;
}

VertexShader::VertexShader(ResourceManager *manager, const rx::Renderer *renderer, GLuint handle)
    : Shader(manager, renderer, handle)
{
}

VertexShader::~VertexShader()
{
}

GLenum VertexShader::getType() const
{
    return GL_VERTEX_SHADER;
}

void VertexShader::uncompile()
{
    Shader::uncompile();

    // set by ParseAttributes
    mActiveAttributes.clear();
}

void VertexShader::compile()
{
    uncompile();

    compileToHLSL(mVertexCompiler);
    parseAttributes();
    parseVaryings(mVertexCompiler);
}

int VertexShader::getSemanticIndex(const std::string &attributeName)
{
    if (!attributeName.empty())
    {
        int semanticIndex = 0;
        for (unsigned int attributeIndex = 0; attributeIndex < mActiveAttributes.size(); attributeIndex++)
        {
            const sh::ShaderVariable &attribute = mActiveAttributes[attributeIndex];

            if (attribute.name == attributeName)
            {
                return semanticIndex;
            }

            semanticIndex += VariableRegisterCount(attribute.type);
        }
    }

    return -1;
}

void VertexShader::parseAttributes()
{
    const std::string &hlsl = getHLSL();
    if (!hlsl.empty())
    {
        mActiveAttributes = *GetShaderVariables(ShGetAttributes(mVertexCompiler));
    }
}

FragmentShader::FragmentShader(ResourceManager *manager, const rx::Renderer *renderer, GLuint handle)
    : Shader(manager, renderer, handle)
{
}

FragmentShader::~FragmentShader()
{
}

GLenum FragmentShader::getType() const
{
    return GL_FRAGMENT_SHADER;
}

void FragmentShader::compile()
{
    uncompile();

    compileToHLSL(mFragmentCompiler);
    parseVaryings(mFragmentCompiler);
    std::sort(mVaryings.begin(), mVaryings.end(), compareVarying);

    const std::string &hlsl = getHLSL();
    if (!hlsl.empty())
    {
        mActiveOutputVariables = *GetShaderVariables(ShGetOutputVariables(mFragmentCompiler));
    }
}

void FragmentShader::uncompile()
{
    Shader::uncompile();

    mActiveOutputVariables.clear();
}

const std::vector<sh::Attribute> &FragmentShader::getOutputVariables() const
{
    return mActiveOutputVariables;
}

ShShaderOutput Shader::getCompilerOutputType(GLenum shader)
{
    void *compiler = NULL;

    switch (shader)
    {
      case GL_VERTEX_SHADER:   compiler = mVertexCompiler;   break;
      case GL_FRAGMENT_SHADER: compiler = mFragmentCompiler; break;
      default: UNREACHABLE();  return SH_HLSL9_OUTPUT;
    }

    size_t outputType = 0;
    ShGetInfo(compiler, SH_OUTPUT_TYPE, &outputType);

    return static_cast<ShShaderOutput>(outputType);
}

}
