//
// Created by maks on 05.03.2024.
//
#include <GLES3/gl31.h>
#include "proc.h"
#include "egl.h"
#include "main.h"

const GLchar*const basevertex_offset_shader =
        "#version 310 es\n"
        "layout(std430) buffer;\n"
        "layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;\n"
        "layout(binding = 0) readonly buffer sourceBuffer {\n"
        "    uint src_elements[];\n"
        "};\n"
        "layout(binding = 1) writeonly buffer destinationBuffer {\n"
        "    uint dst_elements[];\n"
        "};\n"
        "layout(binding = 2) readonly buffer dataBuffer {\n"
        "    uint baseVertex;\n"
        "    uint elementCount;\n"
        "    uint inputBitWidth;\n"
        "};\n"
        "\n"
        "void main()\n"
        "{\n"
        "    uint ident = gl_GlobalInvocationID.x;\n"
        "    uint inputByteWidth = (uint(32) / inputBitWidth);\n"
        "    uint ident_src = ident / inputByteWidth;\n"
        "    dst_elements[ident] = bitfieldExtract(src_elements[ident_src], int((ident % inputByteWidth) * inputBitWidth), int(inputBitWidth)) + baseVertex;\n"
        "}\n";



typedef struct {
    GLuint baseVertex;
    GLuint elementCount;
    GLuint inputBitWidth;
} data_buffer_t;

void basevertex_init(context_t* context) {
    basevertex_renderer_t *renderer = &context->basevertex;
    while(es3_functions.glGetError() != 0) {}
    GLuint offset_shader = es3_functions.glCreateShader(GL_COMPUTE_SHADER);
    es3_functions.glShaderSource(offset_shader, 1, &basevertex_offset_shader, NULL);
    es3_functions.glCompileShader(offset_shader);
    GLint comp_status;
    es3_functions.glGetShaderiv(offset_shader, GL_COMPILE_STATUS, &comp_status);
    if(comp_status != GL_TRUE) {
        GLchar infolog[2049];
        es3_functions.glGetShaderInfoLog(offset_shader, 2048, NULL, infolog);
        printf("tinywrapper: Failed to compile compute shader: %s\n", infolog);
        es3_functions.glDeleteShader(offset_shader);
        return;
    }
    renderer->computeProgram = es3_functions.glCreateProgram();
    es3_functions.glAttachShader(renderer->computeProgram, offset_shader);
    es3_functions.glLinkProgram(renderer->computeProgram);
    es3_functions.glDeleteShader(offset_shader);
    es3_functions.glGetProgramiv(renderer->computeProgram, GL_LINK_STATUS, &comp_status);
    if(comp_status != GL_TRUE) {
        GLchar infolog[2049];
        es3_functions.glGetProgramInfoLog(renderer->computeProgram, 2048, NULL, infolog);
        printf("tinywrapper: Failed to link compute program: %s\n", infolog);
        es3_functions.glDeleteProgram(offset_shader);
        return;
    }
    GLenum error = es3_functions.glGetError();
    if(error != GL_NO_ERROR) {
        printf("tinywrapper: Failed to initialize compute shader: %x\n", error);
        return;
    }
    while(es3_functions.glGetError() != 0) {}
    es3_functions.glGenBuffers(1, &renderer->computeIndexBuffer);
    es3_functions.glGenBuffers(1, &renderer->computeMetaBuffer);
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer->computeMetaBuffer);
    es3_functions.glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(data_buffer_t), NULL, GL_DYNAMIC_DRAW);
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    error = es3_functions.glGetError();
    if(error != GL_NO_ERROR) {
        printf("tinywrapper: Failed to initialize compute buffers: %x\n", error);
        return;
    }
    renderer->ready = true;
}

GLint type_bits(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return 8;
        case GL_UNSIGNED_SHORT: return 16;
        case GL_UNSIGNED_INT: return 32;
        default: return -1;
    }
}

GLint type_bytes(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_UNSIGNED_INT: return 4;
        default: return -1;
    }
}


static void restore_state(GLuint element_buffer) {
    unordered_map* ssbos = current_context->bound_basebuffers[get_base_buffer_index(GL_SHADER_STORAGE_BUFFER)];
    if(unordered_map_size(ssbos) != 0) {
        unordered_map_iterator iterator;
        unordered_map_iterator_alloc_local(ssbos, &iterator);
        void *k;
        basebuffer_binding_t *v;
        while(unordered_map_iterator_next(&iterator, &k, (void*)&v)) {
            if(v->ranged) es3_functions.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, (GLuint)k, v->buffer, v->offset, v->size);
            else es3_functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)k, v->buffer);
        }
    }
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, current_context->bound_buffers[get_buffer_index(GL_SHADER_STORAGE_BUFFER)]);
}

