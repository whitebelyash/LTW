/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, CADIndie.
 * For use under LGPL-3.0
 */

#ifndef POJAVLAUNCHER_MAIN_H
#define POJAVLAUNCHER_MAIN_H

int get_buffer_index(GLenum buffer);
int get_base_buffer_index(GLenum buffer);
GLenum get_base_buffer_enum(int buffer_index);
int get_textarget_slot(GLenum target);

#define GL_TEXTURE_2D_SLOT 0
#define GL_ELEMENT_ARRAY_BUFFER_SLOT 9

#endif //POJAVLAUNCHER_MAIN_H
