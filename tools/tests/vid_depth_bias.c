#include <windows.h>
#include <GL/gl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <crtdbg.h>
#include "../../src/vid_ogl.c"

sint32 xport_is_headless(void) { return 0; }
void xport_message_error(const char *title, const char *message) { fprintf(stderr, "%s: %s\n", title, message); }

int main(void)
{
    WNDCLASSA cls = {0};
    VID_GEOMETRY *mesh;
    VID_SHADER *shader;
    VID_MATERIAL *base, *overlay;
    VID_SAMPLE *sample;
    HWND window;
    unsigned test, i;
    const float xy[6][2] = {{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    cls.lpfnWndProc = DefWindowProcA;
    cls.style = CS_OWNDC;
    cls.hInstance = GetModuleHandleA(0);
    cls.lpszClassName = "VidDepthBiasTest";
    assert(RegisterClassA(&cls));
    window = CreateWindowA(cls.lpszClassName, "Depth bias", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, cls.hInstance, 0);
    assert(window && vid_init(window));
    mesh = vid_mesh_create_2d(18);
    shader = vid_shader_create(0, "void main(){outColor=vColor;}");
    base = vid_mat_create();
    overlay = vid_mat_create();
    sample = vid_sample_create();
    assert(mesh && shader && base && overlay && sample);
    vid_mat_data(base)->shader = shader;
    vid_mat_data(base)->depth_test = vid_mat_data(base)->depth_write = 1;
    vid_mat_commit(base);
    for (test = 0; test < 4; ++test)
    {
        VID_VERTEX2D *vertices = vid_mesh_data_2d(mesh);
        VID_MATERIAL_DATA *material = vid_mat_data(overlay);
        memset(vertices, 0, 18 * sizeof(*vertices));
        *material = *vid_mat_read(base);
        material->depth_write = 0;
        material->depth_bias_slope = test ? -1.0f : 0;
        material->depth_bias_units = test ? -1.0f : 0;
        vid_mat_commit(overlay);
        for (i = 0; i < 18; ++i)
        {
            vertices[i].pos[0] = xy[i % 6][0];
            vertices[i].pos[1] = xy[i % 6][1];
            vertices[i].pos[2] = (test == 2 ? -0.8f : xy[i % 6][0] * 0.5f) + (i >= 6 ? (test == 2 ? 1.0f : 0.00001f) : 0);
            vertices[i].pos[3] = 1;
            vertices[i].color[i < 6 ? 0 : i < 12 ? 1 : 2] = 255;
            vertices[i].color[3] = 255;
            vertices[i].alpha = 1;
        }
        vid_mesh_commit(mesh);
        assert(vid_begin(0, 0, 0));
        vid_pass(base);
        assert(vid_submit(mesh, 0, 0, 6));
        vid_pass(overlay);
        assert(vid_submit(mesh, 0, 6, 6));
        if (test == 3)
        {
            vid_pass(base);
            assert(vid_submit(mesh, 0, 12, 6));
        }
        assert(vid_sample(sample, 0.5f, 0.5f) && vid_end());
        assert(vid_sample_color(sample) == (test == 0 || test == 2 ? 0xff0000 : 0x00ff00));
        assert(!glIsEnabled(GL_POLYGON_OFFSET_FILL) && glGetError() == GL_NO_ERROR);
    }
    vid_sample_free(sample);
    vid_mat_free(base);
    vid_mat_free(overlay);
    vid_shader_free(shader);
    vid_mesh_free(mesh);
    vid_free();
    DestroyWindow(window);
    puts("depth bias: coplanar overlay, slope, foreground occlusion, pass reset and flush reset PASS");
    return 0;
}
