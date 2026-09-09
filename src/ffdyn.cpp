// Playback Machine — FFmpeg loader.
//
// Binds the ffdyn function pointers either to statically linked FFmpeg
// (FFMPEG_STATIC) or to shared libraries discovered at runtime.

#include "ffdyn_raw.h"

#include <cstring>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

enum Mod { M_AVUTIL, M_AVCODEC, M_AVFORMAT, M_SWS, M_SWR, M_COUNT };

struct Entry { const char *name; void **ptr; Mod mod; };

const Entry kEntries[] = {
    {"av_log_set_level", (void **)&ffdyn::p_av_log_set_level, M_AVUTIL},
    {"av_log_set_callback", (void **)&ffdyn::p_av_log_set_callback, M_AVUTIL},
    {"av_strerror", (void **)&ffdyn::p_av_strerror, M_AVUTIL},
    {"av_rescale_q", (void **)&ffdyn::p_av_rescale_q, M_AVUTIL},
    {"av_frame_alloc", (void **)&ffdyn::p_av_frame_alloc, M_AVUTIL},
    {"av_frame_free", (void **)&ffdyn::p_av_frame_free, M_AVUTIL},
    {"av_frame_ref", (void **)&ffdyn::p_av_frame_ref, M_AVUTIL},
    {"av_frame_unref", (void **)&ffdyn::p_av_frame_unref, M_AVUTIL},
    {"av_frame_get_buffer", (void **)&ffdyn::p_av_frame_get_buffer, M_AVUTIL},
    {"av_hwdevice_ctx_create", (void **)&ffdyn::p_av_hwdevice_ctx_create, M_AVUTIL},
    {"av_hwframe_transfer_data", (void **)&ffdyn::p_av_hwframe_transfer_data, M_AVUTIL},
    {"av_hwframe_transfer_get_formats", (void **)&ffdyn::p_av_hwframe_transfer_get_formats, M_AVUTIL},
    {"av_buffer_ref", (void **)&ffdyn::p_av_buffer_ref, M_AVUTIL},
    {"av_buffer_unref", (void **)&ffdyn::p_av_buffer_unref, M_AVUTIL},
    {"av_free", (void **)&ffdyn::p_av_free, M_AVUTIL},

    {"avcodec_alloc_context3", (void **)&ffdyn::p_avcodec_alloc_context3, M_AVCODEC},
    {"avcodec_find_decoder", (void **)&ffdyn::p_avcodec_find_decoder, M_AVCODEC},
    {"avcodec_parameters_to_context", (void **)&ffdyn::p_avcodec_parameters_to_context, M_AVCODEC},
    {"avcodec_open2", (void **)&ffdyn::p_avcodec_open2, M_AVCODEC},
    {"avcodec_send_packet", (void **)&ffdyn::p_avcodec_send_packet, M_AVCODEC},
    {"avcodec_receive_frame", (void **)&ffdyn::p_avcodec_receive_frame, M_AVCODEC},
    {"avcodec_flush_buffers", (void **)&ffdyn::p_avcodec_flush_buffers, M_AVCODEC},
    {"avcodec_free_context", (void **)&ffdyn::p_avcodec_free_context, M_AVCODEC},
    {"avcodec_get_hw_config", (void **)&ffdyn::p_avcodec_get_hw_config, M_AVCODEC},
    {"av_packet_alloc", (void **)&ffdyn::p_av_packet_alloc, M_AVCODEC},
    {"av_packet_unref", (void **)&ffdyn::p_av_packet_unref, M_AVCODEC},
    {"av_packet_free", (void **)&ffdyn::p_av_packet_free, M_AVCODEC},

    {"avformat_open_input", (void **)&ffdyn::p_avformat_open_input, M_AVFORMAT},
    {"avformat_find_stream_info", (void **)&ffdyn::p_avformat_find_stream_info, M_AVFORMAT},
    {"av_find_best_stream", (void **)&ffdyn::p_av_find_best_stream, M_AVFORMAT},
    {"av_seek_frame", (void **)&ffdyn::p_av_seek_frame, M_AVFORMAT},
    {"av_read_frame", (void **)&ffdyn::p_av_read_frame, M_AVFORMAT},
    {"avformat_close_input", (void **)&ffdyn::p_avformat_close_input, M_AVFORMAT},

    {"sws_getContext", (void **)&ffdyn::p_sws_getContext, M_SWS},
    {"sws_scale", (void **)&ffdyn::p_sws_scale, M_SWS},
    {"sws_freeContext", (void **)&ffdyn::p_sws_freeContext, M_SWS},

    {"swr_alloc", (void **)&ffdyn::p_swr_alloc, M_SWR},
    {"swr_alloc_set_opts2", (void **)&ffdyn::p_swr_alloc_set_opts2, M_SWR},
    {"swr_init", (void **)&ffdyn::p_swr_init, M_SWR},
    {"swr_convert", (void **)&ffdyn::p_swr_convert, M_SWR},
    {"swr_close", (void **)&ffdyn::p_swr_close, M_SWR},
    {"swr_free", (void **)&ffdyn::p_swr_free, M_SWR},
};

