/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, CADIndie.
 * For use under LGPL-3.0
 */

#ifndef GL4ES_WRAPPER_BASEVERTEX_H
#define GL4ES_WRAPPER_BASEVERTEX_H

#include <stddef.h>
#include "egl.h"

GLint type_bytes(GLenum type);

/*
 * Writes size bytes from data (or NULL) into the currently bound target,
 * keeping the already allocated store around instead of respecifying it.
 * capacity holds the size the target was last allocated with and is updated
 * whenever the store has to grow.
 */
void dynamic_buffer_upload(GLenum target, GLsizeiptr* capacity, GLsizeiptr size, const void* data);

#endif //GL4ES_WRAPPER_BASEVERTEX_H
