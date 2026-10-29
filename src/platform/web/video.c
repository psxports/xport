#include <emscripten.h>

#include "psx_spu.h"
#include "xport.h"

// clang-format off
EM_JS(sint32, video_open, (const char *path), {
    if (!Module.xportVideoOpen) return -1;
    const generation = (Module.xportVideoBridgeGeneration || 0) + 1;
    Module.xportVideoBridgeGeneration = generation;
    Module.xportVideoOpenStatus = 0;
    Module.xportVideoOpen(UTF8ToString(path)).then(() => {
        if (generation === Module.xportVideoBridgeGeneration) Module.xportVideoOpenStatus = 1;
    }).catch(error => {
        if (generation !== Module.xportVideoBridgeGeneration) return;
        Module.xportVideoOpenStatus = -1;
        console.error('Movie playback: ' + error.message);
    });
    return 1;
});

EM_JS(sint32, video_next, (uint8 *rgb, uint32 capacity, XportFmvFrame *frame), {
    if (!Module.xportVideoNext) return -1;
    if (Module.xportVideoOpenStatus !== 1) return Module.xportVideoOpenStatus || 0;
    const result = Module.xportVideoNext(HEAPU8.subarray(rgb, rgb + capacity));
    if (result.status === 1) {
        HEAPU32[frame >> 2] = result.width;
        HEAPU32[(frame >> 2) + 1] = result.height;
        HEAPU32[(frame >> 2) + 2] = result.number;
    }
    return result.status;
});

EM_JS(void, video_pause, (sint32 paused), {
    if (Module.xportVideoPause) Module.xportVideoPause(paused !== 0);
});

EM_JS(void, video_close, (void), {
    Module.xportVideoBridgeGeneration = (Module.xportVideoBridgeGeneration || 0) + 1;
    Module.xportVideoOpenStatus = -1;
    if (Module.xportVideoClose) Module.xportVideoClose();
});

EM_JS(uint32, video_pcm, (sint16 *output, uint32 count), {
    return Module.xportVideoReadPcm ? Module.xportVideoReadPcm(HEAP16.subarray(output >> 1, (output >> 1) + count * 2), count) : 0;
});
EM_JS(sint32, video_has_audio, (), {
    return Module.xportVideoHasAudio && Module.xportVideoHasAudio() ? 1 : 0;
});
// clang-format on

static uint32 video_cd_pcm(void *context, sint16 *output, uint32 frames)
{
    return video_pcm(output, frames);
}

static sint32 video_audio_attached;

sint32 xport_web_fmv_open(const char *path)
{
    sint32 result;
    spu_set_cd_pcm_source(NULL, NULL);
    result = video_open(path);
    video_audio_attached = 0;
    return result;
}

sint32 xport_web_fmv_next(uint8 *rgb, uint32 capacity, XportFmvFrame *frame)
{
    if (!video_audio_attached && video_has_audio())
    {
        spu_set_cd_pcm_source(video_cd_pcm, NULL);
        video_audio_attached = 1;
    }
    return video_next(rgb, capacity, frame);
}

void xport_web_fmv_pause(sint32 paused)
{
    video_pause(paused);
}

void xport_web_fmv_close(void)
{
    spu_set_cd_pcm_source(NULL, NULL);
    video_audio_attached = 0;
    video_close();
}