void *g_handles[M_COUNT] = { nullptr };
bool g_loaded = false;

// Versioned module names, tried in order (newest first).
const char *kModuleNames[M_COUNT][8] = {
    { "avutil-60.dll", "avutil-59.dll", "avutil-58.dll", "avutil-57.dll", "avutil-56.dll", "libavutil.so.60", "libavutil.so.59", "libavutil.so.58" },
    { "avcodec-62.dll", "avcodec-61.dll", "avcodec-60.dll", "avcodec-59.dll", "libavcodec.so.62", "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59" },
    { "avformat-62.dll", "avformat-61.dll", "avformat-60.dll", "avformat-59.dll", "libavformat.so.62", "libavformat.so.61", "libavformat.so.60", "libavformat.so.59" },
    { "swscale-9.dll", "swscale-8.dll", "swscale-7.dll", "swscale-6.dll", "libswscale.so.9", "libswscale.so.8", "libswscale.so.7", "libswscale.so.6" },
    { "swresample-6.dll", "swresample-5.dll", "swresample-4.dll", "libswresample.so.6", "libswresample.so.5", "libswresample.so.4", "libswresample.so", "libavutil.so.57" },
};

std::string dirPrefix() {
    const char *d = std::getenv("PLAYBACK_MACHINE_FFMPEG_DIR");
    if (d && *d)
        return std::string(d) + "/";
    return std::string();
}

bool loadModule(Mod m, std::string *err) {
    for (int i = 0; i < 8; ++i) {
        const char *n = kModuleNames[m][i];
        if (!n || !*n)
            break;
        std::string name = dirPrefix() + n;
#ifdef _WIN32
        g_handles[m] = (void *)LoadLibraryA(name.c_str());
        if (!g_handles[m]) {
            DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_MOD_NOT_FOUND)
                break;
        }
#else
        g_handles[m] = dlopen(name.c_str(), RTLD_NOW | RTLD_GLOBAL);
#endif
        if (g_handles[m])
            return true;
    }
    *err = "Could not load a FFmpeg module (tried: " + std::string(kModuleNames[m][0]) + " and older versions).";
    return false;
}

} // namespace

#if defined(FFMPEG_STATIC) && FFMPEG_STATIC

