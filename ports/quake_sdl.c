/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
/* SDL platform adapter derived from TyrQuake's vid_null.c. Compiled as guest code. */

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "keys.h"
#include "sys.h"
#include "client.h"
#include "common.h"
#include "d_local.h"
#include "r_local.h"
#include "quakedef.h"

#ifdef NQ_HACK
#include "host.h"
#endif

viddef_t vid;			// global video state

#define	BASEWIDTH	320
#define	BASEHEIGHT	200

byte vid_buffer[BASEWIDTH * BASEHEIGHT];
short zbuffer[BASEWIDTH * BASEHEIGHT];

unsigned short d_8to16table[256];
unsigned d_8to24table[256];

void
VID_GetDesktopRect(vrect_t *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->width = 0;
    rect->height = 0;
}

void
VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t colors[256], pixels[BASEWIDTH * BASEHEIGHT];
static unsigned frames;
static unsigned test_phase;
static double phase_time;
static qboolean mouse_grabbed;

static void test_key(SDL_Keycode key, int down)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.keysym.sym = key;
    if (SDL_PushEvent(&event) < 0) Sys_Error("SDL_PushEvent: %s", SDL_GetError());
}

static void capture_frame(const char *label)
{
    const char *prefix = getenv("R2_QUAKE_CAPTURE");
    if (prefix != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s-%s.argb", prefix, label);
        FILE *file = fopen(path, "wb");
        if (!file) Sys_Error("capture failed: %s", path);
        uint32_t header[] = {BASEWIDTH, BASEHEIGHT, frames};
        if (fwrite(header, sizeof(header), 1, file) != 1 || fwrite(pixels, sizeof(pixels), 1, file) != 1 || fclose(file) != 0)
            Sys_Error("capture write failed");
    }
    entity_t *entity = &cl_entities[cl.viewentity];
    printf("ROZETKA2_QUAKE state %s frame=%u time=%.6f x=%.6f y=%.6f yaw=%.6f ammo=%i\n",
           label, frames, cl.time, entity->origin[0], entity->origin[1], cl.viewangles[1], cl.stats[STAT_SHELLS]);
    fflush(stdout);
    printf("ROZETKA2_QUAKE viewmodel name=%s models=%i viewport_height=%i\n",
           cl.viewent.model ? cl.viewent.model->name : "none", r_amodels_drawn, r_refdef.vrect.height);
}
void
VID_SetPalette(const byte *palette)
{
    for (unsigned i = 0; i < 256; i++) colors[i] = (palette[i*3] << 16) | (palette[i*3+1] << 8) | palette[i*3+2];
}

void
VID_Init(const byte *palette)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) Sys_Error("SDL_Init: %s", SDL_GetError());
    window = SDL_CreateWindow("Quake x86_64 through rozetka2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600, SDL_WINDOW_SHOWN);
    if (!window) Sys_Error("SDL_CreateWindow: %s", SDL_GetError());
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) Sys_Error("SDL renderer: %s", SDL_GetError());
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, BASEWIDTH, BASEHEIGHT);
    if (!texture) Sys_Error("SDL texture: %s", SDL_GetError());
    VID_SetPalette(palette);
    printf("ROZETKA2_QUAKE initialized %ix%i\n", BASEWIDTH, BASEHEIGHT); fflush(stdout);
    vid.width = vid.conwidth = BASEWIDTH;
    vid.height = vid.conheight = BASEHEIGHT;
    vid.aspect = 1.0;
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = vid_buffer;
    vid.rowbytes = vid.conrowbytes = BASEWIDTH;

    d_pzbuffer = zbuffer;
    int cache_size = D_SurfaceCacheForRes(BASEWIDTH, BASEHEIGHT);
    byte *cache = Hunk_AllocName(cache_size, "surface cache");
    D_InitCaches(cache, cache_size);
}

void
VID_InitColormap(const byte *palette)
{
}

void
VID_Shutdown(void)
{
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
}

