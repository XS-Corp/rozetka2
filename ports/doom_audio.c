/* SPDX-License-Identifier: GPL-2.0-or-later
 * DoomGeneric platform audio. This file runs as x86_64 guest code.
 * DMX effects are mixed here; SDL only consumes queued PCM. MIDI uses the
 * system synthesizer through declared AudioToolbox C APIs, without callbacks.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_audio.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "memio.h"
#include "mus2mid.h"

#define RATE 22050
#define CHANNELS 16
#define BLOCK 256
#define TARGET_FRAMES 1536

int use_libsamplerate;
float libsamplerate_scale = 1.0f;

typedef struct sample_s {
    struct sample_s *next;
    sfxinfo_t *owner;
    unsigned length, step;
    byte data[];
} sample_t;
typedef struct {
    sample_t *sample;
    uint64_t position;
    int left, right;
} channel_t;
static SDL_AudioDeviceID device;
static channel_t channels[CHANNELS];
static sample_t *samples;
static boolean prefix;
static FILE *capture;
static uint32_t captured_bytes;
static unsigned starts, nonzero_samples;
static uint64_t queued_bytes;
static MusicPlayer player;
static MusicSequence sequence;
static AudioUnit music_output;
static int music_volume = 64;
static boolean music_paused;

static void audio_error(const char *operation)
{
    fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    exit(1);
}

static void wav_header(FILE *file, uint32_t bytes)
{
    /* The supported guest/host pair is little endian. */
    uint32_t size = bytes + 36, rate = RATE, byte_rate = RATE * 4;
    uint16_t pcm = 1, stereo = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, file); fwrite(&size, 4, 1, file); fwrite("WAVEfmt ", 1, 8, file);
    size = 16; fwrite(&size, 4, 1, file); fwrite(&pcm, 2, 1, file); fwrite(&stereo, 2, 1, file);
    fwrite(&rate, 4, 1, file); fwrite(&byte_rate, 4, 1, file);
    fwrite(&align, 2, 1, file); fwrite(&bits, 2, 1, file);
    fwrite("data", 1, 4, file); fwrite(&bytes, 4, 1, file);
}

static boolean init_sfx(boolean use_prefix)
{
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) audio_error("SDL audio init");
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = RATE; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 512;
    device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (device == 0) audio_error("SDL_OpenAudioDevice");
    prefix = use_prefix;
    const char *path = getenv("R2_DOOM_AUDIO_CAPTURE");
    if (path != NULL) {
        capture = fopen(path, "wb");
        if (capture == NULL) { perror(path); exit(1); }
        wav_header(capture, 0);
    }
    SDL_PauseAudioDevice(device, 0);
    printf("ROZETKA2_DOOM audio initialized rate=%i channels=2 driver=%s\n", RATE, SDL_GetCurrentAudioDriver());
    return true;
}

void R2_DoomAudioPump(void)
{
    if (device == 0) return;
    unsigned queued = SDL_GetQueuedAudioSize(device);
    while (queued < TARGET_FRAMES * 4) {
        int32_t mix[BLOCK * 2] = {0};
        int16_t pcm[BLOCK * 2];
        for (unsigned c = 0; c < CHANNELS; c++) {
            channel_t *ch = &channels[c];
            sample_t *sample = ch->sample;
            if (sample == NULL) continue;
            for (unsigned i = 0; i < BLOCK; i++) {
                unsigned pos = (unsigned)(ch->position >> 16);
                if (pos >= sample->length) { ch->sample = NULL; break; }
                int value = ((int)sample->data[pos] - 128) * 256;
                if (pos + 1 < sample->length)
                    value += ((int)sample->data[pos + 1] - sample->data[pos]) * (int)(ch->position & 65535) / 256;
                mix[i * 2] += value * ch->left / 32385;
                mix[i * 2 + 1] += value * ch->right / 32385;
                ch->position += sample->step;
            }
        }
        for (unsigned i = 0; i < BLOCK * 2; i++) {
            int value = mix[i];
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            pcm[i] = (int16_t)value;
            if (value != 0) nonzero_samples++;
        }
        if (SDL_QueueAudio(device, pcm, sizeof(pcm)) != 0) audio_error("SDL_QueueAudio");
        queued += sizeof(pcm); queued_bytes += sizeof(pcm);
        if (capture != NULL) {
            if (captured_bytes > UINT32_MAX - 36u - sizeof(pcm) ||
                fwrite(pcm, 1, sizeof(pcm), capture) != sizeof(pcm)) {
                fprintf(stderr, "cannot capture Doom audio\n"); exit(1);
            }
            captured_bytes += sizeof(pcm);
        }
    }
}