bool ff_dyn_load(std::string *errOut) {
    (void)errOut;
    ffdyn::p_av_log_set_level = &av_log_set_level;
    ffdyn::p_av_log_set_callback = &av_log_set_callback;
    ffdyn::p_av_strerror = &av_strerror;
    ffdyn::p_av_rescale_q = &av_rescale_q;
    ffdyn::p_av_frame_alloc = &av_frame_alloc;
    ffdyn::p_av_frame_free = &av_frame_free;
    ffdyn::p_av_frame_ref = &av_frame_ref;
    ffdyn::p_av_frame_unref = &av_frame_unref;
    ffdyn::p_av_frame_get_buffer = &av_frame_get_buffer;
    ffdyn::p_av_hwdevice_ctx_create = &av_hwdevice_ctx_create;
    ffdyn::p_av_hwframe_transfer_data = &av_hwframe_transfer_data;
    ffdyn::p_av_hwframe_transfer_get_formats = &av_hwframe_transfer_get_formats;
    ffdyn::p_av_buffer_ref = &av_buffer_ref;
    ffdyn::p_av_buffer_unref = &av_buffer_unref;
    ffdyn::p_av_free = &av_free;
    ffdyn::p_avcodec_alloc_context3 = &avcodec_alloc_context3;
    ffdyn::p_avcodec_find_decoder = &avcodec_find_decoder;
    ffdyn::p_avcodec_parameters_to_context = &avcodec_parameters_to_context;
    ffdyn::p_avcodec_open2 = &avcodec_open2;
    ffdyn::p_avcodec_send_packet = &avcodec_send_packet;
    ffdyn::p_avcodec_receive_frame = &avcodec_receive_frame;
    ffdyn::p_avcodec_flush_buffers = &avcodec_flush_buffers;
    ffdyn::p_avcodec_free_context = &avcodec_free_context;
    ffdyn::p_avcodec_get_hw_config = &avcodec_get_hw_config;
    ffdyn::p_av_packet_alloc = &av_packet_alloc;
    ffdyn::p_av_packet_unref = &av_packet_unref;
    ffdyn::p_av_packet_free = &av_packet_free;
    ffdyn::p_avformat_open_input = &avformat_open_input;
    ffdyn::p_avformat_find_stream_info = &avformat_find_stream_info;
    ffdyn::p_av_find_best_stream = &av_find_best_stream;
    ffdyn::p_av_seek_frame = &av_seek_frame;
    ffdyn::p_av_read_frame = &av_read_frame;
    ffdyn::p_avformat_close_input = &avformat_close_input;
    ffdyn::p_sws_getContext = &sws_getContext;
    ffdyn::p_sws_scale = &sws_scale;
    ffdyn::p_sws_freeContext = &sws_freeContext;
    ffdyn::p_swr_alloc = &swr_alloc;
    ffdyn::p_swr_alloc_set_opts2 = &swr_alloc_set_opts2;
    ffdyn::p_swr_init = &swr_init;
    ffdyn::p_swr_convert = &swr_convert;
    ffdyn::p_swr_close = &swr_close;
    ffdyn::p_swr_free = &swr_free;
    g_loaded = true;
    return true;
}

void ff_dyn_shutdown() { g_loaded = false; } // static build: nothing to unload

#else // dynamic loading

bool ff_dyn_load(std::string *errOut) {
    if (g_loaded)
        return true;
    for (int m = 0; m < M_COUNT; ++m) {
        if (!loadModule((Mod)m, errOut))
            return false;
    }
    for (const auto &e : kEntries) {
        void *h = g_handles[e.mod];
        void *fn = nullptr;
#ifdef _WIN32
        fn = (void *)GetProcAddress((HMODULE)h, e.name);
#else
        fn = dlsym(h, e.name);
#endif
        if (!fn) {
            *errOut = std::string("Missing FFmpeg symbol: ") + e.name;
            return false;
        }
        *(void **)e.ptr = fn;
    }
    g_loaded = true;
    return true;
}

void ff_dyn_shutdown() {
    if (!g_loaded)
        return;
    for (int m = 0; m < M_COUNT; ++m) {
#ifdef _WIN32
        if (g_handles[m])
            FreeLibrary((HMODULE)g_handles[m]);
#else
        if (g_handles[m])
            dlclose(g_handles[m]);
#endif
        g_handles[m] = nullptr;
    }
    g_loaded = false;
}

#endif

bool ff_dyn_loaded() { return g_loaded; }
