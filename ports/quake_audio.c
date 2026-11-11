/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest Quake DMA ring -> SDL queued playback; no native-to-guest callbacks. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "sound.h"
#include "sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_AudioDeviceID device;
static dma_t dma;
static unsigned submitted;
static uint64_t total_bytes, nonzero_samples;
static FILE *capture;
static uint32_t captured_bytes;

static void wav_header(FILE *file, uint32_t bytes)
{
    uint32_t size = bytes + 36, rate = 22050, byte_rate = 22050 * 4;
    uint16_t pcm = 1, stereo = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, file); fwrite(&size, 4, 1, file); fwrite("WAVEfmt ", 1, 8, file);
    size = 16; fwrite(&size, 4, 1, file); fwrite(&pcm, 2, 1, file); fwrite(&stereo, 2, 1, file);
    fwrite(&rate, 4, 1, file); fwrite(&byte_rate, 4, 1, file); fwrite(&align, 2, 1, file); fwrite(&bits, 2, 1, file);
    fwrite("data", 1, 4, file); fwrite(&bytes, 4, 1, file);
}

qboolean SNDDMA_Init(void)
{
    SDL_AudioSpec want = {0};
    want.freq = 22050; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 512;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!device) return false;
    memset(&dma, 0, sizeof(dma));
    dma.speed = 22050; dma.channels = 2; dma.samplebits = 16; dma.samples = 32768; dma.submission_chunk = 1;
    dma.buffer = Hunk_AllocName(dma.samples * 2, "audio ring");
    shm = &dma;
    submitted = 0;
    const char *path = getenv("R2_QUAKE_AUDIO_CAPTURE");
    if (path != NULL) {
        capture = fopen(path, "wb");
        if (!capture) Sys_Error("cannot open audio capture");
        wav_header(capture, 0);
    }
    SDL_PauseAudioDevice(device, 0);
    printf("ROZETKA2_QUAKE audio initialized driver=%s\n", SDL_GetCurrentAudioDriver());
    return true;
}
int SNDDMA_GetDMAPos(void)
{
    if (!device) return 0;
    unsigned queued = SDL_GetQueuedAudioSize(device) / 4;
    unsigned consumed = submitted >= queued ? submitted - queued : 0;
    return (consumed * 2) & (dma.samples - 1);
}
void SNDDMA_Submit(void)
{
    if (!device) return;
    while (submitted < (unsigned)paintedtime) {
        unsigned position = submitted & (dma.samples / 2 - 1);
        unsigned count = (unsigned)paintedtime - submitted;
        if (count > (unsigned)dma.samples / 2 - position) count = dma.samples / 2 - position;
        if (SDL_QueueAudio(device, dma.buffer + position * 4, count * 4) != 0) Sys_Error("SDL_QueueAudio: %s", SDL_GetError());
        const int16_t *pcm = (const int16_t *)(dma.buffer + position * 4);
        for (unsigned i = 0; i < count * 2; i++) if (pcm[i] != 0) nonzero_samples++;
        total_bytes += count * 4;
        if (capture != NULL) {
            if (captured_bytes > UINT32_MAX - 36u - count * 4 || fwrite(pcm, 4, count, capture) != count) Sys_Error("audio capture write failed");
            captured_bytes += count * 4;
        }
        submitted += count;
    }
}
void SNDDMA_Shutdown(void)
{
    if (device) {
        printf("ROZETKA2_QUAKE audio stats nonzero=%llu queued=%llu consumed=%llu playing=%i\n",
            (unsigned long long)nonzero_samples, (unsigned long long)total_bytes,
            (unsigned long long)(total_bytes - SDL_GetQueuedAudioSize(device)), SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING);
        SDL_CloseAudioDevice(device);
    }
    device = 0;
    if (capture) {
        if (fseek(capture, 0, SEEK_SET) != 0) Sys_Error("audio capture seek failed");
        wav_header(capture, captured_bytes);
        if (fclose(capture) != 0) Sys_Error("audio capture close failed");
        capture = NULL;
    }
}
int SNDDMA_LockBuffer(void) { return 0; }
void SNDDMA_UnlockBuffer(void) { }
void S_BlockSound(void) { snd_blocked++; if (device) SDL_PauseAudioDevice(device, 1); }
void S_UnblockSound(void) { if (snd_blocked) snd_blocked--; if (device && !snd_blocked) SDL_PauseAudioDevice(device, 0); }
