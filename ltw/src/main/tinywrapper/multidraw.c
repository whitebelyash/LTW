/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, CADIndie.
 * For use under LGPL-3.0
 */

#include <proc.h>
#include <egl.h>
#include "basevertex.h"
#include "main.h"
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

void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride){
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