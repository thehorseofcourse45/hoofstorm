// Shared viewer startup bandwidth workload; returns -1 when accurate profiling is unavailable.
#pragma once
#include "stdtypes.h"
class LLGLSLShader;
F32 measureGpuBandwidth(LLGLSLShader& shader);
