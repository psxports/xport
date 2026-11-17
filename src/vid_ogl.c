#if !defined(GAPI_OGL)
    #error Define GAPI_OGL when compiling vid_ogl.c
#endif

#include "vid.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

typedef char VID_VERTEX16_SIZE[(sizeof(VID_VERTEX16) == 52) ? 1 : -1];
typedef char VID_VERTEX2D_SIZE[(sizeof(VID_VERTEX2D) == 44) ? 1 : -1];

struct VID_GEOMETRY
{
    struct VID_GEOMETRY *next;
    void *vertices;
    uint32 stride;
    uint32 count, version, uploaded, object, array;
    uint32 refs, released, immutable;
    size_t storage_bytes;
    uint32 dirty_first, dirty_last;
};

struct VID_INDEX
{
    VID_INDEX *next;
    uint32 *values;
    uint32 capacity, count, version, uploaded, object, refs, released, immutable;
    size_t storage_bytes;
};

static VID_INDEX *index_buffers;

struct VID_TEXTURE
{
    struct VID_TEXTURE *next;
    uint8 *pixels;
    uint32 width, height, format, version, uploaded, object, allocated, layers, array;
    uint32 refs, released, draw_refs, immutable;
    uint32 sampler_flags, mip_version, mip_levels, compare_ready;
    uint32 dirty_first, dirty_last, cube;
};

struct VID_TARGET
{
    VID_TARGET *next;
    VID_TEXTURE *texture, *depth_texture, *surface_texture;
    uint32 framebuffer, depth, refs, released, ready;
    uint32 fixed_width, fixed_height, face, completed_faces;
    uint32 *framebuffers;
    uint32 framebuffer_count, storage_valid;
    uint32 mip_storage_levels, mip_written, mip_base_version;
    uint32 mip_framebuffers[32], mip_fbo_valid;
};

// gpu writes invalidate derived mips without scheduling a CPU upload
static void vid_texture_rendered(VID_TEXTURE *texture)
{
    if (!++texture->version)
        texture->version = 1;
    texture->uploaded = texture->version;
    texture->mip_version = 0;
}

static VID_TARGET *targets, *frame_target;
static sint32 frame_target_present = 1;
static float frame_target_offset;
static uint32 frame_target_mip;

struct VID_SHADER
{
    struct VID_SHADER *next;
    uint32 program, refs, released;
    sint32 textured, params, fog, uniforms, aux[VID_AUX_TEXTURES];
    uint32 uniform_vectors, aux_shadow_mask, aux_2d_mask;
    uint64 draw_count;
};

static VID_SHADER *shaders;

struct VID_MATERIAL
{
    struct VID_MATERIAL *next;
    VID_MATERIAL_DATA data;
    VID_TEXTURE *held_texture;
    VID_TEXTURE *held_aux[VID_AUX_TEXTURES];
    VID_SHADER *held_shader;
    uint32 refs, released;
    uint64 revision;
};

struct VID_SAMPLE
{
    struct VID_SAMPLE *next;
    uint32 color, refs, released;
};

static VID_SAMPLE *samples;

typedef struct VID_DRAW
{
    struct VID_DRAW *next;
    VID_GEOMETRY *geometry;
    VID_INDEX *indices;
    uint32 pass;
    VID_MATERIAL *material;
    VID_MATERIAL_DATA data;
    VID_RECT viewport;
    uint32 first, count;
    VID_SAMPLE *sample;
    float fog[12];
    float sample_x, sample_y;
    const float *uniforms;
    uint32 uniform_count;
} VID_DRAW;

#if defined(AP_WEB)
    #include <emscripten.h>
    #include <emscripten/html5.h>
    #include <GLES3/gl3.h>
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE vid_context;
    #define VID_GLSL "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
    #include <windows.h>
    #include <GL/gl.h>
    #include "libs/gl/glext.h"
    #include "libs/gl/wglext.h"
static HWND vid_window;
static HDC vid_device;
static HGLRC vid_context;
    #define VID_GLSL "#version 330 core\n"
static PFNGLTEXIMAGE3DPROC vid_glTexImage3D;
    #define glTexImage3D vid_glTexImage3D
static PFNGLTEXSUBIMAGE3DPROC vid_glTexSubImage3D;
    #define glTexSubImage3D vid_glTexSubImage3D
static PFNGLGENERATEMIPMAPPROC vid_glGenerateMipmap;
    #define glGenerateMipmap vid_glGenerateMipmap
static PFNGLGETSTRINGIPROC vid_glGetStringi;
    #define glGetStringi vid_glGetStringi
static PFNGLCREATESHADERPROC vid_glCreateShader;
    #define glCreateShader vid_glCreateShader
static PFNGLSHADERSOURCEPROC vid_glShaderSource;
    #define glShaderSource vid_glShaderSource
static PFNGLCOMPILESHADERPROC vid_glCompileShader;
    #define glCompileShader vid_glCompileShader
static PFNGLGETSHADERIVPROC vid_glGetShaderiv;
    #define glGetShaderiv vid_glGetShaderiv
static PFNGLGETSHADERINFOLOGPROC vid_glGetShaderInfoLog;
    #define glGetShaderInfoLog vid_glGetShaderInfoLog
static PFNGLDELETESHADERPROC vid_glDeleteShader;
    #define glDeleteShader vid_glDeleteShader
static PFNGLCREATEPROGRAMPROC vid_glCreateProgram;
    #define glCreateProgram vid_glCreateProgram
static PFNGLATTACHSHADERPROC vid_glAttachShader;
    #define glAttachShader vid_glAttachShader
static PFNGLLINKPROGRAMPROC vid_glLinkProgram;
    #define glLinkProgram vid_glLinkProgram
static PFNGLGETPROGRAMIVPROC vid_glGetProgramiv;
    #define glGetProgramiv vid_glGetProgramiv
static PFNGLGETPROGRAMINFOLOGPROC vid_glGetProgramInfoLog;
    #define glGetProgramInfoLog vid_glGetProgramInfoLog
static PFNGLDELETEPROGRAMPROC vid_glDeleteProgram;
    #define glDeleteProgram vid_glDeleteProgram
static PFNGLUSEPROGRAMPROC vid_glUseProgram;
    #define glUseProgram vid_glUseProgram
static PFNGLGETUNIFORMLOCATIONPROC vid_glGetUniformLocation;
    #define glGetUniformLocation vid_glGetUniformLocation
static PFNGLCLEARBUFFERFVPROC vid_glClearBufferfv;
    #define glClearBufferfv vid_glClearBufferfv
static PFNGLDRAWBUFFERSPROC vid_glDrawBuffers;
    #define glDrawBuffers vid_glDrawBuffers
static PFNGLGETACTIVEUNIFORMPROC vid_glGetActiveUniform;
    #define glGetActiveUniform vid_glGetActiveUniform
static PFNGLUNIFORM4FVPROC vid_glUniform4fv;
    #define glUniform4fv vid_glUniform4fv
static PFNGLUNIFORM1IPROC vid_glUniform1i;
    #define glUniform1i vid_glUniform1i
static PFNGLGENVERTEXARRAYSPROC vid_glGenVertexArrays;
    #define glGenVertexArrays vid_glGenVertexArrays
static PFNGLBINDVERTEXARRAYPROC vid_glBindVertexArray;
    #define glBindVertexArray vid_glBindVertexArray
static PFNGLDELETEVERTEXARRAYSPROC vid_glDeleteVertexArrays;
    #define glDeleteVertexArrays vid_glDeleteVertexArrays
static PFNGLGENBUFFERSPROC vid_glGenBuffers;
    #define glGenBuffers vid_glGenBuffers
static PFNGLBINDBUFFERPROC vid_glBindBuffer;
    #define glBindBuffer vid_glBindBuffer
static PFNGLBUFFERDATAPROC vid_glBufferData;
    #define glBufferData vid_glBufferData
static PFNGLBUFFERSUBDATAPROC vid_glBufferSubData;
    #define glBufferSubData vid_glBufferSubData
static PFNGLDELETEBUFFERSPROC vid_glDeleteBuffers;
    #define glDeleteBuffers vid_glDeleteBuffers
static PFNGLDISABLEVERTEXATTRIBARRAYPROC vid_glDisableVertexAttribArray;
    #define glDisableVertexAttribArray vid_glDisableVertexAttribArray
static PFNGLENABLEVERTEXATTRIBARRAYPROC vid_glEnableVertexAttribArray;
    #define glEnableVertexAttribArray vid_glEnableVertexAttribArray
static PFNGLVERTEXATTRIBIPOINTERPROC vid_glVertexAttribIPointer;
    #define glVertexAttribIPointer vid_glVertexAttribIPointer
static PFNGLVERTEXATTRIBPOINTERPROC vid_glVertexAttribPointer;
    #define glVertexAttribPointer vid_glVertexAttribPointer
static PFNGLACTIVETEXTUREPROC vid_glActiveTexture;
    #define glActiveTexture vid_glActiveTexture
static PFNGLBLENDEQUATIONPROC vid_glBlendEquation;
    #define glBlendEquation vid_glBlendEquation
static PFNGLGENFRAMEBUFFERSPROC vid_glGenFramebuffers;
    #define glGenFramebuffers vid_glGenFramebuffers
static PFNGLBINDFRAMEBUFFERPROC vid_glBindFramebuffer;
    #define glBindFramebuffer vid_glBindFramebuffer
static PFNGLDELETEFRAMEBUFFERSPROC vid_glDeleteFramebuffers;
    #define glDeleteFramebuffers vid_glDeleteFramebuffers
static PFNGLFRAMEBUFFERTEXTURE2DPROC vid_glFramebufferTexture2D;
    #define glFramebufferTexture2D vid_glFramebufferTexture2D
static PFNGLFRAMEBUFFERTEXTURELAYERPROC vid_glFramebufferTextureLayer;
    #define glFramebufferTextureLayer vid_glFramebufferTextureLayer
static PFNGLCHECKFRAMEBUFFERSTATUSPROC vid_glCheckFramebufferStatus;
    #define glCheckFramebufferStatus vid_glCheckFramebufferStatus
static PFNGLGENRENDERBUFFERSPROC vid_glGenRenderbuffers;
    #define glGenRenderbuffers vid_glGenRenderbuffers
static PFNGLBINDRENDERBUFFERPROC vid_glBindRenderbuffer;
    #define glBindRenderbuffer vid_glBindRenderbuffer
static PFNGLDELETERENDERBUFFERSPROC vid_glDeleteRenderbuffers;
    #define glDeleteRenderbuffers vid_glDeleteRenderbuffers
static PFNGLRENDERBUFFERSTORAGEPROC vid_glRenderbufferStorage;
    #define glRenderbufferStorage vid_glRenderbufferStorage
static PFNGLFRAMEBUFFERRENDERBUFFERPROC vid_glFramebufferRenderbuffer;
    #define glFramebufferRenderbuffer vid_glFramebufferRenderbuffer
static PFNGLBLITFRAMEBUFFERPROC vid_glBlitFramebuffer;
    #define glBlitFramebuffer vid_glBlitFramebuffer

static PROC vid_proc(const char *name)
{
    PROC address = wglGetProcAddress(name);
    uintptr_t value = (uintptr_t)address;
    if (!address || value <= 3 || value == UINTPTR_MAX)
        address = GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
    return address;
}

