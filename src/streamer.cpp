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
#include <string>
#include <thread>
#include <vector>

struct Rendition {
  std::string name;
  int width;
  int height;
  int video_bitrate;
};

struct EncodeOutput {
  AVFormatContext *fmt = nullptr;
  AVStream *vstream = nullptr;
  AVStream *astream = nullptr;
  AVCodecContext *venc = nullptr;
  SwsContext *sws = nullptr;
  AVFrame *sws_frame = nullptr;
  AVPacket *enc_pkt = nullptr;
};

static void print_usage(const char *argv0) {
  /** Print CLI usage */
  std::fprintf(
      stderr,
      "Usage: %s <input_url> <output_path> [--rtsp-tcp] [--reconnect-sec N] "
      "[--max-keep-minutes M] [--hls-time S]\n"
      "Example: %s rtsp://cam/stream out.m3u8 --max-keep-minutes 5\n",
      argv0, argv0);
}

static std::string av_err2str_cpp(int errnum) {
  /** Format FFmpeg error code */
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(errnum, buf, sizeof(buf));
  return std::string(buf);
}

static bool starts_with(const std::string &s, const char *prefix) {
  /** Check string prefix */
  return s.rfind(prefix, 0) == 0;
}

static bool is_hls_input(const std::string &url) {
  /** Detect HLS input by extension */
  return url.find(".m3u8") != std::string::npos;
}

static std::string base_without_ext(const std::string &path) {
  /** Remove last extension from path */
  size_t dot = path.rfind('.');
  if (dot == std::string::npos) {
    return path;
  }
  return path.substr(0, dot);
}

static int set_hls_output_options(AVDictionary **opts, int max_keep_minutes,
                                  int hls_time_sec,
                                  const std::string &segment_pattern) {
  /** Normalize inputs */
  if (hls_time_sec <= 0) {
    hls_time_sec = 4;
  }

  /** Compute list size from keep window */
  int list_size = (max_keep_minutes * 60) / hls_time_sec;
  if (list_size < 2) {
    list_size = 2;
  }

  /** Apply HLS options */
  av_dict_set_int(opts, "hls_time", hls_time_sec, 0);
  av_dict_set_int(opts, "hls_list_size", list_size, 0);
  av_dict_set(opts, "hls_flags", "delete_segments", 0);
  av_dict_set(opts, "hls_segment_filename", segment_pattern.c_str(), 0);

  return 0;
}

