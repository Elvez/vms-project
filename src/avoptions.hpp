#pragma once

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

#include <string>

namespace utils {

/**
 * @brief Set the hls output options
 * 
 * @param opts dictionary to store HLS options
 * @param max_keep_minutes maximum minutes to keep in HLS playlist, 0 for unlimited
 * @param hls_time_sec size of one segment
 * @param segment_pattern name of segments with pattern, e.g. "segment_%03d.ts"
 * @return AVERROR code, 0 on success
 */
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

/**
 * @brief Set the h264 encoder options for low latency streaming
 * 
 * @param priv_data 
 */
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

