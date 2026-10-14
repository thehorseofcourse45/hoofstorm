// Writing GL-style uniform values into a CPU copy of a std140 uniform block (the layout from
// LLShaderTranslator::computeBlockLayout). The Vulkan shader backend uses it for glUniform*-style setters; std library only.
//
//   vectors: glUniform{1,2,3,4}{f,i,ui}[v]:  'srcComps' words per element, 'count' elements, tightly packed
//   matrices: glUniformMatrix{2,3,4,3x4}fv:  'cols' x 'rows' floats per element, column-major unless 'transpose'
// Arrays use the member's array stride (16 bytes for scalars and vectors in std140), matrix columns start on 16 byte
// boundaries, and anything that does not fit the member (more elements than the array has, more components than the
// type has) is ignored, like GL ignores the excess.
#pragma once

#include "llshadertranslator.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace std140pack
{

struct Elem
{
    int comps = 0;      // rows of a column / components of a vector
    int cols = 1;
    bool matrix = false;
};

inline Elem elemOf(const std::string& t)
{
    Elem e;
    if (t == "float" || t == "int" || t == "uint" || t == "bool")
    {
        e.comps = 1;
    }
    else if (t.size() == 5 && t.compare(1, 3, "vec") == 0 && (t[0] == 'i' || t[0] == 'u' || t[0] == 'b'))
    {
        e.comps = t[4] - '0';
    }
    else if (t.size() == 4 && t.compare(0, 3, "vec") == 0)
    {
        e.comps = t[3] - '0';
    }
    else if (t.compare(0, 3, "mat") == 0 && (t.size() == 4 || t.size() == 6))
    {
        e.matrix = true;
        e.cols = t[3] - '0';
        e.comps = t.size() == 6 ? t[5] - '0' : e.cols;      // matCxR: C columns of R rows
    }
    return e;
}

inline int arrayCount(const LLShaderTranslator::BlockMember& m)
{
    return m.arrayStride ? m.size / m.arrayStride : 1;
}

inline void writeVectors(uint8_t* block, const LLShaderTranslator::BlockMember& m, const std::string& type, int srcComps, int count,
                         const void* src)
{
    const Elem e = elemOf(type);
    count = std::min(count, arrayCount(m));
    const int comps = std::min(srcComps, e.comps * e.cols);
    for (int i = 0; i < count; ++i)
    {
        memcpy(block + m.offset + (size_t) i * m.arrayStride, (const uint8_t*) src + (size_t) i * srcComps * 4, (size_t) comps * 4);
    }
}

inline void writeMatrices(uint8_t* block, const LLShaderTranslator::BlockMember& m, const std::string& type, int cols, int rows, int count,
                          bool transpose, const float* src)
{
    const Elem e = elemOf(type);
    count = std::min(count, arrayCount(m));
    for (int i = 0; i < count; ++i)
    {
        const float* mat = src + (size_t) i * cols * rows;
        for (int c = 0; c < std::min(cols, e.cols); ++c)
        {
            for (int r = 0; r < std::min(rows, e.comps); ++r)
            {
                const float v = transpose ? mat[r * cols + c] : mat[c * rows + r];
                memcpy(block + m.offset + (size_t) i * m.arrayStride + c * 16 + r * 4, &v, 4);
            }
        }
    }
}

} // namespace std140pack
