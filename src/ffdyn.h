// Playback Machine — FFmpeg binding layer (name mapping + loader API).
//
// Include THIS header (not ffdyn_raw.h) from code that calls FFmpeg; the
// #defines below turn every FFmpeg call into a call through the ffdyn
// function pointers, which are resolved once at startup by ff_dyn_load().

#pragma once
#include <string>
#include "ffdyn_raw.h"

// Map plain FFmpeg names onto the ffdyn pointers.
#define av_log_set_level               ffdyn::p_av_log_set_level
#define av_log_set_callback            ffdyn::p_av_log_set_callback
#define av_strerror                    ffdyn::p_av_strerror
#define av_q2d                         ffdyn::p_av_q2d
#define av_rescale_q                   ffdyn::p_av_rescale_q
#define av_frame_alloc                 ffdyn::p_av_frame_alloc
#define av_frame_free                  ffdyn::p_av_frame_free
#define av_frame_ref                   ffdyn::p_av_frame_ref
#define av_frame_unref                 ffdyn::p_av_frame_unref
#define av_frame_get_buffer            ffdyn::p_av_frame_get_buffer
#define av_hwdevice_ctx_create         ffdyn::p_av_hwdevice_ctx_create
#define av_hwframe_transfer_data       ffdyn::p_av_hwframe_transfer_data
#define av_hwframe_transfer_get_formats ffdyn::p_av_hwframe_transfer_get_formats
#define av_buffer_ref                  ffdyn::p_av_buffer_ref
#define av_buffer_unref                ffdyn::p_av_buffer_unref
#define av_free                        ffdyn::p_av_free
#define avcodec_alloc_context3         ffdyn::p_avcodec_alloc_context3
#define avcodec_find_decoder           ffdyn::p_avcodec_find_decoder
#define avcodec_parameters_to_context  ffdyn::p_avcodec_parameters_to_context
#define avcodec_open2                  ffdyn::p_avcodec_open2
#define avcodec_send_packet            ffdyn::p_avcodec_send_packet
#define avcodec_receive_frame          ffdyn::p_avcodec_receive_frame
#define avcodec_flush_buffers          ffdyn::p_avcodec_flush_buffers
#define avcodec_free_context           ffdyn::p_avcodec_free_context
#define avcodec_get_hw_config          ffdyn::p_avcodec_get_hw_config
#define av_packet_alloc                ffdyn::p_av_packet_alloc
#define av_packet_unref                ffdyn::p_av_packet_unref
#define av_packet_free                 ffdyn::p_av_packet_free
#define avformat_open_input            ffdyn::p_avformat_open_input
#define avformat_find_stream_info      ffdyn::p_avformat_find_stream_info
#define av_find_best_stream            ffdyn::p_av_find_best_stream
#define av_seek_frame                  ffdyn::p_av_seek_frame
#define av_read_frame                  ffdyn::p_av_read_frame
#define avformat_close_input           ffdyn::p_avformat_close_input
#define sws_getContext                 ffdyn::p_sws_getContext
#define sws_scale                      ffdyn::p_sws_scale
#define sws_freeContext                ffdyn::p_sws_freeContext
#define swr_alloc                      ffdyn::p_swr_alloc
#define swr_alloc_set_opts2            ffdyn::p_swr_alloc_set_opts2
#define swr_init                       ffdyn::p_swr_init
#define swr_convert                    ffdyn::p_swr_convert
#define swr_close                      ffdyn::p_swr_close
#define swr_free                       ffdyn::p_swr_free

// Load/resolve every bound FFmpeg function.  Idempotent.  Returns false and
// fills errOut with a human-readable message on failure.
bool ff_dyn_load(std::string *errOut);
bool ff_dyn_loaded();
void ff_dyn_shutdown();
