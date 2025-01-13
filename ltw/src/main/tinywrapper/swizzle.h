/**
 * Created by: artDev
 * Copyright (c) 2025 artDev, SerpentSpirale, PojavLauncherTeam, Digital Genesis LLC. All rights reserved.
 */

#ifndef GL4ES_WRAPPER_SWIZZLE_H
#define GL4ES_WRAPPER_SWIZZLE_H

#include "egl.h"

void swizzle_process_upload(GLenum target, GLenum *format, GLenum *type);
void swizzle_process_swizzle_param(GLenum target, GLenum swizzle_param, const GLenum* swizzle);

#endif //GL4ES_WRAPPER_SWIZZLE_H
