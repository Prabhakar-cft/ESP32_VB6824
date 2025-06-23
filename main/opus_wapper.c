#include "opus_wapper.h"
#include "esp_log.h"

#define TAG "opus_warpper"

OpusCodec* opus_codec_init(int enc_fs, int dec_fs, int duration_ms, int channels) {
    OpusCodec* codec = (OpusCodec*)malloc(sizeof(OpusCodec));
    if (codec == NULL) {
        ESP_LOGE(TAG, "fail to malloc mem");
        return NULL; // 内存分配失败
    }
    
    int error;
    codec->dec_fs = dec_fs;
    codec->enc_fs = enc_fs;
    codec->duration_ms = duration_ms;
    codec->decoder = opus_decoder_create(16000, 1, &error);
    if (error != OPUS_OK) {
        free(codec);
        ESP_LOGE(TAG, "fail to opus decoder");
        return NULL; // 解码器创建失败
    }
    opus_decoder_ctl(codec->decoder, OPUS_RESET_STATE);
    codec->channels = channels;
    codec->max_data_bytes = 1500; // 根据需要调整最大数据字节数

    return codec;
}


int opus_codec_decode(OpusCodec* codec, const uint8_t* data, int data_size, int16_t* output) {
    int frame_size = opus_decode(codec->decoder, data, data_size, output, 320*3, 0);
    if(frame_size < 0) {
        ESP_LOGE(TAG, "Decode failed: %s", opus_strerror(frame_size));
        return -1;
    }
    return frame_size;
}


void opus_set_resample(OpusCodec* codec, int input_sample_rate, int output_sample_rate){
    int encode = input_sample_rate > output_sample_rate ? 1 : 0;
    int ret = silk_resampler_init(&codec->resampler, input_sample_rate, output_sample_rate, encode);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to initialize resampler");
        return;
    }
    codec->input_sample_rate = input_sample_rate;
    codec->out_sample_rate = output_sample_rate;
    ESP_LOGI(TAG, "Resampler configured with input sample rate %d and output sample rate %d", input_sample_rate, output_sample_rate);
}

int opus_resamples(OpusCodec* codec, const int16_t *input, int input_samples, int16_t *output) {
    int ret = silk_resampler(&codec->resampler, output, input, input_samples);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to process resampler");
    }
    return ret;
}