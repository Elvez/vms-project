#pragma once

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

#include <string>

namespace utils {

static inline int set_hls_output_options(
    AVDictionary **opts,
    int max_keep_minutes,
    int hls_time_sec,
    const std::string &segment_pattern
) {
  if (hls_time_sec > 0) {
    av_dict_set_int(
        opts,
        "hls_time",
        hls_time_sec,
        0
    );
  }

  if (hls_time_sec > 0 && max_keep_minutes > 0) {
    int list_size = (max_keep_minutes * 60) / hls_time_sec;
    if (list_size < 2) {
      list_size = 2;
    }
    av_dict_set_int(
        opts,
        "hls_list_size",
        list_size,
        0
    );
  }

  av_dict_set(
      opts,
      "hls_flags",
      "delete_segments",
      0
  );

  av_dict_set(
      opts,
      "hls_segment_filename",
      segment_pattern.c_str(),
      0
  );

  return 0;
}

static inline void set_h264_encoder_options(
    void *priv_data
) {
  av_opt_set(
      priv_data,
      "preset",
      "veryfast",
      0
  );
  av_opt_set(
      priv_data,
      "tune",
      "zerolatency",
      0
  );
}

}  // namespace utils

