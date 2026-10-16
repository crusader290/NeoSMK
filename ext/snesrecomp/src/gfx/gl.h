/*
 * gl.h — minimal OpenGL ES 3.0 function loader.
 *
 * The renderer is written against the OpenGL ES 3.0 feature set (the Pi 4's
 * V3D driver exposes ES 3.1). The same subset exists in desktop OpenGL 3.3
 * core, which is the fallback on desktop drivers that can't hand out an ES
 * context (some Windows drivers, VMs). Only shader #version headers differ.
 *
 * No system GL headers are used: Windows only ships GL 1.1 headers, and
 * mixing GLES and desktop GL headers is fragile. The handful of types,
 * enums and entry points the renderer needs are declared here and resolved
 * at runtime through SDL_GL_GetProcAddress.
 */
#ifndef SNESRECOMP_GFX_GL_H
#define SNESRECOMP_GFX_GL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && !defined(_WIN64)
#define SMKGL_APIENTRY __stdcall
#else
#define SMKGL_APIENTRY
#endif

typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLuint;
typedef float         GLfloat;
typedef char          GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t     GLsizeiptr;
typedef ptrdiff_t     GLintptr;

#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_NO_ERROR                       0
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_ONE                            1
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_CW                             0x0900
#define GL_CCW                            0x0901
#define GL_CULL_FACE                      0x0B44
#define GL_DEPTH_TEST                     0x0B71
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_TEST                   0x0C11
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_TEXTURE_2D                     0x0DE1
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406
#define GL_RGBA                           0x1908
#define GL_RED                            0x1903
#define GL_R8                             0x8229
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_LEQUAL                         0x0203
#define GL_LESS                           0x0201
#define GL_FUNC_ADD                       0x8006
#define GL_RGBA8                          0x8058
#define GL_TEXTURE0                       0x84C0
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STREAM_DRAW                    0x88E0
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_COLOR_BUFFER_BIT               0x00004000

/* X-macro list of every entry point the renderer + ImGui backend use. All are
 * core in both OpenGL ES 3.0 and desktop OpenGL 3.3. */
