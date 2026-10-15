/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, CADIndie.
 * For use under LGPL-3.0
 */

#include <proc.h>
#include <egl.h>
#include "basevertex.h"
#include "main.h"
#include <stdbool.h>

void glMultiDrawArrays( GLenum mode, GLint *first, GLsizei *count, GLsizei primcount )
{
    // We'd need to merge each buffer attached to the VBO to properly achieve this. Nuh-uh. Aint no way im doin allat
    if(!current_context) return;
    for (int i = 0; i < primcount; i++) {
        if (count[i] > 0)
            es3_functions.glDrawArrays(mode, first[i], count[i]);
    }
}

void glMultiDrawElements( GLenum mode, GLsizei *count, GLenum type, const void * const *indices, GLsizei primcount )
{
    if(!current_context) return;
    GLint typebytes = type_bytes(type);
    if(typebytes <= 0 || primcount <= 0) return;
    GLuint elementbuffer = current_context->bound_buffers[GL_ELEMENT_ARRAY_BUFFER_SLOT];
    GLuint copywritebuffer = current_context->bound_buffers[get_buffer_index(GL_COPY_WRITE_BUFFER)];
    es3_functions.glBindBuffer(GL_COPY_WRITE_BUFFER, current_context->multidraw_element_buffer);
    GLsizeiptr total = 0;
    for (GLsizei i = 0; i < primcount; i++) {
        if(count[i] > 0) total += count[i];
    }
    if(total <= 0) {
        es3_functions.glBindBuffer(GL_COPY_WRITE_BUFFER, copywritebuffer);
        return;
    }
    GLsizeiptr offset = 0;
    // Only respecify the store when it outgrew the one we already have, otherwise
    // every call orphans the buffer and the driver allocates a new one.
    dynamic_buffer_upload(GL_COPY_WRITE_BUFFER, &current_context->multidraw_element_buffer_capacity, total*typebytes, NULL);
    if(elementbuffer != 0) {
        for (GLsizei i = 0; i < primcount; i++) {
            GLsizei icount = count[i];
            if(icount <= 0) continue;
            es3_functions.glCopyBufferSubData(GL_ELEMENT_ARRAY_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)indices[i], offset, icount*typebytes);
            offset += icount*typebytes;
        }
    } else {
        // Client side indices that happen to sit next to each other in memory can go
        // out in one upload instead of one call per primitive.
        const char* run_start = NULL;
        size_t run_length = 0;
        for (GLsizei i = 0; i < primcount; i++) {
            GLsizei icount = count[i];
            if(icount <= 0) continue;
            const char* pointer = (const char*)indices[i];
            if(run_start != NULL && pointer == run_start + run_length) {
                run_length += (size_t)icount * typebytes;
                continue;
            }
            if(run_start != NULL) {
                es3_functions.glBufferSubData(GL_COPY_WRITE_BUFFER, offset, (GLsizeiptr)run_length, run_start);
                offset += (GLsizeiptr)run_length;
            }
            run_start = pointer;
            run_length = (size_t)icount * typebytes;
        }
        if(run_start != NULL) {
            es3_functions.glBufferSubData(GL_COPY_WRITE_BUFFER, offset, (GLsizeiptr)run_length, run_start);
            offset += (GLsizeiptr)run_length;
        }
    }
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, current_context->multidraw_element_buffer);
    es3_functions.glDrawElements(mode, (GLsizei)total, type, 0);
    // Both bindings have to go back, otherwise client side draws after this would
    // read our index buffer instead of the app's pointer.
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementbuffer);
    es3_functions.glBindBuffer(GL_COPY_WRITE_BUFFER, copywritebuffer);
}

/*
 * GL_EXT_multi_draw and GL_ANGLE_multi_draw, emulated on top of multi draw indirect
 * where the host has it. The direct entry points take per draw counts from client
 * memory, which is exactly the shape of an indirect command array, so build one and
 * hand it to the host in a single call.
 *
 * Falls back to a loop of plain draws when the host has neither, which is slow but
 * correct.
 */

typedef struct {
    GLuint count;
    GLuint instanceCount;
    GLuint firstIndex;
} indirect_arrays_head_t;

// Grows the renderer's own indirect buffer to hold size bytes. dynamic_buffer_upload
// acts on whatever is currently bound, so bind first and remember what was there.
static bool multidraw_reserve(multidraw_renderer_t* renderer, GLsizeiptr size) {
    if(renderer->capacity >= size) return true;
    if(!renderer->ready) {
        es3_functions.glGenBuffers(1, &renderer->buffer);
        renderer->ready = true;
    }
    GLuint previous = current_context->bound_buffers[get_buffer_index(GL_DRAW_INDIRECT_BUFFER)];
    es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, renderer->buffer);
    dynamic_buffer_upload(GL_DRAW_INDIRECT_BUFFER, &renderer->capacity, size, NULL);
    es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, previous);
    return true;
}

void glMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count, GLsizei drawcount) {
    if(!current_context || drawcount <= 0) return;
    if(!current_context->multidraw_indirect) {
        for(GLsizei i = 0; i < drawcount; i++)
            if(count[i] > 0) es3_functions.glDrawArrays(mode, first[i], count[i]);
        return;
    }
    size_t stride = current_context->indirect_arrays_stride;
    GLuint previous = current_context->bound_buffers[get_buffer_index(GL_DRAW_INDIRECT_BUFFER)];
    multidraw_reserve(&current_context->multidraw_arrays, (GLsizeiptr)(drawcount * stride));
    indirect_arrays_head_t* head = malloc(sizeof(*head) * drawcount);
    if(head == NULL) return;
    GLsizei drawn = 0;
    for(GLsizei i = 0; i < drawcount; i++) {
        if(count[i] <= 0 || first[i] < 0) continue;
        head[drawn].count = (GLuint)count[i];
        head[drawn].instanceCount = 1;
        head[drawn].firstIndex = (GLuint)first[i];
        if(stride == INDIRECT_ARRAYS_STRIDE_BASE_INSTANCE)
            ((GLuint*)&head[drawn])[3] = 0;
        drawn++;
    }
    if(drawn == 0) { free(head); return; }
    es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, current_context->multidraw_arrays.buffer);
    es3_functions.glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)(drawn * stride), head);
    es3_functions.glMultiDrawArraysIndirectEXT(mode, (const void*)0, drawn, (GLsizei)stride);
    free(head);
    if(previous != current_context->multidraw_arrays.buffer)
        es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, previous);
}

void glMultiDrawArraysInstancedEXT(GLenum mode, const GLint *first, const GLsizei *count,
                                   GLsizei instancecount, GLsizei drawcount) {
    if(!current_context || drawcount <= 0 || instancecount <= 0) return;
    if(!current_context->multidraw_indirect) {
        for(GLsizei i = 0; i < drawcount; i++)
            if(count[i] > 0) es3_functions.glDrawArraysInstanced(mode, first[i], count[i], instancecount);
        return;
    }
    size_t stride = current_context->indirect_arrays_stride;
    GLuint previous = current_context->bound_buffers[get_buffer_index(GL_DRAW_INDIRECT_BUFFER)];
    multidraw_reserve(&current_context->multidraw_arrays, (GLsizeiptr)(drawcount * stride));
    indirect_arrays_head_t* head = malloc(sizeof(*head) * drawcount);
    if(head == NULL) return;
    GLsizei drawn = 0;
    for(GLsizei i = 0; i < drawcount; i++) {
        if(count[i] <= 0 || first[i] < 0) continue;
        head[drawn].count = (GLuint)count[i];
        head[drawn].instanceCount = (GLuint)instancecount;
        head[drawn].firstIndex = (GLuint)first[i];
        if(stride == INDIRECT_ARRAYS_STRIDE_BASE_INSTANCE)
            ((GLuint*)&head[drawn])[3] = 0;
        drawn++;
    }
    if(drawn == 0) { free(head); return; }
    es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, current_context->multidraw_arrays.buffer);
    es3_functions.glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)(drawn * stride), head);
    es3_functions.glMultiDrawArraysIndirectEXT(mode, (const void*)0, drawn, (GLsizei)stride);
    free(head);
    if(previous != current_context->multidraw_arrays.buffer)
        es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, previous);
}

void glMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type,
                            const void * const *indices, GLsizei drawcount) {
    if(!current_context || drawcount <= 0) return;
    if(!current_context->multidraw_indirect) {
        glMultiDrawElements(mode, (GLsizei*)count, type, indices, drawcount);
        return;
    }
    GLint typebytes = type_bytes(type);
    if(typebytes <= 0) return;
    GLuint elementbuffer = current_context->bound_buffers[GL_ELEMENT_ARRAY_BUFFER_SLOT];
    if(elementbuffer == 0) {
        // Client side indices have to be gathered into a buffer before they can be
        // referenced by an indirect command, which is what the merged draw does.
        glMultiDrawElements(mode, (GLsizei*)count, type, indices, drawcount);
        return;
    }
    size_t stride = current_context->indirect_elements_stride;
    GLuint previous = current_context->bound_buffers[get_buffer_index(GL_DRAW_INDIRECT_BUFFER)];
    multidraw_reserve(&current_context->multidraw_elements, (GLsizeiptr)(drawcount * stride));
    indirect_pass_t* head = malloc(sizeof(*head) * drawcount);
    if(head == NULL) return;
    GLsizei drawn = 0;
    for(GLsizei i = 0; i < drawcount; i++) {
        if(count[i] <= 0) continue;
        uintptr_t pointer = (uintptr_t)indices[i];
        if(pointer % typebytes != 0) continue;
        head[drawn].count = (GLuint)count[i];
        head[drawn].instanceCount = 1;
        head[drawn].firstIndex = (GLuint)(pointer / typebytes);
        head[drawn].baseVertex = 0;
        head[drawn].reservedMustBeZero = 0;
        drawn++;
    }
    if(drawn == 0) { free(head); return; }
    es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, current_context->multidraw_elements.buffer);
    es3_functions.glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)(drawn * stride), head);
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementbuffer);
    es3_functions.glMultiDrawElementsIndirectEXT(mode, type, (const void*)0, drawn, (GLsizei)stride);
    free(head);
    if(previous != current_context->multidraw_elements.buffer)
        es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, previous);
}

void glMultiDrawElementsInstancedEXT(GLenum mode, const GLsizei *count, GLenum type,
                                     const void * const *indices, GLsizei instancecount, GLsizei drawcount) {
    if(!current_context || drawcount <= 0 || instancecount <= 0) return;
    if(!current_context->multidraw_indirect) {
        for(GLsizei i = 0; i < drawcount; i++)
            if(count[i] > 0) es3_functions.glDrawElementsInstanced(mode, count[i], type, indices[i], instancecount);
        return;
    }
    GLint typebytes = type_bytes(type);
    if(typebytes <= 0) return;
    GLuint elementbuffer = current_context->bound_buffers[GL_ELEMENT_ARRAY_BUFFER_SLOT];
    if(elementbuffer == 0) {
        for(GLsizei i = 0; i < drawcount; i++)
            if(count[i] > 0) es3_functions.glDrawElementsInstanced(mode, count[i], type, indices[i], instancecount);
        return;
    }
    size_t stride = current_context->indirect_elements_stride;
    GLuint previous = current_context->bound_buffers[get_buffer_index(GL_DRAW_INDIRECT_BUFFER)];
    multidraw_reserve(&current_context->multidraw_elements, (GLsizeiptr)(drawcount * stride));
    indirect_pass_t* head = malloc(sizeof(*head) * drawcount);
    if(head == NULL) return;
    GLsizei drawn = 0;
    for(GLsizei i = 0; i < drawcount; i++) {
        if(count[i] <= 0) continue;
        uintptr_t pointer = (uintptr_t)indices[i];
        if(pointer % typebytes != 0) continue;
        head[drawn].count = (GLuint)count[i];
        head[drawn].instanceCount = (GLuint)instancecount;
        head[drawn].firstIndex = (GLuint)(pointer / typebytes);
        head[drawn].baseVertex = 0;
        head[drawn].reservedMustBeZero = 0;
        drawn++;
    }
    if(drawn == 0) { free(head); return; }
    es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, current_context->multidraw_elements.buffer);
    es3_functions.glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)(drawn * stride), head);
    es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementbuffer);
    es3_functions.glMultiDrawElementsIndirectEXT(mode, type, (const void*)0, drawn, (GLsizei)stride);
    free(head);
    if(previous != current_context->multidraw_elements.buffer)
        es3_functions.glBindBuffer(GL_DRAW_INDIRECT_BUFFER, previous);
}

void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride) {
    if(!current_context) return;
    if(current_context->multidraw_indirect) {
        es3_functions.glMultiDrawElementsIndirectEXT(mode, type, indirect, drawcount, stride);
    }
    // TODO: handle MDI emulation if needed
}

void glMultiDrawArraysIndirect(GLenum mode, const void *indirect, GLsizei drawcount, GLsizei stride) {
    if(!current_context) return;
    if(current_context->multidraw_indirect) {
        es3_functions.glMultiDrawArraysIndirectEXT(mode, indirect, drawcount, stride);
    }
}

void glMultiDrawElementsIndirectARB(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride) {
    glMultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
}

void glMultiDrawArraysIndirectARB(GLenum mode, const void *indirect, GLsizei drawcount, GLsizei stride) {
    glMultiDrawArraysIndirect(mode, indirect, drawcount, stride);
}