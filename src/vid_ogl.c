#if !defined(GAPI_OGL)
    #error Define GAPI_OGL when compiling vid_ogl.c
#endif

#include "vid.h"
#include <stdlib.h>
#include <string.h>

struct VID_GEOMETRY
{
    struct VID_GEOMETRY *next;
    VID_VERTEX *vertices;
    uint32 count, version, uploaded, object;
    uint32 refs, released;
};

struct VID_TEXTURE
{
    struct VID_TEXTURE *next;
    uint8 *pixels;
    uint32 width, height, format, version, uploaded, object;
    uint32 refs, released, draw_refs;
};

struct VID_SHADER
{
    struct VID_SHADER *next;
    uint32 program, refs, released;
    sint32 textured, params;
};

static VID_SHADER *shaders;

struct VID_MATERIAL
{
    struct VID_MATERIAL *next;
    VID_MATERIAL_DATA data;
    VID_TEXTURE *held_texture;
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
    VID_MATERIAL *material;
    VID_MATERIAL_DATA data;
    VID_RECT viewport;
    uint32 first, count;
    VID_SAMPLE *sample;
    float sample_x, sample_y;
} VID_DRAW;

#if defined(__EMSCRIPTEN__)
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
static PFNGLENABLEVERTEXATTRIBARRAYPROC vid_glEnableVertexAttribArray;
    #define glEnableVertexAttribArray vid_glEnableVertexAttribArray
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
    vid_glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)vid_proc("glEnableVertexAttribArray");
    if (!vid_glEnableVertexAttribArray)
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
                                                "out vec2 vUV; out vec4 vColor;\n"
                                                "void main(){gl_Position=aPos;vUV=aUV;vColor=aColor;}\n";

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
#if defined(__EMSCRIPTEN__)
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
#if defined(__EMSCRIPTEN__)
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
#if defined(__EMSCRIPTEN__)
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
#if defined(__EMSCRIPTEN__)
    return glGetError() == GL_NO_ERROR;
#else
    return SwapBuffers(vid_device) && glGetError() == GL_NO_ERROR;
#endif
}

static void vid_vertices(GLuint buffer)
{
    glBindVertexArray(vid_array);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(VID_VERTEX), (const void *)offsetof(VID_VERTEX, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VID_VERTEX), (const void *)offsetof(VID_VERTEX, uv));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(VID_VERTEX), (const void *)offsetof(VID_VERTEX, color));
}

static void vid_tex_upload(VID_TEXTURE *texture, uint32 filter, uint32 repeat)
{
    if (!texture->object)
        glGenTextures(1, &texture->object);
    glBindTexture(GL_TEXTURE_2D, texture->object);
    if (texture->uploaded != texture->version)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, texture->format == VID_RGBA5551 ? GL_RGB5_A1 : GL_RGBA, texture->width, texture->height, 0, GL_RGBA, texture->format == VID_RGBA5551 ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_BYTE, texture->pixels);
        texture->uploaded = texture->version;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

static sint32 vid_render(const VID_DRAW *draws, const float *clear, sint32 frame_width, sint32 frame_height)
{
    sint32 width, height;
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
        const VID_MATERIAL_DATA *material = &draws->data;
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
        glViewport(viewport.x, height - viewport.y - viewport.h, viewport.w, viewport.h);
        glScissor(viewport.x, height - viewport.y - viewport.h, viewport.w, viewport.h);
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
        if (material->depth_test)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        glDepthMask(material->depth_write ? GL_TRUE : GL_FALSE);
        if (material->blend)
        {
            glEnable(GL_BLEND);
            glBlendEquation(material->blend == VID_BLEND_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, material->blend == VID_BLEND_ALPHA ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
        }
        else
            glDisable(GL_BLEND);
        if (material->shader)
        {
            glUseProgram(material->shader->program);
            glUniform1i(material->shader->textured, material->texture != NULL);
            glUniform4fv(material->shader->params, 2, material->params);
        }
        else
        {
            glUseProgram(vid_program);
            glUniform1i(vid_textured, material->texture != NULL);
        }
        if (material->texture)
            vid_tex_upload(material->texture, material->filter, material->repeat);
        if (!geometry->object)
            glGenBuffers(1, &geometry->object);
        vid_vertices(geometry->object);
        if (geometry->uploaded != geometry->version)
        {
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)geometry->count * sizeof(VID_VERTEX)), geometry->vertices, GL_DYNAMIC_DRAW);
            geometry->uploaded = geometry->version;
        }
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
static sint32 frame_width, frame_height;
static float frame_clear[3];
static sint32 frame_active, initialized;

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
    free(material);
}

