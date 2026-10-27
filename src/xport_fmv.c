#include <stdlib.h>
#include <string.h>

#include "psx_gpu.h"
#include "psx_press.h"
#include "psx_spu.h"
#include "psx_stream.h"

enum
{
    FMV_RING_SECTORS = 64,
    FMV_MAX_WIDTH = 640,
    FMV_MAX_HEIGHT = 256,
    FMV_VLC_WORDS = FMV_MAX_WIDTH * FMV_MAX_HEIGHT
};

typedef struct FmvBuffers
{
#if defined(AP_WEB)
    uint8 rgb[FMV_MAX_WIDTH * FMV_MAX_HEIGHT * 3];
#else
    uint32 ring[FMV_RING_SECTORS * 2048 / 4];
    uint32 vlc[FMV_VLC_WORDS];
#endif
    uint32 slice[24 * FMV_MAX_HEIGHT / 2];
} FmvBuffers;

static struct
{
    FmvBuffers *buffers;
    DISPENV display;
    SpuCommonAttr sound;
    PsxInterruptCallback vsync;
    CdlCB sync, read, ready;
    DecDCTCallback input, output;
    XportCleanup cleanup;
    sint32 active, display_valid, displayed, raster, decoded;
} fmv;

static sint32 fmv_audio_ready;

void xport_fmv_discard_audio(void)
{
    fmv_audio_ready = 0;
    spu_discard_xa_pcm();
}

sint32 xport_fmv_take_audio(XportFmvAudioRemainder *audio)
{
    if (!audio || fmv.active || !fmv_audio_ready)
        return 0;
    fmv_audio_ready = 0;
    return spu_take_xa_pcm(audio);
}

sint32 xport_fmv_ready(void)
{
    return fmv.active && fmv.decoded;
}

static void fmv_close(void)
{
    if (!fmv.active)
        return;
#if defined(AP_WEB)
    xport_fmv_close();
#else
    CdControlB(CdlStop, NULL, NULL);
    StUnSetRing();
    StSetRing(NULL, 0);
    cd_unmount_image();
    DecDCTReset(1);
#endif
    DecDCTinCallback(fmv.input);
    DecDCToutCallback(fmv.output);
    CdReadyCallback(fmv.ready);
    CdReadCallback(fmv.read);
    CdSyncCallback(fmv.sync);
    SpuSetCommonAttr(&fmv.sound);
    if (fmv.display_valid)
        PutDispEnv(&fmv.display);
    SetDispMask(fmv.displayed);
    psx_bios_vsync_callback(fmv.vsync);
    gpu_set_raster(fmv.raster);
    free(fmv.buffers);
    fmv.buffers = NULL;
    fmv.active = 0;
}

static void fmv_shutdown(void)
{
    XportCleanup cleanup = fmv.cleanup;
    xport_fmv_discard_audio();
    fmv_close();
    if (cleanup != NULL)
        cleanup();
}

static sint32 fmv_rectangle(uint32 width, uint32 height, sint32 page, const XportFmvPresentation *presentation, PSX_RECT *rect)
{
    uint32 canvas_width = presentation && presentation->width ? presentation->width : width;
    uint32 canvas_height = presentation && presentation->height ? presentation->height : height;
    uint32 page_height = (canvas_height + 15) & ~15;
    uint32 decoded_height = (height + 15) & ~15;
    sint32 x = presentation ? presentation->video_x : 0;
    sint32 y = presentation ? presentation->video_y : 0;
    uint32 available;
    uint32 rows;
    if (!width || width > FMV_MAX_WIDTH || (width & 15) || !height || height > FMV_MAX_HEIGHT || !canvas_width || canvas_width > FMV_MAX_WIDTH || (canvas_width & 15) || !canvas_height || canvas_height > FMV_MAX_HEIGHT || x < 0 || y < 0 || (uint32)x > canvas_width || width > canvas_width - (uint32)x || (uint32)y >= page_height || (presentation && presentation->height && (uint32)y >= canvas_height))
        return 0;
    available = presentation && presentation->height ? canvas_height - (uint32)y : page_height - (uint32)y;
    if (decoded_height > available || (presentation && presentation->bottom_rows > FMV_MAX_HEIGHT))
        return 0;
    rows = decoded_height + (presentation ? presentation->bottom_rows : 0);
    if (rows > available)
        rows = available;
    rect->x = (sint16)(x * 3 / 2);
    rect->y = (sint16)(page * page_height + y);
    rect->w = 24;
    rect->h = (sint16)rows;
    return 1;
}