static int open_input(const std::string &input_url, bool rtsp_tcp,
                      AVFormatContext **in_ctx) {
  AVDictionary *opts = nullptr;

  /** Apply RTSP transport and timeout options */
  if (starts_with(input_url, "rtsp")) {
    if (rtsp_tcp) {
      av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    }
    av_dict_set(&opts, "stimeout", "5000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);
  }

  /** Apply HTTP reconnect options for HLS and HTTP(S) inputs */
  if (starts_with(input_url, "http") || is_hls_input(input_url)) {
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);
  }

  /** Open input */
  int ret = avformat_open_input(in_ctx, input_url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to open input: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  /** Load stream info */
  ret = avformat_find_stream_info(*in_ctx, nullptr);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to find stream info: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int open_copy_output(const std::string &output_path,
                            AVFormatContext *in_ctx,
                            AVFormatContext **out_ctx, int max_keep_minutes,
                            int hls_time_sec) {
  /** Create output context (HLS) */
  int ret = avformat_alloc_output_context2(out_ctx, nullptr, "hls",
                                           output_path.c_str());
  if (ret < 0 || !*out_ctx) {
    std::fprintf(stderr, "Failed to create output context: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret < 0 ? ret : AVERROR_UNKNOWN;
  }

  /** Copy input streams */
  for (unsigned int i = 0; i < in_ctx->nb_streams; ++i) {
    AVStream *in_stream = in_ctx->streams[i];
    AVStream *out_stream = avformat_new_stream(*out_ctx, nullptr);
    if (!out_stream) {
      std::fprintf(stderr, "Failed to allocate output stream\n");
      return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    if (ret < 0) {
      std::fprintf(stderr, "Failed to copy codec parameters: %s\n",
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
      std::fprintf(stderr, "Failed to open output file: %s\n",
                   av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  /** Write header with HLS options */
  AVDictionary *hls_opts = nullptr;
  std::string base = base_without_ext(output_path);
  std::string seg_pattern = base + "_seg_%d.ts";
  set_hls_output_options(&hls_opts, max_keep_minutes, hls_time_sec,
                         seg_pattern);
  ret = avformat_write_header(*out_ctx, &hls_opts);
  av_dict_free(&hls_opts);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to write header: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int add_audio_stream_copy(AVFormatContext *in_ctx,
                                 AVFormatContext *out_ctx,
                                 int audio_index) {
  /** Create audio stream copy */
  AVStream *in_stream = in_ctx->streams[audio_index];
  AVStream *out_stream = avformat_new_stream(out_ctx, nullptr);
  if (!out_stream) {
    std::fprintf(stderr, "Failed to allocate audio stream\n");
    return AVERROR(ENOMEM);
  }

  int ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to copy audio codec parameters: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  out_stream->codecpar->codec_tag = 0;
  out_stream->time_base = in_stream->time_base;
  return 0;
}

static int init_video_encoder(EncodeOutput &out, const Rendition &rendition,
                              AVRational fps, bool global_header) {
  /** Find H.264 encoder */
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec) {
    std::fprintf(stderr, "H.264 encoder not found\n");
    return AVERROR_ENCODER_NOT_FOUND;
  }

  /** Create encoder context */
  out.venc = avcodec_alloc_context3(codec);
  if (!out.venc) {
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
  av_opt_set(out.venc->priv_data, "preset", "veryfast", 0);
  av_opt_set(out.venc->priv_data, "tune", "zerolatency", 0);

  /** Open encoder */
  int ret = avcodec_open2(out.venc, codec, nullptr);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to open H.264 encoder: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int init_reencode_output(const std::string &output_path,
                                AVFormatContext *in_ctx,
                                int audio_index, const Rendition &rendition,
                                int max_keep_minutes, int hls_time_sec,
                                AVRational fps, EncodeOutput &out) {
  /** Create output context (HLS) */
  int ret = avformat_alloc_output_context2(&out.fmt, nullptr, "hls",
                                           output_path.c_str());
  if (ret < 0 || !out.fmt) {
    std::fprintf(stderr, "Failed to create output context: %s\n",
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
    return AVERROR(ENOMEM);
  }

  /** Add video stream */
  out.vstream = avformat_new_stream(out.fmt, nullptr);
  if (!out.vstream) {
    std::fprintf(stderr, "Failed to allocate video stream\n");
    return AVERROR(ENOMEM);
  }

  ret = avcodec_parameters_from_context(out.vstream->codecpar, out.venc);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to set video stream params: %s\n",
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
      std::fprintf(stderr, "Failed to open output file: %s\n",
                   av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  /** Write header with HLS options */
  AVDictionary *hls_opts = nullptr;
  std::string base = base_without_ext(output_path);
  std::string seg_pattern = base + "_seg_%d.ts";
  set_hls_output_options(&hls_opts, max_keep_minutes, hls_time_sec,
                         seg_pattern);
  ret = avformat_write_header(out.fmt, &hls_opts);
  av_dict_free(&hls_opts);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to write header: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int init_sws_for_output(EncodeOutput &out, AVFrame *in_frame) {
  /** Build swscale context */
  out.sws = sws_getContext(in_frame->width, in_frame->height,
                           static_cast<AVPixelFormat>(in_frame->format),
                           out.venc->width, out.venc->height,
                           out.venc->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
                           nullptr);
  if (!out.sws) {
    std::fprintf(stderr, "Failed to create swscale context\n");
    return AVERROR(EINVAL);
  }

  /** Allocate scaled frame */
  out.sws_frame = av_frame_alloc();
  if (!out.sws_frame) {
    return AVERROR(ENOMEM);
  }

  out.sws_frame->format = out.venc->pix_fmt;
  out.sws_frame->width = out.venc->width;
  out.sws_frame->height = out.venc->height;

  int ret = av_frame_get_buffer(out.sws_frame, 32);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to allocate sws frame: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

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
    std::fprintf(stderr, "Write error: %s\n", av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int encode_and_write_frame(EncodeOutput &out, AVFrame *in_frame,
                                  int64_t pts) {
  /** Prepare scaled frame */
  int ret = av_frame_make_writable(out.sws_frame);
  if (ret < 0) {
    return ret;
  }

  sws_scale(out.sws, in_frame->data, in_frame->linesize, 0, in_frame->height,
            out.sws_frame->data, out.sws_frame->linesize);

  out.sws_frame->pts = pts;

  /** Send to encoder */
  ret = avcodec_send_frame(out.venc, out.sws_frame);
  if (ret < 0) {
    std::fprintf(stderr, "Encode send error: %s\n", av_err2str_cpp(ret).c_str());
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
      std::fprintf(stderr, "Encode receive error: %s\n",
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
      std::fprintf(stderr, "Write error: %s\n", av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  return 0;
}

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
    std::fprintf(stderr, "Write audio error: %s\n", av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int flush_encoders(std::vector<EncodeOutput> &outputs) {
  /** Flush each encoder */
  for (auto &out : outputs) {
    int ret = avcodec_send_frame(out.venc, nullptr);
    if (ret < 0) {
      return ret;
    }

    while (true) {
      ret = avcodec_receive_packet(out.venc, out.enc_pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        av_packet_unref(out.enc_pkt);
        return ret;
      }

      out.enc_pkt->stream_index = out.vstream->index;
      av_packet_rescale_ts(out.enc_pkt, out.venc->time_base,
                           out.vstream->time_base);

      ret = av_interleaved_write_frame(out.fmt, out.enc_pkt);
      av_packet_unref(out.enc_pkt);
      if (ret < 0) {
        return ret;
      }
    }
  }

  return 0;
}

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

int main(int argc, char **argv) {
  if (argc < 3) {
    print_usage(argv[0]);
    return 1;
  }

  /** Capture CLI inputs */
  std::string input_url = argv[1];
  std::string output_path = argv[2];
  bool rtsp_tcp = false;
  int reconnect_sec = 0;
  int max_keep_minutes = 5;
  int hls_time_sec = 4;

  /** Parse CLI */
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--rtsp-tcp") == 0) {
      rtsp_tcp = true;
    } else if (std::strcmp(argv[i], "--reconnect-sec") == 0 && i + 1 < argc) {
      reconnect_sec = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--max-keep-minutes") == 0 && i + 1 < argc) {
      max_keep_minutes = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--hls-time") == 0 && i + 1 < argc) {
      hls_time_sec = std::atoi(argv[i + 1]);
      ++i;
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  /** Static ladder for low/mid/high */
  std::vector<Rendition> renditions = {
      {"low", 426, 240, 400000},
      {"mid", 854, 480, 1200000},
      {"high", 1280, 720, 2500000},
  };

  /** Initialize FFmpeg */
  av_log_set_level(AV_LOG_INFO);
  avformat_network_init();

  /** Reconnect loop */
  int exit_code = 0;
  while (true) {
    AVFormatContext *in_ctx = nullptr;
    AVFormatContext *copy_ctx = nullptr;

    /** Open input */
    int ret = open_input(input_url, rtsp_tcp, &in_ctx);
    if (ret < 0) {
      if (reconnect_sec > 0) {
        std::fprintf(stderr, "Retrying in %d seconds...\n", reconnect_sec);
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
      std::fprintf(stderr, "No video stream found\n");
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
      std::fprintf(stderr, "Video decoder not found\n");
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    AVCodecContext *vdec = avcodec_alloc_context3(decoder);
    if (!vdec) {
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    ret = avcodec_parameters_to_context(vdec, video_stream->codecpar);
    if (ret < 0) {
      std::fprintf(stderr, "Failed to set decoder params: %s\n",
                   av_err2str_cpp(ret).c_str());
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    ret = avcodec_open2(vdec, decoder, nullptr);
    if (ret < 0) {
      std::fprintf(stderr, "Failed to open decoder: %s\n",
                   av_err2str_cpp(ret).c_str());
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    /** Prepare output paths */
    std::string base = base_without_ext(output_path);
    std::string copy_path = output_path;

    /** Open copy HLS output */
    ret = open_copy_output(copy_path, in_ctx, &copy_ctx, max_keep_minutes,
                           hls_time_sec);
    if (ret < 0) {
      avcodec_free_context(&vdec);
      avformat_close_input(&in_ctx);
      exit_code = 4;
      break;
    }

    /** Initialize renditions */
    AVRational fps = av_guess_frame_rate(in_ctx, video_stream, nullptr);
    if (fps.num <= 0 || fps.den <= 0) {
      fps = {30, 1};
    }

    std::vector<EncodeOutput> outputs;
    outputs.reserve(renditions.size());

    for (const auto &rendition : renditions) {
      EncodeOutput out;
      std::string path = base + "_" + rendition.name + ".m3u8";

      ret = init_reencode_output(path, in_ctx, audio_index, rendition,
                                 max_keep_minutes, hls_time_sec, fps, out);
      if (ret < 0) {
        close_copy_output(copy_ctx);
        close_reencode_outputs(outputs);
        avcodec_free_context(&vdec);
        avformat_close_input(&in_ctx);
        exit_code = 4;
        goto cleanup_loop;
      }

      outputs.push_back(out);
    }

    /** Decode and transcode loop */
    {
      AVPacket *pkt = av_packet_alloc();
      if (!pkt) {
        std::fprintf(stderr, "Failed to allocate packet\n");
        exit_code = 5;
        goto cleanup_loop;
      }

      AVPacket *audio_pkt = av_packet_alloc();
      if (!audio_pkt) {
        std::fprintf(stderr, "Failed to allocate audio packet\n");
        av_packet_free(&pkt);
        exit_code = 5;
        goto cleanup_loop;
      }

      AVFrame *decoded = av_frame_alloc();
      if (!decoded) {
        std::fprintf(stderr, "Failed to allocate decode frame\n");
        av_packet_free(&audio_pkt);
        av_packet_free(&pkt);
        exit_code = 5;
        goto cleanup_loop;
      }

      int64_t fallback_pts = 0;

      while (true) {
        ret = av_read_frame(in_ctx, pkt);
        if (ret == AVERROR_EOF) {
          break;
        }
        if (ret < 0) {
          std::fprintf(stderr, "Read error: %s\n", av_err2str_cpp(ret).c_str());
          break;
        }

        /** Decode video for renditions */
        if (pkt->stream_index == video_index) {
          ret = avcodec_send_packet(vdec, pkt);
          if (ret < 0) {
            std::fprintf(stderr, "Decode send error: %s\n",
                         av_err2str_cpp(ret).c_str());
            av_packet_unref(pkt);
            break;
          }

          while (true) {
            ret = avcodec_receive_frame(vdec, decoded);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
              break;
            }
            if (ret < 0) {
              std::fprintf(stderr, "Decode receive error: %s\n",
                           av_err2str_cpp(ret).c_str());
              break;
            }

            /** Initialize sws once we have the first frame */
            for (auto &out : outputs) {
              if (!out.sws) {
                ret = init_sws_for_output(out, decoded);
                if (ret < 0) {
                  break;
                }
              }
            }
            if (ret < 0) {
              break;
            }

            /** Derive PTS for encoder timebase */
            int64_t in_pts = decoded->best_effort_timestamp;
            if (in_pts == AV_NOPTS_VALUE) {
              in_pts = fallback_pts++;
            }

            /** Encode all renditions */
            for (auto &out : outputs) {
              int64_t enc_pts = av_rescale_q(in_pts, video_stream->time_base,
                                             out.venc->time_base);
              ret = encode_and_write_frame(out, decoded, enc_pts);
              if (ret < 0) {
                break;
              }
            }
            if (ret < 0) {
              break;
            }
          }
          if (ret < 0) {
            av_packet_unref(pkt);
            break;
          }
        }

        /** Write audio packets to rendition outputs */
        if (audio_index >= 0 && pkt->stream_index == audio_index) {
          for (auto &out : outputs) {
            if (!out.astream) {
              continue;
            }
            av_packet_unref(audio_pkt);
            ret = av_packet_ref(audio_pkt, pkt);
            if (ret < 0) {
              break;
            }

            ret = write_audio_packet_to_output(in_ctx, out.fmt, audio_index,
                                               out.astream->index, audio_pkt);
            av_packet_unref(audio_pkt);
            if (ret < 0) {
              break;
            }
          }
          if (ret < 0) {
            av_packet_unref(pkt);
            break;
          }
        }

        /** Write copy output */
        ret = write_copy_packet(in_ctx, copy_ctx, pkt);
        if (ret < 0) {
          av_packet_unref(pkt);
          break;
        }

        av_packet_unref(pkt);
      }

      av_frame_free(&decoded);
      av_packet_free(&audio_pkt);
      av_packet_free(&pkt);
    }

    /** Flush encoders */
    ret = flush_encoders(outputs);
    if (ret < 0) {
      std::fprintf(stderr, "Flush error: %s\n", av_err2str_cpp(ret).c_str());
    }

    /** Cleanup outputs */
    close_copy_output(copy_ctx);
    close_reencode_outputs(outputs);
    avcodec_free_context(&vdec);
    avformat_close_input(&in_ctx);

    if (ret == AVERROR_EOF || ret == 0) {
      exit_code = 0;
      break;
    }

    if (ret < 0) {
      if (reconnect_sec > 0) {
        std::fprintf(stderr, "Stream error, reconnecting in %d seconds...\n",
                     reconnect_sec);
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_sec));
        continue;
      }
      exit_code = 4;
      break;
    }

  cleanup_loop:
    if (exit_code != 0) {
      break;
    }
  }

  avformat_network_deinit();
  return exit_code;
}