static void shutdown_sfx(void)
{
    if (device == 0) return;
    unsigned queued = SDL_GetQueuedAudioSize(device);
    printf("ROZETKA2_DOOM audio stats starts=%u nonzero=%u queued=%llu consumed=%llu playing=%i\n",
           starts, nonzero_samples, (unsigned long long)queued_bytes,
           (unsigned long long)(queued_bytes - queued), SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING);
    SDL_CloseAudioDevice(device); device = 0;
    if (capture != NULL) {
        if (fseek(capture, 0, SEEK_SET) != 0) { perror("audio capture seek"); exit(1); }
        wav_header(capture, captured_bytes);
        if (fclose(capture) != 0) { perror("audio capture close"); exit(1); }
        capture = NULL;
    }
    while (samples != NULL) {
        sample_t *next = samples->next;
        samples->owner->driver_data = NULL;
        free(samples); samples = next;
    }
    memset(channels, 0, sizeof(channels));
}

static int lump_number(sfxinfo_t *sfx)
{
    char name[12];
    if (sfx->link != NULL) sfx = sfx->link;
    snprintf(name, sizeof(name), "%s%.8s", prefix ? "ds" : "", sfx->name);
    return W_GetNumForName(name);
}

static void update_params(int channel, int volume, int separation)
{
    if (channel < 0 || channel >= CHANNELS) return;
    channels[channel].left = volume * (254 - separation);
    channels[channel].right = volume * separation;
}

static int start_sfx(sfxinfo_t *sfx, int channel, int volume, int separation)
{
    if (device == 0 || channel < 0 || channel >= CHANNELS) return -1;
    sample_t *sample = sfx->driver_data;
    if (sample == NULL) {
        int lump = sfx->lumpnum >= 0 ? sfx->lumpnum : lump_number(sfx);
        int size = W_LumpLength(lump);
        if (size < 8) return -1;
        byte *data = W_CacheLumpNum(lump, PU_STATIC);
        unsigned rate = data[2] | ((unsigned)data[3] << 8);
        unsigned length = data[4] | ((unsigned)data[5] << 8) | ((unsigned)data[6] << 16) | ((unsigned)data[7] << 24);
        if (data[0] != 3 || data[1] != 0 || rate == 0 || length > (unsigned)size - 8 || length <= 48) {
            W_ReleaseLumpNum(lump); return -1;
        }
        /* Match DMX's leading/trailing 16-sample padding convention. */
        length -= 32;
        sample = malloc(sizeof(*sample) + length);
        if (sample == NULL) { W_ReleaseLumpNum(lump); return -1; }
        sample->length = length; sample->step = (rate << 16) / RATE;
        memcpy(sample->data, data + 24, length);
        W_ReleaseLumpNum(lump);
        sample->next = samples; samples = sample; sfx->driver_data = sample;
        sample->owner = sfx;
    }
    channels[channel].sample = sample; channels[channel].position = 0;
    update_params(channel, volume, separation);
    starts++;
    if (getenv("R2_DOOM_AUDIO_CAPTURE") != NULL)
        printf("ROZETKA2_DOOM audio start name=%.8s channel=%i volume=%i separation=%i\n", sfx->name, channel, volume, separation);
    return channel;
}
static void stop_sfx(int channel) { if (channel >= 0 && channel < CHANNELS) channels[channel].sample = NULL; }
static boolean playing_sfx(int channel) { return channel >= 0 && channel < CHANNELS && channels[channel].sample != NULL; }

static snddevice_t devices[] = {SNDDEVICE_SB};
sound_module_t DG_sound_module = {devices, 1, init_sfx, shutdown_sfx, lump_number,
    R2_DoomAudioPump, update_params, start_sfx, stop_sfx, playing_sfx, NULL};

/* MusicTimeStamp and float parameters below are copied, never calculated by
 * unsupported guest floating point instructions. */