#if !defined(AP_WEB)
static sint32 xport_fmv_decode(uint32 *src, uint32 *vlc, uint32 *slice, const StHEADER *header, sint32 page, const XportFmvPresentation *presentation)
{
    PSX_RECT rect;
    sint32 height;
    uint32 x;
    if (!fmv_rectangle(header->width, header->height, page, presentation, &rect) || (src[0] & 0xFFFF) + 1 > FMV_VLC_WORDS)
        return 0;
    if (DecDCTvlc(src, vlc) != 0)
        return 0;
    height = (header->height + 15) & ~15;
    DecDCTin(vlc, 3);
    for (x = 0; x < header->width; x += 16)
    {
        DecDCTout(slice, 12 * height);
        if (DecDCToutSync(1) != 0)
            return 0;
        if (rect.h > height)
            memset(slice + 12 * height, 0, (rect.h - height) * 48);
        if (presentation && presentation->strip && presentation->strip(presentation->context, (uint32)rect.x, (uint8 *)slice, (uint32)rect.h) <= 0)
            return 0;
        LoadImagePSX(&rect, slice);
        rect.x += rect.w;
    }
    return 1;
}
#else
static sint32 fmv_upload_rgb(const XportFmvFrame *frame, sint32 page, const XportFmvPresentation *presentation)
{
    uint32 x, y;
    PSX_RECT rect;
    if (!fmv_rectangle(frame->width, frame->height, page, presentation, &rect))
        return 0;
    for (x = 0; x < frame->width; x += 16)
    {
        memset(fmv.buffers->slice, 0, rect.w * rect.h * 2);
        for (y = 0; y < frame->height; ++y)
            memcpy((uint8 *)fmv.buffers->slice + y * 48, fmv.buffers->rgb + (y * frame->width + x) * 3, 48);
        if (presentation && presentation->strip && presentation->strip(presentation->context, (uint32)rect.x, (uint8 *)fmv.buffers->slice, (uint32)rect.h) <= 0)
            return 0;
        LoadImagePSX(&rect, fmv.buffers->slice);
        rect.x += rect.w;
    }
    return 1;
}
#endif

