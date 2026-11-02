#ifndef EK_AUDIO_H
#define EK_AUDIO_H
#include <stdint.h>

#define EK_SAMPLE_RATE 16000

#ifdef __cplusplus
extern "C" {
#endif

void ek_audio_init(void);
int  ek_audio_read(void *buf, uint32_t len);   /* stereo 16-bit frames */

#ifdef __cplusplus
}
#endif
#endif