static sint32 vid_load_gl(void)
{
    vid_glTexImage3D = (PFNGLTEXIMAGE3DPROC)vid_proc("glTexImage3D");
    vid_glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)vid_proc("glTexSubImage3D");
    if (!vid_glTexImage3D || !vid_glTexSubImage3D)
        return 0;
    vid_glGenerateMipmap = (PFNGLGENERATEMIPMAPPROC)vid_proc("glGenerateMipmap");
    vid_glGetStringi = (PFNGLGETSTRINGIPROC)vid_proc("glGetStringi");
    if (!vid_glGenerateMipmap || !vid_glGetStringi)
        return 0;
    vid_glCreateShader = (PFNGLCREATESHADERPROC)vid_proc("glCreateShader");
    if (!vid_glCreateShader)
        return 0;
    vid_glShaderSource = (PFNGLSHADERSOURCEPROC)vid_proc("glShaderSource");
    if (!vid_glShaderSource)
        return 0;
    vid_glCompileShader = (PFNGLCOMPILESHADERPROC)vid_proc("glCompileShader");
    if (!vid_glCompileShader)
        return 0;
    vid_glGetShaderiv = (PFNGLGETSHADERIVPROC)vid_proc("glGetShaderiv");
    if (!vid_glGetShaderiv)
        return 0;
    vid_glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)vid_proc("glGetShaderInfoLog");
    if (!vid_glGetShaderInfoLog)
        return 0;
    vid_glDeleteShader = (PFNGLDELETESHADERPROC)vid_proc("glDeleteShader");
    if (!vid_glDeleteShader)
        return 0;
    vid_glCreateProgram = (PFNGLCREATEPROGRAMPROC)vid_proc("glCreateProgram");
    if (!vid_glCreateProgram)
        return 0;
    vid_glAttachShader = (PFNGLATTACHSHADERPROC)vid_proc("glAttachShader");
    if (!vid_glAttachShader)
        return 0;
    vid_glLinkProgram = (PFNGLLINKPROGRAMPROC)vid_proc("glLinkProgram");
    if (!vid_glLinkProgram)
        return 0;
    vid_glGetProgramiv = (PFNGLGETPROGRAMIVPROC)vid_proc("glGetProgramiv");
    if (!vid_glGetProgramiv)
        return 0;
    vid_glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)vid_proc("glGetProgramInfoLog");
    if (!vid_glGetProgramInfoLog)
        return 0;
    vid_glDeleteProgram = (PFNGLDELETEPROGRAMPROC)vid_proc("glDeleteProgram");
    if (!vid_glDeleteProgram)
        return 0;
    vid_glUseProgram = (PFNGLUSEPROGRAMPROC)vid_proc("glUseProgram");
    if (!vid_glUseProgram)
        return 0;
    vid_glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)vid_proc("glGetUniformLocation");
    if (!vid_glGetUniformLocation)
        return 0;
    vid_glClearBufferfv = (PFNGLCLEARBUFFERFVPROC)vid_proc("glClearBufferfv");
    vid_glDrawBuffers = (PFNGLDRAWBUFFERSPROC)vid_proc("glDrawBuffers");
    vid_glGetActiveUniform = (PFNGLGETACTIVEUNIFORMPROC)vid_proc("glGetActiveUniform");
    if (!vid_glDrawBuffers || !vid_glGetActiveUniform || !vid_glClearBufferfv)
        return 0;
    vid_glUniform4fv = (PFNGLUNIFORM4FVPROC)vid_proc("glUniform4fv");
    if (!vid_glUniform4fv)
        return 0;
    vid_glUniform1i = (PFNGLUNIFORM1IPROC)vid_proc("glUniform1i");
    if (!vid_glUniform1i)
        return 0;
    vid_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)vid_proc("glGenVertexArrays");
    if (!vid_glGenVertexArrays)
        return 0;
    vid_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)vid_proc("glBindVertexArray");
    if (!vid_glBindVertexArray)
        return 0;
    vid_glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)vid_proc("glDeleteVertexArrays");
    if (!vid_glDeleteVertexArrays)
        return 0;
    vid_glGenBuffers = (PFNGLGENBUFFERSPROC)vid_proc("glGenBuffers");
    if (!vid_glGenBuffers)
        return 0;
    vid_glBindBuffer = (PFNGLBINDBUFFERPROC)vid_proc("glBindBuffer");
    if (!vid_glBindBuffer)
        return 0;
    vid_glBufferData = (PFNGLBUFFERDATAPROC)vid_proc("glBufferData");
    vid_glBufferSubData = (PFNGLBUFFERSUBDATAPROC)vid_proc("glBufferSubData");
    if (!vid_glBufferData || !vid_glBufferSubData)
        return 0;
    vid_glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)vid_proc("glDeleteBuffers");
    if (!vid_glDeleteBuffers)
        return 0;
    vid_glDisableVertexAttribArray = (PFNGLDISABLEVERTEXATTRIBARRAYPROC)vid_proc("glDisableVertexAttribArray");
    if (!vid_glDisableVertexAttribArray)
        return 0;
    vid_glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)vid_proc("glEnableVertexAttribArray");
    if (!vid_glEnableVertexAttribArray)
        return 0;
    vid_glVertexAttribIPointer = (PFNGLVERTEXATTRIBIPOINTERPROC)vid_proc("glVertexAttribIPointer");
    if (!vid_glVertexAttribIPointer)
        return 0;
    vid_glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)vid_proc("glVertexAttribPointer");
    if (!vid_glVertexAttribPointer)
        return 0;
    vid_glActiveTexture = (PFNGLACTIVETEXTUREPROC)vid_proc("glActiveTexture");
    if (!vid_glActiveTexture)
        return 0;
    vid_glBlendEquation = (PFNGLBLENDEQUATIONPROC)vid_proc("glBlendEquation");
    if (!vid_glBlendEquation)
        return 0;
    vid_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)vid_proc("glGenFramebuffers");
    vid_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)vid_proc("glBindFramebuffer");
    vid_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)vid_proc("glDeleteFramebuffers");
    vid_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)vid_proc("glFramebufferTexture2D");
    vid_glFramebufferTextureLayer = (PFNGLFRAMEBUFFERTEXTURELAYERPROC)vid_proc("glFramebufferTextureLayer");
    vid_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)vid_proc("glCheckFramebufferStatus");
    vid_glGenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)vid_proc("glGenRenderbuffers");
    vid_glBindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)vid_proc("glBindRenderbuffer");
    vid_glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)vid_proc("glDeleteRenderbuffers");
    vid_glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)vid_proc("glRenderbufferStorage");
    vid_glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)vid_proc("glFramebufferRenderbuffer");
    vid_glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)vid_proc("glBlitFramebuffer");
    if (!vid_glGenFramebuffers || !vid_glBindFramebuffer || !vid_glDeleteFramebuffers || !vid_glFramebufferTexture2D || !vid_glFramebufferTextureLayer || !vid_glCheckFramebufferStatus || !vid_glGenRenderbuffers || !vid_glBindRenderbuffer || !vid_glDeleteRenderbuffers || !vid_glRenderbufferStorage || !vid_glFramebufferRenderbuffer || !vid_glBlitFramebuffer)
        return 0;
    return 1;
}
#endif

static const char *vid_vertex_source = VID_GLSL "layout(location=0) in vec4 aPos;\n"
                                                "layout(location=1) in vec2 aUV;\n"
                                                "layout(location=2) in vec4 aColor;\n"
                                                "out vec2 vUV; out vec4 vColor; out float vDepth; out vec2 vFog; uniform vec4 uFog[3];\n"
                                                "void main(){gl_Position=aPos;vUV=aUV;vColor=aColor;vDepth=aPos.w;vFog=vec2(0.0);"
                                                "if(uFog[0].y>uFog[0].x){"
                                                "float d=length(vec3(aPos.x*uFog[2].x,-aPos.y*uFog[2].y,aPos.w));"
                                                "vFog=clamp(vec2(log(max(d,uFog[0].x)/uFog[0].x)/log(uFog[0].y/uFog[0].x),"
                                                "log(max(d,uFog[0].y)/uFog[0].y)/log(uFog[0].z/uFog[0].y)),0.0,1.0);vFog*=vFog;}}\n";

static GLuint vid_program, vid_array, vid_blit_buffer, vid_blit_texture, vid_empty_shadow, vid_empty_2d;
static GLint vid_textured;
static uint8 *vid_blit_pixels;
static size_t vid_blit_capacity, vid_blit_storage;
static sint32 vid_blit_width, vid_blit_height;

static GLuint vid_compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint success;
    char log[1024];
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        log[sizeof(log) - 1] = 0;
        xport_message_error("VID shader", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void vid_context_free(void)
{
    if (vid_program)
        glDeleteProgram(vid_program);
    if (vid_array)
        glDeleteVertexArrays(1, &vid_array);
    if (vid_blit_buffer)
        glDeleteBuffers(1, &vid_blit_buffer);
    if (vid_blit_texture)
        glDeleteTextures(1, &vid_blit_texture);
    if (vid_empty_shadow)
        glDeleteTextures(1, &vid_empty_shadow);
    if (vid_empty_2d)
        glDeleteTextures(1, &vid_empty_2d);
    vid_program = vid_array = vid_blit_buffer = vid_blit_texture = vid_empty_shadow = vid_empty_2d = 0;
    free(vid_blit_pixels);
    vid_blit_pixels = NULL;
    vid_blit_capacity = vid_blit_storage = 0;
    vid_blit_width = vid_blit_height = 0;
#if defined(AP_WEB)
    if (vid_context)
        emscripten_webgl_destroy_context(vid_context);
#else
    wglMakeCurrent(NULL, NULL);
    if (vid_context)
        wglDeleteContext(vid_context);
    if (vid_device)
        ReleaseDC(vid_window, vid_device);
    vid_device = NULL;
    vid_window = NULL;
#endif
    vid_context = 0;
}

static sint32 vid_context_init(void *window)
{
    GLuint vertex, fragment;
    GLint success;
    char log[1024];
    const char *fragment_source = VID_GLSL "in vec2 vUV; in vec4 vColor;\n"
                                           "uniform sampler2D uTex; uniform int uTextured;\n"
                                           "layout(location=0) out vec4 outColor;\n"
                                           "void main(){vec4 c=vColor; if(uTextured!=0)c*=texture(uTex,vUV);"
                                           "if(c.a<0.001)discard;outColor=c;}\n";
#if defined(AP_WEB)
    EmscriptenWebGLContextAttributes attributes;
    (void)window;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    attributes.alpha = EM_FALSE;
    attributes.depth = EM_TRUE;
    attributes.antialias = EM_FALSE;
    vid_context = emscripten_webgl_create_context("#canvas", &attributes);
    if (vid_context <= 0 || emscripten_webgl_make_context_current(vid_context) != EMSCRIPTEN_RESULT_SUCCESS)
    {
        vid_context = 0;
        return 0;
    }
#else
    PIXELFORMATDESCRIPTOR format = {0};
    PFNWGLCREATECONTEXTATTRIBSARBPROC create_context;
    HGLRC legacy;
    int pixel_format;
    const int attributes[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    vid_window = window;
    vid_device = GetDC(vid_window);
    if (!vid_device)
        return 0;
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 24;
    format.cDepthBits = 24;
    pixel_format = ChoosePixelFormat(vid_device, &format);
    if (!pixel_format || !SetPixelFormat(vid_device, pixel_format, &format))
    {
        vid_context_free();
        return 0;
    }
    legacy = wglCreateContext(vid_device);
    if (!legacy || !wglMakeCurrent(vid_device, legacy))
    {
        if (legacy)
            wglDeleteContext(legacy);
        vid_context_free();
        return 0;
    }
    create_context = (PFNWGLCREATECONTEXTATTRIBSARBPROC)vid_proc("wglCreateContextAttribsARB");
    vid_context = create_context ? create_context(vid_device, NULL, attributes) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!vid_context || !wglMakeCurrent(vid_device, vid_context) || !vid_load_gl())
    {
        vid_context_free();
        return 0;
    }
    {
        PFNWGLSWAPINTERVALEXTPROC swap_interval = (PFNWGLSWAPINTERVALEXTPROC)vid_proc("wglSwapIntervalEXT");
        if (swap_interval)
            swap_interval(1);
    }
#endif
    vertex = vid_compile_shader(GL_VERTEX_SHADER, vid_vertex_source);
    fragment = vid_compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment)
    {
        if (vertex)
            glDeleteShader(vertex);
        if (fragment)
            glDeleteShader(fragment);
        vid_context_free();
        return 0;
    }
    vid_program = glCreateProgram();
    glAttachShader(vid_program, vertex);
    glAttachShader(vid_program, fragment);
    glLinkProgram(vid_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glGetProgramiv(vid_program, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(vid_program, sizeof(log), NULL, log);
        log[sizeof(log) - 1] = 0;
        xport_message_error("VID program", log);
        vid_context_free();
        return 0;
    }
    glUseProgram(vid_program);
    vid_textured = glGetUniformLocation(vid_program, "uTextured");
    glUniform1i(glGetUniformLocation(vid_program, "uTex"), 0);
    glGenVertexArrays(1, &vid_array);
    glGenBuffers(1, &vid_blit_buffer);
    glGenTextures(1, &vid_blit_texture);
    glDepthFunc(GL_LEQUAL);
    if (glGetError() != GL_NO_ERROR)
    {
        vid_context_free();
        return 0;
    }
    return 1;
}

static void vid_update_size(sint32 *width, sint32 *height)
{
#if defined(AP_WEB)
    double css_width, css_height;
    double scale = emscripten_get_device_pixel_ratio();
    int old_width, old_height;
    if (emscripten_get_element_css_size("#canvas", &css_width, &css_height) != EMSCRIPTEN_RESULT_SUCCESS)
    {
        *width = *height = 0;
        return;
    }
    *width = (sint32)(css_width * scale);
    *height = (sint32)(css_height * scale);
    emscripten_get_canvas_element_size("#canvas", &old_width, &old_height);
    if (*width > 0 && *height > 0 && (*width != old_width || *height != old_height))
        emscripten_set_canvas_element_size("#canvas", *width, *height);
#else
    RECT rect;
    *width = *height = 0;
    if (GetClientRect(vid_window, &rect))
    {
        *width = rect.right - rect.left;
        *height = rect.bottom - rect.top;
    }
#endif
}

static uint32 vid_allocation_failed;

static sint32 vid_render_status(void)
{
#if defined(AP_WEB) && defined(NDEBUG)
    return !vid_allocation_failed && !emscripten_is_webgl_context_lost(vid_context);
#else
    return !vid_allocation_failed && glGetError() == GL_NO_ERROR;
#endif
}

static VID_STATS vid_stats;

void vid_stats_read(VID_STATS *stats)
{
    if (stats)
        *stats = vid_stats;
}

uint64 vid_shader_draw_count(const VID_SHADER *shader)
{
    return shader ? shader->draw_count : 0;
}

static void vid_buffer_upload(GLenum target, size_t bytes, const void *data, GLenum usage, size_t *storage, size_t capacity)
{
    if (*storage < capacity)
    {
        glBufferData(target, (GLsizeiptr)capacity, NULL, usage);
        if (glGetError() != GL_NO_ERROR)
        {
            vid_allocation_failed = 1;
            return;
        }
        *storage = capacity;
    }
    glBufferSubData(target, 0, (GLsizeiptr)bytes, data);
    if (target == GL_ARRAY_BUFFER)
        vid_stats.vertex_bytes += bytes;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        vid_stats.index_bytes += bytes;
}

static sint32 vid_swap(void)
{
#if defined(AP_WEB)
    return vid_render_status();
#else
    return !vid_allocation_failed && SwapBuffers(vid_device) && glGetError() == GL_NO_ERROR;

#endif
}

static void vid_vertices(GLuint buffer, uint32 stride, GLuint array)
{
    glBindVertexArray(array);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    if (stride == sizeof(VID_VERTEX2D))
    {
        for (uint32 attr = 0; attr < 7; ++attr)
            glEnableVertexAttribArray(attr);
        glDisableVertexAttribArray(3);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX2D, pos));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX2D, uv));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX2D, color));
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX2D, alpha));
        glVertexAttribIPointer(5, 4, GL_UNSIGNED_SHORT, stride, (const void *)offsetof(VID_VERTEX2D, tex));
        glVertexAttribIPointer(6, 2, GL_UNSIGNED_SHORT, stride, (const void *)offsetof(VID_VERTEX2D, rect));
        return;
    }
    if (stride == sizeof(VID_VERTEX16))
    {
        for (uint32 attr = 0; attr < 7; ++attr)
            glEnableVertexAttribArray(attr);
        glVertexAttribPointer(0, 4, GL_SHORT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX16, pos));
        glVertexAttribPointer(1, 2, GL_SHORT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX16, uv));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX16, color));
        glVertexAttribPointer(3, 4, GL_SHORT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX16, n));
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX16, extra));
        glVertexAttribIPointer(5, 4, GL_UNSIGNED_SHORT, stride, (const void *)offsetof(VID_VERTEX16, tex));
        glVertexAttribIPointer(6, 2, GL_UNSIGNED_SHORT, stride, (const void *)offsetof(VID_VERTEX16, rect));
        return;
    }
    glDisableVertexAttribArray(5);
    glDisableVertexAttribArray(6);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX, uv));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX, color));
    if (stride == sizeof(VID_VERTEX3D))
    {
        glEnableVertexAttribArray(3);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX3D, n));
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (const void *)offsetof(VID_VERTEX3D, extra));
    }
    else
    {
        glDisableVertexAttribArray(3);
        glDisableVertexAttribArray(4);
    }
}