sint32 xport_fmv_play_presented(const char *path, uint32 final_frame, sint32 repeat, const XportFmvPresentation *presentation)
{
    XportFmvPresentation options;
    FmvBuffers *buffers;
    SpuCommonAttr sound = {0};
    DISPENV display;
#if !defined(AP_WEB)
    CdlLOC location;
#endif
    uint32 held = 0, width = 0, height = 0;
    uint32 controller;
    uint32 controllers = presentation ? presentation->controllers : 1;
    uint32 skip_buttons = presentation ? presentation->skip_buttons : PADstart | PADRdown;
    sint32 page = 0, failed = 0, skipped = 0, ended = 0;
    sint32 finishing = 0;
    uint32 finish_step = 1;
    uint64 last_frame;
    if (path == NULL || fmv.active || (controllers & ~3) || !xport_window_init())
        return -1;
    xport_fmv_discard_audio();
    if (presentation)
    {
        options = *presentation;
        presentation = &options;
    }
    buffers = (FmvBuffers *)malloc(sizeof(*buffers));
    if (buffers == NULL)
        return -1;
#if defined(AP_WEB)
    if (xport_fmv_open(path) <= 0)
#else
    if (!cd_mount_stream_file(path))
#endif
    {
        free(buffers);
        return -1;
    }
    memset(&fmv, 0, sizeof(fmv));
    fmv.buffers = buffers;
    DrawSync(0);
    fmv.display_valid = gpu_get_display_env(&fmv.display);
    fmv.displayed = (_status() & 0x800000) == 0;
    fmv.raster = gpu_get_raster();
    fmv.vsync = psx_bios_vsync_callback(NULL);
    fmv.sync = CdSyncCallback(NULL);
    fmv.read = CdReadCallback(NULL);
    fmv.ready = CdReadyCallback(NULL);
    fmv.input = DecDCTinCallback(NULL);
    fmv.output = DecDCToutCallback(NULL);
    fmv.sound.mask = SPU_COMMON_MVOLL | SPU_COMMON_MVOLR | SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR | SPU_COMMON_CDMIX | SPU_COMMON_CDREV;
    SpuGetCommonAttr(&fmv.sound);
    sound.mask = fmv.sound.mask;
    sound.mvol.left = sound.mvol.right = 0x3FFF;
    sound.cd.volume.left = sound.cd.volume.right = 0x3FFF;
    sound.cd.mix = SPU_ON;
    SpuSetCommonAttr(&sound);
    fmv.cleanup = xport_get_cleanup();
    fmv.active = 1;
    xport_set_cleanup(fmv_shutdown);
#if !defined(AP_WEB)
    DecDCTReset(0);
    DecDCTvlcSize(0);
    StSetRing(buffers->ring, FMV_RING_SECTORS);
    StSetStream(1, 1, 0xFFFFFFF, NULL, NULL);
    stream_set_read_mode(0xC0);
#endif
    gpu_set_raster(1);
    SetDispMask(0);
    for (controller = 0; controller < 2; ++controller)
        if (controllers & (1 << controller))
            held |= xport_input_read(controller) & skip_buttons;
    last_frame = xport_timer_get();
#if !defined(AP_WEB)
    CdIntToPos(0, &location);
    if (!CdControlB(CdlSeekL, (uint8 *)&location, NULL) || !CdRead2(0xC0))
        failed = 1;
#endif
    while (!failed && !skipped && !ended)
    {
        uint32 buttons;
        sint32 presented = 0;
#if defined(AP_WEB)
        XportFmvFrame decoded_frame;
        StHEADER video_header = {0};
        const StHEADER *header = &video_header;
#else
        uint32 *src = NULL, *record = NULL;
        const StHEADER *header = NULL;
#endif
        sint32 progress = 0;
        gpu_frame_begin();
        psx_native_poll_events();
        xport_poll();
        if (presentation && presentation->tick && presentation->tick(presentation->context) <= 0)
            failed = 1;
        buttons = 0;
        for (controller = 0; controller < 2; ++controller)
            if (controllers & (1 << controller))
                buttons |= xport_input_read(controller) & skip_buttons;
        if (!held || (presentation && presentation->skip_held))
            skipped |= buttons != 0;
        else if (!buttons)
            held = 0;
        if (finishing)
        {
            if (!failed && !skipped)
            {
                progress = presentation->finish(presentation->context, finish_step++);
                failed = progress < 0;
                ended = progress == 0;
                presented = 1;
                last_frame = xport_timer_get();
            }
        }
        else
        {
            if (!failed && !skipped)
#if defined(AP_WEB)
            {
                progress = xport_fmv_next(buffers->rgb, sizeof(buffers->rgb), &decoded_frame);
                if (progress == 1)
                {
                    video_header.width = decoded_frame.width;
                    video_header.height = decoded_frame.height;
                    video_header.frameCount = decoded_frame.number;
                }
                if (progress < 0)
                    failed = 1;
            }
            if (!skipped && !failed && progress == 1)
#else
                do
                {
                    progress = stream_pump();
                } while (progress > 0);
            if (!failed && !skipped && StGetNext(&src, &record) == 0)
#endif
            {
                PSX_RECT rectangle;
#if !defined(AP_WEB)
                header = (const StHEADER *)record;
#endif
                if (!fmv_rectangle(header->width, header->height, page, presentation, &rectangle))
                    failed = 1;
                if (!failed && !width)
                {
                    uint32 canvas_width = presentation && presentation->width ? presentation->width : header->width;
                    uint32 canvas_height = presentation && presentation->height ? presentation->height : header->height;
                    PSX_RECT clear = {0, 0, canvas_width * 3 / 2, ((canvas_height + 15) & ~15) * 2};
                    ClearImage(&clear, 0, 0, 0);
                }
                if ((width && width != header->width) || (height && height != header->height))
                    failed = 1;
                if (!failed && presentation && presentation->frame && presentation->frame(presentation->context, header->frameCount) <= 0)
                    failed = 1;
                if (!failed)
#if defined(AP_WEB)
                    failed = !fmv_upload_rgb(&decoded_frame, page, presentation);
#else
                    failed = !xport_fmv_decode(src, buffers->vlc, buffers->slice, header, page, presentation);
#endif
                if (!failed)
                {
                    if (!width)
                    {
                        width = header->width;
                        height = header->height;
                        SetDefDispEnv(&display, 0, 0, presentation && presentation->width ? presentation->width : width, presentation && presentation->height ? presentation->height : height);
                        display.isrgb24 = 1;
                        if (presentation)
                        {
                            display.isinter = presentation->interlace != 0;
                            display.screen.x = (sint16)presentation->screen_x;
                            display.screen.y = (sint16)presentation->screen_y;
                            display.screen.w = (sint16)presentation->screen_width;
                            display.screen.h = (sint16)presentation->screen_height;
                        }
                    }
                    last_frame = xport_timer_get();
                    display.disp.y = page * (((presentation && presentation->height ? presentation->height : height) + 15) & ~15);
                    PutDispEnv(&display);
                    SetDispMask(1);
                    fmv.decoded = 1;
                    page ^= 1;
                    presented = 1;
                    if (final_frame && header->frameCount >= final_frame)
                        ended = 1;
                }
#if !defined(AP_WEB)
                StFreeRing(src);
#endif
            }
#if defined(AP_WEB)
            else if (!skipped && progress == 2)
#else
            else if (!skipped && progress < 0)
#endif
                ended = 1;
        }
        if (xport_timer_get() - last_frame > 5000000)
            failed = 1;
        gpu_frame_end();
        if (!failed && !skipped && (!ended || presented))
            gpu_present();
#if !defined(AP_WEB)
        if (!failed && !skipped && ended && !repeat && !finishing)
            fmv_audio_ready = spu_save_xa_pcm();
#endif
        if (!failed && !skipped && ended && repeat)
        {
#if defined(AP_WEB)
            xport_fmv_close();
            if (xport_fmv_open(path) <= 0)
                failed = 1;
#else
            if (!CdControlB(CdlStop, NULL, NULL))
                failed = 1;
            StClearRing();
            DecDCTReset(0);
            StSetStream(1, 1, 0xFFFFFFF, NULL, NULL);
            stream_set_read_mode(0xC0);
            CdIntToPos(0, &location);
            if (!failed && (!CdControlB(CdlSeekL, (uint8 *)&location, NULL) || !CdRead2(0xC0)))
                failed = 1;
#endif
            ended = 0;
            last_frame = xport_timer_get();
        }
        else if (!failed && !skipped && ended && !finishing && width && presentation && presentation->finish)
        {
            progress = presentation->finish(presentation->context, 0);
            if (progress < 0)
                failed = 1;
            else if (progress > 0)
            {
#if defined(AP_WEB)
                xport_fmv_pause(1);
#else
                if (!CdControlB(CdlStop, NULL, NULL))
                    failed = 1;
#endif
                finishing = 1;
                ended = 0;
                last_frame = xport_timer_get();
            }
        }
    }
    if (failed || skipped)
        xport_fmv_discard_audio();
    fmv_close();
    xport_set_cleanup(fmv.cleanup);
    return failed ? -1 : skipped ? 2 : 1;
}

sint32 xport_fmv_play_range(const char *path, uint32 final_frame, sint32 repeat)
{
    return xport_fmv_play_presented(path, final_frame, repeat, NULL);
}

sint32 xport_fmv_play(const char *path)
{
    return xport_fmv_play_range(path, 0, 0);
}
