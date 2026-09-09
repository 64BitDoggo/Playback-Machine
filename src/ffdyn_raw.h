// Playback Machine — FFmpeg binding layer (raw declarations).
//
// Every FFmpeg function the engine uses is declared here as a function
// pointer inside namespace `ffdyn`. ffdyn.cpp populates them at startup:
//
//   * with FFMPEG_STATIC defined : the pointers are bound straight to the
//     statically linked FFmpeg symbols (single self-contained .exe);
//   * otherwise                  : the FFmpeg shared libraries are loaded at
//     runtime (LoadLibrary / dlopen) and symbols resolved by name, trying a
//     series of soname/dllname versions so any reasonably recent FFmpeg
//     distribution works.
//
// ffdyn.h (included by the engine) maps the plain FFmpeg names onto these
// pointers with #defines, so the rest of the code reads like ordinary
// FFmpeg code.

#pragma once

#include <cstddef>
#include <cstdint> // defines INT64_C & friends required by libavutil/common.h in C++ mode

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <cstdarg>

namespace ffdyn {

// ---- avutil -------------------------------------------------------------
inline void (*p_av_log_set_level)(int level) = nullptr;
inline void (*p_av_log_set_callback)(void (*callback)(const void *avcl, int level, const char *fmt, va_list vl)) = nullptr;
inline void (*p_av_strerror)(int error_code, char *errbuf, size_t errbuf_size) = nullptr;
// av_q2d is a static inline in libavutil (no exported symbol), so we provide it.
inline double p_av_q2d(AVRational a) { return a.num / (double)a.den; }
inline int64_t (*p_av_rescale_q)(int64_t a, AVRational bq, AVRational cq) = nullptr;
inline AVFrame *(*p_av_frame_alloc)(void) = nullptr;
inline void (*p_av_frame_free)(AVFrame **frame) = nullptr;
inline int (*p_av_frame_ref)(AVFrame *dst, const AVFrame *src) = nullptr;
inline void (*p_av_frame_unref)(AVFrame *frame) = nullptr;
inline int (*p_av_frame_get_buffer)(AVFrame *frame, int align) = nullptr;
inline int (*p_av_hwdevice_ctx_create)(AVBufferRef **device_ctx, enum AVHWDeviceType type, const char *device, AVDictionary *opts, int flags) = nullptr;
inline int (*p_av_hwframe_transfer_data)(AVFrame *dst, const AVFrame *src, int flags) = nullptr;
inline int (*p_av_hwframe_transfer_get_formats)(AVBufferRef *hwframe_ctx, enum AVHWFrameTransferDirection dir, enum AVPixelFormat **format_list, int flags) = nullptr;
inline AVBufferRef *(*p_av_buffer_ref)(const AVBufferRef *buffer) = nullptr;
inline int (*p_av_buffer_unref)(AVBufferRef **buf) = nullptr;
inline void (*p_av_free)(void *ptr) = nullptr;

// ---- avcodec (incl. AVPacket, which moved here in FFmpeg 8) -------------
inline AVCodecContext *(*p_avcodec_alloc_context3)(const AVCodec *codec) = nullptr;
inline const AVCodec *(*p_avcodec_find_decoder)(enum AVCodecID id) = nullptr;
inline int (*p_avcodec_parameters_to_context)(AVCodecContext *codec, const AVCodecParameters *par) = nullptr;
inline int (*p_avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options) = nullptr;
inline int (*p_avcodec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt) = nullptr;
inline int (*p_avcodec_receive_frame)(AVCodecContext *avctx, AVFrame *frame) = nullptr;
inline void (*p_avcodec_flush_buffers)(AVCodecContext *avctx) = nullptr;
inline void (*p_avcodec_free_context)(AVCodecContext **avctx) = nullptr;
inline const AVCodecHWConfig *(*p_avcodec_get_hw_config)(const AVCodec *codec, int index) = nullptr;
inline AVPacket *(*p_av_packet_alloc)(void) = nullptr;
inline void (*p_av_packet_unref)(AVPacket *pkt) = nullptr;
inline void (*p_av_packet_free)(AVPacket **pkt) = nullptr;

// ---- avformat -----------------------------------------------------------
// FFmpeg >= 8 signature: the 4th arg is AVDictionary **options (the
// AVIOInterruptCB parameter was removed).
inline int (*p_avformat_open_input)(AVFormatContext **ps, const char *url, const AVInputFormat *fmt, AVDictionary **options) = nullptr;
inline int (*p_avformat_find_stream_info)(AVFormatContext *ic, AVDictionary **options) = nullptr;
inline int (*p_av_find_best_stream)(AVFormatContext *ic, AVMediaType type, int wanted, int related, const AVCodec **decoder_ret, int flags) = nullptr;
inline int (*p_av_seek_frame)(AVFormatContext *s, int stream_index, int64_t timestamp, int flags) = nullptr;
inline int (*p_av_read_frame)(AVFormatContext *s, AVPacket *pkt) = nullptr;
inline void (*p_avformat_close_input)(AVFormatContext **s) = nullptr;

// ---- swscale ------------------------------------------------------------
inline SwsContext *(*p_sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat, int dstW, int dstH, enum AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter, SwsFilter *dstFilter, const double *param) = nullptr;
inline int (*p_sws_scale)(struct SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[], int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]) = nullptr;
inline void (*p_sws_freeContext)(struct SwsContext **swsContext) = nullptr;

// ---- swresample ---------------------------------------------------------
inline SwrContext *(*p_swr_alloc)(void) = nullptr;
inline int (*p_swr_alloc_set_opts2)(SwrContext **s, const AVChannelLayout *out_ch_layout, enum AVSampleFormat out_sample_fmt, int out_sample_rate, const AVChannelLayout *in_ch_layout, enum AVSampleFormat in_sample_fmt, int in_sample_rate, int log_offset, void *log_ctx) = nullptr;
inline int (*p_swr_init)(SwrContext *s) = nullptr;
inline int (*p_swr_convert)(SwrContext *s, uint8_t **out_buf, int out_count, const uint8_t **in_buf, int in_count) = nullptr;
inline void (*p_swr_close)(SwrContext *s) = nullptr; // swr_flush was replaced by swr_close
inline int (*p_swr_free)(SwrContext **s) = nullptr;

} // namespace ffdyn
