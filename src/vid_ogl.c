#if !defined(GAPI_OGL)
    #error Define GAPI_OGL when compiling vid_ogl.c
#endif

#include "vid.h"
#include <stdlib.h>
#include <string.h>

typedef char VID_VERTEX16_SIZE[(sizeof(VID_VERTEX16) == 52) ? 1 : -1];
typedef char VID_VERTEX2D_SIZE[(sizeof(VID_VERTEX2D) == 44) ? 1 : -1];

struct VID_GEOMETRY
{
    struct VID_GEOMETRY *next;
    void *vertices;
    uint32 stride;
    uint32 count, version, uploaded, object, array;
    uint32 refs, released;
};

struct VID_INDEX
{
    VID_INDEX *next;
    uint32 *values;
    uint32 capacity, count, version, uploaded, object, refs, released, immutable;
};

static VID_INDEX *index_buffers;

struct VID_TEXTURE
{
    struct VID_TEXTURE *next;
    uint8 *pixels;
    uint32 width, height, format, version, uploaded, object, allocated;
    uint32 refs, released, draw_refs;
};

struct VID_SHADER
{
    struct VID_SHADER *next;
    uint32 program, refs, released;
    sint32 textured, params, fog, uniforms, aux[2];
};

static VID_SHADER *shaders;

struct VID_MATERIAL
{
    struct VID_MATERIAL *next;
    VID_MATERIAL_DATA data;
    VID_TEXTURE *held_texture;
    VID_TEXTURE *held_aux[2];
    VID_SHADER *held_shader;
    uint32 refs, released;
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
    float uniforms[VID_UNIFORM_VECTORS * 4];
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
    if (!vid_glBufferData)
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

static GLuint vid_program, vid_array, vid_blit_buffer, vid_blit_texture;
static GLint vid_textured;
static uint8 *vid_blit_pixels;
static size_t vid_blit_capacity;

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
    vid_program = vid_array = vid_blit_buffer = vid_blit_texture = 0;
    free(vid_blit_pixels);
    vid_blit_pixels = NULL;
    vid_blit_capacity = 0;
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
                                           "out vec4 outColor;\n"
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
    // Preserve the game clock independently of the desktop swap interval
    {
        PFNWGLSWAPINTERVALEXTPROC swap_interval = (PFNWGLSWAPINTERVALEXTPROC)vid_proc("wglSwapIntervalEXT");
        if (swap_interval)
            swap_interval(0);
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

static sint32 vid_swap(void)
{
#if defined(AP_WEB)
    return glGetError() == GL_NO_ERROR;
#else
    return SwapBuffers(vid_device) && glGetError() == GL_NO_ERROR;
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

static void vid_tex_upload(VID_TEXTURE *texture, uint32 filter, uint32 repeat)
{
    if (!texture->object)
        glGenTextures(1, &texture->object);
    glBindTexture(GL_TEXTURE_2D, texture->object);
    if (texture->uploaded != texture->version)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        GLenum format = texture->format == VID_R8UI ? GL_RED_INTEGER : texture->format == VID_RGBA16UI ? GL_RGBA_INTEGER : GL_RGBA;
        GLenum type = texture->format == VID_RGBA5551 ? GL_UNSIGNED_SHORT_5_5_5_1 : texture->format == VID_RGBA16UI ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        if (!texture->allocated)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, texture->format == VID_RGBA5551 ? GL_RGB5_A1 : texture->format == VID_R8UI ? GL_R8UI : texture->format == VID_RGBA16UI ? GL_RGBA16UI : GL_RGBA, texture->width, texture->height, 0, format, type, texture->pixels);
            texture->allocated = 1;
        }
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height, format, type, texture->pixels);
        texture->uploaded = texture->version;
    }
    if (texture->format >= VID_R8UI)
        filter = repeat = 0;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

