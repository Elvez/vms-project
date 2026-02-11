extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <inttypes.h>
#include <csignal>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "logger.hpp"
#include "utils.hpp"
#include "avoptions.hpp"

/**
 * @brief Struct used for quality
 * version of live stream 
 */
struct Rendition {
  std::string name;
  int width;
  int height;
  int video_bitrate;
};

/**
 * @brief Struct to abstract libav
 * contexts for codec, format, scaler,
 * frame and packet
 */
struct EncodeOutput {
  AVFormatContext *fmt = nullptr;
  AVStream *vstream = nullptr;
  AVStream *astream = nullptr;
  AVCodecContext *venc = nullptr;
  SwsContext *sws = nullptr;
  AVFrame *sws_frame = nullptr;
  AVPacket *enc_pkt = nullptr;
};

/**
 * @brief Aggregated runtime state for a streaming session
 */
struct StreamState {
  AVFormatContext *in_ctx = nullptr;
  AVFormatContext *copy_ctx = nullptr;
  AVCodecContext *vdec = nullptr;
  AVStream *video_stream = nullptr;
  int video_index = -1;
  int audio_index = -1;
  int64_t fallback_pts = 0;
  int64_t packet_count = 0;
  std::vector<int64_t> copy_next_pts;
  std::vector<EncodeOutput> outputs;
  AVFrame *decoded = nullptr;
  AVPacket *audio_pkt = nullptr;
};

static std::atomic<bool> g_stop_requested(false);

/**
 * @brief Suppress libav logging
 */
static void quiet_av_log_callback(void *ptr, int level, const char *fmt,
                                  va_list vl) {
  /** Ignore libav logs */
  (void)ptr;
  (void)level;
  (void)fmt;
  (void)vl;
}

/**
 * @brief Signal handler for graceful shutdown
 *
 * @param signum
 */
static void handle_signal(int signum) {
  /** Request stop */
  (void)signum;
  g_stop_requested.store(true);
}

/**
 * @brief Print CLI usage instructions
 * 
 * @param argv0 
 */
static void print_usage(const char *argv0) {
  /** Print CLI usage */
  std::fprintf(
      stderr,
      "Usage: %s <input_url> <output_path> [--rtsp-tcp] [--reconnect-sec N] "
      "[--copy-max-keep-minutes M] [--encode-max-keep-minutes M] "
      "[--copy-hls-time S] [--encode-hls-time S] "
      "[--log-file PATH]\n"
      "Note: If output_path is a directory, index.m3u8 is created inside.\n"
      "Example: %s rtsp://cam/stream out.m3u8 --max-keep-minutes 5\n",
      argv0, argv0);
}

/**
 * @brief Convert AVERROR to string
 * 
 * @param errnum 
 * @return std::string 
 */
static std::string av_err2str_cpp(int errnum) {
  /** Format FFmpeg error code */
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(errnum, buf, sizeof(buf));
  return std::string(buf);
}

/** 
 * @brief Check if a string starts with a given prefix
 * 
 * @param s The string to check
 * @param prefix The prefix to look for
 * @return true if the string starts with the prefix, false otherwise
 */
/**
 * @brief Check if input scheme is treated as live
 *
 * @param url
 * @return true
 * @return false
 */
static bool is_live_input(const std::string &url) {
  /** RTSP/RTMP are treated as live sources */
  return utils::starts_with(
      url,
      "rtsp"
  ) || utils::starts_with(
      url,
      "rtmp"
  );
}


/**
 * @brief Close copy outputs by writing trailer, closing IO and freeing context
 * 
 * @param out_ctx 
 */
static void close_copy_output(AVFormatContext *out_ctx) {
  /** Close copy output */
  if (!out_ctx) {
    return;
  }

  av_write_trailer(out_ctx);
  if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
    avio_closep(&out_ctx->pb);
  }
  avformat_free_context(out_ctx);
}

/**
 * @brief Close reencode outputs by flushing encoders, writing trailer, closing IO and freeing contexts
 * 
 * @param outputs 
 */
