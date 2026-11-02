#define WND_TITLE "VID test"
#define WND_WIDTH 960
#define WND_HEIGHT 600
#define FIELD_RATE 50
#define GAPI_OGL
#include <windows.h>
#include <GL/gl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned char *capture;
static int capture_width, capture_height;
static unsigned int capture_swap_count;

static BOOL WINAPI capture_swap(HDC device)
{
    ++capture_swap_count;
    (void)device;
    glReadPixels(0, 0, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, capture);
    return TRUE;
}

#define SwapBuffers capture_swap
#include "../../../src/vid_ogl.c"
#undef SwapBuffers

int xport_hd;
sint32 xport_is_headless(void) { return 0; }
void xport_message_error(const char *title, const char *message)
{
    fprintf(stderr, "%s: %s\n", title, message);
}

static void check_pixel(int x, int y, int r, int g, int b)
{
    const unsigned char *pixel = capture + ((capture_height - 1 - y) * capture_width + x) * 4;
    if (pixel[0] != r || pixel[1] != g || pixel[2] != b)
    {
        fprintf(stderr, "Pixel %d,%d: %u,%u,%u expected %d,%d,%d\n", x, y, pixel[0], pixel[1], pixel[2], r, g, b);
        abort();
    }
}

static void check_shader_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    char *source;
    VID_SHADER *shader;
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    source = malloc((size_t)size + 1);
    assert(source && fread(source, 1, (size_t)size, file) == (size_t)size);
    source[size] = 0;
    fclose(file);
    shader = vid_shader_create(NULL, source);
    assert(shader);
    vid_shader_free(shader);
    free(source);
    printf("PASS: Native shader compilation/link: %s\n", path);
}