static sint32 vid_render(const VID_DRAW *draws, const float *clear, sint32 frame_width, sint32 frame_height)
{
    sint32 width, height;
    const VID_MATERIAL_DATA *pass = NULL;
    vid_update_size(&width, &height);
    if (width <= 0 || height <= 0)
        return 1;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(clear[0], clear[1], clear[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(vid_program);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    while (draws)
    {
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
        if (draws->pass != 2)
        {
            glViewport(viewport.x, height - viewport.y - viewport.h, viewport.w, viewport.h);
            glScissor(viewport.x, height - viewport.y - viewport.h, viewport.w, viewport.h);
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
        if (draws->pass != 2)
        {
            if (material->cull)
            {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glFrontFace(material->cull == 1 ? GL_CW : GL_CCW);
            }
            else
                glDisable(GL_CULL_FACE);
            if (material->depth_test)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            glDepthMask(material->depth_write ? GL_TRUE : GL_FALSE);
            uint32 blend = material->blend;
            if (!blend && material->shader && draws->fog[1] > draws->fog[0])
                blend = VID_BLEND_ALPHA;
            if (blend)
            {
                glEnable(GL_BLEND);
                glBlendEquation(blend == VID_BLEND_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
                glBlendFunc(GL_SRC_ALPHA, blend == VID_BLEND_ALPHA ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
            }
            else
                glDisable(GL_BLEND);
            if (material->shader)
            {
                glUseProgram(material->shader->program);
                if (material->shader->textured >= 0)
                    glUniform1i(material->shader->textured, material->texture != NULL);
                if (material->shader->params >= 0)
                    glUniform4fv(material->shader->params, 2, material->params);
                {
                    float fog[12];
                    memcpy(fog, draws->fog, sizeof(fog));
                    fog[3] = (float)material->blend;
                    glUniform4fv(material->shader->fog, 3, fog);
                }
            }
            else
            {
                glUseProgram(vid_program);
                glUniform1i(vid_textured, material->texture != NULL);
            }
            if (material->texture)
                vid_tex_upload(material->texture, material->filter, material->repeat);
            for (uint32 idx = 0; idx < 2; ++idx)
                if (material->aux[idx])
                {
                    glActiveTexture(GL_TEXTURE1 + idx);
                    vid_tex_upload(material->aux[idx], 0, 0);
                    if (material->shader)
                        glUniform1i(material->shader->aux[idx], 1 + idx);
                }
            glActiveTexture(GL_TEXTURE0);
        }
        if (material->shader && draws->uniform_count)
            glUniform4fv(material->shader->uniforms, draws->uniform_count, draws->uniforms);
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
        }
        else
            glBindVertexArray(geometry->array);
        if (geometry->uploaded != geometry->version)
        {
            glBindBuffer(GL_ARRAY_BUFFER, geometry->object);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)geometry->count * geometry->stride), geometry->vertices, GL_DYNAMIC_DRAW);
            geometry->uploaded = geometry->version;
        }
        if (draws->indices)
        {
            VID_INDEX *indices = draws->indices;
            if (!indices->object)
                glGenBuffers(1, &indices->object);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->object);
            if (indices->uploaded != indices->version)
            {
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)indices->count * sizeof(uint32)), indices->values, indices->immutable ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
                indices->uploaded = indices->version;
                if (indices->immutable)
                {
                    free(indices->values);
                    indices->values = NULL;
                }
            }
            glDrawElements(GL_TRIANGLES, draws->count, GL_UNSIGNED_INT, (const void *)((size_t)draws->first * sizeof(uint32)));
        }
        else
            glDrawArrays(GL_TRIANGLES, draws->first, draws->count);
        draws = draws->next;
    }
    glDisable(GL_SCISSOR_TEST);
    return vid_swap();
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
static float frame_uniforms[VID_UNIFORM_VECTORS * 4];
static uint32 frame_uniform_count;
static sint32 frame_width, frame_height;
static float frame_clear[3];
static sint32 frame_active, initialized, frame_pass;

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
    for (uint32 idx = 0; idx < 2; ++idx)
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
    return geometry && geometry->stride == sizeof(VID_VERTEX2D) && !geometry->released && geometry->refs == 1 ? geometry->vertices : NULL;
}

VID_GEOMETRY *vid_mesh_create_16(uint32 vertices)
{
    return vid_mesh_create_size(vertices, sizeof(VID_VERTEX16));
}

VID_VERTEX16 *vid_mesh_data_16(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX16) && !geometry->released && geometry->refs == 1 ? geometry->vertices : NULL;
}

VID_VERTEX3D *vid_mesh_data_3d(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX3D) && !geometry->released && geometry->refs == 1 ? geometry->vertices : NULL;
}

VID_VERTEX *vid_mesh_data(VID_GEOMETRY *geometry)
{
    return geometry && geometry->stride == sizeof(VID_VERTEX) && !geometry->released && geometry->refs == 1 ? geometry->vertices : NULL;
}