void
VID_Update(vrect_t *rects)
{
    (void)rects;
    for (unsigned i = 0; i < BASEWIDTH * BASEHEIGHT; i++) pixels[i] = colors[vid_buffer[i]];
    if (SDL_UpdateTexture(texture, NULL, pixels, BASEWIDTH * 4) != 0) Sys_Error("SDL texture update: %s", SDL_GetError());
    SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL); SDL_RenderPresent(renderer);
    frames++;
    if (getenv("R2_QUAKE_TEST_KEYS") && cls.signon == SIGNONS && cl.worldmodel != NULL && cl.stats[STAT_HEALTH] > 0) {
        if (test_phase == 0 && cl.time > 0.3) {
            capture_frame("before"); test_key(SDLK_UP, 1); phase_time = cl.time; test_phase = 1;
        } else if (test_phase == 1 && cl.time - phase_time >= 0.4) {
            test_key(SDLK_UP, 0); capture_frame("moved"); test_key(SDLK_RIGHT, 1); phase_time = cl.time; test_phase = 2;
        } else if (test_phase == 2 && cl.time - phase_time >= 0.4) {
            test_key(SDLK_RIGHT, 0); capture_frame("turned"); test_key(SDLK_LCTRL, 1); phase_time = cl.time; test_phase = 3;
        } else if (test_phase == 3 && cl.time - phase_time >= 0.5) {
            test_key(SDLK_LCTRL, 0); capture_frame("fired");
            SDL_Event event; memset(&event, 0, sizeof(event));
            event.type = SDL_MOUSEBUTTONDOWN; event.button.button = SDL_BUTTON_LEFT;
            SDL_PushEvent(&event); phase_time = cl.time; test_phase = 4;
        } else if (test_phase == 4 && cl.time - phase_time >= 0.1) {
            SDL_Event event; memset(&event, 0, sizeof(event));
            event.type = SDL_MOUSEMOTION; event.motion.xrel = 40; event.motion.yrel = 10;
            SDL_PushEvent(&event); phase_time = cl.time; test_phase = 5;
        } else if (test_phase == 5 && cl.time - phase_time >= 0.2) {
            capture_frame("mouse"); SDL_SetRelativeMouseMode(SDL_FALSE); Sys_Quit();
        }
    }
    const char *limit = getenv("R2_QUAKE_FRAMES");
    if (limit && frames >= (unsigned)atoi(limit)) {
        capture_frame("final");
        Sys_Quit();
    }
}

/*
================
D_BeginDirectRect
================
*/
void
D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void
D_EndDirectRect(int x, int y, int width, int height)
{
}

qboolean
VID_CheckAdequateMem(int width, int height)
{
    return true;
}

void
VID_ProcessEvents()
{
    if (key_dest != key_game && mouse_grabbed) { SDL_SetRelativeMouseMode(SDL_FALSE); mouse_grabbed = false; }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) Sys_Quit();
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            SDL_SetRelativeMouseMode(SDL_FALSE); mouse_grabbed = false; Key_ClearStates();
        }
        if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
            if (event.type == SDL_MOUSEBUTTONDOWN && key_dest == key_game && !mouse_grabbed)
                mouse_grabbed = SDL_SetRelativeMouseMode(SDL_TRUE) == 0;
            int key = event.button.button == SDL_BUTTON_LEFT ? K_MOUSE1 :
                      event.button.button == SDL_BUTTON_RIGHT ? K_MOUSE2 : K_MOUSE3;
            Key_Event(key, event.type == SDL_MOUSEBUTTONDOWN);
        }
        if (event.type == SDL_MOUSEMOTION && mouse_grabbed && key_dest == key_game) {
            cl.viewangles[1] -= event.motion.xrel * 0.15f;
            cl.viewangles[0] += event.motion.yrel * 0.15f;
            if (cl.viewangles[0] > 80) cl.viewangles[0] = 80;
            if (cl.viewangles[0] < -70) cl.viewangles[0] = -70;
        }
        if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) continue;
        int key = event.key.keysym.sym;
        switch (key) {
        case SDLK_UP: key=K_UPARROW; break; case SDLK_DOWN: key=K_DOWNARROW; break;
        case SDLK_LEFT: key=K_LEFTARROW; break; case SDLK_RIGHT: key=K_RIGHTARROW; break;
        case SDLK_LCTRL: case SDLK_RCTRL: key=K_CTRL; break;
        case SDLK_LSHIFT: case SDLK_RSHIFT: key=K_SHIFT; break;
        case SDLK_LALT: case SDLK_RALT: key=K_ALT; break;
        case SDLK_PAUSE: key=K_PAUSE; break;
        case SDLK_BACKSPACE: key=K_BACKSPACE; break;
        case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4: case SDLK_F5: case SDLK_F6:
        case SDLK_F7: case SDLK_F8: case SDLK_F9: case SDLK_F10: case SDLK_F11: case SDLK_F12:
            key = K_F1 + key - SDLK_F1; break;
        default: if (key < 0 || key >= 128) key=0; break;
        }
        if (key) Key_Event(key, event.type == SDL_KEYDOWN);
        if (key == K_ESCAPE && mouse_grabbed) { SDL_SetRelativeMouseMode(SDL_FALSE); mouse_grabbed = false; }
    }
}

void
VID_LockBuffer(void)
{
}

void
VID_UnlockBuffer(void)
{
}

void
VID_AddCommands()
{
}

void
VID_RegisterVariables()
{
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    return false;
}

void
VID_SetDefaultMode()
{
}

qboolean window_visible(void)
{
    return true;
}

#ifdef GLQUAKE
float gldepthmin, gldepthmax;
void *
GL_GetProcAddress(const char *name)
{
    return NULL;
}
#endif

#ifdef _WIN32
qboolean DDActive;
#endif

void Sys_SendKeyEvents(void) { VID_ProcessEvents(); }
