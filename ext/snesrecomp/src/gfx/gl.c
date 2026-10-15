/*
 * gl.c — runtime GL entry-point loader + shader helpers (see gl.h).
 */
#include "gl.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define SMKGL_DEFINE(ret, name, args) PFN_smk_##name smkgl_##name = NULL;
SMKGL_FUNCS(SMKGL_DEFINE)
#undef SMKGL_DEFINE

static bool s_es = true;

bool smkgl_is_es(void) { return s_es; }

bool smkgl_load(bool es) {
    s_es = es;
    bool ok = true;
#define SMKGL_RESOLVE(ret, name, args)                                         \
    smkgl_##name = (PFN_smk_##name)SDL_GL_GetProcAddress(#name);               \
    if (!smkgl_##name) {                                                       \
        fprintf(stderr, "gl: missing entry point %s\n", #name);                \
        ok = false;                                                            \
    }
    SMKGL_FUNCS(SMKGL_RESOLVE)
#undef SMKGL_RESOLVE
    return ok;
}

static GLuint compile(const char *name, GLenum type, const char *body) {
    /* ES: GLSL ES 3.00 needs an explicit float precision in fragment shaders.
     * Desktop: GLSL 3.30 core accepts (and ignores) precision qualifiers, so
     * the same bodies compile on both. */
    const char *header = s_es
        ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
        : "#version 330 core\n";
    const char *srcs[2] = { header, body };
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 2, srcs, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "gl: %s %s shader failed to compile:\n%s\n", name,
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint smkgl_build_program(const char *name, const char *vs_body, const char *fs_body,
                           const char *const *attribs) {
    GLuint vs = compile(name, GL_VERTEX_SHADER, vs_body);
    GLuint fs = compile(name, GL_FRAGMENT_SHADER, fs_body);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    for (GLuint i = 0; attribs && attribs[i]; i++)
        glBindAttribLocation(prog, i, attribs[i]);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "gl: %s program failed to link:\n%s\n", name, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}
