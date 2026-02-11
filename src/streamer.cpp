#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static void print_usage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s <input_url> <output_path> [--rtsp-tcp] [--reconnect-sec N]\n"
      "Example: %s rtsp://cam/stream out.m3u8\n",
      argv0, argv0);
}

static std::string av_err2str_cpp(int errnum) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(errnum, buf, sizeof(buf));
  return std::string(buf);
}

static int open_input(const std::string &input_url, bool rtsp_tcp,
                      AVFormatContext **in_ctx) {
  AVDictionary *opts = nullptr;
  if (rtsp_tcp) {
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  }
  // Reasonable network timeouts (microseconds)
  av_dict_set(&opts, "stimeout", "5000000", 0);
  av_dict_set(&opts, "rw_timeout", "5000000", 0);

  int ret = avformat_open_input(in_ctx, input_url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to open input: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  ret = avformat_find_stream_info(*in_ctx, nullptr);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to find stream info: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int open_output(const std::string &output_path, AVFormatContext *in_ctx,
                       AVFormatContext **out_ctx) {
  int ret = avformat_alloc_output_context2(out_ctx, nullptr, nullptr,
                                           output_path.c_str());
  if (ret < 0 || !*out_ctx) {
    std::fprintf(stderr, "Failed to create output context: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret < 0 ? ret : AVERROR_UNKNOWN;
  }

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

  if (!((*out_ctx)->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&(*out_ctx)->pb, output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      std::fprintf(stderr, "Failed to open output file: %s\n",
                   av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  ret = avformat_write_header(*out_ctx, nullptr);
  if (ret < 0) {
    std::fprintf(stderr, "Failed to write header: %s\n",
                 av_err2str_cpp(ret).c_str());
    return ret;
  }

  return 0;
}

static int remux_loop(AVFormatContext *in_ctx, AVFormatContext *out_ctx) {
  AVPacket pkt;
  av_init_packet(&pkt);

  while (true) {
    int ret = av_read_frame(in_ctx, &pkt);
    if (ret == AVERROR_EOF) {
      return AVERROR_EOF;
    }
    if (ret < 0) {
      std::fprintf(stderr, "Read error: %s\n", av_err2str_cpp(ret).c_str());
      return ret;
    }

    AVStream *in_stream = in_ctx->streams[pkt.stream_index];
    AVStream *out_stream = out_ctx->streams[pkt.stream_index];

    // Rescale timestamps to output timebase
    pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base,
                              out_stream->time_base,
                              AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base,
                              out_stream->time_base,
                              AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base,
                                out_stream->time_base);
    pkt.pos = -1;

    ret = av_interleaved_write_frame(out_ctx, &pkt);
    av_packet_unref(&pkt);
    if (ret < 0) {
      std::fprintf(stderr, "Write error: %s\n", av_err2str_cpp(ret).c_str());
      return ret;
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    print_usage(argv[0]);
    return 1;
  }

  std::string input_url = argv[1];
  std::string output_path = argv[2];
  bool rtsp_tcp = false;
  int reconnect_sec = 0;

  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--rtsp-tcp") == 0) {
      rtsp_tcp = true;
    } else if (std::strcmp(argv[i], "--reconnect-sec") == 0 && i + 1 < argc) {
      reconnect_sec = std::atoi(argv[i + 1]);
      ++i;
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  av_log_set_level(AV_LOG_INFO);
  avformat_network_init();

  int exit_code = 0;
  while (true) {
    AVFormatContext *in_ctx = nullptr;
    AVFormatContext *out_ctx = nullptr;

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

    ret = open_output(output_path, in_ctx, &out_ctx);
    if (ret < 0) {
      avformat_close_input(&in_ctx);
      exit_code = 3;
      break;
    }

    ret = remux_loop(in_ctx, out_ctx);

    av_write_trailer(out_ctx);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&out_ctx->pb);
    }
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);

    if (ret == AVERROR_EOF) {
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

    exit_code = 0;
    break;
  }

  avformat_network_deinit();
  return exit_code;
}