#define SMKGL_FUNCS(X) \
    X(GLenum,        glGetError,               (void)) \
    X(const GLubyte*,glGetString,              (GLenum name)) \
    X(void,          glGetIntegerv,            (GLenum pname, GLint *data)) \
    X(void,          glViewport,               (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void,          glScissor,                (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void,          glClearColor,             (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void,          glClear,                  (GLbitfield mask)) \
    X(void,          glEnable,                 (GLenum cap)) \
    X(void,          glDisable,                (GLenum cap)) \
    X(void,          glBlendEquation,          (GLenum mode)) \
    X(void,          glBlendFuncSeparate,      (GLenum sfrgb, GLenum dfrgb, GLenum sfa, GLenum dfa)) \
    X(void,          glDepthFunc,              (GLenum func)) \
    X(void,          glDepthMask,              (GLboolean flag)) \
    X(void,          glCullFace,               (GLenum mode)) \
    X(void,          glFrontFace,              (GLenum mode)) \
    X(void,          glPixelStorei,            (GLenum pname, GLint param)) \
    X(void,          glGenTextures,            (GLsizei n, GLuint *textures)) \
    X(void,          glDeleteTextures,         (GLsizei n, const GLuint *textures)) \
    X(void,          glBindTexture,            (GLenum target, GLuint texture)) \
    X(void,          glActiveTexture,          (GLenum texture)) \
    X(void,          glTexParameteri,          (GLenum target, GLenum pname, GLint param)) \
    X(void,          glTexImage2D,             (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *pixels)) \
    X(void,          glTexSubImage2D,          (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *pixels)) \
    X(void,          glGenFramebuffers,        (GLsizei n, GLuint *fbs)) \
    X(void,          glDeleteFramebuffers,     (GLsizei n, const GLuint *fbs)) \
    X(void,          glBindFramebuffer,        (GLenum target, GLuint fb)) \
    X(void,          glFramebufferTexture2D,   (GLenum target, GLenum attach, GLenum textarget, GLuint tex, GLint level)) \
    X(void,          glFramebufferRenderbuffer,(GLenum target, GLenum attach, GLenum rbtarget, GLuint rb)) \
    X(GLenum,        glCheckFramebufferStatus, (GLenum target)) \
    X(void,          glGenRenderbuffers,       (GLsizei n, GLuint *rbs)) \
    X(void,          glDeleteRenderbuffers,    (GLsizei n, const GLuint *rbs)) \
    X(void,          glBindRenderbuffer,       (GLenum target, GLuint rb)) \
    X(void,          glRenderbufferStorage,    (GLenum target, GLenum ifmt, GLsizei w, GLsizei h)) \
    X(GLuint,        glCreateShader,           (GLenum type)) \
    X(void,          glShaderSource,           (GLuint sh, GLsizei count, const GLchar *const *src, const GLint *len)) \
    X(void,          glCompileShader,          (GLuint sh)) \
    X(void,          glGetShaderiv,            (GLuint sh, GLenum pname, GLint *params)) \
    X(void,          glGetShaderInfoLog,       (GLuint sh, GLsizei max, GLsizei *len, GLchar *log)) \
    X(void,          glDeleteShader,           (GLuint sh)) \
    X(GLuint,        glCreateProgram,          (void)) \
    X(void,          glAttachShader,           (GLuint prog, GLuint sh)) \
    X(void,          glBindAttribLocation,     (GLuint prog, GLuint index, const GLchar *name)) \
    X(void,          glLinkProgram,            (GLuint prog)) \
    X(void,          glGetProgramiv,           (GLuint prog, GLenum pname, GLint *params)) \
    X(void,          glGetProgramInfoLog,      (GLuint prog, GLsizei max, GLsizei *len, GLchar *log)) \
    X(void,          glDeleteProgram,          (GLuint prog)) \
    X(void,          glUseProgram,             (GLuint prog)) \
    X(GLint,         glGetUniformLocation,     (GLuint prog, const GLchar *name)) \
    X(void,          glUniform1i,              (GLint loc, GLint v0)) \
    X(void,          glUniform1f,              (GLint loc, GLfloat v0)) \
    X(void,          glUniform2f,              (GLint loc, GLfloat v0, GLfloat v1)) \
    X(void,          glUniform3f,              (GLint loc, GLfloat v0, GLfloat v1, GLfloat v2)) \
    X(void,          glUniform4f,              (GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void,          glUniformMatrix4fv,       (GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v)) \
    X(void,          glGenVertexArrays,        (GLsizei n, GLuint *arrays)) \
    X(void,          glDeleteVertexArrays,     (GLsizei n, const GLuint *arrays)) \
    X(void,          glBindVertexArray,        (GLuint array)) \
    X(void,          glGenBuffers,             (GLsizei n, GLuint *buffers)) \
    X(void,          glDeleteBuffers,          (GLsizei n, const GLuint *buffers)) \
    X(void,          glBindBuffer,             (GLenum target, GLuint buffer)) \
    X(void,          glBufferData,             (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(void,          glEnableVertexAttribArray,(GLuint index)) \
    X(void,          glVertexAttribPointer,    (GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)) \
    X(void,          glDrawArrays,             (GLenum mode, GLint first, GLsizei count)) \
    X(void,          glDrawElements,           (GLenum mode, GLsizei count, GLenum type, const void *indices)) \
    X(void,          glReadPixels,             (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *pixels))

#define SMKGL_DECLARE(ret, name, args) \
    typedef ret (SMKGL_APIENTRY *PFN_smk_##name) args; \
    extern PFN_smk_##name smkgl_##name;
SMKGL_FUNCS(SMKGL_DECLARE)
#undef SMKGL_DECLARE

/* Route plain GL names to the loaded pointers. */
#define glGetError                smkgl_glGetError
#define glGetString               smkgl_glGetString
#define glGetIntegerv             smkgl_glGetIntegerv
#define glViewport                smkgl_glViewport
#define glScissor                 smkgl_glScissor
#define glClearColor              smkgl_glClearColor
#define glClear                   smkgl_glClear
#define glEnable                  smkgl_glEnable
#define glDisable                 smkgl_glDisable
#define glBlendEquation           smkgl_glBlendEquation
#define glBlendFuncSeparate       smkgl_glBlendFuncSeparate
#define glDepthFunc               smkgl_glDepthFunc
#define glDepthMask               smkgl_glDepthMask
#define glCullFace                smkgl_glCullFace
#define glFrontFace               smkgl_glFrontFace
#define glPixelStorei             smkgl_glPixelStorei
#define glGenTextures             smkgl_glGenTextures
#define glDeleteTextures          smkgl_glDeleteTextures
#define glBindTexture             smkgl_glBindTexture
#define glActiveTexture           smkgl_glActiveTexture
#define glTexParameteri           smkgl_glTexParameteri
#define glTexImage2D              smkgl_glTexImage2D
#define glTexSubImage2D           smkgl_glTexSubImage2D
#define glGenFramebuffers         smkgl_glGenFramebuffers
#define glDeleteFramebuffers      smkgl_glDeleteFramebuffers
#define glBindFramebuffer         smkgl_glBindFramebuffer
#define glFramebufferTexture2D    smkgl_glFramebufferTexture2D
#define glFramebufferRenderbuffer smkgl_glFramebufferRenderbuffer
#define glCheckFramebufferStatus  smkgl_glCheckFramebufferStatus
#define glGenRenderbuffers        smkgl_glGenRenderbuffers
#define glDeleteRenderbuffers     smkgl_glDeleteRenderbuffers
#define glBindRenderbuffer        smkgl_glBindRenderbuffer
#define glRenderbufferStorage     smkgl_glRenderbufferStorage
#define glCreateShader            smkgl_glCreateShader
#define glShaderSource            smkgl_glShaderSource
#define glCompileShader           smkgl_glCompileShader
#define glGetShaderiv             smkgl_glGetShaderiv
#define glGetShaderInfoLog        smkgl_glGetShaderInfoLog
#define glDeleteShader            smkgl_glDeleteShader
#define glCreateProgram           smkgl_glCreateProgram
#define glAttachShader            smkgl_glAttachShader
#define glBindAttribLocation      smkgl_glBindAttribLocation
#define glLinkProgram             smkgl_glLinkProgram
#define glGetProgramiv            smkgl_glGetProgramiv
#define glGetProgramInfoLog       smkgl_glGetProgramInfoLog
#define glDeleteProgram           smkgl_glDeleteProgram
#define glUseProgram              smkgl_glUseProgram
#define glGetUniformLocation      smkgl_glGetUniformLocation
#define glUniform1i               smkgl_glUniform1i
#define glUniform1f               smkgl_glUniform1f
#define glUniform2f               smkgl_glUniform2f
#define glUniform3f               smkgl_glUniform3f
#define glUniform4f               smkgl_glUniform4f
#define glUniformMatrix4fv        smkgl_glUniformMatrix4fv
#define glGenVertexArrays         smkgl_glGenVertexArrays
#define glDeleteVertexArrays      smkgl_glDeleteVertexArrays
#define glBindVertexArray         smkgl_glBindVertexArray
#define glGenBuffers              smkgl_glGenBuffers
#define glDeleteBuffers           smkgl_glDeleteBuffers
#define glBindBuffer              smkgl_glBindBuffer
#define glBufferData              smkgl_glBufferData
#define glEnableVertexAttribArray smkgl_glEnableVertexAttribArray
#define glVertexAttribPointer     smkgl_glVertexAttribPointer
#define glDrawArrays              smkgl_glDrawArrays
#define glDrawElements            smkgl_glDrawElements
#define glReadPixels              smkgl_glReadPixels

/* Resolve every entry point for the current context. `es` selects the GLSL
 * dialect (ES 3.00 vs desktop 3.30 core). Returns false if any is missing. */
bool smkgl_load(bool es);

/* True when the current context is OpenGL ES. */
bool smkgl_is_es(void);

/* Compile + link a program from bodies without a #version line; the correct
 * header for the context is prepended. Attribute locations are bound in
 * order from `attribs` (NULL-terminated). Returns 0 on failure. */
GLuint smkgl_build_program(const char *name, const char *vs_body, const char *fs_body,
                           const char *const *attribs);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_GFX_GL_H */