void vid_mesh_commit(VID_GEOMETRY *geometry)
{
    if (!geometry || geometry->released || geometry->refs != 1)
        abort();
    if (!++geometry->version)
        geometry->version = 1;
    geometry->uploaded = 0;
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
    if (format > VID_RGBA16UI || !width || !height || width > SIZE_MAX / 8 / height)
        return NULL;
    texture = calloc(1, sizeof(*texture));
    if (!texture)
        return NULL;
    texture->pixels = calloc((size_t)width * height, format == VID_RGBA5551 ? 2 : format == VID_R8UI ? 1 : format == VID_RGBA16UI ? 8 : 4);
    if (!texture->pixels)
    {
        free(texture);
        return NULL;
    }
    texture->format = format;
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

uint8 *vid_tex_data(VID_TEXTURE *texture)
{
    // Material ownership alone does not prevent edits between frames
    return texture && !texture->released ? texture->pixels : NULL;
}

void vid_tex_commit(VID_TEXTURE *texture)
{
    if (!texture || texture->released)
        abort();
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

VID_SHADER *vid_shader_create(const char *vertex_source, const char *fragment)
{
    static const char prefix[] = VID_GLSL "in vec2 vUV; in vec4 vColor; in float vDepth; uniform sampler2D uTex; uniform int uTextured; uniform vec4 uParams[2]; out vec4 outColor;\n"
                                          "in vec2 vFog; uniform vec4 uFog[3];\n"
                                          "vec4 vidFog(vec4 color){if(uFog[0].y<=uFog[0].x)return color;"
                                          "float f=vFog.x;"
                                          "if(uFog[0].w>=2.0)color.rgb*=1.0-f;"
                                          "else {color.rgb=mix(color.rgb,uFog[1].rgb,f);"
                                          "float h=vFog.y;"
                                          "color.a*=1.0-h;if(color.a<=0.0)discard;}return color;}\n";
    VID_SHADER *shader;
    GLuint vertex, pixel;
    GLint success;
    char log[1024];
    char *source;
    size_t bytes;
    if (!initialized || !fragment)
        return NULL;
    bytes = strlen(fragment);
    if (bytes > SIZE_MAX - sizeof(prefix))
        return NULL;
    source = malloc(sizeof(prefix) + bytes);
    shader = calloc(1, sizeof(*shader));
    if (!source || !shader)
    {
        free(source);
        free(shader);
        return NULL;
    }
    memcpy(source, prefix, sizeof(prefix) - 1);
    memcpy(source + sizeof(prefix) - 1, fragment, bytes + 1);
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
    shader->aux[0] = glGetUniformLocation(shader->program, "uAux0");
    shader->aux[1] = glGetUniformLocation(shader->program, "uAux1");
    shader->refs = 1;
    shader->next = shaders;
    shaders = shader;
    return shader;
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

void vid_mat_commit(VID_MATERIAL *material)
{
    VID_TEXTURE *texture;
    VID_SHADER *shader;
    if (!material || material->released || material->refs != 1)
        abort();
    texture = material->data.texture;
    shader = material->data.shader;
    for (uint32 idx = 0; idx < 2; ++idx)
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
    frame_pass = 0;
    frame_active = 1;
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
    if (vectors)
        memcpy(frame_uniforms, values, vectors * 4 * sizeof(float));
}

static sint32 vid_record(VID_GEOMETRY *geometry, VID_INDEX *indices, uint32 first, uint32 count, VID_MATERIAL *material, uint32 pass)
{
    VID_DRAW *draw;
    if (!frame_active || (pass == 2 && !frame_pass))
        return 0;
    if (pass != 1 && (!geometry || geometry->released || !count || count % 3))
        return 0;
    if (pass != 1 && (indices ? indices->released || first > indices->count || count > indices->count - first : first > geometry->count || count > geometry->count - first))
        return 0;
    if (pass != 2 && (!material || material->released || material->data.texture != material->held_texture || material->data.shader != material->held_shader || material->data.aux[0] != material->held_aux[0] || material->data.aux[1] != material->held_aux[1]))
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
    if (!pass)
        frame_pass = 0;
    draw->pass = pass;
    draw->indices = indices;
    if (material)
        draw->data = material->data;
    draw->uniform_count = frame_uniform_count;
    memcpy(draw->uniforms, frame_uniforms, frame_uniform_count * 4 * sizeof(float));
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
    for (uint32 idx = 0; idx < 2; ++idx)
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
    if (!vid_record(NULL, NULL, 0, 0, material, 1))
        abort();
    frame_pass = 1;
}

sint32 vid_submit(VID_GEOMETRY *geometry, VID_INDEX *indices, uint32 first, uint32 count)
{
    return vid_record(geometry, indices, first, count, NULL, 2);
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
        for (uint32 idx = 0; idx < 2; ++idx)
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
    frame_active = 0;
    memset(frame_fog, 0, sizeof(frame_fog));
}

sint32 vid_end(void)
{
    sint32 result;
    if (!frame_active)
        return 0;
    result = initialized ? vid_render(draw_head, frame_clear, frame_width, frame_height) : xport_is_headless();
    vid_cancel();
    return result;
}

sint32 vid_init(void *window)
{
    if (initialized)
        return 1;
    initialized = vid_context_init(window);
    return initialized;
}

void vid_free(void)
{
    VID_DRAW *draw;
    vid_cancel();
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, vid_blit_pixels);
    vid_vertices(vid_blit_buffer, sizeof(VID_VERTEX), vid_array);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    return vid_swap();
}
