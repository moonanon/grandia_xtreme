/* OpenGL 3.3 core, the functions the port uses, loaded at run time through
   SDL_GL_GetProcAddress (port/src/gl/gl.c). No GL headers or import
   library: the types and constants are here, and each function is a
   global pointer with GL's own name (glClear, ...), filled by grx_gl_load()
   once a context is current. Don't include another GL header next to it.

   To use another function, add it to GRX_GL_FUNCS (and its constants
   below). Everything here exists in GL 3.3 core, GL 4.1 (macOS) and, but
   for the noted few, GLES 3.0 / WebGL2. */
#ifndef GRX_GL_H
#define GRX_GL_H

#include <stddef.h>
#include <stdint.h>

/* GL's calling convention: __stdcall on 32-bit Windows (APIENTRY) */
#if defined(_WIN32) && defined(__i386__)
#define GRX_GLAPI __attribute__((stdcall))
#else
#define GRX_GLAPI
#endif

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef char GLchar;
typedef float GLfloat;
typedef uint64_t GLuint64;
typedef int64_t GLint64;
typedef struct __GLsync *GLsync;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1 /* not in GLES/WebGL2: upload as GL_RGBA there */
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
/* The GS's GPU renderer (port/src/gs_gl/) */
#define GL_NONE 0
#define GL_ZERO 0
#define GL_ONE 1
#define GL_NEVER 0x0200
#define GL_GREATER 0x0204
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_DEPTH_COMPONENT 0x1902
#define GL_EXTENSIONS 0x1F03
#define GL_REPEAT 0x2901
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
/* GL_EXT_texture_compression_s3tc, for the texture replacements (gsgl_replace.c) */
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_R16UI 0x8234
#define GL_R32UI 0x8236
#define GL_RED_INTEGER 0x8D94
#define GL_NUM_EXTENSIONS 0x821D
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_SRC1_COLOR 0x88F9
#define GL_LOWER_LEFT 0x8CA1
#define GL_ZERO_TO_ONE 0x935F
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull

/* X(return type, name without "gl", parameters) */
#define GRX_GL_FUNCS(X)                                                                                           \
    X(const GLubyte *, GetString, (GLenum name))                                                                  \
    X(void, GetIntegerv, (GLenum pname, GLint *data))                                                             \
    X(GLenum, GetError, (void))                                                                                   \
    X(void, Enable, (GLenum cap))                                                                                 \
    X(void, Disable, (GLenum cap))                                                                                \
    X(void, Viewport, (GLint x, GLint y, GLsizei width, GLsizei height))                                          \
    X(void, Scissor, (GLint x, GLint y, GLsizei width, GLsizei height))                                           \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                             \
    X(void, Clear, (GLbitfield mask))                                                                             \
    X(void, Flush, (void))                                                                                        \
    X(void, Finish, (void))                                                                                       \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                             \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels))       \
    /* Textures and samplers */                                                                                   \
    X(void, GenTextures, (GLsizei n, GLuint *textures))                                                           \
    X(void, DeleteTextures, (GLsizei n, const GLuint *textures))                                                  \
    X(void, BindTexture, (GLenum target, GLuint texture))                                                         \
    X(void, ActiveTexture, (GLenum texture))                                                                      \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                                            \
    X(void, TexImage2D,                                                                                           \
      (GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border, GLenum format,       \
       GLenum type, const void *pixels))                                                                          \
    X(void, TexSubImage2D,                                                                                        \
      (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,            \
       const void *pixels))                                                                                       \
    X(void, CompressedTexImage2D,                                                                                 \
      (GLenum target, GLint level, GLenum internalformat, GLsizei w, GLsizei h, GLint border, GLsizei imageSize,  \
       const void *data))                                                                                         \
    X(void, GenSamplers, (GLsizei n, GLuint *samplers))                                                           \
    X(void, DeleteSamplers, (GLsizei n, const GLuint *samplers))                                                  \
    X(void, BindSampler, (GLuint unit, GLuint sampler))                                                           \
    X(void, SamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                                       \
    /* Shaders */                                                                                                 \
    X(GLuint, CreateShader, (GLenum type))                                                                        \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length))       \
    X(void, CompileShader, (GLuint shader))                                                                       \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint *params))                                            \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log))                        \
    X(void, DeleteShader, (GLuint shader))                                                                        \
    X(GLuint, CreateProgram, (void))                                                                              \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                        \
    X(void, LinkProgram, (GLuint program))                                                                        \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint *params))                                          \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei *length, GLchar *log))                      \
    X(void, DeleteProgram, (GLuint program))                                                                      \
    X(void, UseProgram, (GLuint program))                                                                         \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar *name))                                            \
    X(void, Uniform1i, (GLint location, GLint v0))                                                                \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                                              \
    X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))                          \
    /* Drawing */                                                                                                 \
    X(void, GenVertexArrays, (GLsizei n, GLuint *arrays))                                                         \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint *arrays))                                                \
    X(void, BindVertexArray, (GLuint array))                                                                      \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                                \
    /* Framebuffers */                                                                                            \
    X(void, GenFramebuffers, (GLsizei n, GLuint *framebuffers))                                                   \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))                                          \
    X(void, BindFramebuffer, (GLenum target, GLuint framebuffer))                                                 \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                            \
    X(void, BlitFramebuffer,                                                                                      \
      (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,   \
       GLenum filter))                                                                                            \
    /* Sync objects: shared between contexts of one share group */                                               \
    X(GLsync, FenceSync, (GLenum condition, GLbitfield flags))                                                    \
    X(void, WaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                                          \
    X(GLenum, ClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                                  \
    X(void, DeleteSync, (GLsync sync))                                                                         \
    /* The GS's GPU renderer (port/src/gs_gl/) */                                                                 \
    X(const GLubyte *, GetStringi, (GLenum name, GLuint index))                                                   \
    X(void, GenBuffers, (GLsizei n, GLuint *buffers))                                                             \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                           \
    X(void, BufferData, (GLenum target, ptrdiff_t size, const void *data, GLenum usage))                          \
    X(void *, MapBufferRange, (GLenum target, ptrdiff_t offset, ptrdiff_t length, GLbitfield access))             \
    X(GLboolean, UnmapBuffer, (GLenum target))                                                                    \
    X(void, VertexAttribPointer,                                                                                  \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))         \
    X(void, VertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer))   \
    X(void, EnableVertexAttribArray, (GLuint index))                                                              \
    X(void, DrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instancecount))                \
    X(void, BlendFuncSeparate, (GLenum srgb, GLenum drgb, GLenum salpha, GLenum dalpha))                          \
    X(void, BlendEquationSeparate, (GLenum rgb, GLenum alpha))                                                    \
    X(void, DepthFunc, (GLenum func))                                                                             \
    X(void, DepthMask, (GLboolean flag))                                                                          \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                      \
    X(void, DrawBuffer, (GLenum buf))                                                                             \
    X(void, Uniform1ui, (GLint location, GLuint v0))                                                              \
    X(void, Uniform4i, (GLint location, GLint v0, GLint v1, GLint v2, GLint v3))                                  \
    X(void, Uniform2i, (GLint location, GLint v0, GLint v1))                                                      \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                                                  \
    X(void, Uniform4iv, (GLint location, GLsizei count, const GLint *v))                                          \
    X(void, Uniform4fv, (GLint location, GLsizei count, const GLfloat *v))                                        \
    X(void, CopyTexSubImage2D,                                                                                    \
      (GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h))               \
    X(void, ClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat *value))

#define GRX_GL_DECLARE(ret, name, params)                                                                         \
    typedef ret(GRX_GLAPI *grx_PFNgl##name) params;                                                               \
    extern grx_PFNgl##name gl##name;
GRX_GL_FUNCS(GRX_GL_DECLARE)
#undef GRX_GL_DECLARE

/* Fill the function pointers, with a context current. 0, or -1 (logged,
   naming the first function missing). The pointers are the same for
   every context SDL creates for the window, so once is enough. */
int grx_gl_load(void);

/* Optional, from extensions (NULL when the context lacks them; grx_gl_load
   fills them): GL_ARB_texture_barrier (or NV_), GL_ARB_clip_control */
typedef void(GRX_GLAPI *grx_PFNglTextureBarrier)(void);
typedef void(GRX_GLAPI *grx_PFNglClipControl)(GLenum origin, GLenum depth);
extern grx_PFNglTextureBarrier glTextureBarrier;
extern grx_PFNglClipControl glClipControl;
/* 1 if the current context lists the extension */
int grx_gl_has(const char *ext);

/* Compile and link a program from GLSL vertex and fragment source; `what`
   names it in the log. Returns the program, or 0 (logged). */
GLuint grx_gl_program(const char *vertex, const char *fragment, const char *what);

/* --video-test's checks of the presenter's interfaces (gl_test.c):
   GRX_VIDEO_GL_TEST, a stand-in GPU renderer (init before
   grx_frame_start, then a frame after each game frame, on the game
   thread); GRX_VIDEO_HOOK_TEST, a present hook that cross-fades by t */
int grx_gl_test_init(void);
void grx_gl_test_frame(uint64_t field);
void grx_gl_test_hook_install(void);

#endif
