/* DoomGeneric platform adapter. The game and this adapter are x86_64;
 * only the declared SDL/libc API calls cross to host libraries. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "p_local.h"
#include "i_system.h"
#include "doom_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static unsigned frame_count, frame_limit;
static unsigned test_phase;
static int phase_tic;
static int running = 1, scripted;
static const char *capture;
static unsigned short keys[128];
static unsigned read_key, write_key;

static void fail(const char *operation)
{
    fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    exit(1);
}

static unsigned char doom_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RETURN: return KEY_ENTER;
    case SDLK_ESCAPE: return KEY_ESCAPE;
    case SDLK_PAUSE: return KEY_PAUSE;
    case SDLK_LEFT: return KEY_LEFTARROW;
    case SDLK_RIGHT: return KEY_RIGHTARROW;
    case SDLK_UP: return KEY_UPARROW;
    case SDLK_DOWN: return KEY_DOWNARROW;
    case SDLK_LCTRL: case SDLK_RCTRL: return KEY_FIRE;
    case SDLK_SPACE: return KEY_USE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return KEY_RSHIFT;
    case SDLK_LALT: case SDLK_RALT: return KEY_LALT;
    case SDLK_F1: return KEY_F1;
    case SDLK_F2: return KEY_F2;
    case SDLK_F3: return KEY_F3;
    case SDLK_F4: return KEY_F4;
    case SDLK_F5: return KEY_F5;
    case SDLK_F6: return KEY_F6;
    case SDLK_F7: return KEY_F7;
    case SDLK_F8: return KEY_F8;
    case SDLK_F9: return KEY_F9;
    case SDLK_F10: return KEY_F10;
    case SDLK_F11: return KEY_F11;
    case SDLK_F12: return KEY_F12;
    default: return key >= 0 && key < 128 ? (unsigned char)key : 0;
    }
}

void DG_Init(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) fail("SDL_Init");
    window = SDL_CreateWindow("Doom x86_64 through rozetka2", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, 960, 600, SDL_WINDOW_SHOWN);
    if (window == NULL) fail("SDL_CreateWindow");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer == NULL) fail("SDL_CreateRenderer");
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING,
                                DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    if (texture == NULL) fail("SDL_CreateTexture");
    const char *limit = getenv("R2_DOOM_FRAMES");
    frame_limit = limit != NULL ? (unsigned)atoi(limit) : 0;
    scripted = getenv("R2_DOOM_TEST_KEYS") != NULL;
    capture = getenv("R2_DOOM_CAPTURE");
    printf("ROZETKA2_DOOM initialized %ux%u\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    fflush(stdout);
}

static void push_test_key(SDL_Keycode key, int down)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = key;
    SDL_PushEvent(&event);
}

static void capture_frame(const char *label)
{
    if (capture == NULL) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s-%s.argb", capture, label);
    FILE *file = fopen(path, "wb");
    if (file == NULL) { perror(path); exit(1); }
    uint32_t header[3] = {DOOMGENERIC_RESX, DOOMGENERIC_RESY, frame_count};
    if (fwrite(header, sizeof(header), 1, file) != 1 ||
        fwrite(DG_ScreenBuffer, sizeof(pixel_t), DOOMGENERIC_RESX * DOOMGENERIC_RESY, file)
            != DOOMGENERIC_RESX * DOOMGENERIC_RESY || fclose(file) != 0) {
        fprintf(stderr, "cannot capture Doom frame\n"); exit(1);
    }
    player_t *player = &players[consoleplayer];
    printf("ROZETKA2_DOOM state %s frame=%u tic=%i x=%i y=%i angle=%u ammo=%i\n",
           label, frame_count, leveltime, player->mo != NULL ? player->mo->x : 0,
           player->mo != NULL ? player->mo->y : 0, player->mo != NULL ? player->mo->angle : 0,
           player->ammo[am_clip]);
    fflush(stdout);
}

void DG_DrawFrame(void)
{
    R2_DoomAudioPump();
    if (SDL_UpdateTexture(texture, NULL, DG_ScreenBuffer, DOOMGENERIC_RESX * sizeof(pixel_t)) != 0)
        fail("SDL_UpdateTexture");
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    frame_count += 1;
    if (scripted && gamestate == GS_LEVEL && players[consoleplayer].mo != NULL && leveltime >= 3) {
        if (test_phase == 0) {
            capture_frame("before");
            push_test_key(SDLK_UP, 1);
            phase_tic = leveltime;
            test_phase = 1;
        } else if (test_phase == 1 && leveltime - phase_tic >= 12) {
            push_test_key(SDLK_UP, 0);
            capture_frame("moved");
            push_test_key(SDLK_RIGHT, 1);
            phase_tic = leveltime;
            test_phase = 2;
        } else if (test_phase == 2 && leveltime - phase_tic >= 12) {
            push_test_key(SDLK_RIGHT, 0);
            capture_frame("turned");
            push_test_key(SDLK_LCTRL, 1);
            phase_tic = leveltime;
            test_phase = 3;
        } else if (test_phase == 3 && leveltime - phase_tic >= 8) {
            push_test_key(SDLK_LCTRL, 0);
            capture_frame("fired");
            test_phase = 4;
            running = 0;
        }
    }
    if (frame_limit != 0 && frame_count == frame_limit) capture_frame("final");
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) running = 0;
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            unsigned char key = doom_key(event.key.keysym.sym);
            if (key != 0 && write_key - read_key < 128) {
                keys[write_key++ & 127u] = (unsigned short)(key | ((event.type == SDL_KEYDOWN) << 8));
                printf("ROZETKA2_DOOM key %u %s\n", key, event.type == SDL_KEYDOWN ? "down" : "up");
            }
        }
    }
    if (frame_limit != 0 && frame_count >= frame_limit) running = 0;
}

void DG_SleepMs(uint32_t milliseconds) { SDL_Delay(milliseconds); }
uint32_t DG_GetTicksMs(void) { return SDL_GetTicks(); }
int DG_GetKey(int *pressed, unsigned char *key)
{
    if (read_key == write_key) return 0;
    unsigned short value = keys[read_key++ & 127u];
    *pressed = value >> 8;
    *key = (unsigned char)value;
    return 1;
}
void DG_SetWindowTitle(const char *title)
{
    if (window != NULL) SDL_SetWindowTitle(window, title);
}
int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    while (running) doomgeneric_Tick();
    I_Quit();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("ROZETKA2_DOOM completed %u frames\n", frame_count);
    return 0;
}
