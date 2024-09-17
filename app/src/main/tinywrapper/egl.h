//
// Created by maks on 03.03.2024.
//

#ifndef POJAVLAUNCHER_EGL_H
#define POJAVLAUNCHER_EGL_H

#include <stdbool.h>
#include <EGL/egl.h>
#include "proc.h"
#include "unordered_map/unordered_map.h"

#define MAX_BOUND_BUFFERS 9
#define MAX_BOUND_BASEBUFFERS 4
#define MAX_DRAWBUFFERS 8
#define MAX_FBTARGETS 8

typedef struct {
    bool ready;
    GLuint indirectRenderBuffer;
} basevertex_renderer_t;

typedef struct {
    GLuint index;
    GLuint buffer;
    bool ranged;
    GLintptr  offset;
    GLintptr  size;
} basebuffer_binding_t;

typedef struct {
    GLuint color_targets[MAX_FBTARGETS];
    GLuint color_objects[MAX_FBTARGETS];
    GLuint color_levels[MAX_FBTARGETS];
    GLuint color_layers[MAX_FBTARGETS];
    GLenum virt_drawbuffers[MAX_DRAWBUFFERS];
    GLenum phys_drawbuffers[MAX_DRAWBUFFERS];
    GLsizei nbuffers;
} framebuffer_t;

typedef struct {
    bool ready;
    GLuint temp_texture;
    GLuint tempfb;
    GLuint destfb;
    void* depthData;
    GLsizei depthWidth, depthHeight;
} framebuffer_copier_t;

typedef struct {
    GLint internalformat;
    GLenum format;
    GLenum type;
    GLsizei width, height;
} texture_t;

typedef struct {
    EGLContext phys_context;
    bool context_rdy;
    bool es31;
    basevertex_renderer_t basevertex;
    GLuint multidraw_element_buffer;
    framebuffer_copier_t framebuffer_copier;
    unordered_map* shader_map;
    unordered_map* program_map;
    unordered_map* framebuffer_map;
    unordered_map* texture_map;
    unordered_map* bound_basebuffers[MAX_BOUND_BASEBUFFERS];
    int proxy_width, proxy_height, proxy_intformat, maxTextureSize;
    GLint max_drawbuffers;
    GLuint bound_buffers[MAX_BOUND_BUFFERS];
    GLuint program;
    GLuint draw_framebuffer;
    GLuint read_framebuffer;
} context_t;

extern thread_local context_t *current_context;
extern void init_egl();

#endif //POJAVLAUNCHER_EGL_H
