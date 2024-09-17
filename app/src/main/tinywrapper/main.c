//#import <Foundation/Foundation.h>
#include <stdio.h>
#include <dlfcn.h>

#include <stdbool.h>
#include "GL/gl.h"
#include <GLES3/gl3.h>
#include "string_utils.h"
#include <stdlib.h>
#include <string.h>
#include "proc.h"
#include "egl.h"
#include "glformats.h"
#include "main.h"

void glClearDepth(GLdouble depth) {
    if(!current_context) return;
    es3_functions.glClearDepthf((GLfloat) depth);
}

void *glMapBuffer(GLenum target, GLenum access) {
    if(!current_context) return NULL;

    GLenum access_range;
    GLint length;

    switch (target) {
        // GL 4.2
        case GL_ATOMIC_COUNTER_BUFFER:
        // GL 4.3
        case GL_DISPATCH_INDIRECT_BUFFER:
        case GL_SHADER_STORAGE_BUFFER:
        // GL 4.4
        case GL_QUERY_BUFFER:
            printf("ERROR: glMapBuffer unsupported target=0x%x", target);
            break; // not supported for now
	    case GL_DRAW_INDIRECT_BUFFER:
        case GL_TEXTURE_BUFFER:
            printf("ERROR: glMapBuffer unimplemented target=0x%x", target);
            break;
    }

    switch (access) {
        case GL_READ_ONLY:
            access_range = GL_MAP_READ_BIT;
            break;

        case GL_WRITE_ONLY:
            access_range = GL_MAP_WRITE_BIT;
            break;

        case GL_READ_WRITE:
            access_range = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
            break;
    }

    es3_functions.glGetBufferParameteriv(target, GL_BUFFER_SIZE, &length);
    return es3_functions.glMapBufferRange(target, 0, length, access_range);
}

int isProxyTexture(GLenum target) {
    switch (target) {
        case GL_PROXY_TEXTURE_1D:
        case GL_PROXY_TEXTURE_2D:
        case GL_PROXY_TEXTURE_3D:
        case GL_PROXY_TEXTURE_RECTANGLE_ARB:
            return 1;
    }
    return 0;
}

static int inline nlevel(int size, int level) {
    if(size) {
        size>>=level;
        if(!size) size=1;
    }
    return size;
}

