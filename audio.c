// Einkster audio: ES8311 capture via Waveshare's codec_board layer.
#include <stdio.h>
#include "audio.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"

static esp_codec_dev_handle_t playback = NULL;
static esp_codec_dev_handle_t record = NULL;

void ek_audio_init(void)
{
  set_codec_board_type("S3_ePaper_1_54");
  codec_init_cfg_t cfg = {
    .in_mode = CODEC_I2S_MODE_STD,
    .out_mode = CODEC_I2S_MODE_STD,
    .in_use_tdm = false,
    .reuse_dev = false,
  };
  init_codec(&cfg);
  playback = get_playback_handle();
  record = get_record_handle();

  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = EK_SAMPLE_RATE;
  fs.channel = 2;             /* the codec delivers stereo frames */
  fs.bits_per_sample = 16;
  esp_codec_dev_set_out_vol(playback, 70.0);
  esp_codec_dev_set_in_gain(record, 45.0);   /* Waveshare's tested value */
  esp_codec_dev_open(playback, &fs);
  esp_codec_dev_open(record, &fs);
}

int ek_audio_read(void *buf, uint32_t len)
{
  return esp_codec_dev_read(record, buf, len) == ESP_CODEC_DEV_OK ? 0 : -1;
}
