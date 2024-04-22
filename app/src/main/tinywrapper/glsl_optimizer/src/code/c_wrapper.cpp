//
// Created by serpentspirale on 17/06/23.
//

#include <cstring>
#include "c_wrapper.h"
#include "GlslConvert.h"

#ifdef __cplusplus
extern "C" {
#endif

GlslConvert::OptimizationStruct optimizationStruct {}; // Default struct with everything enabled

char *optimize_shader(char *source, int isVertex, int vGLSLVersion, int vTargetGLSLVersion) {
    GlslConvert& converter = GlslConvert::Instance();

    char * optimized_shader = converter.Optimize(
            source,
            isVertex ? GlslConvert::MESA_SHADER_VERTEX : GlslConvert::MESA_SHADER_FRAGMENT,
            GlslConvert::API_OPENGL_COMPAT,
            GlslConvert::LANGUAGE_TARGET_GLSL,
            vGLSLVersion,
            vTargetGLSLVersion,
            true,
            optimizationStruct
            );
    if(converter.Failed()) {
        printf("Shader conversion failed!\n%s\n", converter.GetLog().c_str());
        return nullptr;
    }
    return optimized_shader;
}

#ifdef __cplusplus
}
#endif