static bool trigger_texlevelparameter = false;

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) {
    if(!current_context) return;
    // NSLog("glGetTexLevelParameteriv(%x, %d, %x, %p)", target, level, pname, params);
    if (isProxyTexture(target)) {
        switch (pname) {
            case GL_TEXTURE_WIDTH:
                (*params) = nlevel(current_context->proxy_width, level);
                break;
            case GL_TEXTURE_HEIGHT:
                (*params) = nlevel(current_context->proxy_height, level);
                break;
            case GL_TEXTURE_INTERNAL_FORMAT:
                (*params) = current_context->proxy_intformat;
                break;
        }
    } else {
        if(trigger_texlevelparameter) return;
        if(!current_context->es31) {
            printf("glGetTexLevelParameter* functions are not supported on your device");
            trigger_texlevelparameter = true;
            return;
        }
        es3_functions.glGetTexLevelParameteriv(target, level, pname, params);
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *data) {
    if(!current_context) return;
    if (isProxyTexture(target)) {
        current_context->proxy_width = ((width<<level)>current_context->maxTextureSize)?0:width;
        current_context->proxy_height = ((height<<level)>current_context->maxTextureSize)?0:height;
        current_context->proxy_intformat = internalformat;
    } else {
        pick_internalformat(&internalformat, &type, &format, &data);
        es3_functions.glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
            if(param == GL_LINEAR) param = GL_NEAREST;
            break;
    }
    es3_functions.glTexParameteri(target, pname, param);
}

const GLubyte* glGetString(GLenum name) {
    if(!current_context) return NULL;
    switch(name) {
        case GL_VERSION:
            return (const GLubyte*)"3.0 Large Thin Wrapper";
        case GL_SHADING_LANGUAGE_VERSION:
            return (const GLubyte*)"4.60 LTW";
        case GL_VENDOR:
            return (const GLubyte*)"PojavLauncherTeam & QuestCraft Developers";
        default:
            return es3_functions.glGetString(name);
    }
}

void glEnable(GLenum cap) {
    if(!current_context || cap == GL_DEBUG_OUTPUT) return;
    es3_functions.glEnable(cap);
}

int get_buffer_index(GLenum buffer) {
    switch (buffer) {
        case GL_ARRAY_BUFFER: return 0;
        case GL_COPY_READ_BUFFER: return 1;
        case GL_COPY_WRITE_BUFFER: return 2;
        case GL_PIXEL_PACK_BUFFER: return 3;
        case GL_PIXEL_UNPACK_BUFFER: return 4;
        case GL_TRANSFORM_FEEDBACK_BUFFER: return 5;
        case GL_UNIFORM_BUFFER: return 6;
        case GL_SHADER_STORAGE_BUFFER: return 7;
        case GL_DRAW_INDIRECT_BUFFER: return 8;
        default: return -1;
    }
}

int get_base_buffer_index(GLenum buffer) {
    switch (buffer) {
        case GL_ATOMIC_COUNTER_BUFFER: return 0;
        case GL_SHADER_STORAGE_BUFFER: return 1;
        case GL_TRANSFORM_FEEDBACK_BUFFER: return 2;
        case GL_UNIFORM_BUFFER: return 3;
        default: return -1;
    }
}

GLenum get_base_buffer_enum(int buffer_index) {
    switch (buffer_index) {
        case 0: return GL_ATOMIC_COUNTER_BUFFER;
        case 1: return GL_SHADER_STORAGE_BUFFER;
        case 2: return GL_TRANSFORM_FEEDBACK_BUFFER;
        case 3: return GL_UNIFORM_BUFFER;
        default: return -1;
    }
}

void glBindBuffer(GLenum buffer, GLuint name) {
    if(!current_context) return;
    es3_functions.glBindBuffer(buffer, name);
    int buffer_index = get_buffer_index(buffer);
    if(buffer_index == -1) return;
    current_context->bound_buffers[buffer_index] = name;
}

static basebuffer_binding_t* set_basebuffer(GLenum target, GLuint index, GLuint buffer) {
    int buffer_mapindex = get_base_buffer_index(target);
    if(buffer_mapindex == -1) return NULL;
    if(!buffer) {
        basebuffer_binding_t *old_binding = unordered_map_remove(current_context->bound_basebuffers[buffer_mapindex], (void*)index);
        free(old_binding);
        return NULL;
    }else {
        basebuffer_binding_t *binding = unordered_map_get(current_context->bound_basebuffers[buffer_mapindex], (void*)index);
        if(binding == NULL) {
            binding = calloc(1, sizeof(basebuffer_binding_t));
            unordered_map_put(current_context->bound_basebuffers[buffer_mapindex], (void*)index, binding);
        }
        binding->index = index;
        binding->buffer = buffer;
        return binding;
    }
}

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    if(!current_context) return;
    es3_functions.glBindBufferBase(target, index, buffer);
    basebuffer_binding_t * binding = set_basebuffer(target, index, buffer);
    if(!binding) return;
    binding->ranged = false;
}

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    if(!current_context) return;
    es3_functions.glBindBufferRange(target, index, buffer, offset, size);
    basebuffer_binding_t * binding = set_basebuffer(target, index, buffer);
    if(!binding) return;
    binding->ranged = true;
    binding->size = size;
    binding->offset = offset;
}

void glUseProgram(GLuint program) {
    if(!current_context) return;
    es3_functions.glUseProgram(program);
    current_context->program = program;
}

void glGetIntegerv(GLenum pname, GLint* data) {
    if(!current_context) return;
    switch (pname) {
        case GL_MAX_COLOR_ATTACHMENTS:
            *data = MAX_FBTARGETS;
            return;
        case GL_MAX_DRAW_BUFFERS:
            *data = current_context->max_drawbuffers;
            break;
        default:
            es3_functions.glGetIntegerv(pname, data);
    }
}

void glGetQueryObjectiv( 	GLuint id,
                            GLenum pname,
                            GLint * params) {
    if(!current_context) return;
    // This is not recommended but i don't care
    es3_functions.glGetQueryObjectuiv(id, pname, (GLuint*)params);
}

void glDepthRange(GLdouble nearVal,
                  GLdouble farVal) {
    if(!current_context) return;
    es3_functions.glDepthRangef((GLfloat)nearVal, (GLfloat)farVal);
}

void glDebugMessageControl( 	GLenum source,
                               GLenum type,
                               GLenum severity,
                               GLsizei count,
                               const GLuint *ids,
                               GLboolean enabled) {
    //STUB
}