VID_GEOMETRY *vid_mesh_create(uint32 vertices)
{
    VID_GEOMETRY *geometry;
    if (!vertices || vertices > SIZE_MAX / sizeof(VID_VERTEX))
        return NULL;
    geometry = calloc(1, sizeof(*geometry));
    if (!geometry)
        return NULL;
    geometry->vertices = calloc(vertices, sizeof(VID_VERTEX));
    if (!geometry->vertices)
    {
        free(geometry);
        return NULL;
    }
    geometry->count = vertices;
    if (initialized)
        glGenBuffers(1, &geometry->object);
    geometry->refs = geometry->version = 1;
    geometry->next = geometries;
    geometries = geometry;
    return geometry;
}

VID_VERTEX *vid_mesh_data(VID_GEOMETRY *geometry)
{
    return geometry && !geometry->released && geometry->refs == 1 ? geometry->vertices : NULL;
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
    if (format > VID_RGBA5551 || !width || !height || width > SIZE_MAX / 4 / height)
        return NULL;
    texture = calloc(1, sizeof(*texture));
    if (!texture)
        return NULL;
    texture->pixels = calloc((size_t)width * height, format == VID_RGBA5551 ? 2 : 4);
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

VID_SHADER *vid_shader_create(const char *fragment)
{
    static const char prefix[] = VID_GLSL "in vec2 vUV; in vec4 vColor; uniform sampler2D uTex; uniform int uTextured; uniform vec4 uParams[2]; out vec4 outColor;\n";
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
    frame_active = 1;
    return 1;
}

void vid_viewport(VID_RECT viewport)
{
    if (!frame_active || viewport.w <= 0 || viewport.h <= 0)
        abort();
    frame_viewport = viewport;
}

sint32 vid_draw(VID_GEOMETRY *geometry, uint32 first, uint32 count, VID_MATERIAL *material)
{
    VID_DRAW *draw;
    if (!frame_active || !geometry || !material || geometry->released || material->released || !count || count % 3 || first > geometry->count || count > geometry->count - first || material->data.texture != material->held_texture || material->data.shader != material->held_shader)
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
    draw->data = material->data;
    draw->viewport = frame_viewport;
    draw->first = first;
    draw->count = count;
    ++geometry->refs;
    ++material->refs;
    if (draw->data.texture)
    {
        ++draw->data.texture->refs;
        ++draw->data.texture->draw_refs;
    }
    if (draw_tail)
        draw_tail->next = draw;
    else
        draw_head = draw;
    draw_tail = draw;
    return 1;
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

void vid_cancel(void)
{
    VID_DRAW *draw;
    while ((draw = draw_head) != NULL)
    {
        draw_head = draw->next;
        if (draw->sample)
            sample_release(draw->sample);
        else
            geometry_release(draw->geometry);
        if (draw->data.texture)
        {
            --draw->data.texture->draw_refs;
            texture_release(draw->data.texture);
        }
        if (draw->material)
            material_release(draw->material);
        draw->next = draw_pool;
        draw_pool = draw;
    }
    draw_tail = NULL;
    frame_active = 0;
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
    vid_vertices(vid_blit_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    return vid_swap();
}