static boolean init_music(void) { return true; }
static void check_music(OSStatus status, const char *operation)
{
    if (status != noErr) { fprintf(stderr, "%s failed: %i\n", operation, (int)status); exit(1); }
}
static void music_gain(int volume)
{
    static const float gains[] = {0.0f, 0.062992126f, 0.125984252f, 0.188976378f,
        0.251968504f, 0.314960630f, 0.377952756f, 0.440944882f, 0.503937008f,
        0.566929134f, 0.629921260f, 0.692913386f, 0.755905512f, 0.818897638f,
        0.881889764f, 0.944881890f, 1.0f};
    music_volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    if (music_output != NULL)
        check_music(AudioUnitSetParameter(music_output, kHALOutputParam_Volume,
                    kAudioUnitScope_Global, 0, gains[(music_volume + 4) / 8], 0), "music volume");
}
static void stop_music(void)
{
    if (player == NULL) return;
    check_music(MusicPlayerStop(player), "MusicPlayerStop");
    check_music(DisposeMusicPlayer(player), "DisposeMusicPlayer");
    player = NULL; music_output = NULL; music_paused = false;
}
static void shutdown_music(void)
{
    stop_music();
    if (sequence != NULL) { check_music(DisposeMusicSequence(sequence), "DisposeMusicSequence"); sequence = NULL; }
}
static void pause_music(void) { if (player != NULL) { check_music(MusicPlayerStop(player), "music pause"); music_paused = true; } }
static void resume_music(void) { if (player != NULL && music_paused) { check_music(MusicPlayerStart(player), "music resume"); music_paused = false; } }
static void *register_song(void *data, int length)
{
    if (length < 4) return NULL;
    const char *format = "MIDI";
    MEMFILE *converted = NULL;
    if (memcmp(data, "MUS\x1a", 4) == 0) {
        format = "MUS";
        MEMFILE *input = mem_fopen_read(data, length);
        converted = mem_fopen_write();
        if (mus2mid(input, converted)) { mem_fclose(input); mem_fclose(converted); return NULL; }
        mem_fclose(input);
        size_t size; mem_get_buf(converted, &data, &size); length = (int)size;
    }
    MusicSequence song = NULL;
    if (memcmp(data, "MThd", 4) == 0) {
        CFDataRef midi = CFDataCreate(NULL, data, length);
        if (midi != NULL) {
            check_music(NewMusicSequence(&song), "NewMusicSequence");
            OSStatus status = MusicSequenceFileLoadData(song, midi, kMusicSequenceFile_MIDIType, 0);
            CFRelease(midi);
            if (status != noErr) { DisposeMusicSequence(song); song = NULL; }
        }
    }
    if (converted != NULL) mem_fclose(converted);
    if (song != NULL) printf("ROZETKA2_DOOM music registered format=%s\n", format);
    return song;
}
static void unregister_song(void *song)
{
    if (song == sequence) { stop_music(); sequence = NULL; }
    if (song != NULL) check_music(DisposeMusicSequence(song), "DisposeMusicSequence");
}
static void play_song(void *song, boolean looping)
{
    stop_music();
    if (song == NULL) return;
    sequence = song;
    UInt32 tracks = 0;
    check_music(MusicSequenceGetTrackCount(sequence, &tracks), "music track count");
    uint64_t max_length = 0;
    for (UInt32 i = 0; i < tracks; i++) {
        MusicTrack track; uint64_t length = 0; UInt32 size = sizeof(length);
        check_music(MusicSequenceGetIndTrack(sequence, i, &track), "music track");
        check_music(MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength, &length, &size), "music length");
        if (length > max_length) max_length = length; /* Nonnegative IEEE double bits. */
    }
    for (UInt32 i = 0; i < tracks; i++) {
        MusicTrack track; MusicTrackLoopInfo loop;
        memset(&loop, 0, sizeof(loop));
        if (looping) memcpy(&loop.loopDuration, &max_length, sizeof(max_length));
        check_music(MusicSequenceGetIndTrack(sequence, i, &track), "music track");
        check_music(MusicTrackSetProperty(track, kSequenceTrackProperty_TrackLength, &max_length, sizeof(max_length)), "music track length");
        check_music(MusicTrackSetProperty(track, kSequenceTrackProperty_LoopInfo, &loop, sizeof(loop)), "music loop");
    }
    check_music(NewMusicPlayer(&player), "NewMusicPlayer");
    check_music(MusicPlayerSetSequence(player, sequence), "MusicPlayerSetSequence");
    check_music(MusicPlayerSetTime(player, 0), "MusicPlayerSetTime");
    check_music(MusicPlayerPreroll(player), "MusicPlayerPreroll");
    AUGraph graph = NULL; UInt32 nodes = 0;
    check_music(MusicSequenceGetAUGraph(sequence, &graph), "music graph");
    check_music(AUGraphGetNodeCount(graph, &nodes), "music graph nodes");
    for (UInt32 i = 0; i < nodes; i++) {
        AUNode node; AudioComponentDescription description; AudioUnit unit;
        check_music(AUGraphGetIndNode(graph, i, &node), "music graph node");
        check_music(AUGraphNodeInfo(graph, node, &description, &unit), "music graph info");
        if (description.componentType == kAudioUnitType_Output) music_output = unit;
    }
    music_gain(music_volume);
    check_music(MusicPlayerStart(player), "MusicPlayerStart");
    printf("ROZETKA2_DOOM music started tracks=%u looping=%i\n", (unsigned)tracks, looping);
}
static boolean playing_music(void)
{
    Boolean playing = false;
    if (player != NULL) check_music(MusicPlayerIsPlaying(player, &playing), "music status");
    return playing;
}
static void poll_music(void)
{
    if (player == NULL || getenv("R2_DOOM_AUDIO_CAPTURE") == NULL) return;
    static boolean reported;
    MusicTimeStamp time; uint64_t bits;
    check_music(MusicPlayerGetTime(player, &time), "music time"); memcpy(&bits, &time, 8);
    if (!reported && bits != 0 && playing_music()) {
        AUGraph graph; Boolean running = false;
        check_music(MusicSequenceGetAUGraph(sequence, &graph), "music graph");
        check_music(AUGraphIsRunning(graph, &running), "music graph status");
        if (running) { printf("ROZETKA2_DOOM music running advancing=1\n"); reported = true; }
    }
}
music_module_t DG_music_module = {devices, 1, init_music, shutdown_music, music_gain,
    pause_music, resume_music, register_song, unregister_song, play_song, stop_music, playing_music, poll_music};
