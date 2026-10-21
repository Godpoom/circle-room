// Minimal OpenGL 3.3 core loader: only the entry points this app uses.
#pragma once
#include <cstddef>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef char GLchar;

#define GLP_APIENTRY __stdcall

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_RGBA8 0x8058
#define GL_RGBA16 0x805B
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_RGBA16F 0x881A
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40

#define GLP_FUNCS(X) \
  X(const GLubyte*, glGetString, (GLenum name)) \
  X(void, glGetIntegerv, (GLenum pname, GLint* data)) \
  X(GLenum, glGetError, (void)) \
  X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
  X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
  X(void, glClear, (GLbitfield mask)) \
  X(void, glEnable, (GLenum cap)) \
  X(void, glDisable, (GLenum cap)) \
  X(void, glFinish, (void)) \
  X(void, glGenTextures, (GLsizei n, GLuint* t)) \
  X(void, glDeleteTextures, (GLsizei n, const GLuint* t)) \
  X(void, glBindTexture, (GLenum target, GLuint t)) \
  X(void, glActiveTexture, (GLenum unit)) \
  X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* data)) \
  X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* data)) \
  X(void, glTexParameteri, (GLenum target, GLenum pname, GLint v)) \
  X(void, glGenerateMipmap, (GLenum target)) \
  X(void, glPixelStorei, (GLenum pname, GLint v)) \
  X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* data)) \
  X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
  X(GLuint, glCreateShader, (GLenum type)) \
  X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len)) \
  X(void, glCompileShader, (GLuint s)) \
  X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint* v)) \
  X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log)) \
  X(void, glDeleteShader, (GLuint s)) \
  X(GLuint, glCreateProgram, (void)) \
  X(void, glAttachShader, (GLuint p, GLuint s)) \
  X(void, glLinkProgram, (GLuint p)) \
  X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint* v)) \
  X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log)) \
  X(void, glDeleteProgram, (GLuint p)) \
  X(void, glUseProgram, (GLuint p)) \
  X(GLint, glGetUniformLocation, (GLuint p, const GLchar* name)) \
  X(void, glUniform1i, (GLint loc, GLint v)) \
  X(void, glUniform1f, (GLint loc, GLfloat v)) \
  X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b)) \
  X(void, glUniform3f, (GLint loc, GLfloat a, GLfloat b, GLfloat c)) \
  X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d)) \
  X(void, glUniform1fv, (GLint loc, GLsizei n, const GLfloat* v)) \
  X(void, glUniformMatrix3fv, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat* v)) \
  X(void, glGenVertexArrays, (GLsizei n, GLuint* a)) \
  X(void, glBindVertexArray, (GLuint a)) \
  X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* a)) \
  X(void, glGenFramebuffers, (GLsizei n, GLuint* f)) \
  X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* f)) \
  X(void, glBindFramebuffer, (GLenum target, GLuint f)) \
  X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level)) \
  X(GLenum, glCheckFramebufferStatus, (GLenum target))

// Pointers live in a namespace so they never collide with opengl32 exports.
namespace glp {
#define GLP_DECL(ret, name, args) typedef ret(GLP_APIENTRY* PFN_##name) args; extern PFN_##name name;
GLP_FUNCS(GLP_DECL)
#undef GLP_DECL
}  // namespace glp
using namespace glp;

// Optional (GL 4.2+ / ARB_texture_storage); nullptr when unavailable.
typedef void(GLP_APIENTRY* PFN_glTexStorage2D)(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h);
extern PFN_glTexStorage2D glpTexStorage2D;

typedef void* (*GlpGetProc)(const char* name);
// Returns the name of the first missing function, or nullptr on success.
const char* glpLoad(GlpGetProc getProc);
