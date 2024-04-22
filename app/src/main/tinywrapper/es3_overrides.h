//
// Created by maks on 03.03.2024.
//
void glClearDepth(double depth);
void* glMapBuffer(GLenum target, GLenum access);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glDebugMessageControl( 	GLenum source,
                               GLenum type,
                               GLenum severity,
                               GLsizei count,
                               const GLuint *ids,
                               GLboolean enabled);
void glMultiDrawArrays( GLenum mode, GLint *first, GLsizei *count, GLsizei primcount);
void glMultiDrawElements( GLenum mode, GLsizei *count, GLenum type, void **indices, GLsizei primcount);
void glMultiDrawElementsBaseVertex( 	GLenum mode,
                                       const GLsizei *count,
                                       GLenum type,
                                       const void * const *indices,
                                       GLsizei drawcount,
                                       const GLint *basevertex);
void glBindFragDataLocation(GLuint program,
                            GLuint colorNumber,
                            const char * name);

GLESOVERRIDE(glClearDepth)
GLESOVERRIDE(glMapBuffer)
GLESOVERRIDE(glGetTexLevelParameteriv)
GLESOVERRIDE(glCreateShader)
GLESOVERRIDE(glDeleteShader)
GLESOVERRIDE(glShaderSource)
GLESOVERRIDE(glTexImage2D)
GLESOVERRIDE(glDebugMessageControl)
GLESOVERRIDE(glGetString)
GLESOVERRIDE(glEnable)
GLESOVERRIDE(glMultiDrawArrays)
GLESOVERRIDE(glMultiDrawElements)
GLESOVERRIDE(glMultiDrawElementsBaseVertex)
GLESOVERRIDE(glBindBufferBase)
GLESOVERRIDE(glBindBufferRange)
GLESOVERRIDE(glBindBuffer)
GLESOVERRIDE(glUseProgram)
GLESOVERRIDE(glGetIntegerv)
GLESOVERRIDE(glBindFramebuffer)
GLESOVERRIDE(glGenFramebuffers)
GLESOVERRIDE(glDeleteFramebuffers)
GLESOVERRIDE(glFramebufferTexture2D)
GLESOVERRIDE(glFramebufferTextureLayer)
GLESOVERRIDE(glFramebufferRenderbuffer)
GLESOVERRIDE(glGetFramebufferAttachmentParameteriv)
GLESOVERRIDE(glDrawBuffers)
GLESOVERRIDE(glClearBufferiv)
GLESOVERRIDE(glClearBufferuiv)
GLESOVERRIDE(glClearBufferfv)
GLESOVERRIDE(glReadPixels)
GLESOVERRIDE(glTexSubImage2D)
GLESOVERRIDE(glCopyTexSubImage2D)
GLESOVERRIDE(glTexParameteri)
GLESOVERRIDE(glBindFragDataLocation)