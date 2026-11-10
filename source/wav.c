#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define WAV_FILE_MAX (16u * 1024u * 1024u)
#define CWAV_INFO_SIZE 0x60u
#define CWAV_DATA_OFFSET 0xA0u
#define CWAV_MAX_RATE 22050u
#define CWAV_MIN_RATE 8000u

static uint16_t rd16(const uint8_t *p)
{
   return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
          (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

static int fail(char *err, size_t errlen, const char *message)
{
   if (err && errlen)
      snprintf(err, errlen, "%s", message);
   return -1;
}

static int32_t pcm16(const uint8_t *p)
{
   uint16_t value = rd16(p);
   return value & 0x8000 ? (int32_t)value - 0x10000 : value;
}

static int wav_to_cwav(const uint8_t *wav, size_t wav_size, uint8_t **out,
                       size_t *out_size, size_t max_cwav_size,
                       char *err, size_t errlen)
{
   size_t pos, data_off = 0, data_size = 0, samples, channel_size, total;
   size_t output_samples, max_samples;
   uint32_t sample_rate = 0;
   uint32_t output_rate;
   uint16_t channels = 0, bits = 0, format = 0, block_align = 0;
   uint8_t *cwav, *info, *data, *left, *right;
   size_t i;
   int ch;

   if (wav_size < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4))
      return fail(err, errlen, "The .banner.wav file is not a RIFF WAV.");

   pos = 12;
   while (pos + 8 <= wav_size)
   {
      uint32_t chunk_size = rd32(wav + pos + 4);
      size_t chunk_data = pos + 8;
      if ((size_t)chunk_size > wav_size - chunk_data)
         return fail(err, errlen, "The .banner.wav file has a truncated chunk.");
      if (!memcmp(wav + pos, "fmt ", 4))
      {
         if (chunk_size < 16)
            return fail(err, errlen, "The .banner.wav format chunk is invalid.");
         format = rd16(wav + chunk_data);
         channels = rd16(wav + chunk_data + 2);
         sample_rate = rd32(wav + chunk_data + 4);
         block_align = rd16(wav + chunk_data + 12);
         bits = rd16(wav + chunk_data + 14);
      }
      else if (!memcmp(wav + pos, "data", 4) && !data_size)
      {
         data_off = chunk_data;
         data_size = chunk_size;
      }
      pos = chunk_data + chunk_size + (chunk_size & 1);
   }
   if (format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
       !sample_rate || block_align != channels * 2)
      return fail(err, errlen, "Use uncompressed 16-bit mono or stereo PCM WAV audio.");
   if (!data_size || !block_align || data_size % block_align)
      return fail(err, errlen, "The .banner.wav file has no complete PCM samples.");

   samples = data_size / block_align;
   if (max_cwav_size < CWAV_DATA_OFFSET + 0x20 + 4)
      return fail(err, errlen, "The banner template has too little room for audio.");
   max_samples = (max_cwav_size - CWAV_DATA_OFFSET - 0x20) / 4;
   output_rate = sample_rate > CWAV_MAX_RATE ? CWAV_MAX_RATE : sample_rate;
   output_samples = (size_t)((uint64_t)samples * output_rate / sample_rate);
   if (!output_samples)
      output_samples = 1;
   if (output_samples > max_samples)
   {
      uint32_t min_rate = sample_rate < CWAV_MIN_RATE ? sample_rate : CWAV_MIN_RATE;
      uint64_t fit_rate = (uint64_t)max_samples * sample_rate / samples;
      if (fit_rate < min_rate)
         return fail(err, errlen, "The .banner.wav is too long to fit; use a shorter sound.");
      output_rate = fit_rate > output_rate ? output_rate : (uint32_t)fit_rate;
      output_samples = (size_t)((uint64_t)samples * output_rate / sample_rate);
      if (!output_samples)
         output_samples = 1;
   }
   channel_size = output_samples * 2;
   total = CWAV_DATA_OFFSET + 0x20 + channel_size * 2;
   if (total > max_cwav_size || total > UINT32_MAX)
      return fail(err, errlen, "The .banner.wav is too long to fit; use a shorter sound.");

   cwav = calloc(1, total);
   if (!cwav)
      return fail(err, errlen, "Not enough memory to convert .banner.wav.");

   memcpy(cwav, "CWAV", 4);
   wr16(cwav + 4, 0xFEFF);
   wr16(cwav + 6, 0x40);
   wr32(cwav + 8, 0x02010000);
   wr32(cwav + 12, (uint32_t)total);
   wr16(cwav + 16, 2);
   wr16(cwav + 20, 0x7000);
   wr32(cwav + 24, 0x40);
   wr32(cwav + 28, CWAV_INFO_SIZE);
   wr16(cwav + 32, 0x7001);
   wr32(cwav + 36, CWAV_DATA_OFFSET);
   wr32(cwav + 40, (uint32_t)(0x20 + channel_size * 2));

   info = cwav + 0x40;
   memcpy(info, "INFO", 4);
   wr32(info + 4, CWAV_INFO_SIZE);
   info[8] = 1;
   wr32(info + 12, output_rate);
   wr32(info + 20, (uint32_t)output_samples);
   wr32(info + 28, 2);
   wr16(info + 32, 0x7100);
   wr32(info + 36, 0x14);
   wr16(info + 40, 0x7100);
   wr32(info + 44, 0x28);
   for (i = 0; i < 2; i++)
   {
      uint8_t *channel = info + 48 + i * 20;
      wr16(channel, 0x1F00);
      wr32(channel + 4, (uint32_t)(0x18 + i * channel_size));
      wr32(channel + 12, UINT32_MAX);
   }

   data = cwav + CWAV_DATA_OFFSET;
   memcpy(data, "DATA", 4);
   wr32(data + 4, (uint32_t)(0x20 + channel_size * 2));
   left = data + 0x20;
   right = left + channel_size;
   for (i = 0; i < output_samples; i++)
   {
      for (ch = 0; ch < 2; ch++)
      {
         uint64_t start = (uint64_t)i * sample_rate;
         uint64_t end = (uint64_t)(i + 1) * sample_rate;
         size_t first = (size_t)(start / output_rate);
         size_t last = (size_t)((end + output_rate - 1) / output_rate);
         int64_t sum = 0;
         uint64_t weight = 0;
         size_t j;
         for (j = first; j < last && j < samples; j++)
         {
            uint64_t frame_start = (uint64_t)j * output_rate;
            uint64_t frame_end = frame_start + output_rate;
            uint64_t low = start > frame_start ? start : frame_start;
            uint64_t high = end < frame_end ? end : frame_end;
            const uint8_t *frame = wav + data_off + j * block_align;
            int32_t sample = pcm16(frame + (channels == 2 ? ch * 2 : 0));
            sum += (int64_t)sample * (int64_t)(high - low);
            weight += high - low;
         }
         wr16((ch ? right : left) + i * 2,
              (uint16_t)(sum / (int64_t)weight));
      }
   }

   *out = cwav;
   *out_size = total;
   return 0;
}

int wav_to_cwav_file(const char *path, size_t max_cwav_size,
                     uint8_t **cwav, size_t *cwav_size,
                     char *err, size_t errlen)
{
   FILE *f;
   long file_size;
   uint8_t *wav;
   int result;

   *cwav = NULL;
   *cwav_size = 0;
   f = fopen(path, "rb");
   if (!f)
   {
      if (errno == ENOENT)
         return 1;
      return fail(err, errlen, "Could not open the .banner.wav file.");
   }
   if (fseek(f, 0, SEEK_END) || (file_size = ftell(f)) <= 0 ||
       (unsigned long)file_size > WAV_FILE_MAX || fseek(f, 0, SEEK_SET))
   {
      fclose(f);
      return fail(err, errlen, "The .banner.wav file is empty or too large.");
   }
   wav = malloc((size_t)file_size);
   if (!wav)
   {
      fclose(f);
      return fail(err, errlen, "Not enough memory to read .banner.wav.");
   }
   if (fread(wav, 1, (size_t)file_size, f) != (size_t)file_size)
   {
      fclose(f);
      free(wav);
      return fail(err, errlen, "Could not read the .banner.wav file.");
   }
   fclose(f);
   result = wav_to_cwav(wav, (size_t)file_size, cwav, cwav_size,
                        max_cwav_size, err, errlen);
   free(wav);
   return result;
}