static float vid_anisotropy_limit;

static float vid_anisotropy(void)
{
    // cache capabilities for this context, reset before creating another context
    float maximum = 1.0f;
    if (vid_anisotropy_limit > 0.0f)
        return vid_anisotropy_limit;
#if AP_WEB
    if (emscripten_webgl_enable_extension(vid_context, "EXT_texture_filter_anisotropic"))
        glGetFloatv(0x84FF, &maximum);
#else
    GLint count, index;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (index = 0; index < count; ++index)
    {
        const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);
        if (!strcmp(extension, "GL_EXT_texture_filter_anisotropic") || !strcmp(extension, "GL_ARB_texture_filter_anisotropic"))
        {
            glGetFloatv(0x84FF, &maximum);
            break;
        }
    }
#endif
    vid_anisotropy_limit = maximum < 8.0f ? maximum : 8.0f;
    return vid_anisotropy_limit;
}

typedef struct VID_FORMAT_INFO
{
    GLenum internal, external, type;
    uint32 bytes, depth, nearest, comparison;
} VID_FORMAT_INFO;

static const VID_FORMAT_INFO vid_formats[] = {{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 0, 0, 0}, {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, 0, 0, 0}, {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1, 0, 1, 0}, {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, 8, 0, 1, 0}, {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, 1, 0, 1}, {GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, 0, 1, 0}, {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, 0, 0, 0}, {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, 1, 1, 0}, {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 0, 0, 0}};

static const VID_FORMAT_INFO *vid_format(uint32 format)
{
    if (format >= sizeof(vid_formats) / sizeof(vid_formats[0]))
        abort();
    return &vid_formats[format];
}

static void vid_tex_upload(VID_TEXTURE *texture, uint32 filter, uint32 repeat)
{
    GLenum target = texture->cube ? GL_TEXTURE_CUBE_MAP : texture->array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    const VID_FORMAT_INFO *info = vid_format(texture->format);
    if (!texture->object)
        glGenTextures(1, &texture->object);
    glBindTexture(target, texture->object);
    ++vid_stats.texture_binds;
    if (texture->uploaded != texture->version)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        GLenum format = info->external, type = info->type;
        if (!texture->allocated)
        {
            if (texture->array)
                glTexImage3D(target, 0, info->internal, texture->width, texture->height, texture->layers, 0, format, type, texture->pixels);
            else
                glTexImage2D(target, 0, info->internal, texture->width, texture->height, 0, format, type, texture->pixels);
            texture->allocated = 1;
            if (glGetError() != GL_NO_ERROR)
                vid_allocation_failed = 1;
        }
        else
        {
            uint32 stride = texture->width * info->bytes;
            if (texture->array)
                glTexSubImage3D(target, 0, 0, 0, 0, texture->width, texture->height, texture->layers, format, type, texture->pixels);
            else
                glTexSubImage2D(target, 0, 0, texture->dirty_first, texture->width, texture->dirty_last - texture->dirty_first, format, type, texture->pixels + texture->dirty_first * stride);
        }
        texture->uploaded = texture->version;
        if (texture->immutable)
        {
            free(texture->pixels);
            texture->pixels = NULL;
        }
    }
    if (info->nearest)
        filter = repeat = 0;
    if (info->depth && !info->comparison)
    {
        filter = repeat = 0;
        if (!texture->compare_ready)
            glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        texture->compare_ready = 1;
    }
    if (info->comparison)
    {
        filter = VID_FILTER_LINEAR;
        repeat = 0;
        if (!texture->compare_ready)
        {
            glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            texture->compare_ready = 1;
        }
    }
    uint32 sampler = 8 | (filter << 1) | (repeat ? 1 : 0);
    if ((filter == VID_FILTER_ANISOTROPIC || filter == VID_FILTER_TRILINEAR) && texture->mip_version != texture->version)
    {
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, texture->mip_levels ? (GLint)texture->mip_levels - 1 : 1000);
        glGenerateMipmap(target);
        texture->mip_version = texture->version;
    }
    if (!(texture->sampler_flags & 8) || ((texture->sampler_flags ^ sampler) & 6))
    {
        float anisotropy = vid_anisotropy();
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, (filter == VID_FILTER_ANISOTROPIC || filter == VID_FILTER_TRILINEAR) ? GL_LINEAR_MIPMAP_LINEAR : filter ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
        if (anisotropy > 1.0f)
            glTexParameterf(target, 0x84FE, filter == VID_FILTER_ANISOTROPIC ? anisotropy : 1.0f);
    }
    if (!(texture->sampler_flags & 8) || ((texture->sampler_flags ^ sampler) & 1))
    {
        glTexParameteri(target, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        if (texture->cube)
            glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }
    texture->sampler_flags = sampler;
}

static sint32 vid_target_storage(VID_TARGET *target, uint32 width, uint32 height)
{
    VID_TEXTURE *texture = target->texture;
    uint32 count = texture->cube ? 6 : texture->array ? texture->layers : 1;
    if (texture->draw_refs || (target->depth_texture && target->depth_texture->draw_refs) || (target->surface_texture && target->surface_texture->draw_refs) || !width || !height || target->face >= count)
        return 0;
    if (target->storage_valid && texture->allocated && width == texture->width && height == texture->height)
    {
        target->framebuffer = target->framebuffers[target->face];
        glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
        if (target->surface_texture)
        {
            GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glDrawBuffers(2, buffers);
        }
        return 1;
    }
    target->storage_valid = target->ready = target->completed_faces = 0;
    target->mip_storage_levels = 1;
    target->mip_fbo_valid = 0;
    target->mip_written = target->mip_base_version = 0;
    if (!target->framebuffers)
    {
        target->framebuffers = calloc(count, sizeof(*target->framebuffers));
        if (!target->framebuffers)
            return 0;
        target->framebuffer_count = count;
        glGenFramebuffers(count, target->framebuffers);
    }
    if (!target->depth && !target->depth_texture && texture->format != VID_DEPTH24)
        glGenRenderbuffers(1, &target->depth);
    if (!texture->object)
        glGenTextures(1, &texture->object);
    glBindTexture(texture->cube ? GL_TEXTURE_CUBE_MAP : texture->array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D, texture->object);
    const VID_FORMAT_INFO *info = vid_format(texture->format);
    if (texture->cube)
    {
        for (uint32 face = 0; face < 6; ++face)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, info->internal, width, height, 0, info->external, info->type, NULL);
    }
    else if (texture->array)
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, info->internal, width, height, texture->layers, 0, info->external, info->type, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, info->internal, width, height, 0, info->external, info->type, NULL);
    if (target->depth_texture)
    {
        VID_TEXTURE *depth = target->depth_texture;
        if (!depth->object)
            glGenTextures(1, &depth->object);
        glBindTexture(depth->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, depth->object);
        for (uint32 face = 0; face < (depth->cube ? 6u : 1u); ++face)
            glTexImage2D(depth->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D, 0, vid_format(depth->format)->internal, width, height, 0, vid_format(depth->format)->external, vid_format(depth->format)->type, NULL);
        depth->width = width;
        depth->height = height;
        depth->allocated = 1;
        depth->uploaded = depth->version;
        depth->sampler_flags = 0;
    }
    else if (texture->format != VID_DEPTH24)
    {
        glBindRenderbuffer(GL_RENDERBUFFER, target->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    }
    if (target->surface_texture)
    {
        VID_TEXTURE *surface = target->surface_texture;
        if (!surface->object)
            glGenTextures(1, &surface->object);
        glBindTexture(GL_TEXTURE_2D, surface->object);
        glTexImage2D(GL_TEXTURE_2D, 0, vid_format(surface->format)->internal, width, height, 0, vid_format(surface->format)->external, vid_format(surface->format)->type, NULL);
        surface->width = width;
        surface->height = height;
        surface->allocated = 1;
        surface->uploaded = surface->version;
        surface->sampler_flags = 0;
    }
    for (uint32 face = 0; face < count; ++face)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffers[face]);
        if (texture->format == VID_DEPTH24)
        {
            GLenum none = GL_NONE;
            if (texture->array)
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture->object, 0, face);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture->object, 0);
            glDrawBuffers(1, &none);
            glReadBuffer(GL_NONE);
        }
        else
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D, texture->object, 0);
            if (target->depth_texture)
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target->depth_texture->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D, target->depth_texture->object, 0);
            else
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target->depth);
        }
        if (target->surface_texture)
        {
            GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, target->surface_texture->object, 0);
            glDrawBuffers(2, buffers);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return 0;
    }
    if (glGetError() != GL_NO_ERROR)
        return 0;
    texture->width = width;
    texture->height = height;
    texture->allocated = 1;
    texture->uploaded = texture->version;
    target->storage_valid = 1;
    target->framebuffer = target->framebuffers[target->face];
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    return 1;
}

static uint32 vid_texture_mip_count(const VID_TEXTURE *texture)
{
    uint32 levels = 1, size = texture->width > texture->height ? texture->width : texture->height;
    for (; size > 1; size >>= 1)
        ++levels;
    return texture->mip_levels && texture->mip_levels < levels ? texture->mip_levels : levels;
}

static sint32 vid_target_mip_storage(VID_TARGET *target, uint32 level)
{
    VID_TEXTURE *texture = target->texture;
    const VID_FORMAT_INFO *info = vid_format(texture->format);
    uint32 levels = vid_texture_mip_count(texture);
    if ((target->mip_fbo_valid & (1u << level)) && target->mip_storage_levels == levels)
    {
        target->framebuffer = target->mip_framebuffers[level];
        glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
        return 1;
    }
    glBindTexture(GL_TEXTURE_2D, texture->object);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    for (uint32 index = target->mip_storage_levels; index < levels; ++index)
    {
        uint32 width = texture->width >> index, height = texture->height >> index;
        glTexImage2D(GL_TEXTURE_2D, index, info->internal, width ? width : 1, height ? height : 1, 0, info->external, info->type, NULL);
    }
    target->mip_storage_levels = levels;
    if (!target->mip_framebuffers[level])
        glGenFramebuffers(1, &target->mip_framebuffers[level]);
    target->framebuffer = target->mip_framebuffers[level];
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->object, level);
    GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR)
        return 0;
    target->mip_fbo_valid |= 1u << level;
    return 1;
}