int main(int argc, char **argv)
{
    WNDCLASSA cls = {0};
    HWND window;
    RECT window_rect = {0, 0, WND_WIDTH, WND_HEIGHT};
    GLuint framebuffer, target;
    PFNGLGENFRAMEBUFFERSPROC gen_framebuffers;
    PFNGLBINDFRAMEBUFFERPROC bind_framebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC attach_texture;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer;
    PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers;
    VID_GEOMETRY *mesh;
    VID_MATERIAL *material;
    VID_VERTEX *vertices;
    const uint32 pixels[4] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff};
    VID_RECT src = {0, 0, 2, 2};
    VID_RECT fit;
    cls.lpfnWndProc = DefWindowProcA;
    cls.style = CS_OWNDC;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "VIDTest";
    assert(RegisterClassA(&cls));
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
    window = CreateWindowA(cls.lpszClassName, WND_TITLE, WS_OVERLAPPEDWINDOW, 0, 0,
        window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, NULL, NULL, cls.hInstance, NULL);
    assert(window && vid_init(window));
    vid_resize(&capture_width, &capture_height);
    assert(capture_width == WND_WIDTH && capture_height == WND_HEIGHT);
    printf("GL: %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    if (argc > 1)
        check_shader_file(argv[1]);
    capture = malloc((size_t)capture_width * capture_height * 4);
    assert(capture);
    gen_framebuffers = (PFNGLGENFRAMEBUFFERSPROC)vid_proc("glGenFramebuffers");
    bind_framebuffer = (PFNGLBINDFRAMEBUFFERPROC)vid_proc("glBindFramebuffer");
    attach_texture = (PFNGLFRAMEBUFFERTEXTURE2DPROC)vid_proc("glFramebufferTexture2D");
    check_framebuffer = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)vid_proc("glCheckFramebufferStatus");
    delete_framebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)vid_proc("glDeleteFramebuffers");
    assert(gen_framebuffers && bind_framebuffer && attach_texture && check_framebuffer && delete_framebuffers);
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, capture_width, capture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gen_framebuffers(1, &framebuffer);
    bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
    attach_texture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    assert(check_framebuffer(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    fit = vid_best_fit(capture_width, capture_height);
    assert(fit.x == 80 && fit.y == 0 && fit.w == 800 && fit.h == 600);
    assert(vid_blit(pixels, 2, 2, src));
    check_pixel(0, 300, 0, 0, 0);
    check_pixel(280, 150, 255, 0, 0);
    check_pixel(680, 150, 0, 255, 0);
    check_pixel(280, 450, 0, 0, 255);
    check_pixel(680, 450, 255, 255, 255);
    src.x = 1; src.w = src.h = 1;
    assert(vid_blit(pixels, 2, 2, src));
    check_pixel(480, 300, 0, 255, 0);
    src.x = 2;
    assert(!vid_blit(pixels, 2, 2, src));

    mesh = vid_mesh_create(3);
    material = vid_mat_create();
    assert(mesh && material);
    vertices = vid_mesh_data(mesh);
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[i].pos[0] = i == 0 ? -0.5f : i == 1 ? 0.5f : 0.0f;
        vertices[i].pos[1] = i == 2 ? 0.5f : -0.5f;
        vertices[i].pos[3] = 1.0f;
        vertices[i].color[0] = vertices[i].color[3] = 1.0f;
    }
    vid_mesh_commit(mesh);
    assert(vid_begin(0, 0, 1));
    assert(!vid_draw(mesh, 1, 3, material));
    assert(vid_draw(mesh, 0, 3, material));
    assert(!vid_mesh_data(mesh) && !vid_mat_data(material));
    vid_mesh_free(mesh);
    vid_mat_free(material);
    assert(vid_end());
    check_pixel(480, 300, 255, 0, 0);
    check_pixel(10, 10, 0, 0, 255);
    assert(!geometries && !materials);

    material = vid_mat_create();
    {
        VID_TEXTURE *texture = vid_tex_create(1, 1);
        VID_MATERIAL_DATA *data = vid_mat_data(material);
        assert(texture);
        data->texture = texture;
        vid_mat_commit(material);
        vid_tex_free(texture);
        assert(textures);
        vid_mat_free(material);
        assert(!textures && !materials);
    }
    {
        static const uint8 order[6] = {0, 1, 2, 2, 1, 3};
        static const uint8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        VID_TEXTURE *texture = vid_tex_create(2, 2);
        VID_MATERIAL_DATA *data;
        mesh = vid_mesh_create(6);
        material = vid_mat_create();
        assert(texture && mesh && material);
        memcpy(vid_tex_data(texture), texels, sizeof(texels));
        vid_tex_commit(texture);
        data = vid_mat_data(material);
        data->texture = texture;
        data->repeat = 1;
        vid_mat_commit(material);
        vertices = vid_mesh_data(mesh);
        for (unsigned i = 0; i < 6; ++i)
        {
            unsigned corner = order[i];
            vertices[i].pos[0] = corner & 1 ? 1.0f : -1.0f;
            vertices[i].pos[1] = corner & 2 ? -1.0f : 1.0f;
            vertices[i].pos[3] = 1;
            vertices[i].uv[0] = corner & 1 ? 2.0f : 0;
            vertices[i].uv[1] = corner & 2 ? 2.0f : 0;
            for (unsigned channel = 0; channel < 4; ++channel)
                vertices[i].color[channel] = 1;
        }
        vid_mesh_commit(mesh);
        assert(vid_begin(0, 0, 0) && vid_draw(mesh, 0, 6, material) && vid_end());
        check_pixel(capture_width / 8, capture_height / 8, 255, 0, 0);
        check_pixel(capture_width * 3 / 8, capture_height / 8, 0, 255, 0);
        check_pixel(capture_width * 5 / 8, capture_height / 8, 255, 0, 0);
        check_pixel(capture_width / 8, capture_height * 5 / 8, 255, 0, 0);
        vid_mat_data(material)->repeat = 0;
        vid_mat_commit(material);
        assert(vid_begin(0, 0, 0) && vid_draw(mesh, 0, 6, material) && vid_end());
        check_pixel(capture_width * 5 / 8, capture_height / 8, 0, 255, 0);
        check_pixel(capture_width / 8, capture_height * 5 / 8, 0, 0, 255);
        {
            VID_SHADER *shader = vid_shader_create(NULL, "void main(){outColor=vec4(texture(uTex,vUV).rgb*uParams[0].rgb,1.0);}");
            VID_TEXTURE *packed = vid_tex_create_fmt(2, 2, VID_RGBA5551);
            uint16 *data16;
            assert(shader && packed);
            data16 = (uint16 *)vid_tex_data(packed);
            data16[0] = 0xf801;
            data16[1] = 0x07c1;
            data16[2] = 0x003f;
            data16[3] = 0xffff;
            vid_tex_commit(packed);
            data = vid_mat_data(material);
            data->texture = packed;
            data->shader = shader;
            data->params[0] = data->params[2] = 1;
            vid_mat_commit(material);
            vid_shader_free(shader);
            assert(shaders);
            assert(vid_begin(0, 0, 0) && vid_draw(mesh, 0, 6, material));
            data16 = (uint16 *)vid_tex_data(packed);
            assert(data16);
            data16[0] = 0x003f;
            vid_tex_commit(packed);
            assert(vid_end());
            check_pixel(capture_width / 8, capture_height / 8, 0, 0, 255);
            check_pixel(capture_width * 5 / 8, capture_height / 8, 0, 0, 0);
            check_pixel(capture_width * 5 / 8, capture_height * 5 / 8, 255, 0, 255);
            vid_tex_free(packed);
        }
        {
            VID_TEXTURE *array = vid_tex_create_array_fmt(8, 8, 2, VID_RGBA5551);
            VID_SHADER *shader = vid_shader_create_array(NULL, "void main(){outColor=vec4(texture(uTex,vec3(vUV,1.0)).rgb,1.0);}");
            uint16 *pixels16;
            GLint maximum_level, internal_format;
            assert(array && shader);
            pixels16 = (uint16 *)vid_tex_data(array);
            for (unsigned i = 0; i < 64; ++i)
            {
                pixels16[i] = 0xf801;
                pixels16[64 + i] = 0x07c1;
            }
            vid_tex_set_mip_levels(array, 3);
            vid_tex_commit(array);
            vid_tex_static(array);
            assert(vid_tex_data(array) == NULL && array->pixels != NULL);
            vid_mesh_static(mesh);
            assert(vid_mesh_data(mesh) == NULL && mesh->vertices == NULL);
            data = vid_mat_data(material);
            data->texture = array;
            data->shader = shader;
            data->filter = VID_FILTER_ANISOTROPIC;
            vid_mat_commit(material);
            assert(vid_begin(0, 0, 0) && vid_draw(mesh, 0, 6, material) && vid_end());
            assert(array->pixels == NULL && array->uploaded == array->version && array->mip_version == array->version);
            glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, &maximum_level);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
            assert(maximum_level == 2 && internal_format == GL_RGB5_A1 && glGetError() == GL_NO_ERROR);
            check_pixel(capture_width / 2, capture_height / 2, 0, 255, 0);
            assert(vid_begin(0, 0, 0) && vid_draw(mesh, 0, 6, material) && vid_end());
            check_pixel(capture_width / 2, capture_height / 2, 0, 255, 0);
            vid_shader_free(shader);
            vid_tex_free(array);
        }
        vid_mesh_free(mesh);
        vid_mat_free(material);
        vid_tex_free(texture);
        assert(!geometries && !materials && !textures && !shaders);
    }
    {
        static const uint8 order[6] = {0, 1, 2, 2, 1, 3};
        VID_GEOMETRY *halves[2];
        RECT resized = {0, 0, 961, 601};
        VID_SAMPLE *probes[2] = {vid_sample_create(), vid_sample_create()};
        assert(probes[0] && probes[1]);
        material = vid_mat_create();
        assert(material && vid_begin(0, 0, 0));
        for (unsigned side = 0; side < 2; ++side)
        {
            halves[side] = vid_mesh_create(6);
            assert(halves[side]);
            vertices = vid_mesh_data(halves[side]);
            for (unsigned i = 0; i < 6; ++i)
            {
                unsigned corner = order[i];
                vertices[i].pos[0] = corner & 1 ? 1.0f : -1.0f;
                vertices[i].pos[1] = corner & 2 ? -1.0f : 1.0f;
                vertices[i].pos[3] = 1;
                vertices[i].color[side * 2] = vertices[i].color[3] = 1;
            }
            vid_mesh_commit(halves[side]);
            vid_mesh_static(halves[side]);
            assert(vid_mesh_data(halves[side]) == NULL && halves[side]->vertices != NULL);
            vid_viewport((VID_RECT){(sint32)side * 480, 0, 480, 600});
            assert(vid_draw(halves[side], 0, 6, material));
            assert(vid_sample(probes[side], 0.5f, 0.5f));
            assert(vid_sample_color(probes[side]) == 0);
            vid_mesh_free(halves[side]);
        }
        AdjustWindowRect(&resized, WS_OVERLAPPEDWINDOW, FALSE);
        assert(SetWindowPos(window, NULL, 0, 0, resized.right - resized.left, resized.bottom - resized.top, SWP_NOMOVE | SWP_NOZORDER));
        vid_resize(&capture_width, &capture_height);
        assert(capture_width == 961 && capture_height == 601);
        free(capture);
        capture = malloc((size_t)capture_width * capture_height * 4);
        assert(capture);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, capture_width, capture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        vid_mat_free(material);
        assert(vid_end());
        check_pixel(0, 600, 255, 0, 0);
        check_pixel(479, 300, 255, 0, 0);
        check_pixel(480, 300, 0, 0, 255);
        check_pixel(960, 600, 0, 0, 255);
        assert(vid_sample_color(probes[0]) == 0xff0000);
        assert(vid_sample_color(probes[1]) == 0x0000ff);
        assert(vid_begin(0, 1, 0));
        assert(!vid_sample(probes[0], 1, 0.5f));
        assert(vid_sample(probes[0], 0.5f, 0.5f));
        vid_cancel();
        assert(vid_sample_color(probes[0]) == 0xff0000);
        assert(probes[0]->refs == 1);
        assert(vid_begin(0, 1, 0));
        assert(vid_sample(probes[0], 0.5f, 0.5f));
        vid_sample_free(probes[0]);
        assert(vid_end());
        vid_sample_free(probes[1]);
        assert(!samples);
        assert(!geometries && !materials);
    }
    bind_framebuffer(GL_FRAMEBUFFER, 0);
    delete_framebuffers(1, &framebuffer);
    glDeleteTextures(1, &target);
    assert(glGetError() == GL_NO_ERROR);
    vid_free();
    vid_free();
    free(capture);
    DestroyWindow(window);
    puts("PASS: OG/HD pixels; repeat/clamp; RGBA5551/custom shader; final texture update; deferred resource release; queued split resize; ordered color probes, cancellation and deferred sample release");
    return 0;
}