void glDrawElementsBaseVertex(GLenum mode,
                              GLsizei count,
                              GLenum type,
                              void *indices,
                              GLint basevertex) {
    if(!current_context) return;
    basevertex_renderer_t *renderer = &current_context->basevertex;
    if(!renderer->ready) return;
    GLint elementbuffer;
    es3_functions.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementbuffer);
    if(elementbuffer == 0) {
        // I am not bothered enough to implement this.
        printf("tinywrapper: Base vertex draws without element buffer are not supported\n");
        return;
    }

    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer->computeIndexBuffer);
    es3_functions.glBufferData(GL_SHADER_STORAGE_BUFFER, count * (GLint)sizeof(GLuint), NULL, GL_DYNAMIC_DRAW);
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer->computeMetaBuffer);
    data_buffer_t buffer_info;
    buffer_info.baseVertex = basevertex;
    buffer_info.elementCount = count;
    buffer_info.inputBitWidth = type_bits(type);
    es3_functions.glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(data_buffer_t), &buffer_info);
    es3_functions.glUseProgram(renderer->computeProgram);

    uintptr_t offset = (uintptr_t)indices;

    es3_functions.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, elementbuffer, (GLintptr)offset, count *
            type_bytes(type));
    es3_functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, renderer->computeIndexBuffer);
    es3_functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, renderer->computeMetaBuffer);
    es3_functions.glDispatchCompute((count + (256-1))/256, 1, 1);

    es3_functions.glUseProgram(current_context->program);
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->computeIndexBuffer);
    es3_functions.glDrawElements(mode, count, GL_UNSIGNED_INT, NULL);

    restore_state(elementbuffer);
}


/*void glMultiDrawElementsBaseVertex(GLenum mode,
 const GLsizei *count,
                                   GLenum type,
                                   const void * const *indices,
                                   GLsizei drawcount,
                                   const GLint *basevertex) {
    for(GLsizei i = 0; i < drawcount; i++) {
        glDrawElementsBaseVertex(mode, count[i], type, (void*)indices[i], basevertex[i]);
    }
}*/

void glMultiDrawElementsBaseVertex( 	GLenum mode,
                                       const GLsizei *count,
                                       GLenum type,
                                       const void * const *indices,
                                       GLsizei drawcount,
                                       const GLint *basevertex) {
    if(!current_context) return;
    basevertex_renderer_t *renderer = &current_context->basevertex;
    if(!renderer->ready) return;
    GLint elementbuffer;
    es3_functions.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementbuffer);
    if(elementbuffer == 0) {
        // I am not bothered enough to implement this.
        printf("tinywrapper: Base vertex draws without element buffer are not supported\n");
        return;
    }
    GLsizei total_count = 0;
    for (int i = 0; i < drawcount; i++) total_count += count[i];
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer->computeIndexBuffer);
    es3_functions.glBufferData(GL_SHADER_STORAGE_BUFFER, total_count * 4, NULL, GL_DYNAMIC_DRAW);
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer->computeMetaBuffer);
    es3_functions.glUseProgram(renderer->computeProgram);
    data_buffer_t buffer_info;
    buffer_info.inputBitWidth = type_bits(type);
    GLint current_type_bytes = type_bytes(type);
    GLsizei inserted_count = 0;
    es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    for (int i = 0; i < drawcount; i++) {
        GLsizei local_count = count[i];
        uintptr_t local_indices = (uintptr_t)indices[i];
        GLint local_basevertex = basevertex[i];
        es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, renderer->computeMetaBuffer);
        buffer_info.elementCount = local_count;
        buffer_info.baseVertex = local_basevertex;
        es3_functions.glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(data_buffer_t), &buffer_info);
        es3_functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        es3_functions.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, elementbuffer, (GLintptr)local_indices, local_count * current_type_bytes);
        es3_functions.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, renderer->computeIndexBuffer, inserted_count * 4, local_count * 4);
        es3_functions.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, renderer->computeMetaBuffer, 0, sizeof(data_buffer_t));
        GLint temp;
        es3_functions.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &temp);
        es3_functions.glDispatchCompute((local_count + (256-1))/256, 1, 1);

        inserted_count += local_count;
    }
    es3_functions.glFlush();
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->computeIndexBuffer);
    es3_functions.glUseProgram(current_context->program);
    es3_functions.glDrawElements(mode, inserted_count, GL_UNSIGNED_INT, NULL);
    restore_state(elementbuffer);
}