static void vid_target_mip_written(VID_TARGET *target, uint32 level)
{
    VID_TEXTURE *texture = target->texture;
    if (target->mip_base_version != texture->version)
    {
        target->mip_base_version = texture->version;
        target->mip_written = 1;
    }
    target->mip_written |= 1u << level;
    // explicit mip rendering owns this chain; later passes may sample written levels
    texture->mip_version = texture->version;
}

static sint32 vid_render(const VID_DRAW *draws, const float *clear, sint32 frame_width, sint32 frame_height)
{
    sint32 width, height;
    GLint previous_read = 0, previous_draw = 0;

    const VID_MATERIAL_DATA *pass = NULL;
    // only this uninterrupted queue owns GL state; discard all knowledge after the flush
    VID_RECT applied_viewport = {0};
    uint32 viewport_valid = 0, applied_cull = UINT32_MAX, applied_depth = UINT32_MAX, applied_surface = UINT32_MAX;
    uint32 applied_write = UINT32_MAX, applied_blend = UINT32_MAX;
    uint32 applied_offset = UINT32_MAX;
    float applied_offset_slope = 0, applied_offset_units = 0;
    GLuint applied_program = 0, applied_array = 0, applied_indices = UINT32_MAX;
    uint32 active_unit = 0, uniform_valid = 0, params_valid = 0, fog_valid = 0, texture_valid = 0;
    sint32 applied_textured = -1;
    uint32 applied_uniform_count = 0;
    const float *applied_uniform_snapshot = NULL;
    float applied_uniforms[VID_UNIFORM_VECTORS * 4], applied_params[8], applied_fog[12];
    VID_TEXTURE *applied_textures[VID_AUX_TEXTURES + 1] = {0};
    uint32 missing_shadow_mask = 0, missing_2d_mask = 0;
    uint32 texture_filters[VID_AUX_TEXTURES + 1] = {0}, texture_repeats[VID_AUX_TEXTURES + 1] = {0};
    vid_update_size(&width, &height);
    if (width <= 0 || height <= 0)
        return 1;
    if (frame_target)
    {
#if !defined(AP_WEB) || !defined(NDEBUG)
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
#endif
        if (frame_target->fixed_width)
        {
            width = (sint32)frame_target->fixed_width;
            height = (sint32)frame_target->fixed_height;
        }
        if (frame_target_mip)
        {
            width = frame_target->texture->width;
            height = frame_target->texture->height;
        }
        if (!vid_target_storage(frame_target, width, height) || (frame_target_mip && !vid_target_mip_storage(frame_target, frame_target_mip)))
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
            if (frame_target_mip)
                frame_target->framebuffer = frame_target->framebuffers[frame_target->face];
            return 0;
        }
        if (frame_target_mip)
        {
            width >>= frame_target_mip;
            height >>= frame_target_mip;
            if (!width)
                width = 1;
            if (!height)
                height = 1;
        }
        frame_target->ready = 0;
    }
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(clear[0], clear[1], clear[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (frame_target && !frame_target_mip && frame_target->surface_texture)
    {
        const float zero[4] = {0};
        glClearBufferfv(GL_COLOR, 1, zero);
    }
    glUseProgram(vid_program);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    while (draws)
    {
        if (draws->pass == 4)
        {
            if (frame_target->texture->cube)
                frame_target->completed_faces |= 1u << frame_target->face;
            frame_target->face = draws->first;
            frame_target->framebuffer = frame_target->framebuffers[frame_target->face];
            glBindFramebuffer(GL_FRAMEBUFFER, frame_target->framebuffer);
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClear(GL_DEPTH_BUFFER_BIT | (frame_target->texture->format == VID_DEPTH24 ? 0 : GL_COLOR_BUFFER_BIT));
            glEnable(GL_SCISSOR_TEST);
            applied_write = 1;
            pass = NULL;
            draws = draws->next;
            continue;
        }
        VID_GEOMETRY *geometry = draws->geometry;
        const VID_MATERIAL_DATA *material = draws->pass == 2 ? pass : &draws->data;
        VID_RECT viewport = draws->viewport;
        if (width != frame_width || height != frame_height)
        {
            sint32 right = (sint32)(((sint64)viewport.x + viewport.w) * width / frame_width);
            sint32 bottom = (sint32)(((sint64)viewport.y + viewport.h) * height / frame_height);
            viewport.x = (sint32)((sint64)viewport.x * width / frame_width);
            viewport.y = (sint32)((sint64)viewport.y * height / frame_height);
            viewport.w = right - viewport.x;
            viewport.h = bottom - viewport.y;
        }
        if (draws->pass != 2 && (!viewport_valid || memcmp(&applied_viewport, &viewport, sizeof(viewport)) != 0))
        {
            glViewport(viewport.x, height - viewport.y - viewport.h, viewport.w, viewport.h);
            glScissor(viewport.x, height - viewport.y - viewport.h, viewport.w, viewport.h);
            applied_viewport = viewport;
            viewport_valid = 1;
        }
        if (draws->sample)
        {
            uint8 color[4];
            sint32 x = viewport.x + (sint32)(draws->sample_x * viewport.w);
            sint32 y = viewport.y + (sint32)(draws->sample_y * viewport.h);
            if (x >= 0 && x < width && y >= 0 && y < height)
            {
                glReadPixels(x, height - y - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color);
                draws->sample->color = (uint32)color[0] << 16 | (uint32)color[1] << 8 | color[2];
            }
            draws = draws->next;
            continue;
        }
        if (draws->pass == 3)
        {
            if (applied_write != 1)
                glDepthMask(GL_TRUE);
            applied_write = 1;
            glClear(GL_DEPTH_BUFFER_BIT);
            pass = NULL;
            draws = draws->next;
            continue;
        }
        if (draws->pass != 2)
        {
            uint32 blend = material->blend;
            GLuint program = material->shader ? material->shader->program : vid_program;
            sint32 textured = material->texture != NULL;
            if (frame_target && frame_target->surface_texture && applied_surface != (material->surface_write != 0))
            {
                GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
                glDrawBuffers(material->surface_write ? 2 : 1, buffers);
                applied_surface = material->surface_write != 0;
            }
            if (applied_cull != material->cull)
            {
                ++vid_stats.state_changes;
                if (material->cull)
                {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                    glFrontFace(material->cull == 1 ? GL_CW : GL_CCW);
                }
                else
                    glDisable(GL_CULL_FACE);
                applied_cull = material->cull;
            }
            if (applied_depth != (material->depth_test != 0))
            {
                ++vid_stats.state_changes;
                if (material->depth_test)
                    glEnable(GL_DEPTH_TEST);
                else
                    glDisable(GL_DEPTH_TEST);
                applied_depth = material->depth_test != 0;
            }
            if (applied_write != (material->depth_write != 0))
            {
                ++vid_stats.state_changes;
                glDepthMask(material->depth_write ? GL_TRUE : GL_FALSE);
                applied_write = material->depth_write != 0;
            }
            uint32 offset = material->depth_bias_slope != 0 || material->depth_bias_units != 0;
            if (applied_offset != offset)
            {
                ++vid_stats.state_changes;
                if (offset)
                    glEnable(GL_POLYGON_OFFSET_FILL);
                else
                    glDisable(GL_POLYGON_OFFSET_FILL);
            }
            if (offset && (applied_offset == UINT32_MAX || applied_offset_slope != material->depth_bias_slope || applied_offset_units != material->depth_bias_units))
            {
                glPolygonOffset(material->depth_bias_slope, material->depth_bias_units);
                applied_offset_slope = material->depth_bias_slope;
                applied_offset_units = material->depth_bias_units;
            }
            applied_offset = offset;
            if (!blend && material->shader && draws->fog[1] > draws->fog[0])
                blend = VID_BLEND_ALPHA;
            if (applied_blend != blend)
            {
                ++vid_stats.state_changes;
                if (blend)
                {
                    glEnable(GL_BLEND);
                    glBlendEquation(blend == VID_BLEND_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
                    glBlendFunc(blend == VID_BLEND_PREMULTIPLIED ? GL_ONE : GL_SRC_ALPHA, blend == VID_BLEND_ALPHA || blend == VID_BLEND_PREMULTIPLIED ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
                }
                else
                    glDisable(GL_BLEND);
                applied_blend = blend;
            }
            if (applied_program != program)
            {
                glUseProgram(program);
                ++vid_stats.program_changes;
                applied_program = program;
                applied_textured = -1;
                uniform_valid = params_valid = fog_valid = 0;
            }
            if (textured != applied_textured)
            {
                GLint location = material->shader ? material->shader->textured : vid_textured;
                if (location >= 0)
                    glUniform1i(location, textured);
                applied_textured = textured;
            }
            if (material->shader)
            {
                float fog[12];
                if (material->shader->params >= 0 && (!params_valid || memcmp(applied_params, material->params, sizeof(applied_params)) != 0))
                {
                    glUniform4fv(material->shader->params, 2, material->params);
                    memcpy(applied_params, material->params, sizeof(applied_params));
                    params_valid = 1;
                }
                memcpy(fog, draws->fog, sizeof(fog));
                fog[3] = (float)material->blend;
                if (material->shader->fog >= 0 && (!fog_valid || memcmp(applied_fog, fog, sizeof(fog)) != 0))
                {
                    glUniform4fv(material->shader->fog, 3, fog);
                    memcpy(applied_fog, fog, sizeof(fog));
                    fog_valid = 1;
                }
            }
            for (uint32 unit = 0; unit < VID_AUX_TEXTURES + 1; ++unit)
            {
                VID_TEXTURE *texture = unit == 0 ? material->texture : material->aux[unit - 1];
                uint32 filter = texture && texture->format == VID_DEPTH24 ? VID_FILTER_LINEAR : unit == 0 ? material->filter : material->aux_filter[unit - 1];
                uint32 repeat = unit == 0 ? material->repeat : 0;
                const VID_FORMAT_INFO *info = texture ? vid_format(texture->format) : NULL;
                uint32 effective_filter = info && info->comparison ? VID_FILTER_LINEAR : info && info->nearest ? 0 : filter;
                uint32 effective_repeat = info && (info->depth || info->nearest) ? 0 : repeat;
                uint32 sampler = 8 | (effective_filter << 1) | (effective_repeat ? 1 : 0);
                if (texture != NULL && (applied_textures[unit] != texture || texture->uploaded != texture->version || texture_filters[unit] != filter || texture_repeats[unit] != repeat || texture->sampler_flags != sampler))
                {
                    if (active_unit != unit)
                    {
                        glActiveTexture(GL_TEXTURE0 + unit);
                        active_unit = unit;
                    }
                    vid_tex_upload(texture, filter, repeat);
                    applied_textures[unit] = texture;
                    missing_shadow_mask &= ~(1u << unit);
                    missing_2d_mask &= ~(1u << unit);
                    texture_filters[unit] = filter;
                    texture_repeats[unit] = repeat;
                }
                uint32 empty_shadow = !texture && unit != 0 && material->shader && (material->shader->aux_shadow_mask & (1u << (unit - 1)));
                uint32 empty_2d = !texture && unit != 0 && material->shader && (material->shader->aux_2d_mask & (1u << (unit - 1)));
                if (!texture && (applied_textures[unit] || !(texture_valid & (1u << unit)) || !!(missing_shadow_mask & (1u << unit)) != !!empty_shadow || !!(missing_2d_mask & (1u << unit)) != !!empty_2d))
                {
                    if (active_unit != unit)
                    {
                        glActiveTexture(GL_TEXTURE0 + unit);
                        active_unit = unit;
                    }
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
                    if (empty_shadow)
                    {
                        // inactive shadow branches still require a valid comparison texture
                        if (!vid_empty_shadow)
                        {
                            const uint32 depth = UINT32_MAX;
                            glGenTextures(1, &vid_empty_shadow);
                            glBindTexture(GL_TEXTURE_2D_ARRAY, vid_empty_shadow);
                            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, 1, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &depth);
                            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
                        }
                        else
                            glBindTexture(GL_TEXTURE_2D_ARRAY, vid_empty_shadow);
                        missing_shadow_mask |= 1u << unit;
                    }
                    else
                        missing_shadow_mask &= ~(1u << unit);
                    if (empty_2d)
                    {
                        if (!vid_empty_2d)
                        {
                            const uint8 pixel[4] = {0, 0, 0, 255};
                            glGenTextures(1, &vid_empty_2d);
                            glBindTexture(GL_TEXTURE_2D, vid_empty_2d);
                            ++vid_stats.texture_binds;
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        }
                        else
                            glBindTexture(GL_TEXTURE_2D, vid_empty_2d);
                        ++vid_stats.texture_binds;
                        missing_2d_mask |= 1u << unit;
                    }
                    else
                        missing_2d_mask &= ~(1u << unit);
                    applied_textures[unit] = NULL;
                }
                texture_valid |= 1u << unit;
            }
        }
        uint32 uniform_count = material->shader && draws->uniform_count > material->shader->uniform_vectors ? material->shader->uniform_vectors : draws->uniform_count;
        if (material->shader && material->shader->uniforms >= 0 && uniform_count)
        {
            if (!uniform_valid || applied_uniform_count != uniform_count || (applied_uniform_snapshot != draws->uniforms && memcmp(applied_uniforms, draws->uniforms, uniform_count * 4 * sizeof(float)) != 0))
            {
                glUniform4fv(material->shader->uniforms, uniform_count, draws->uniforms);
                ++vid_stats.uniform_uploads;
                vid_stats.uniform_bytes += uniform_count * 4 * sizeof(float);
                memcpy(applied_uniforms, draws->uniforms, uniform_count * 4 * sizeof(float));
                applied_uniform_count = uniform_count;
                uniform_valid = 1;
            }
            applied_uniform_snapshot = draws->uniforms;
        }
        if (draws->pass == 1)
        {
            pass = material;
            draws = draws->next;
            continue;
        }
        if (!geometry->object)
            glGenBuffers(1, &geometry->object);
        if (!geometry->array)
        {
            glGenVertexArrays(1, &geometry->array);
            vid_vertices(geometry->object, geometry->stride, geometry->array);
            applied_array = geometry->array;
            applied_indices = UINT32_MAX;
        }
        else if (applied_array != geometry->array)
        {
            glBindVertexArray(geometry->array);
            applied_array = geometry->array;
            applied_indices = UINT32_MAX;
        }
        if (geometry->uploaded != geometry->version)
        {
            glBindBuffer(GL_ARRAY_BUFFER, geometry->object);
            // ANGLE dynamic vertex storage rebases gl_VertexID twice for indexed draws
            if (geometry->uploaded != 0 && geometry->dirty_last > geometry->dirty_first && geometry->storage_bytes >= (size_t)geometry->count * geometry->stride)
            {
                size_t first = (size_t)geometry->dirty_first * geometry->stride;
                size_t bytes = (size_t)(geometry->dirty_last - geometry->dirty_first) * geometry->stride;
                glBufferSubData(GL_ARRAY_BUFFER, first, bytes, (const unsigned char *)geometry->vertices + first);
#if defined(_DEBUG)
                vid_stats.vertex_bytes += bytes;
#endif
            }
            else
                vid_buffer_upload(GL_ARRAY_BUFFER, (size_t)geometry->count * geometry->stride, geometry->vertices, GL_STATIC_DRAW, &geometry->storage_bytes, (size_t)geometry->count * geometry->stride);
            geometry->uploaded = geometry->version;
            geometry->dirty_first = geometry->dirty_last = 0;
            if (geometry->immutable)
            {
                free(geometry->vertices);
                geometry->vertices = NULL;
            }
        }
        if (draws->indices)
        {
            VID_INDEX *indices = draws->indices;
            if (!indices->object)
                glGenBuffers(1, &indices->object);
            if (applied_indices != indices->object)
            {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->object);
                applied_indices = indices->object;
            }
            if (indices->uploaded != indices->version)
            {
                vid_buffer_upload(GL_ELEMENT_ARRAY_BUFFER, (size_t)indices->count * sizeof(uint32), indices->values, indices->immutable ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW, &indices->storage_bytes, (size_t)indices->capacity * sizeof(uint32));
                indices->uploaded = indices->version;
                if (indices->immutable)
                {
                    free(indices->values);
                    indices->values = NULL;
                }
            }
            glDrawElements(GL_TRIANGLES, draws->count, GL_UNSIGNED_INT, (const void *)((size_t)draws->first * sizeof(uint32)));
            ++vid_stats.draws;
            vid_stats.triangles += draws->count / 3;
        }
        else
        {
            glDrawArrays(GL_TRIANGLES, draws->first, draws->count);
            ++vid_stats.draws;
            vid_stats.triangles += draws->count / 3;
        }
        if (material->shader)
            ++material->shader->draw_count;
        draws = draws->next;
    }
    if (active_unit != 0)
        glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    if (frame_target)
    {
        frame_target->ready = vid_render_status();
        if (frame_target->ready && frame_target->texture->format != VID_DEPTH24)
        {
            if (frame_target->texture->cube)
                frame_target->completed_faces |= 1u << frame_target->face;
            if (frame_target_mip)
                vid_target_mip_written(frame_target, frame_target_mip);
            else
                vid_texture_rendered(frame_target->texture);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_target->framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
        if (frame_target_present)
        {
            float scaled = frame_target_offset * height / frame_height;
            if (scaled != 0)
            {
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            if (scaled > -height && scaled < height)
            {
                sint32 shift = (sint32)floorf(scaled + 0.5f);
                sint32 source = shift > 0 ? shift : 0, destination = shift < 0 ? -shift : 0;
                sint32 extent = height - source - destination;
                if (extent > 0)
                    glBlitFramebuffer(0, source, width, source + extent, 0, destination, width, destination + extent, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
        if (frame_target_mip)
            frame_target->framebuffer = frame_target->framebuffers[frame_target->face];
    }

    return frame_target && !frame_target_present ? vid_render_status() : vid_swap();
}

static void vid_mesh_delete(VID_GEOMETRY *geometry)
{
    if (geometry->object)
        glDeleteBuffers(1, &geometry->object);
    if (geometry->array)
        glDeleteVertexArrays(1, &geometry->array);
}

static void vid_tex_delete(VID_TEXTURE *texture)
{
    if (texture->object)
        glDeleteTextures(1, &texture->object);
}

static VID_GEOMETRY *geometries;
static VID_TEXTURE *textures;
static VID_MATERIAL *materials;
static VID_DRAW *draw_head, *draw_tail, *draw_pool;
static VID_RECT frame_viewport;
static float frame_fog[12];

typedef struct VID_UNIFORM_CHUNK
{
    struct VID_UNIFORM_CHUNK *next;
    uint32 used;
    float values[16384];
} VID_UNIFORM_CHUNK;

static VID_UNIFORM_CHUNK *uniform_chunks, *uniform_cursor;
static const float *frame_uniforms;
static uint32 frame_uniform_count;
static sint32 frame_width, frame_height;
static float frame_clear[3];
static sint32 frame_active, initialized, frame_pass;
static const VID_DRAW *frame_pass_snapshot;

static void sample_release(VID_SAMPLE *sample)
{
    VID_SAMPLE **entry;
    if (--sample->refs)
        return;
    for (entry = &samples; *entry != sample; entry = &(*entry)->next)
    {
    }
    *entry = sample->next;
    free(sample);
}

VID_SAMPLE *vid_sample_create(void)
{
    VID_SAMPLE *sample = calloc(1, sizeof(*sample));
    if (sample)
    {
        sample->refs = 1;
        sample->next = samples;
        samples = sample;
    }
    return sample;
}

void vid_sample_free(VID_SAMPLE *sample)
{
    if (sample && !sample->released)
    {
        sample->released = 1;
        sample_release(sample);
    }
}

uint32 vid_sample_color(const VID_SAMPLE *sample)
{
    return sample && !sample->released ? sample->color : 0;
}

static void index_release(VID_INDEX *indices)
{
    if (--indices->refs)
        return;
    VID_INDEX **entry = &index_buffers;
    while (*entry != indices)
        entry = &(*entry)->next;
    *entry = indices->next;
    if (indices->object)
        glDeleteBuffers(1, &indices->object);
    free(indices->values);
    free(indices);
}

VID_INDEX *vid_index_create(uint32 count)
{
    if (!count || count > SIZE_MAX / sizeof(uint32))
        return NULL;
    VID_INDEX *indices = calloc(1, sizeof(*indices));
    if (!indices)
        return NULL;
    indices->values = calloc(count, sizeof(uint32));
    if (!indices->values)
    {
        free(indices);
        return NULL;
    }
    indices->capacity = count;
    indices->refs = indices->version = 1;
    indices->next = index_buffers;
    index_buffers = indices;
    return indices;
}

uint32 *vid_index_data(VID_INDEX *indices)
{
    return indices && !indices->released && !indices->immutable && indices->refs == 1 ? indices->values : NULL;
}

void vid_index_commit(VID_INDEX *indices, uint32 count)
{
    if (!indices || indices->released || indices->refs != 1 || indices->immutable || count > indices->capacity)
        abort();
    indices->count = count;
    if (!++indices->version)
        indices->version = 1;
    indices->uploaded = 0;
}

void vid_index_static(VID_INDEX *indices)
{
    if (!indices || indices->released || indices->refs != 1 || indices->immutable)
        abort();
    indices->immutable = 1;
    if (indices->uploaded == indices->version)
    {
        free(indices->values);
        indices->values = NULL;
    }
}

void vid_index_free(VID_INDEX *indices)
{
    if (!indices)
        return;
    if (indices->released)
        abort();
    indices->released = 1;
    index_release(indices);
}

static void geometry_release(VID_GEOMETRY *geometry)
{
    VID_GEOMETRY **entry;
    if (--geometry->refs)
        return;
    for (entry = &geometries; *entry != geometry; entry = &(*entry)->next)
    {
    }
    *entry = geometry->next;
    vid_mesh_delete(geometry);
    free(geometry->vertices);
    free(geometry);
}

static void texture_release(VID_TEXTURE *texture)
{
    VID_TEXTURE **entry;
    if (--texture->refs)
        return;
    for (entry = &textures; *entry != texture; entry = &(*entry)->next)
    {
    }
    *entry = texture->next;
    vid_tex_delete(texture);
    free(texture->pixels);
    free(texture);
}

static void shader_release(VID_SHADER *shader)
{
    VID_SHADER **entry;
    if (--shader->refs)
        return;
    for (entry = &shaders; *entry != shader; entry = &(*entry)->next)
    {
    }
    *entry = shader->next;
    GLint current_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    if ((GLuint)current_program == shader->program)
        glUseProgram(0);
    glDeleteProgram(shader->program);
    free(shader);
}

static void material_release(VID_MATERIAL *material)
{
    VID_MATERIAL **entry;
    if (--material->refs)
        return;
    for (entry = &materials; *entry != material; entry = &(*entry)->next)
    {
    }
    *entry = material->next;
    if (material->held_texture)
        texture_release(material->held_texture);
    if (material->held_shader)
        shader_release(material->held_shader);
    for (uint32 idx = 0; idx < VID_AUX_TEXTURES; ++idx)
        if (material->held_aux[idx])
            texture_release(material->held_aux[idx]);
    free(material);
}

static VID_GEOMETRY *vid_mesh_create_size(uint32 vertices, uint32 stride)
{
    VID_GEOMETRY *geometry;
    if (!vertices || vertices > SIZE_MAX / stride)
        return NULL;
    geometry = calloc(1, sizeof(*geometry));
    if (!geometry)
        return NULL;
    geometry->vertices = calloc(vertices, stride);
    if (!geometry->vertices)
    {
        free(geometry);
        return NULL;
    }
    geometry->count = vertices;
    geometry->stride = stride;
    if (initialized)
        glGenBuffers(1, &geometry->object);
    geometry->refs = geometry->version = 1;
    geometry->next = geometries;
    geometries = geometry;
    return geometry;
}

VID_GEOMETRY *vid_mesh_create(uint32 vertices)
{
    return vid_mesh_create_size(vertices, sizeof(VID_VERTEX));
}

VID_GEOMETRY *vid_mesh_create_3d(uint32 vertices)
{
    return vid_mesh_create_size(vertices, sizeof(VID_VERTEX3D));
}

VID_GEOMETRY *vid_mesh_create_2d(uint32 vertices)
{
    return vid_mesh_create_size(vertices, sizeof(VID_VERTEX2D));
}

VID_VERTEX2D *vid_mesh_data_2d(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX2D) && !geometry->released && !geometry->immutable && geometry->refs == 1 ? geometry->vertices : NULL;
}

VID_GEOMETRY *vid_mesh_create_16(uint32 vertices)
{
    return vid_mesh_create_size(vertices, sizeof(VID_VERTEX16));
}

VID_VERTEX16 *vid_mesh_data_16(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX16) && !geometry->released && !geometry->immutable && geometry->refs == 1 ? geometry->vertices : NULL;
}

VID_VERTEX3D *vid_mesh_data_3d(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX3D) && !geometry->released && !geometry->immutable && geometry->refs == 1 ? geometry->vertices : NULL;
}

VID_VERTEX *vid_mesh_data(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX) && !geometry->released && !geometry->immutable && geometry->refs == 1 ? geometry->vertices : NULL;
}

void vid_mesh_commit(VID_GEOMETRY *geometry)
{
    if (!geometry || geometry->released || geometry->immutable || geometry->refs != 1)
        abort();
    if (!++geometry->version)
        geometry->version = 1;
    geometry->uploaded = 0;
    geometry->dirty_first = 0;
    geometry->dirty_last = geometry->count;
}

void vid_mesh_commit_range(VID_GEOMETRY *geometry, uint32 first, uint32 count)
{
    if (!geometry || geometry->released || geometry->immutable || geometry->refs != 1 || first > geometry->count || count > geometry->count - first)
        abort();
    if (!count)
        return;
    if (geometry->dirty_last <= geometry->dirty_first)
    {
        geometry->dirty_first = first;
        geometry->dirty_last = first + count;
    }
    else
    {
        if (first < geometry->dirty_first)
            geometry->dirty_first = first;
        if (first + count > geometry->dirty_last)
            geometry->dirty_last = first + count;
    }
    if (!++geometry->version)
    {
        geometry->version = 1;
        geometry->uploaded = 0;
    }
}

void vid_mesh_static(VID_GEOMETRY *geometry)
{
    if (!geometry || geometry->released || !geometry->vertices)
        abort();
    geometry->immutable = 1;
    if (geometry->uploaded == geometry->version)
    {
        free(geometry->vertices);
        geometry->vertices = NULL;
    }
}

void vid_mesh_free(VID_GEOMETRY *geometry)
{
    if (!geometry)
        return;
    if (geometry->released)
        abort();
    geometry->released = 1;
    geometry_release(geometry);
}

VID_TEXTURE *vid_tex_create_fmt(uint32 width, uint32 height, uint32 format)
{
    VID_TEXTURE *texture;
    if ((format > VID_RGBA16UI && format != VID_RGBA32F && format != VID_RGBA16F && format != VID_R8) || !width || !height || width > SIZE_MAX / (format == VID_RGBA32F ? 16 : 8) / height)
        return NULL;
    texture = calloc(1, sizeof(*texture));
    if (!texture)
        return NULL;
    texture->pixels = calloc((size_t)width * height, vid_format(format)->bytes);
    if (!texture->pixels)
    {
        free(texture);
        return NULL;
    }
    texture->format = format;
    texture->layers = 1;
    texture->width = width;
    texture->height = height;
    texture->refs = texture->version = 1;
    texture->next = textures;
    textures = texture;
    return texture;
}

VID_TEXTURE *vid_tex_create(uint32 width, uint32 height)
{
    return vid_tex_create_fmt(width, height, VID_RGBA8);
}

VID_TEXTURE *vid_tex_create_array_fmt(uint32 width, uint32 height, uint32 layers, uint32 format)
{
    VID_TEXTURE *texture;
    uint8 *pixels;
    uint32 stride = format <= VID_RGBA5551 ? vid_format(format)->bytes : 0;
    if (format > VID_RGBA5551 || layers < 1 || layers > 256 || !width || !height || (size_t)width > SIZE_MAX / stride / height / layers)
        return NULL;
    pixels = calloc((size_t)width * height * layers, stride);
    if (!pixels)
        return NULL;
    texture = vid_tex_create_fmt(width, height, format);
    if (!texture)
    {
        free(pixels);
        return NULL;
    }
    free(texture->pixels);
    texture->pixels = pixels;
    texture->layers = layers;
    texture->array = 1;
    return texture;
}

VID_TEXTURE *vid_tex_create_array(uint32 width, uint32 height, uint32 layers)
{
    return vid_tex_create_array_fmt(width, height, layers, VID_RGBA8);
}

void vid_tex_static(VID_TEXTURE *texture)
{
    if (!texture || texture->released || !texture->pixels)
        abort();
    texture->immutable = 1;
    if (texture->uploaded == texture->version)
    {
        free(texture->pixels);
        texture->pixels = NULL;
    }
}

uint8 *vid_tex_data(VID_TEXTURE *texture)
{
    // Material ownership alone does not prevent edits between frames
    return texture && !texture->released && !texture->immutable ? texture->pixels : NULL;
}

void vid_tex_commit(VID_TEXTURE *texture)
{
    if (!texture || texture->released || texture->immutable || !texture->pixels)
        abort();
    if (!++texture->version)
        texture->version = 1;
    texture->uploaded = 0;
    texture->dirty_first = 0;
    texture->dirty_last = texture->height;
}

void vid_tex_set_mip_levels(VID_TEXTURE *texture, uint32 levels)
{
    if (!texture || texture->released || texture->draw_refs || levels > 32)
        abort();
    if (texture->mip_levels != levels)
    {
        texture->mip_levels = levels;
        texture->mip_version = 0;
        texture->sampler_flags = 0;
    }
}

void vid_tex_commit_rows(VID_TEXTURE *texture, uint32 first, uint32 count)
{
    if (!texture || texture->released || texture->immutable || !texture->pixels || first > texture->height || count > texture->height - first)
        abort();
    if (!count)
        return;
    if (texture->uploaded == texture->version)
    {
        texture->dirty_first = first;
        texture->dirty_last = first + count;
    }
    else
    {
        if (first < texture->dirty_first)
            texture->dirty_first = first;
        if (first + count > texture->dirty_last)
            texture->dirty_last = first + count;
    }
    if (!++texture->version)
        texture->version = 1;
    texture->uploaded = 0;
}

void vid_tex_free(VID_TEXTURE *texture)
{
    if (!texture)
        return;
    if (texture->released)
        abort();
    texture->released = 1;
    texture_release(texture);
}

VID_TARGET *vid_target_create(void)
{
    VID_TARGET *target = calloc(1, sizeof(*target));
    VID_TEXTURE *texture;
    if (!target)
        return NULL;
    texture = calloc(1, sizeof(*texture));
    if (!texture)
    {
        free(target);
        return NULL;
    }
    texture->refs = texture->version = 1;
    texture->next = textures;
    textures = texture;
    target->texture = texture;
    target->refs = 1;
    target->next = targets;
    targets = target;
    return target;
}

VID_TARGET *vid_target_create_hdr(void)
{
    VID_TARGET *target = vid_target_create();
    if (target)
        target->texture->format = VID_RGBA16F;
    return target;
}

static VID_TARGET *vid_target_attach_depth(VID_TARGET *target)
{
    VID_TEXTURE *depth;
    if (!target)
        return NULL;
    depth = calloc(1, sizeof(*depth));
    if (!depth)
    {
        vid_target_free(target);
        return NULL;
    }
    depth->refs = depth->version = depth->layers = 1;
    depth->format = VID_DEPTH24_RAW;
    depth->cube = target->texture->cube;
    depth->next = textures;
    textures = depth;
    target->depth_texture = depth;
    return target;
}

VID_TARGET *vid_target_create_hdr_depth(void)
{
    return vid_target_attach_depth(vid_target_create_hdr());
}

VID_TEXTURE *vid_target_depth_texture(VID_TARGET *target)
{
    return target && !target->released ? target->depth_texture : NULL;
}

static VID_TARGET *vid_target_attach_surface(VID_TARGET *target)
{
    if (!target)
        return NULL;
    VID_TEXTURE *surface = calloc(1, sizeof(*surface));
    if (!surface)
    {
        vid_target_free(target);
        return NULL;
    }
    surface->refs = surface->version = surface->layers = 1;
    surface->format = VID_R8;
    surface->next = textures;
    textures = surface;
    target->surface_texture = surface;
    return target;
}

VID_TARGET *vid_target_create_scene(void)
{
    return vid_target_attach_surface(vid_target_create_hdr_depth());
}

VID_TARGET *vid_target_create_scene_ldr(void)
{
    return vid_target_attach_surface(vid_target_create());
}

VID_TEXTURE *vid_target_surface_texture(VID_TARGET *target)
{
    return target && !target->released ? target->surface_texture : NULL;
}

VID_TARGET *vid_target_create_depth(void)
{
    VID_TARGET *target = vid_target_create();
    if (target)
        target->texture->format = VID_DEPTH24;
    return target;
}

VID_TARGET *vid_target_create_depth_array(uint32 size, uint32 layers)
{
    VID_TARGET *target;
    if (!size || size > 16384 || !layers || layers > 256)
        return NULL;
    target = vid_target_create_depth();
    if (target)
    {
        target->texture->array = 1;
        target->texture->layers = layers;
        target->texture->width = target->texture->height = size;
    }
    return target;
}

VID_TARGET *vid_target_create_cube(uint32 size)
{
    VID_TARGET *target;
    if (!size || size > 16384)
        return NULL;
    target = vid_target_create();
    if (target)
    {
        target->texture->cube = 1;
        target->texture->width = target->texture->height = size;
    }
    return target;
}

VID_TARGET *vid_target_create_cube_depth(uint32 size)
{
    return vid_target_attach_depth(vid_target_create_cube(size));
}

VID_TEXTURE *vid_target_texture(VID_TARGET *target)
{
    return target && !target->released ? target->texture : NULL;
}

static void target_release(VID_TARGET *target)
{
    VID_TARGET **link;
    if (--target->refs)
        return;
    link = &targets;
    while (*link != target)
        link = &(*link)->next;
    *link = target->next;
    if (target->framebuffers)
        glDeleteFramebuffers(target->framebuffer_count, target->framebuffers);
    free(target->framebuffers);
    for (uint32 level = 0; level < 32; ++level)
        if (target->mip_framebuffers[level])
            glDeleteFramebuffers(1, &target->mip_framebuffers[level]);
    if (target->depth)
        glDeleteRenderbuffers(1, &target->depth);
    vid_tex_free(target->texture);
    vid_tex_free(target->depth_texture);
    vid_tex_free(target->surface_texture);
    free(target);
}

void vid_target_free(VID_TARGET *target)
{
    if (!target)
        return;
    if (target->released)
        abort();
    target->released = 1;
    target_release(target);
}

sint32 vid_target_copy(VID_TARGET *destination, const VID_TARGET *source)
{
    GLint previous_read, previous_draw;
    sint32 result;
    if (!initialized || frame_active || !destination || !source || destination == source || destination->released || source->released || !source->ready || destination->texture->draw_refs || destination->texture->format == VID_DEPTH24 || source->texture->format == VID_DEPTH24 || destination->texture->cube || source->texture->cube)
        return 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    result = vid_target_storage(destination, source->texture->width, source->texture->height);
    destination->ready = 0;
    if (result)
    {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source->framebuffer);
        {
            GLenum color = GL_COLOR_ATTACHMENT0;
            glReadBuffer(color);
            glDrawBuffers(1, &color);
            glBlitFramebuffer(0, 0, source->texture->width, source->texture->height, 0, 0, source->texture->width, source->texture->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (source->surface_texture && destination->surface_texture)
            {
                color = GL_COLOR_ATTACHMENT1;
                glReadBuffer(color);
                glDrawBuffers(1, &color);
                glBlitFramebuffer(0, 0, source->texture->width, source->texture->height, 0, 0, source->texture->width, source->texture->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                color = GL_COLOR_ATTACHMENT0;
                glReadBuffer(color);
                glDrawBuffers(1, &color);
            }
        }
        result = glGetError() == GL_NO_ERROR;
        destination->ready = result;
        if (result)
        {
            vid_texture_rendered(destination->texture);
            if (source->surface_texture && destination->surface_texture)
                vid_texture_rendered(destination->surface_texture);
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
    return result;
}

sint32 vid_target_copy_mip(VID_TARGET *destination, uint32 level, const VID_TARGET *source)
{
    if (!initialized || frame_active || !destination || !source || destination == source || destination->released || source->released || !destination->ready || !source->ready)
        return 0;
    VID_TEXTURE *texture = destination->texture;
    if (texture->draw_refs || texture->cube || texture->array || source->texture->cube || source->texture->array || (texture->format != VID_RGBA16F && texture->format != VID_RGBA8) || source->texture->format != texture->format)
        return 0;
    uint32 levels = 1, size = texture->width > texture->height ? texture->width : texture->height;
    for (; size > 1; size >>= 1)
        ++levels;
    if (texture->mip_levels && texture->mip_levels < levels)
        levels = texture->mip_levels;
    if (!level || level >= levels)
        return 0;
    uint32 width = texture->width >> level, height = texture->height >> level;
    if (!width)
        width = 1;
    if (!height)
        height = 1;
    if (source->texture->width < width || source->texture->height < height)
        return 0;
    GLint previous_read, previous_texture;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, texture->object);
    for (uint32 index = destination->mip_storage_levels; index < levels; ++index)
    {
        uint32 w = texture->width >> index, h = texture->height >> index;
        glTexImage2D(GL_TEXTURE_2D, index, texture->format == VID_RGBA16F ? GL_RGBA16F : GL_RGBA8, w ? w : 1, h ? h : 1, 0, GL_RGBA, texture->format == VID_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
    }
    sint32 result = glGetError() == GL_NO_ERROR;
    if (result)
    {
        destination->mip_storage_levels = levels;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source->framebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glCopyTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, 0, 0, width, height);
        result = glGetError() == GL_NO_ERROR;
    }
    if (destination->mip_base_version != texture->version)
    {
        destination->mip_base_version = texture->version;
        destination->mip_written = 1;
    }
    if (result)
    {
        destination->mip_written |= 1u << level;
        uint32 complete = levels == 32 ? UINT32_MAX : (1u << levels) - 1;
        if ((destination->mip_written & complete) == complete)
            texture->mip_version = texture->version;
    }
    glBindTexture(GL_TEXTURE_2D, previous_texture);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    return result;
}

static VID_SHADER *vid_shader_create_impl(const char *vertex_source, const char *fragment, uint32 array)
{
    static const char prefix[] = VID_GLSL "in vec2 vUV; in vec4 vColor; in float vDepth; uniform sampler2D uTex; uniform int uTextured; uniform vec4 uParams[2]; layout(location=0) out vec4 outColor;\n"
                                          "in vec2 vFog; uniform vec4 uFog[3];\n"
                                          "vec4 vidFog(vec4 color){if(uFog[0].y<=uFog[0].x)return color;"
                                          "float f=vFog.x;"
                                          "if(uFog[0].w>=2.0)color.rgb*=1.0-f;"
                                          "else {color.rgb=mix(color.rgb,uFog[1].rgb,f);"
                                          "float h=vFog.y;"
                                          "color.a*=1.0-h;if(color.a<=0.0)discard;}return color;}\n";
    const char *fragment_prefix = prefix;
    char array_prefix[sizeof(prefix) + 16];
    size_t prefix_length;
    VID_SHADER *shader;
    GLuint vertex, pixel;
    GLint success;
    char log[1024];
    char *source;
    size_t bytes;
    if (!initialized || !fragment)
        return NULL;
    if (array)
    {
        const char *sampler = strstr(prefix, "sampler2D uTex");
        size_t before = (size_t)(sampler - prefix);
        memcpy(array_prefix, prefix, before);
        strcpy(array_prefix + before, array == 2 ? "highp samplerCube uTex" : "highp sampler2DArray uTex");
        strcat(array_prefix, sampler + strlen("sampler2D uTex"));
        fragment_prefix = array_prefix;
    }
    prefix_length = strlen(fragment_prefix);
    bytes = strlen(fragment);
    if (bytes > SIZE_MAX - prefix_length - 1)
        return NULL;
    source = malloc(prefix_length + bytes + 1);
    shader = calloc(1, sizeof(*shader));
    if (!source || !shader)
    {
        free(source);
        free(shader);
        return NULL;
    }
    memcpy(source, fragment_prefix, prefix_length);
    memcpy(source + prefix_length, fragment, bytes + 1);
    if (vertex_source)
    {
        size_t length = strlen(vertex_source);
        char *vertex_text = malloc(sizeof(VID_GLSL) + length);
        if (!vertex_text)
        {
            free(source);
            free(shader);
            return NULL;
        }
        memcpy(vertex_text, VID_GLSL, sizeof(VID_GLSL) - 1);
        memcpy(vertex_text + sizeof(VID_GLSL) - 1, vertex_source, length + 1);
        vertex = vid_compile_shader(GL_VERTEX_SHADER, vertex_text);
        free(vertex_text);
    }
    else
        vertex = vid_compile_shader(GL_VERTEX_SHADER, vid_vertex_source);
    pixel = vid_compile_shader(GL_FRAGMENT_SHADER, source);
    free(source);
    if (!vertex || !pixel)
    {
        if (vertex)
            glDeleteShader(vertex);
        if (pixel)
            glDeleteShader(pixel);
        free(shader);
        return NULL;
    }
    shader->program = glCreateProgram();
    glAttachShader(shader->program, vertex);
    glAttachShader(shader->program, pixel);
    glLinkProgram(shader->program);
    glDeleteShader(vertex);
    glDeleteShader(pixel);
    glGetProgramiv(shader->program, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(shader->program, sizeof(log), NULL, log);
        log[sizeof(log) - 1] = 0;
        xport_message_error("VID material shader", log);
        glDeleteProgram(shader->program);
        free(shader);
        return NULL;
    }
    shader->textured = glGetUniformLocation(shader->program, "uTextured");
    shader->params = glGetUniformLocation(shader->program, "uParams");
    shader->fog = glGetUniformLocation(shader->program, "uFog");
    shader->uniforms = glGetUniformLocation(shader->program, "uDraw");
    if (shader->uniforms >= 0)
    {
        GLint count;
        glGetProgramiv(shader->program, GL_ACTIVE_UNIFORMS, &count);
        for (GLint index = 0; index < count; ++index)
        {
            char name[128];
            GLsizei length;
            GLint size;
            GLenum type;
            glGetActiveUniform(shader->program, (GLuint)index, sizeof(name), &length, &size, &type, name);
            if (!strcmp(name, "uDraw[0]") || !strcmp(name, "uDraw"))
                shader->uniform_vectors = (uint32)size;
        }
    }
    {
        static const char *const names[VID_AUX_TEXTURES] = {"uAux0", "uAux1", "uAux2", "uAux3", "uAux4", "uAux5", "uAux6", "uAux7"};
        for (uint32 index = 0; index < VID_AUX_TEXTURES; ++index)
            shader->aux[index] = glGetUniformLocation(shader->program, names[index]);
        GLint active;
        glGetProgramiv(shader->program, GL_ACTIVE_UNIFORMS, &active);
        for (GLint item = 0; item < active; ++item)
        {
            char name[128];
            GLsizei length;
            GLint size;
            GLenum type;
            glGetActiveUniform(shader->program, (GLuint)item, sizeof(name), &length, &size, &type, name);
            if (type == GL_SAMPLER_2D_ARRAY_SHADOW)
                for (uint32 index = 0; index < VID_AUX_TEXTURES; ++index)
                    if (!strcmp(name, names[index]))
                        shader->aux_shadow_mask |= 1u << index;
            if (type == GL_SAMPLER_2D)
                for (uint32 index = 0; index < VID_AUX_TEXTURES; ++index)
                    if (!strcmp(name, names[index]))
                        shader->aux_2d_mask |= 1u << index;
        }
    }
    {
        GLint previous_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
        glUseProgram(shader->program);
        GLint texture = glGetUniformLocation(shader->program, "uTex");
        if (texture >= 0)
            glUniform1i(texture, 0);
        for (uint32 unit = 0; unit < VID_AUX_TEXTURES; ++unit)
            if (shader->aux[unit] >= 0)
                glUniform1i(shader->aux[unit], unit + 1);
        glUseProgram((GLuint)previous_program);
    }
    shader->refs = 1;
    shader->next = shaders;
    shaders = shader;
    return shader;
}

VID_SHADER *vid_shader_create(const char *vertex_source, const char *fragment)
{
    return vid_shader_create_impl(vertex_source, fragment, 0);
}

VID_SHADER *vid_shader_create_array(const char *vertex_source, const char *fragment)
{
    return vid_shader_create_impl(vertex_source, fragment, 1);
}

void vid_shader_free(VID_SHADER *shader)
{
    if (!shader)
        return;
    if (shader->released)
        abort();
    shader->released = 1;
    shader_release(shader);
}

VID_SHADER *vid_shader_create_cube(const char *vertex, const char *fragment)
{
    return vid_shader_create_impl(vertex, fragment, 2);
}

VID_MATERIAL *vid_mat_create(void)
{
    VID_MATERIAL *material = calloc(1, sizeof(*material));
    if (!material)
        return NULL;
    material->refs = 1;
    material->next = materials;
    materials = material;
    return material;
}

VID_MATERIAL_DATA *vid_mat_data(VID_MATERIAL *material)
{
    return material && !material->released && material->refs == 1 ? &material->data : NULL;
}

const VID_MATERIAL_DATA *vid_mat_read(const VID_MATERIAL *material)
{
    return material && !material->released ? &material->data : NULL;
}

static uint64 material_revision;

uint64 vid_mat_revision(const VID_MATERIAL *material)
{
    return material && !material->released ? material->revision : 0;
}

void vid_mat_commit(VID_MATERIAL *material)
{
    VID_TEXTURE *texture;
    VID_SHADER *shader;
    if (!material || material->released || material->refs != 1)
        abort();
    material->revision = ++material_revision;
    texture = material->data.texture;
    shader = material->data.shader;
    for (uint32 idx = 0; idx < VID_AUX_TEXTURES; ++idx)
        if (material->data.aux[idx] != material->held_aux[idx])
        {
            VID_TEXTURE *aux = material->data.aux[idx];
            if (aux)
            {
                if (aux->released)
                    abort();
                ++aux->refs;
            }
            if (material->held_aux[idx])
                texture_release(material->held_aux[idx]);
            material->held_aux[idx] = aux;
        }
    if (texture != material->held_texture)
    {
        if (texture)
        {
            if (texture->released)
                abort();
            ++texture->refs;
        }
        if (material->held_texture)
            texture_release(material->held_texture);
        material->held_texture = texture;
    }
    if (shader != material->held_shader)
    {
        if (shader)
        {
            if (shader->released)
                abort();
            ++shader->refs;
        }
        if (material->held_shader)
            shader_release(material->held_shader);
        material->held_shader = shader;
    }
}

void vid_mat_free(VID_MATERIAL *material)
{
    if (!material)
        return;
    if (material->released)
        abort();
    material->released = 1;
    material_release(material);
}

VID_RECT vid_best_fit(sint32 width, sint32 height)
{
    VID_RECT rect = {0, 0, 0, 0};
    if (width <= 0 || height <= 0)
        return rect;
    if ((sint64)width * 3 > (sint64)height * 4)
    {
        rect.h = height;
        rect.w = (sint32)((sint64)height * 4 / 3);
    }
    else
    {
        rect.w = width;
        rect.h = (sint32)((sint64)width * 3 / 4);
    }
    rect.x = (width - rect.w) / 2;
    rect.y = (height - rect.h) / 2;
    return rect;
}

void vid_resize(sint32 *width, sint32 *height)
{
    if (!width || !height)
        abort();
    if (!initialized)
    {
        *width = WND_WIDTH;
        *height = WND_HEIGHT;
        return;
    }
    vid_update_size(width, height);
}

sint32 vid_begin(float r, float g, float b)
{
    sint32 width, height;
    if (frame_active)
        return 0;
    vid_resize(&width, &height);
    if (width <= 0 || height <= 0)
        return 0;
    frame_viewport.x = frame_viewport.y = 0;
    frame_viewport.w = width;
    frame_viewport.h = height;
    frame_width = width;
    frame_height = height;
    frame_clear[0] = r;
    frame_clear[1] = g;
    frame_clear[2] = b;
    frame_uniform_count = 0;
    frame_uniforms = NULL;
    uniform_cursor = uniform_chunks;
    for (VID_UNIFORM_CHUNK *chunk = uniform_chunks; chunk; chunk = chunk->next)
        chunk->used = 0;
    frame_pass = 0;
    vid_allocation_failed = 0;
    frame_active = 1;
    return 1;
}

sint32 vid_target_select(VID_TARGET *target)
{
    if (!frame_active || draw_head || frame_target || !target || target->released || target->texture->draw_refs)
        return 0;
    frame_target = target;
    target->fixed_width = target->fixed_height = 0;
    ++target->refs;
    return 1;
}

sint32 vid_target_select_offscreen(VID_TARGET *target)
{
    if (!vid_target_select(target))
        return 0;
    frame_target_present = 0;
    return 1;
}

sint32 vid_target_select_offscreen_size(VID_TARGET *target, uint32 width, uint32 height)
{
    if (!width || !height || width > 16384 || height > 16384 || !vid_target_select_offscreen(target))
        return 0;
    target->fixed_width = width;
    target->fixed_height = height;
    frame_width = (sint32)width;
    frame_height = (sint32)height;
    return 1;
}

sint32 vid_target_select_mip(VID_TARGET *target, uint32 level)
{
    if (!target || !target->ready || target->texture->cube || target->texture->array || (target->texture->format != VID_RGBA8 && target->texture->format != VID_RGBA16F) || !level || !target->texture->mip_levels || level >= vid_texture_mip_count(target->texture) || level >= 32)
        return 0;
    if (!vid_target_select_offscreen(target))
        return 0;
    frame_target_mip = level;
    return 1;
}

sint32 vid_target_select_cube_face(VID_TARGET *target, uint32 face)
{
    if (!target || !target->texture->cube || face >= 6)
        return 0;
    if (!vid_target_select_offscreen_size(target, target->texture->width, target->texture->height))
        return 0;
    target->face = face;
    if (!face)
        target->completed_faces = 0;
    return 1;
}

sint32 vid_target_select_depth_layer(VID_TARGET *target, uint32 layer)
{
    if (!target || !target->texture->array || target->texture->format != VID_DEPTH24 || layer >= target->texture->layers)
        return 0;
    if (!vid_target_select_offscreen_size(target, target->texture->width, target->texture->height))
        return 0;
    target->face = layer;
    return 1;
}

sint32 vid_target_select_offset(VID_TARGET *target, float y)
{
    if (!(y >= -FLT_MAX && y <= FLT_MAX) || !vid_target_select(target))
        return 0;
    frame_target_offset = y;
    return 1;
}

void vid_viewport(VID_RECT viewport)
{
    if (!frame_active || viewport.w <= 0 || viewport.h <= 0)
        abort();
    frame_viewport = viewport;
}

void vid_uniforms(const float *values, uint32 vectors)
{
    if (!frame_active || vectors > VID_UNIFORM_VECTORS || (vectors && !values))
        abort();
    frame_uniform_count = vectors;
    frame_uniforms = NULL;
    if (vectors)
    {
        VID_UNIFORM_CHUNK **link = uniform_cursor ? &uniform_cursor : &uniform_chunks;
        while (*link && (*link)->used + vectors * 4 > 16384)
            link = &(*link)->next;
        if (!*link)
        {
            *link = calloc(1, sizeof(**link));
            if (!*link)
                abort();
        }
        uniform_cursor = *link;
        float *snapshot = uniform_cursor->values + uniform_cursor->used;
        memcpy(snapshot, values, vectors * 4 * sizeof(float));
        uniform_cursor->used += vectors * 4;
        frame_uniforms = snapshot;
    }
}

static sint32 vid_record(VID_GEOMETRY *geometry, VID_INDEX *indices, uint32 first, uint32 count, VID_MATERIAL *material, uint32 pass)
{
    VID_DRAW *draw;
    if (!frame_active || (pass == 2 && !frame_pass))
        return 0;
    if (pass != 1 && pass != 3 && pass != 4 && (!geometry || geometry->released || !count || count % 3))
        return 0;
    if (pass != 1 && pass != 3 && pass != 4 && (indices ? indices->released || first > indices->count || count > indices->count - first : first > geometry->count || count > geometry->count - first))
        return 0;
    if (pass != 2 && pass != 3 && pass != 4 && (!material || material->released || material->data.texture != material->held_texture || material->data.shader != material->held_shader))
        return 0;
    if (pass != 2 && pass != 3 && pass != 4 && frame_target && ((material->data.texture == frame_target->texture || (frame_target->depth_texture && material->data.texture == frame_target->depth_texture) || (frame_target->surface_texture && material->data.texture == frame_target->surface_texture))))
        return 0;
    if (pass != 2 && pass != 3 && pass != 4)
        for (uint32 index = 0; index < VID_AUX_TEXTURES; ++index)
            if (material->data.aux[index] != material->held_aux[index] || (frame_target && (material->data.aux[index] == frame_target->texture || (frame_target->depth_texture && material->data.aux[index] == frame_target->depth_texture) || (frame_target->surface_texture && material->data.aux[index] == frame_target->surface_texture))))
                return 0;
    draw = draw_pool;
    if (draw)
        draw_pool = draw->next;
    else
        draw = malloc(sizeof(*draw));
    if (!draw)
        return 0;
    memset(draw, 0, sizeof(*draw));
    draw->geometry = geometry;
    draw->material = material;
    if (!pass || pass == 3 || pass == 4)
        frame_pass = 0;
    draw->pass = pass;
    draw->indices = indices;
    if (material)
        draw->data = material->data;
    draw->uniform_count = frame_uniform_count;
    draw->uniforms = frame_uniforms;
    draw->viewport = frame_viewport;
    draw->first = first;
    draw->count = count;
    memcpy(draw->fog, frame_fog, sizeof(draw->fog));
    if (geometry)
        ++geometry->refs;
    if (indices)
        ++indices->refs;
    if (material)
        ++material->refs;
    if (draw->data.texture)
    {
        ++draw->data.texture->refs;
        ++draw->data.texture->draw_refs;
    }
    for (uint32 idx = 0; idx < VID_AUX_TEXTURES; ++idx)
        if (draw->data.aux[idx])
        {
            ++draw->data.aux[idx]->refs;
            ++draw->data.aux[idx]->draw_refs;
        }
    if (draw_tail)
        draw_tail->next = draw;
    else
        draw_head = draw;
    draw_tail = draw;
    return 1;
}

sint32 vid_draw(VID_GEOMETRY *geometry, uint32 first, uint32 count, VID_MATERIAL *material)
{
    return vid_record(geometry, NULL, first, count, material, 0);
}

void vid_pass(VID_MATERIAL *material)
{
    if (frame_active && frame_pass && material && !material->released && frame_pass_snapshot && frame_pass_snapshot->material == material && memcmp(&frame_pass_snapshot->viewport, &frame_viewport, sizeof(frame_viewport)) == 0 && memcmp(frame_pass_snapshot->fog, frame_fog, sizeof(frame_fog)) == 0 && (!frame_uniform_count || (draw_tail->uniform_count == frame_uniform_count && (draw_tail->uniforms == frame_uniforms || memcmp(draw_tail->uniforms, frame_uniforms, frame_uniform_count * 4 * sizeof(float)) == 0))))
        return;
    if (!vid_record(NULL, NULL, 0, 0, material, 1))
        abort();
    frame_pass = 1;
    frame_pass_snapshot = draw_tail;
}

sint32 vid_submit(VID_GEOMETRY *geometry, VID_INDEX *indices, uint32 first, uint32 count)
{
    return vid_record(geometry, indices, first, count, NULL, 2);
}

sint32 vid_clear_depth(void)
{
    return vid_record(NULL, NULL, 0, 0, NULL, 3);
}

sint32 vid_target_next_part(uint32 part)
{
    if (!frame_active || !frame_target || frame_target_present || (!frame_target->texture->cube && !frame_target->texture->array) || part >= (frame_target->texture->cube ? 6 : frame_target->texture->layers))
        return 0;
    return vid_record(NULL, NULL, part, 0, NULL, 4);
}

sint32 vid_cube_capture(VID_TARGET *target, const float clear[3], VID_CUBE_RENDER render, void *context)
{
    static const float rotations[6][3][3] = {{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}, {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}, {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
    if (frame_active || !target || target->released || !target->texture->cube || !clear || !render)
        return 0;
    if (!vid_begin(clear[0], clear[1], clear[2]))
        return 0;
    for (uint32 index = 0; index < 6; ++index)
    {
        if (!(index ? vid_target_next_part(index) : vid_target_select_cube_face(target, 0)))
        {
            vid_cancel();
            return 0;
        }
        VID_CUBE_FACE face;
        face.index = index;
        face.size = target->texture->width;
        memcpy(face.rotation, rotations[index], sizeof(face.rotation));
        vid_viewport((VID_RECT){0, 0, (sint32)face.size, (sint32)face.size});
        if (!render(context, &face))
        {
            vid_cancel();
            return 0;
        }
    }
    return vid_end();
}

sint32 vid_sample(VID_SAMPLE *sample, float x, float y)
{
    VID_DRAW *draw;
    if (!frame_active || !sample || sample->released || !(x >= 0 && x < 1 && y >= 0 && y < 1))
        return 0;
    draw = draw_pool;
    if (draw)
        draw_pool = draw->next;
    else
        draw = malloc(sizeof(*draw));
    if (!draw)
        return 0;
    memset(draw, 0, sizeof(*draw));
    draw->sample = sample;
    draw->sample_x = x;
    draw->sample_y = y;
    draw->viewport = frame_viewport;
    ++sample->refs;
    if (draw_tail)
        draw_tail->next = draw;
    else
        draw_head = draw;
    draw_tail = draw;
    return 1;
}

void vid_fog(const VID_FOG *fog)
{
    if (!frame_active || (fog && !(fog->near_dist > 0 && fog->far_dist > fog->near_dist && fog->horizon_dist > fog->far_dist && fog->focal[0] > 0 && fog->focal[1] > 0)))
        abort();
    memset(frame_fog, 0, sizeof(frame_fog));
    if (fog)
    {
        frame_fog[0] = fog->near_dist;
        frame_fog[1] = fog->far_dist;
        frame_fog[2] = fog->horizon_dist;
        memcpy(frame_fog + 4, fog->color, sizeof(fog->color));
        frame_fog[8] = 1.0f / fog->focal[0];
        frame_fog[9] = 1.0f / fog->focal[1];
    }
}

void vid_cancel(void)
{
    VID_DRAW *draw;
    while ((draw = draw_head) != NULL)
    {
        draw_head = draw->next;
        if (draw->sample)
            sample_release(draw->sample);
        else if (draw->geometry)
            geometry_release(draw->geometry);
        if (draw->data.texture)
        {
            --draw->data.texture->draw_refs;
            texture_release(draw->data.texture);
        }
        for (uint32 idx = 0; idx < VID_AUX_TEXTURES; ++idx)
            if (draw->data.aux[idx])
            {
                --draw->data.aux[idx]->draw_refs;
                texture_release(draw->data.aux[idx]);
            }
        if (draw->indices)
            index_release(draw->indices);
        if (draw->material)
            material_release(draw->material);
        draw->next = draw_pool;
        draw_pool = draw;
    }
    draw_tail = NULL;
    if (frame_target)
    {
        target_release(frame_target);
        frame_target = NULL;
    }
    frame_active = 0;
    frame_target_present = 1;
    frame_target_offset = 0;
    frame_target_mip = 0;
    memset(frame_fog, 0, sizeof(frame_fog));
}

sint32 vid_end(void)
{
    sint32 result;
#if defined(AP_WEB)
    sint32 present;
#endif
    if (!frame_active)
        return 0;
#if defined(AP_WEB)
    present = initialized && (!frame_target || frame_target_present);
#endif
    result = initialized ? vid_render(draw_head, frame_clear, frame_width, frame_height) : xport_is_headless();
    vid_cancel();
#if defined(AP_WEB)
    if (result && present)
        xport_present_complete();
#endif
    return result;
}

sint32 vid_init(void *window)
{
    if (initialized)
        return 1;
    vid_anisotropy_limit = 0.0f;
    initialized = vid_context_init(window);
#if !defined(AP_WEB)
    if (initialized)
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
#endif
    return initialized;
}

void vid_free(void)
{
    VID_DRAW *draw;
    vid_cancel();
    while (targets)
        vid_target_free(targets);
    while (samples)
        vid_sample_free(samples);
    while (materials)
        vid_mat_free(materials);
    while (index_buffers)
        vid_index_free(index_buffers);
    while (geometries)
        vid_mesh_free(geometries);
    while (textures)
        vid_tex_free(textures);
    while (shaders)
        vid_shader_free(shaders);
    while ((draw = draw_pool) != NULL)
    {
        draw_pool = draw->next;
        free(draw);
    }
    while (uniform_chunks)
    {
        VID_UNIFORM_CHUNK *chunk = uniform_chunks;
        uniform_chunks = chunk->next;
        free(chunk);
    }
    uniform_cursor = NULL;
    frame_uniforms = NULL;
    if (initialized)
        vid_context_free();
    initialized = 0;
}

sint32 vid_blit(const uint32 *pixels, sint32 width, sint32 height, VID_RECT src)
{
    sint32 client_width, client_height;
    VID_RECT dst;
    size_t count, index;
    float u0, v0, u1, v1;
    VID_VERTEX vertices[6];
    const uint8 corners[6] = {0, 1, 2, 2, 1, 3};
    if (!initialized || !pixels || width <= 0 || height <= 0 || src.x < 0 || src.y < 0 || src.w <= 0 || src.h <= 0 || src.w > width || src.h > height || src.x > width - src.w || src.y > height - src.h || (size_t)width > SIZE_MAX / 4 / height)
        return 0;
    vid_allocation_failed = 0;
    vid_update_size(&client_width, &client_height);
    dst = vid_best_fit(client_width, client_height);
    if (!dst.w || !dst.h)
        return 1;
    count = (size_t)width * height;
    if (count * 4 > vid_blit_capacity)
    {
        uint8 *buffer = realloc(vid_blit_pixels, count * 4);
        if (!buffer)
            return 0;
        vid_blit_pixels = buffer;
        vid_blit_capacity = count * 4;
    }
    for (index = 0; index < count; ++index)
    {
        uint32 color = pixels[index];
        vid_blit_pixels[index * 4] = (uint8)(color >> 16);
        vid_blit_pixels[index * 4 + 1] = (uint8)(color >> 8);
        vid_blit_pixels[index * 4 + 2] = (uint8)color;
        vid_blit_pixels[index * 4 + 3] = 255;
    }
    u0 = (float)src.x / width;
    v0 = (float)src.y / height;
    u1 = (float)(src.x + src.w) / width;
    v1 = (float)(src.y + src.h) / height;
    for (index = 0; index < 6; ++index)
    {
        uint8 corner = corners[index];
        VID_VERTEX *vertex = &vertices[index];
        vertex->pos[0] = corner & 1 ? 1.0f : -1.0f;
        vertex->pos[1] = corner & 2 ? -1.0f : 1.0f;
        vertex->pos[2] = 0.0f;
        vertex->pos[3] = 1.0f;
        vertex->uv[0] = corner & 1 ? u1 : u0;
        vertex->uv[1] = corner & 2 ? v1 : v0;
        vertex->color[0] = vertex->color[1] = vertex->color[2] = vertex->color[3] = 1.0f;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(dst.x, client_height - dst.y - dst.h, dst.w, dst.h);
    glUseProgram(vid_program);
    glUniform1i(vid_textured, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vid_blit_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (vid_blit_width != width || vid_blit_height != height)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, vid_blit_pixels);
        if (glGetError() != GL_NO_ERROR)
            return 0;
        vid_blit_width = width;
        vid_blit_height = height;
    }
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, vid_blit_pixels);
    vid_vertices(vid_blit_buffer, sizeof(VID_VERTEX), vid_array);
    vid_buffer_upload(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW, &vid_blit_storage, sizeof(vertices));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    return vid_swap();
}