static void close_reencode_outputs(std::vector<EncodeOutput> &outputs) {
  /** Close reencoded outputs */
  for (auto &out : outputs) {
    if (out.fmt) {
      av_write_trailer(out.fmt);
      if (!(out.fmt->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out.fmt->pb);
      }
      avformat_free_context(out.fmt);
      out.fmt = nullptr;
    }

    if (out.venc) {
      avcodec_free_context(&out.venc);
    }

    if (out.sws_frame) {
      av_frame_free(&out.sws_frame);
    }

    if (out.sws) {
      sws_freeContext(out.sws);
    }

    if (out.enc_pkt) {
      av_packet_free(&out.enc_pkt);
    }
  }
}

/**
 * @brief Open input stream with appropriate options for RTSP and HLS
 * 
 * @param input_url 
 * @param rtsp_tcp 
 * @param in_ctx 
 * @return int 
 */
static int open_input(const std::string &input_url, bool rtsp_tcp,
                      AVFormatContext **in_ctx) {
  AVDictionary *opts = nullptr;

  /** Apply RTSP transport and timeout options */
  if (utils::starts_with(input_url, "rtsp")) {
    if (rtsp_tcp) {
      av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    }
    av_dict_set(&opts, "stimeout", "10000000", 0);
    av_dict_set(&opts, "rw_timeout", "10000000", 0);
  }

  /** Apply HTTP reconnect options for HLS and HTTP(S) inputs */
  if (utils::starts_with(input_url, "http") || is_live_input(input_url)) {
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);
  }

  /** Open input */
  int ret = avformat_open_input(in_ctx, input_url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    log_message("ERROR", "Failed to open input: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  /** Generate missing PTS if needed */
  (*in_ctx)->flags |= AVFMT_FLAG_GENPTS;

  /** Load stream info */
  ret = avformat_find_stream_info(*in_ctx, nullptr);
  if (ret < 0) {
    log_message("ERROR", "Failed to find stream info: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  /** Log stream discovery */
  log_message("INFO", "Opened input with %u streams", (*in_ctx)->nb_streams);

  return 0;
}

/**
 * @brief Open codec copy output context for HLS with appropriate options
 * 
 * @param output_path 
 * @param in_ctx 
 * @param out_ctx 
 * @param max_keep_minutes 
 * @param hls_time_sec 
 * @return int 
 */
static int open_copy_output(const std::string &output_path,
                            AVFormatContext *in_ctx,
                            AVFormatContext **out_ctx, int max_keep_minutes,
                            int copy_hls_time_sec) {
  /** Create output context (HLS) */
  int ret = avformat_alloc_output_context2(out_ctx, nullptr, "hls",
                                           output_path.c_str());
  if (ret < 0 || !*out_ctx) {
    log_message("ERROR", "Failed to create output context: %s",
                av_err2str_cpp(ret).c_str());
    return ret < 0 ? ret : AVERROR_UNKNOWN;
  }

  /** Copy input streams */
  for (unsigned int i = 0; i < in_ctx->nb_streams; ++i) {
    AVStream *in_stream = in_ctx->streams[i];
    AVStream *out_stream = avformat_new_stream(*out_ctx, nullptr);
    if (!out_stream) {
      log_message("ERROR", "Failed to allocate output stream");
      return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
      log_message("ERROR", "Failed to copy codec parameters: %s",
                  av_err2str_cpp(ret).c_str());
      return ret;
    }

    out_stream->codecpar->codec_tag = 0;
    out_stream->time_base = in_stream->time_base;
  }

  /** Open output IO */
  if (!((*out_ctx)->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&(*out_ctx)->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      log_message("ERROR", "Failed to open output file: %s",
                  av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  /** Write header with HLS options */
  AVDictionary *hls_opts = nullptr;
  std::string base = utils::base_without_ext(
      output_path
  );
  std::string seg_pattern = base + "_seg_%d.ts";
  utils::set_hls_output_options(
      &hls_opts,
      max_keep_minutes,
      copy_hls_time_sec,
      seg_pattern
  );
  ret = avformat_write_header(*out_ctx, &hls_opts);
  av_dict_free(&hls_opts);
  if (ret < 0) {
    log_message("ERROR", "Failed to write header: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Add audio stream copy to output context
 * 
 * @param in_ctx 
 * @param out_ctx 
 * @param audio_index 
 * @return int 
 */
static int add_audio_stream_copy(AVFormatContext *in_ctx,
                                 AVFormatContext *out_ctx,
                                 int audio_index) {
  /** Create audio stream copy */
  AVStream *in_stream = in_ctx->streams[audio_index];
  AVStream *out_stream = avformat_new_stream(out_ctx, nullptr);
  if (!out_stream) {
    log_message("ERROR", "Failed to allocate audio stream");
    return AVERROR(ENOMEM);
  }

  int ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
  if (ret < 0) {
    log_message("ERROR", "Failed to copy audio codec parameters: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  out_stream->codecpar->codec_tag = 0;
  out_stream->time_base = in_stream->time_base;
  return 0;
}

/**
 * @brief Initialize encoder context for a given rendition and output format
 * 
 * @param out The EncodeOutput structure to initialize
 * @param rendition The rendition settings
 * @param fps The frame rate
 * @param global_header Whether to use a global header

 * @return int 
 */
static int init_video_encoder(EncodeOutput &out, const Rendition &rendition,
                              AVRational fps, bool global_header) {
  /** Find H.264 encoder */
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec) {
    log_message("ERROR", "H.264 encoder not found");
    return AVERROR_ENCODER_NOT_FOUND;
  }

  /** Create encoder context */
  out.venc = avcodec_alloc_context3(codec);
  if (!out.venc) {
    log_message("ERROR", "Failed to allocate encoder context");
    return AVERROR(ENOMEM);
  }

  out.venc->codec_id = AV_CODEC_ID_H264;
  out.venc->width = rendition.width;
  out.venc->height = rendition.height;
  out.venc->pix_fmt = AV_PIX_FMT_YUV420P;
  out.venc->time_base = av_inv_q(fps);
  out.venc->framerate = fps;
  out.venc->bit_rate = rendition.video_bitrate;
  out.venc->gop_size = fps.num > 0 ? fps.num * 2 / fps.den : 60;
  out.venc->max_b_frames = 0;

  /** Honor global header requirement */
  if (global_header) {
    out.venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  /** Apply low-latency-ish tune */
  utils::set_h264_encoder_options(
      out.venc->priv_data
  );

  /** Open encoder */
  int ret = avcodec_open2(out.venc, codec, nullptr);
  if (ret < 0) {
    log_message("ERROR", "Failed to open H.264 encoder: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Initialize reencode output context with video encoder and HLS options
 * 
 * @param output_path The path to the output file
 * @param in_ctx The input format context
 * @param audio_index The index of the audio stream to copy
 * @param rendition The rendition settings
 * @param max_keep_minutes 
 * @param hls_time_sec 
 * @param fps 
 * @param out 
 * @return int 
 */
static int init_reencode_output(const std::string &output_path,
                                AVFormatContext *in_ctx,
                                int audio_index, const Rendition &rendition,
                                int max_keep_minutes, int encode_hls_time_sec,
                                AVRational fps, EncodeOutput &out) {
  /** Create output context (HLS) */
  int ret = avformat_alloc_output_context2(&out.fmt, nullptr, "hls",
                                           output_path.c_str());
  if (ret < 0 || !out.fmt) {
    log_message("ERROR", "Failed to create output context: %s",
                av_err2str_cpp(ret).c_str());
    return ret < 0 ? ret : AVERROR_UNKNOWN;
  }

  /** Initialize video encoder */
  ret = init_video_encoder(out, rendition, fps,
                           (out.fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0);
  if (ret < 0) {
    return ret;
  }

  /** Allocate reusable packet */
  out.enc_pkt = av_packet_alloc();
  if (!out.enc_pkt) {
    log_message("ERROR", "Failed to allocate encoder packet");
    return AVERROR(ENOMEM);
  }

  /** Add video stream */
  out.vstream = avformat_new_stream(out.fmt, nullptr);
  if (!out.vstream) {
    log_message("ERROR", "Failed to allocate video stream");
    return AVERROR(ENOMEM);
  }

  ret = avcodec_parameters_from_context(out.vstream->codecpar, out.venc);
  if (ret < 0) {
    log_message("ERROR", "Failed to set video stream params: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  out.vstream->time_base = out.venc->time_base;

  /** Add audio stream (copy) */
  if (audio_index >= 0) {
    ret = add_audio_stream_copy(in_ctx, out.fmt, audio_index);
    if (ret < 0) {
      return ret;
    }
    out.astream = out.fmt->streams[out.fmt->nb_streams - 1];
  }

  /** Open output IO */
  if (!(out.fmt->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&out.fmt->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      log_message("ERROR", "Failed to open output file: %s",
                  av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  /** Write header with HLS options */
  AVDictionary *hls_opts = nullptr;
  std::string base = utils::base_without_ext(
      output_path
  );
  std::string seg_pattern = base + "_seg_%d.ts";
  utils::set_hls_output_options(
      &hls_opts,
      max_keep_minutes,
      encode_hls_time_sec,
      seg_pattern
  );
  ret = avformat_write_header(out.fmt, &hls_opts);
  av_dict_free(&hls_opts);
  if (ret < 0) {
    log_message("ERROR", "Failed to write header: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Init software scaler context and frame for reencode output based on input frame
 * 
 * @param out 
 * @param in_frame 
 * @return int 
 */
static int init_sws_for_output(EncodeOutput &out, AVFrame *in_frame) {
  /** Build swscale context */
  out.sws = sws_getContext(in_frame->width, in_frame->height,
                           static_cast<AVPixelFormat>(in_frame->format),
                           out.venc->width, out.venc->height,
                           out.venc->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
                           nullptr);
  if (!out.sws) {
    log_message("ERROR", "Failed to create swscale context");
    return AVERROR(EINVAL);
  }

  /** Allocate scaled frame */
  out.sws_frame = av_frame_alloc();
  if (!out.sws_frame) {
    log_message("ERROR", "Failed to allocate sws frame");
    return AVERROR(ENOMEM);
  }

  out.sws_frame->format = out.venc->pix_fmt;
  out.sws_frame->width = out.venc->width;
  out.sws_frame->height = out.venc->height;

  int ret = av_frame_get_buffer(out.sws_frame, 32);
  if (ret < 0) {
    log_message("ERROR", "Failed to allocate sws frame: %s",
                av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Write copy packet to copy output with rescaled timestamps
 * 
 * @param in_ctx 
 * @param out_ctx 
 * @param pkt 
 * @return int 
 */
static int write_copy_packet(AVFormatContext *in_ctx, AVFormatContext *out_ctx,
                             AVPacket *pkt) {
  /** Rescale timestamps and write */
  AVStream *in_stream = in_ctx->streams[pkt->stream_index];
  AVStream *out_stream = out_ctx->streams[pkt->stream_index];

  pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base,
                              out_stream->time_base,
                              static_cast<AVRounding>(
                                  AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
  pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base,
                              out_stream->time_base,
                              static_cast<AVRounding>(
                                  AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
  pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base,
                                out_stream->time_base);
  pkt->pos = -1;

  int ret = av_interleaved_write_frame(out_ctx, pkt);
  if (ret < 0) {
    log_message("ERROR", "Write error: %s", av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Encode frame 
 * 
 * @param out 
 * @param in_frame 
 * @param pts 
 * @return int 
 */
static int encode_and_write_frame(EncodeOutput &out, AVFrame *in_frame,
                                  int64_t pts) {
  /** Prepare scaled frame */
  int ret = av_frame_make_writable(out.sws_frame);
  if (ret < 0) {
    log_message("ERROR", "Frame not writable: %s", av_err2str_cpp(ret).c_str());
    return ret;
  }

  sws_scale(out.sws, in_frame->data, in_frame->linesize, 0, in_frame->height,
            out.sws_frame->data, out.sws_frame->linesize);

  out.sws_frame->pts = pts;

  /** Send to encoder */
  ret = avcodec_send_frame(out.venc, out.sws_frame);
  if (ret < 0) {
    log_message("ERROR", "Encode send error: %s", av_err2str_cpp(ret).c_str());
    return ret;
  }

  /** Drain encoder packets */
  av_packet_unref(out.enc_pkt);
  while (true) {
    ret = avcodec_receive_packet(out.venc, out.enc_pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      log_message("ERROR", "Encode receive error: %s",
                  av_err2str_cpp(ret).c_str());
      av_packet_unref(out.enc_pkt);
      return ret;
    }

    out.enc_pkt->stream_index = out.vstream->index;
    av_packet_rescale_ts(out.enc_pkt, out.venc->time_base,
                         out.vstream->time_base);

    ret = av_interleaved_write_frame(out.fmt, out.enc_pkt);
    av_packet_unref(out.enc_pkt);
    if (ret < 0) {
      log_message("ERROR", "Write error: %s", av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  return 0;
}

/**
 * @brief Normalize packet timestamps for copy output
 *
 * @param state
 * @param pkt
 */
static void normalize_copy_timestamps(StreamState &state, AVPacket *pkt) {
  /** Skip if no stream state */
  if (pkt->stream_index < 0 ||
      pkt->stream_index >= static_cast<int>(state.copy_next_pts.size())) {
    return;
  }

  /** Fill missing timestamps */
  if (pkt->pts == AV_NOPTS_VALUE && pkt->dts == AV_NOPTS_VALUE) {
    int64_t next = state.copy_next_pts[pkt->stream_index];
    pkt->pts = next;
    pkt->dts = next;
  } else if (pkt->pts == AV_NOPTS_VALUE) {
    pkt->pts = pkt->dts;
  } else if (pkt->dts == AV_NOPTS_VALUE) {
    pkt->dts = pkt->pts;
  }

  /** Enforce monotonic DTS */
  int64_t next = state.copy_next_pts[pkt->stream_index];
  if (pkt->dts < next) {
    pkt->dts = next;
  }
  if (pkt->pts < pkt->dts) {
    pkt->pts = pkt->dts;
  }

  /** Advance next PTS */
  int64_t inc = pkt->duration > 0 ? pkt->duration : 1;
  state.copy_next_pts[pkt->stream_index] = pkt->dts + inc;
}

/**
 * @brief Write audio packet to output context with rescaled timestamps
 * 
 * @param in_ctx The input format context
 * @param out_ctx The output format context
 * @param in_index The index of the input audio stream
 * @param out_index The index of the output audio stream
 * @param pkt 
 * @return int 
 */
static int write_audio_packet_to_output(AVFormatContext *in_ctx,
                                        AVFormatContext *out_ctx,
                                        int in_index, int out_index,
                                        AVPacket *pkt) {
  /** Rescale timestamps and write audio packet */
  AVStream *in_stream = in_ctx->streams[in_index];
  AVStream *out_stream = out_ctx->streams[out_index];

  pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base,
                              out_stream->time_base,
                              static_cast<AVRounding>(
                                  AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
  pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base,
                              out_stream->time_base,
                              static_cast<AVRounding>(
                                  AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
  pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base,
                                out_stream->time_base);
  pkt->pos = -1;
  pkt->stream_index = out_index;

  int ret = av_interleaved_write_frame(out_ctx, pkt);
  if (ret < 0) {
    log_message("ERROR", "Write audio error: %s", av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Open copy and reencode outputs
 *
 * @param state
 * @param output_path
 * @param renditions
 * @param max_keep_minutes
 * @param hls_time_sec
 * @return int
 */
static int open_outputs(StreamState &state, const std::string &output_path,
                        const std::vector<Rendition> &renditions,
                        int copy_max_keep_minutes, int copy_hls_time_sec,
                        int encode_max_keep_minutes, int encode_hls_time_sec) {
  /** Open copy output */
  int ret = open_copy_output(output_path, state.in_ctx, &state.copy_ctx,
                             copy_max_keep_minutes, copy_hls_time_sec);
  if (ret < 0) {
    return ret;
  }

  /** Initialize renditions */
  AVRational fps = av_guess_frame_rate(state.in_ctx, state.video_stream, nullptr);
  if (fps.num <= 0 || fps.den <= 0) {
    fps = {30, 1};
  }

  state.outputs.clear();
  state.outputs.reserve(renditions.size());

  std::string base = utils::base_without_ext(
      output_path
  );
  for (const auto &rendition : renditions) {
    EncodeOutput out;
    std::string path = base + "_" + rendition.name + ".m3u8";

    ret = init_reencode_output(path, state.in_ctx, state.audio_index, rendition,
                               encode_max_keep_minutes, encode_hls_time_sec,
                               fps, out);
    if (ret < 0) {
      close_copy_output(state.copy_ctx);
      state.copy_ctx = nullptr;
      close_reencode_outputs(state.outputs);
      return ret;
    }

    state.outputs.push_back(out);
  }

  /** Initialize copy timestamp tracking */
  state.copy_next_pts.assign(state.in_ctx->nb_streams, 0);

  return 0;
}

/**
 * @brief Distribute packet to copy and reencode outputs
 *
 * @param state
 * @param pkt
 * @return int
 */
static int distribute_outputs(StreamState &state, AVPacket *pkt) {
  /** Decode video for renditions */
  if (pkt->stream_index == state.video_index) {
    int ret = avcodec_send_packet(state.vdec, pkt);
    if (ret < 0) {
      log_message("ERROR", "Decode send error: %s",
                  av_err2str_cpp(ret).c_str());
      return ret;
    }

    while (true) {
      ret = avcodec_receive_frame(state.vdec, state.decoded);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        log_message("ERROR", "Decode receive error: %s",
                    av_err2str_cpp(ret).c_str());
        return ret;
      }

      /** Initialize sws once we have the first frame */
      for (auto &out : state.outputs) {
        if (!out.sws) {
          ret = init_sws_for_output(out, state.decoded);
          if (ret < 0) {
            log_message("ERROR", "Init sws error: %s",
                        av_err2str_cpp(ret).c_str());
            return ret;
          }
        }
      }

      /** Derive PTS for encoder timebase */
      int64_t in_pts = state.decoded->best_effort_timestamp;
      if (in_pts == AV_NOPTS_VALUE) {
        in_pts = state.fallback_pts++;
      }

      /** Encode all renditions */
      for (auto &out : state.outputs) {
        int64_t enc_pts = av_rescale_q(in_pts, state.video_stream->time_base,
                                       out.venc->time_base);
        ret = encode_and_write_frame(out, state.decoded, enc_pts);
        if (ret < 0) {
          log_message("ERROR", "Encode/write error: %s",
                      av_err2str_cpp(ret).c_str());
          return ret;
        }
      }
    }
  }

  /** Write audio packets to rendition outputs */
  if (state.audio_index >= 0 && pkt->stream_index == state.audio_index) {
    for (auto &out : state.outputs) {
      if (!out.astream) {
        continue;
      }
      av_packet_unref(state.audio_pkt);
      int ret = av_packet_ref(state.audio_pkt, pkt);
      if (ret < 0) {
        log_message("ERROR", "Audio packet ref error: %s",
                    av_err2str_cpp(ret).c_str());
        return ret;
      }

      ret = write_audio_packet_to_output(state.in_ctx, out.fmt, state.audio_index,
                                         out.astream->index, state.audio_pkt);
      av_packet_unref(state.audio_pkt);
      if (ret < 0) {
        log_message("ERROR", "Audio write error: %s",
                    av_err2str_cpp(ret).c_str());
        return ret;
      }
    }
  }

  /** Write copy output */
  normalize_copy_timestamps(state, pkt);
  int ret = write_copy_packet(state.in_ctx, state.copy_ctx, pkt);
  if (ret < 0) {
    log_message("ERROR", "Copy write error: %s", av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

/**
 * @brief Read packets and distribute to outputs
 *
 * @param state
 * @return int
 */
static int run_loop(StreamState &state) {
  /** Allocate packet for reading */
  AVPacket *pkt = av_packet_alloc();
  if (!pkt) {
    log_message("ERROR", "Failed to allocate packet");
    return AVERROR(ENOMEM);
  }

  int ret = 0;
  while (true) {
    if (g_stop_requested.load()) {
      log_message("INFO", "Stop requested, ending loop");
      ret = AVERROR_EXIT;
      break;
    }

    ret = av_read_frame(state.in_ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR(ETIMEDOUT)) {
      log_message("WARN", "Read timeout, retrying...");
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    if (ret == AVERROR_EOF) {
      log_message("WARN", "Input reached EOF");
      break;
    }
    if (ret < 0) {
      log_message("ERROR", "Read error: %s", av_err2str_cpp(ret).c_str());
      break;
    }

    ret = distribute_outputs(state, pkt);
    av_packet_unref(pkt);
    if (ret < 0) {
      break;
    }

    state.packet_count++;
    if (state.packet_count % 300 == 0) {
      log_message("INFO", "Processed %" PRId64 " packets",
                  state.packet_count);
    }
  }

  av_packet_free(&pkt);
  return ret;
}

/**
 * @brief Flush encoders at the end of stream to ensure all packets are written
 * 
 * @param outputs The list of encode outputs
 * @return int 0 on success, negative error code on failure
 */
static int flush_encoders(std::vector<EncodeOutput> &outputs) {
  /** Flush each encoder */
  for (auto &out : outputs) {
    int ret = avcodec_send_frame(out.venc, nullptr);
    if (ret < 0) {
      log_message("ERROR", "Flush send error: %s", av_err2str_cpp(ret).c_str());
      return ret;
    }

    while (true) {
      ret = avcodec_receive_packet(out.venc, out.enc_pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        log_message("ERROR", "Flush receive error: %s",
                    av_err2str_cpp(ret).c_str());
        av_packet_unref(out.enc_pkt);
        return ret;
      }

      out.enc_pkt->stream_index = out.vstream->index;
      av_packet_rescale_ts(out.enc_pkt, out.venc->time_base,
                           out.vstream->time_base);

      ret = av_interleaved_write_frame(out.fmt, out.enc_pkt);
      av_packet_unref(out.enc_pkt);
      if (ret < 0) {
        log_message("ERROR", "Flush write error: %s",
                    av_err2str_cpp(ret).c_str());
        return ret;
      }
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    print_usage(argv[0]);
    return 1;
  }

  /** Capture CLI inputs */
  std::string input_url = argv[1];
  std::string output_path = argv[2];
  std::string log_file = "streamer.log";
  bool rtsp_tcp = false;
  int reconnect_sec = 0;
  int copy_max_keep_minutes = 0;
  int encode_max_keep_minutes = 5;
  int copy_hls_time_sec = 0;
  int encode_hls_time_sec = 4;
  bool live_input = is_live_input(input_url);

  /** Parse CLI */
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--rtsp-tcp") == 0) {
      rtsp_tcp = true;
    } else if (std::strcmp(argv[i], "--reconnect-sec") == 0 && i + 1 < argc) {
      reconnect_sec = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--copy-max-keep-minutes") == 0 && i + 1 < argc) {
      copy_max_keep_minutes = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--encode-max-keep-minutes") == 0 && i + 1 < argc) {
      encode_max_keep_minutes = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--copy-hls-time") == 0 && i + 1 < argc) {
      copy_hls_time_sec = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--encode-hls-time") == 0 && i + 1 < argc) {
      encode_hls_time_sec = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
      log_file = argv[i + 1];
      ++i;
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  /** Initialize logger */
  if (!log_init(log_file)) {
    return 1;
  }

  /** Normalize output path */
  output_path = utils::normalize_output_path(
      output_path
  );

  /** Auto-reconnect for live inputs unless disabled */
  if (live_input && reconnect_sec <= 0) {
    reconnect_sec = 5;
  }

  /** Log configuration */
  log_message("INFO", "Input URL: %s", input_url.c_str());
  log_message("INFO", "Output HLS: %s", output_path.c_str());
  log_message("INFO", "Log file: %s", log_file.c_str());
  log_message("INFO", "Reconnect seconds: %d", reconnect_sec);
  log_message("INFO", "Copy max keep minutes: %d", copy_max_keep_minutes);
  log_message("INFO", "Encode max keep minutes: %d", encode_max_keep_minutes);
  log_message("INFO", "Copy HLS time: %d", copy_hls_time_sec);
  log_message("INFO", "Encode HLS time: %d", encode_hls_time_sec);

  /** Static ladder for low/mid/high */
  std::vector<Rendition> renditions = {
      {"low", 426, 240, 400000},
      {"mid", 854, 480, 1200000},
      {"high", 1280, 720, 2500000},
  };

  /** Initialize FFmpeg */
  av_log_set_level(AV_LOG_QUIET);
  av_log_set_callback(quiet_av_log_callback);
  avformat_network_init();

  /** Install signal handlers */
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  /** Reconnect loop */
  int exit_code = 0;
  while (true) {
    if (g_stop_requested.load()) {
      log_message("INFO", "Stop requested, shutting down");
      exit_code = 0;
      break;
    }

    AVFormatContext *in_ctx = nullptr;

    /** Open input */
    int ret = open_input(input_url, rtsp_tcp, &in_ctx);
    if (ret < 0) {
      if (reconnect_sec > 0) {
        log_message("INFO", "Retrying in %d seconds...", reconnect_sec);
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_sec));
        continue;
      }
      exit_code = 2;
      break;
    }

    /** Find best video stream */
    int video_index = av_find_best_stream(in_ctx, AVMEDIA_TYPE_VIDEO, -1, -1,
                                          nullptr, 0);
    if (video_index < 0) {
      log_message("ERROR", "No video stream found");
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    /** Find best audio stream */
    int audio_index = av_find_best_stream(in_ctx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                          nullptr, 0);

    /** Open decoder */
    AVStream *video_stream = in_ctx->streams[video_index];
    const AVCodec *decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!decoder) {
      log_message("ERROR", "Video decoder not found");
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    AVCodecContext *vdec = avcodec_alloc_context3(decoder);
    if (!vdec) {
      log_message("ERROR", "Failed to allocate decoder context");
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    ret = avcodec_parameters_to_context(vdec, video_stream->codecpar);
    if (ret < 0) {
      log_message("ERROR", "Failed to set decoder params: %s",
                  av_err2str_cpp(ret).c_str());
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    ret = avcodec_open2(vdec, decoder, nullptr);
    if (ret < 0) {
      log_message("ERROR", "Failed to open decoder: %s",
                  av_err2str_cpp(ret).c_str());
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    /** Prepare stream state */
    StreamState state;
    state.in_ctx = in_ctx;
    state.copy_ctx = nullptr;
    state.vdec = vdec;
    state.video_stream = video_stream;
    state.video_index = video_index;
    state.audio_index = audio_index;

    /** Open outputs */
    ret = open_outputs(state, output_path, renditions,
                       copy_max_keep_minutes, copy_hls_time_sec,
                       encode_max_keep_minutes, encode_hls_time_sec);
    if (ret < 0) {
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 4;
      break;
    }

    /** Allocate shared decode resources */
    state.decoded = av_frame_alloc();
    if (!state.decoded) {
      log_message("ERROR", "Failed to allocate decode frame");
      close_copy_output(state.copy_ctx);
      close_reencode_outputs(state.outputs);
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 5;
      break;
    }

    state.audio_pkt = av_packet_alloc();
    if (!state.audio_pkt) {
      log_message("ERROR", "Failed to allocate audio packet");
      av_frame_free(&state.decoded);
      close_copy_output(state.copy_ctx);
      close_reencode_outputs(state.outputs);
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 5;
      break;
    }

    /** Read and distribute packets */
    ret = run_loop(state);

    av_packet_free(&state.audio_pkt);
    av_frame_free(&state.decoded);

    /** Flush encoders */
    ret = flush_encoders(state.outputs);
    if (ret < 0) {
      log_message("ERROR", "Flush error: %s", av_err2str_cpp(ret).c_str());
    }

    /** Cleanup outputs */
    close_copy_output(state.copy_ctx);
    close_reencode_outputs(state.outputs);
    avcodec_free_context(&vdec);
    avformat_close_input(&in_ctx);

    if (ret == AVERROR_EXIT) {
      exit_code = 0;
      break;
    }
    if (ret == AVERROR_EOF || ret == 0) {
      if (reconnect_sec > 0) {
        if (g_stop_requested.load()) {
          exit_code = 0;
          break;
        }
        log_message("INFO", "Restarting after EOF in %d seconds...",
                    reconnect_sec);
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_sec));
        continue;
      }
      exit_code = 0;
      break;
    }

    if (ret < 0) {
      if (reconnect_sec > 0) {
        if (g_stop_requested.load()) {
          exit_code = 0;
          break;
        }
        log_message("INFO", "Stream error, reconnecting in %d seconds...",
                    reconnect_sec);
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_sec));
        continue;
      }
      exit_code = 4;
      break;
    }

    if (exit_code != 0) {
      break;
    }
  }

  avformat_network_deinit();
  log_message("INFO", "Exiting with code %d", exit_code);
  log_close();
  return exit_code;
}
