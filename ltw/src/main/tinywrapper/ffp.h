/**
 * Fixed-function pipeline (compatibility) emulation for the OpenGL ES wrapper.
 * Created for LTW.
 * For use under LGPL-3.0
 */

#ifndef POJAVLAUNCHER_FFP_H
#define POJAVLAUNCHER_FFP_H

#include <stdbool.h>
#include <GLES3/gl3.h>

/* Returns true when the wrapper is running in a fixed-function compatibility mode. */
bool ffp_compat_enabled(void);

/* Called from egl.c when the context is initialized. */
void ffp_context_init(void* context);

/* Called from egl.c when a context is destroyed. */
void ffp_context_free(void* ffp);

/* Intercepts glEnable/glDisable for fixed-function capabilities.
 * Returns true when the capability was handled and must not reach the ES driver. */
bool ffp_handle_enable(GLenum cap, GLboolean enable);

/* Intercepts glGetIntegerv for fixed-function state.
 * Returns true when the query was handled. */
bool ffp_handle_getintegerv(GLenum pname, GLint* data);

/* Intercepts glGetFloatv/glGetDoublev/glGetBooleanv for fixed-function state.
 * Returns true when the query was handled. */
bool ffp_handle_getfloatv(GLenum pname, GLfloat* data);
bool ffp_handle_getdoublev(GLenum pname, GLdouble* data);

/* Fills the version/vendor strings used when compatibility mode is active. */
const char* ffp_get_version_string(void);
const char* ffp_get_shading_language_string(void);
GLenum ffp_get_profile_mask(void);

#endif //POJAVLAUNCHER_FFP_H
