/**
 * @file llshaderbackend.h
 * @brief Graphics API backend interface for shader objects, programs and uniforms
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

#ifndef LL_LLSHADERBACKEND_H
#define LL_LLSHADERBACKEND_H

#include "stdtypes.h"
#include <string>
#include <vector>

// Everything LLShaderMgr and LLGLSLShader need from the graphics API to create, link, bind and feed shaders.
// See doc/vulkan/shader-layer-design.md. LLGLSLShader keeps its public API, the CPU-side value cache and the
// reserved attribute/uniform tables; the backend owns the API objects.
//
// Handles are plain U32 (0 = none). Shader stages and uniform types use the GL constants (GL_VERTEX_SHADER,
// GL_FLOAT_VEC4, GL_SAMPLER_2D, ...) as the viewer's vocabulary for now; other backends translate them.
// Uniform locations are whatever the backend returns from getUniformLocation(); -1 means "not found".
//
// Threading: call from the thread that owns the graphics context.
class LLShaderBackend
{
public:
    virtual ~LLShaderBackend() = default;

    // --- shader objects ----------------------------------------------------------------------------
    // 'stage' is GL_VERTEX_SHADER, GL_FRAGMENT_SHADER or GL_GEOMETRY_SHADER. Returns 0 on failure.
    virtual U32 createObject(U32 stage) = 0;
    // Set the source (as 'count' separate strings, concatenated) and compile. False on failure; 'log'
    // then holds the API error or the compiler log.
    virtual bool compileObject(U32 object, U32 count, const char* const* text, std::string& log) = 0;
    virtual void deleteObject(U32 object) = 0;

    // --- programs ----------------------------------------------------------------------------------
    virtual U32 createProgram() = 0;
    virtual void attachObject(U32 program, U32 object) = 0;
    // Detach and delete every shader object attached to 'program', then delete the program.
    virtual void deleteProgramAndObjects(U32 program) = 0;
    // Make reserved attributes keep fixed locations; call before link().
    virtual void bindAttribLocation(U32 program, U32 index, const char* name) = 0;
    // True if linking succeeded. Reasons are available from getProgramLog().
    virtual bool link(U32 program) = 0;
    // Validate against the current state; true if the program is valid.
    virtual bool validate(U32 program) = 0;
    // Info log of a shader object OR a program object.
    virtual std::string getObjectLog(U32 object) = 0;
    virtual std::string getProgramLog(U32 program) = 0;
    // Make 'program' current (0 = none).
    virtual void use(U32 program) = 0;

    // --- reflection ----------------------------------------------------------------------------------
    // -1 if the attribute is not used by the program.
    virtual S32 getAttribLocation(U32 program, const char* name) = 0;
    virtual S32 getUniformLocation(U32 program, const char* name) = 0;
    virtual S32 getActiveUniformCount(U32 program) = 0;
    // Describe active uniform 'index' (0 <= index < getActiveUniformCount()); 'size' is the array size and
    // 'type' the GL type constant. Arrays are reported with a "[0]" suffix on the name, as GL does.
    virtual void getActiveUniform(U32 program, S32 index, std::string& name, U32& type, S32& size) = 0;
    // Attach the uniform block called 'name' (if the program has it) to binding point 'binding'.
    virtual void bindUniformBlock(U32 program, const char* name, U32 binding) = 0;

    // --- program binary cache (optional; supportsProgramBinary() == false disables it) ---------------
    virtual bool supportsProgramBinary() const = 0;
    // Empty preserves the existing GL keys; other APIs include their compiler/translator identity.
    virtual std::string programBinaryCacheSalt() const { return {}; }
    // Ask the API to keep a retrievable binary when linking. Call before link().
    virtual void setProgramBinaryRetrievable(U32 program) = 0;
    // True if the program is linked after loading the binary.
    virtual bool loadProgramBinary(U32 program, U32 format, const void* data, S32 length) = 0;
    // Binary size in bytes after a successful link, 0 if unavailable.
    virtual S32 getProgramBinaryLength(U32 program) = 0;
    // 'binary' must be sized getProgramBinaryLength(); true on success.
    virtual bool getProgramBinary(U32 program, std::vector<U8>& binary, U32& format) = 0;

    // --- uniforms (act on 'program'; 'location' comes from getUniformLocation) -----------------------
    virtual void uniform1i(U32 program, S32 location, S32 v) = 0;
    virtual void uniform2i(U32 program, S32 location, S32 x, S32 y) = 0;
    virtual void uniform1f(U32 program, S32 location, F32 v) = 0;
    virtual void uniform2f(U32 program, S32 location, F32 x, F32 y) = 0;
    virtual void uniform3f(U32 program, S32 location, F32 x, F32 y, F32 z) = 0;
    virtual void uniform4f(U32 program, S32 location, F32 x, F32 y, F32 z, F32 w) = 0;
    virtual void uniform1iv(U32 program, S32 location, S32 count, const S32* v) = 0;
    virtual void uniform4iv(U32 program, S32 location, S32 count, const S32* v) = 0;
    virtual void uniform1fv(U32 program, S32 location, S32 count, const F32* v) = 0;
    virtual void uniform2fv(U32 program, S32 location, S32 count, const F32* v) = 0;
    virtual void uniform3fv(U32 program, S32 location, S32 count, const F32* v) = 0;
    virtual void uniform4fv(U32 program, S32 location, S32 count, const F32* v) = 0;
    virtual void uniform4uiv(U32 program, S32 location, S32 count, const U32* v) = 0;
    virtual void uniformMatrix2fv(U32 program, S32 location, S32 count, bool transpose, const F32* v) = 0;
    virtual void uniformMatrix3fv(U32 program, S32 location, S32 count, bool transpose, const F32* v) = 0;
    virtual void uniformMatrix3x4fv(U32 program, S32 location, S32 count, bool transpose, const F32* v) = 0;
    virtual void uniformMatrix4fv(U32 program, S32 location, S32 count, bool transpose, const F32* v) = 0;
};

// OpenGL implementation, in llshaderbackend_gl.cpp
LLShaderBackend* createGLShaderBackend();

// The active backend. Created on first use as the GL backend.
LLShaderBackend& getShaderBackend();

#endif // LL_LLSHADERBACKEND_H
