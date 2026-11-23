#ifndef FUSEBOX_GL_H
#define FUSEBOX_GL_H

#include <stdbool.h>
#include <SDL3/SDL_opengl.h>

/* GL 1.1 functions are linked directly; everything newer is loaded at
 * runtime and called through `gl`, e.g. gl.CreateShader(...). */
#define FUSEBOX_GL_FUNCS(X) \
   X(PFNGLACTIVETEXTUREPROC, ActiveTexture) \
   X(PFNGLCREATESHADERPROC, CreateShader) \
   X(PFNGLSHADERSOURCEPROC, ShaderSource) \
   X(PFNGLCOMPILESHADERPROC, CompileShader) \
   X(PFNGLGETSHADERIVPROC, GetShaderiv) \
   X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
   X(PFNGLDELETESHADERPROC, DeleteShader) \
   X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
   X(PFNGLATTACHSHADERPROC, AttachShader) \
   X(PFNGLLINKPROGRAMPROC, LinkProgram) \
   X(PFNGLGETPROGRAMIVPROC, GetProgramiv) \
   X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
   X(PFNGLDELETEPROGRAMPROC, DeleteProgram) \
   X(PFNGLUSEPROGRAMPROC, UseProgram) \
   X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
   X(PFNGLUNIFORM2FPROC, Uniform2f) \
   X(PFNGLUNIFORM3FPROC, Uniform3f) \
   X(PFNGLUNIFORM1FPROC, Uniform1f) \
   X(PFNGLUNIFORM4FPROC, Uniform4f) \
   X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays) \
   X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray) \
   X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays) \
   X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap)

struct gl_funcs
{
#define X(type, name) type name;
   FUSEBOX_GL_FUNCS(X)
#undef X
};

extern struct gl_funcs gl;

/* Needs a current context. */
bool gl_load(void);

#endif
