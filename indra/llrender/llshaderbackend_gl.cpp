/**
 * @file llshaderbackend_gl.cpp
 * @brief OpenGL implementation of LLShaderBackend
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llshaderbackend.h"
#include "llglheaders.h"
#include "llgl.h"

namespace
{

std::string get_shader_log(GLuint object)
{
    std::string res;

    GLint length = 0;
    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    if (length > 0)
    {
        // the log could be any size, so allocate appropriately
        std::vector<GLchar> log(length);
        glGetShaderInfoLog(object, length, &length, log.data());
        res = std::string(log.data());
    }
    // <FS:LO> Fix intel GLSL compiler spitting out "No errors." instead of an empty string like others do when there are no errors, causing log spam.
    if (res == "No errors.\n")
    {
        res.clear();
    }
    // </FS:LO>
    return res;
}

class LLGLShaderBackend final : public LLShaderBackend
{
public:
    U32 createObject(U32 stage) override
    {
        GLuint object = glCreateShader(stage);

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            LL_WARNS("ShaderLoading") << "GL ERROR in glCreateShader: " << error << LL_ENDL;
            if (object)
            {
                glDeleteShader(object); // no longer need handle
                object = 0;
            }
        }
        return object;
    }

    bool compileObject(U32 object, U32 count, const char* const* text, std::string& log) override
    {
        glShaderSource(object, count, (const GLchar**)text, NULL);

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            log = "GL ERROR in glShaderSource: " + std::to_string(error);
            return false;
        }

        glCompileShader(object);

        error = glGetError();
        if (error != GL_NO_ERROR)
        {
            log = "GL ERROR in glCompileShader: " + std::to_string(error);
            return false;
        }

        GLint success = GL_TRUE;
        glGetShaderiv(object, GL_COMPILE_STATUS, &success);

        error = glGetError();
        if (error != GL_NO_ERROR || success == GL_FALSE)
        {
            log = get_shader_log(object);
            return false;
        }
        return true;
    }

    void deleteObject(U32 object) override
    {
        glDeleteShader(object);
    }

    U32 createProgram() override
    {
        return glCreateProgram();
    }

    void attachObject(U32 program, U32 object) override
    {
        glAttachShader(program, object);
    }

    void deleteProgramAndObjects(U32 program) override
    {
        GLuint obj[1024];
        GLsizei count = 0;
        glGetAttachedShaders(program, 1024, &count, obj);

        for (GLsizei i = 0; i < count; i++)
        {
            glDetachShader(program, obj[i]);
        }

        for (GLsizei i = 0; i < count; i++)
        {
            if (glIsShader(obj[i]))
            {
                glDeleteShader(obj[i]);
            }
        }

        glDeleteProgram(program);
    }

    void bindAttribLocation(U32 program, U32 index, const char* name) override
    {
        glBindAttribLocation(program, index, (const GLchar*)name);
    }

    bool link(U32 program) override
    {
        glLinkProgram(program);

        GLint success = GL_TRUE;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        return success != GL_FALSE;
    }

    bool validate(U32 program) override
    {
        glValidateProgram(program);
        GLint success = GL_TRUE;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        return success != GL_FALSE;
    }

    std::string getObjectLog(U32 object) override
    {
        if (glIsProgram(object))
        {
            return getProgramLog(object);
        }
        llassert(glIsShader(object));
        return get_shader_log(object);
    }

    std::string getProgramLog(U32 program) override
    {
        std::string res;

        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        if (length > 0)
        {
            // the log could be any size, so allocate appropriately
            std::vector<GLchar> log(length);
            glGetProgramInfoLog(program, length, &length, log.data());
            res = std::string(log.data());
        }
        return res;
    }

    void use(U32 program) override
    {
        glUseProgram(program);
    }

    S32 getAttribLocation(U32 program, const char* name) override
    {
        return glGetAttribLocation(program, (const GLchar*)name);
    }

    S32 getUniformLocation(U32 program, const char* name) override
    {
        return glGetUniformLocation(program, name);
    }

    S32 getActiveUniformCount(U32 program) override
    {
        GLint count = 0;
        glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
        return count;
    }

    void getActiveUniform(U32 program, S32 index, std::string& name, U32& type, S32& size) override
    {
        char buf[1024]; /* Flawfinder: ignore */
        buf[0] = 0;
        GLsizei length = 0;
        GLint gl_size = -1;
        GLenum gl_type = 0;
        glGetActiveUniform(program, index, sizeof(buf), &length, &gl_size, &gl_type, (GLchar*)buf);
        name = buf;
        type = gl_type;
        size = gl_size;
    }

    void bindUniformBlock(U32 program, const char* name, U32 binding) override
    {
        GLuint block_index = glGetUniformBlockIndex(program, name);
        if (block_index != GL_INVALID_INDEX)
        {
            glUniformBlockBinding(program, block_index, binding);
        }
    }

    bool supportsProgramBinary() const override
    {
        return true;
    }

    void setProgramBinaryRetrievable(U32 program) override
    {
        glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }

    bool loadProgramBinary(U32 program, U32 format, const void* data, S32 length) override
    {
        GLenum error = glGetError(); // Clear current error
        glProgramBinary(program, format, data, length);

        error = glGetError();
        GLint success = GL_TRUE;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        return error == GL_NO_ERROR && success == GL_TRUE;
    }

    S32 getProgramBinaryLength(U32 program) override
    {
        GLint length = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        return length;
    }

    bool getProgramBinary(U32 program, std::vector<U8>& binary, U32& format) override
    {
        GLenum error = glGetError(); // Clear current error
        GLenum gl_format = 0;
        glGetProgramBinary(program, static_cast<GLsizei>(binary.size() * sizeof(U8)), nullptr, &gl_format, binary.data());
        format = gl_format;
        error = glGetError();
        return error == GL_NO_ERROR;
    }

    void uniform1i(U32, S32 location, S32 v) override { glUniform1i(location, v); }
    void uniform2i(U32, S32 location, S32 x, S32 y) override { glUniform2i(location, x, y); }
    void uniform1f(U32, S32 location, F32 v) override { glUniform1f(location, v); }
    void uniform2f(U32, S32 location, F32 x, F32 y) override { glUniform2f(location, x, y); }
    void uniform3f(U32, S32 location, F32 x, F32 y, F32 z) override { glUniform3f(location, x, y, z); }
    void uniform4f(U32, S32 location, F32 x, F32 y, F32 z, F32 w) override { glUniform4f(location, x, y, z, w); }
    void uniform1iv(U32, S32 location, S32 count, const S32* v) override { glUniform1iv(location, count, v); }
    void uniform4iv(U32, S32 location, S32 count, const S32* v) override { glUniform4iv(location, count, v); }
    void uniform1fv(U32, S32 location, S32 count, const F32* v) override { glUniform1fv(location, count, v); }
    void uniform2fv(U32, S32 location, S32 count, const F32* v) override { glUniform2fv(location, count, v); }
    void uniform3fv(U32, S32 location, S32 count, const F32* v) override { glUniform3fv(location, count, v); }
    void uniform4fv(U32, S32 location, S32 count, const F32* v) override { glUniform4fv(location, count, v); }
    void uniform4uiv(U32, S32 location, S32 count, const U32* v) override { glUniform4uiv(location, count, v); }
    void uniformMatrix2fv(U32, S32 location, S32 count, bool transpose, const F32* v) override
    {
        glUniformMatrix2fv(location, count, transpose ? GL_TRUE : GL_FALSE, v);
    }
    void uniformMatrix3fv(U32, S32 location, S32 count, bool transpose, const F32* v) override
    {
        glUniformMatrix3fv(location, count, transpose ? GL_TRUE : GL_FALSE, v);
    }
    void uniformMatrix3x4fv(U32, S32 location, S32 count, bool transpose, const F32* v) override
    {
        glUniformMatrix3x4fv(location, count, transpose ? GL_TRUE : GL_FALSE, v);
    }
    void uniformMatrix4fv(U32, S32 location, S32 count, bool transpose, const F32* v) override
    {
        glUniformMatrix4fv(location, count, transpose ? GL_TRUE : GL_FALSE, v);
    }
};

} // namespace

LLShaderBackend* createGLShaderBackend()
{
    return new LLGLShaderBackend();
}

LLShaderBackend& getShaderBackend()
{
    static LLShaderBackend* sBackend = createGLShaderBackend();
    return *sBackend;
}
