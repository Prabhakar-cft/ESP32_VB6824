#ifndef OPUS_CODEC_H
#define OPUS_CODEC_H

#include "opus.h"
#include "resampler_structs.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    silk_resampler_state_struct resampler;
    int input_sample_rate;
    int out_sample_rate;

    int enc_fs;
    int dec_fs;
    int duration_ms;
    int channels;
    int max_data_bytes;
} OpusCodec;

/*!
 * Initialize/reset the resampler state for a given pair of input/output sampling rates
*/
opus_int silk_resampler_init(
    silk_resampler_state_struct *S,                 /* I/O  Resampler state                                             */
    opus_int32                  Fs_Hz_in,           /* I    Input sampling rate (Hz)                                    */
    opus_int32                  Fs_Hz_out,          /* I    Output sampling rate (Hz)                                   */
    opus_int                    forEnc              /* I    If 1: encoder; if 0: decoder                                */
);

/*!
 * Resampler: convert from one sampling rate to another
 */
opus_int silk_resampler(
    silk_resampler_state_struct *S,                 /* I/O  Resampler state                                             */
    opus_int16                  out[],              /* O    Output signal                                               */
    const opus_int16            in[],               /* I    Input signal                                                */
    opus_int32                  inLen               /* I    Number of input samples                                     */
);

// extern opus_int silk_resampler_init(
//     silk_resampler_state_struct *S,                 /* I/O  Resampler state                                             */
//     opus_int32                  Fs_Hz_in,           /* I    Input sampling rate (Hz)                                    */
//     opus_int32                  Fs_Hz_out,          /* I    Output sampling rate (Hz)                                   */
//     opus_int                    forEnc              /* I    If 1: encoder; if 0: decoder                                */
// );

// /*!
//  * Resampler: convert from one sampling rate to another
//  */
// extern opus_int silk_resampler(
//     silk_resampler_state_struct *S,                 /* I/O  Resampler state                                             */
//     opus_int16                  out[],              /* O    Output signal                                               */
//     const opus_int16            in[],               /* I    Input signal                                                */
//     opus_int32                  inLen               /* I    Number of input samples                                     */
// );

// 初始化 Opus 编解码器
OpusCodec* opus_codec_init(int enc_fs, int dec_fs, int duration_ms, int channels);

// 编码函数，输入为浮点格式音频数据
uint8_t* opus_codec_encode(OpusCodec* codec, const int16_t* input, int *output_size);

// 解码函数，输出为PCM音频数据
int opus_codec_decode(OpusCodec* codec, const uint8_t* data, int data_size, int16_t* output);

// 释放资源
// void opus_codec_free(OpusCodec* codec);

// void opus_encoder_reset(OpusCodec *codec);
// void opus_decodec_reset(OpusCodec *codec);

// void opus_decoder_set_sample_rate(OpusCodec *codec, int dec_fs);

void opus_set_resample(OpusCodec* codec, int input_sample_rate, int output_sample_rate);
// int opus_get_output_samples(OpusCodec* codec, int input_samples);
int opus_resamples(OpusCodec* codec, const int16_t *input, int input_samples, int16_t *output);
#endif // OPUS_CODEC_
