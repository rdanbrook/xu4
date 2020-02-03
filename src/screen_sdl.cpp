/*
 * $Id: screen_sdl.cpp 3087 2015-01-24 03:44:46Z darren_janeczek $
 */


#include <algorithm>
#include <functional>
#include <vector>
#include <map>
#include <SDL.h>

#include "config.h"
#include "context.h"

#include "cursors.h"

#include "dungeonview.h"
#include "error.h"
#include "event.h"
#include "image.h"
#include "imagemgr.h"
#include "intro.h"
#include "savegame.h"
#include "settings.h"
#include "scale.h"
#include "screen.h"
#include "tileanim.h"
#include "tileset.h"
#include "u4.h"
#include "u4_sdl.h"
#include "u4file.h"
#include "utils.h"

using std::vector;

SDL_Cursor *cursors[5];
Scaler filterScaler;

SDL_Cursor *screenInitCursor(const char * const xpm[]);

extern bool verbose;


void screenRefreshThreadInit();
void screenRefreshThreadEnd();

void screenInit_sys() {
    /* start SDL */
    if (u4_SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
        xu4_error(XU4_LOG_ERR, "unable to init SDL: %s", SDL_GetError());    
    SDL_EnableUNICODE(1);
    SDL_SetGamma(settings.gamma / 100.0f, settings.gamma / 100.0f, settings.gamma / 100.0f);
    atexit(SDL_Quit);

    SDL_WM_SetCaption("Ultima IV", NULL);

    if (!SDL_SetVideoMode(320 * settings.scale, 200 * settings.scale, 0, SDL_HWSURFACE | SDL_ANYFORMAT | (settings.fullscreen ? SDL_FULLSCREEN : 0)))
        xu4_error(XU4_LOG_ERR, "unable to set video: %s", SDL_GetError());

    if (verbose) {
        char driver[32];
        printf("screen initialized [screenInit()], using %s video driver\n", SDL_VideoDriverName(driver, sizeof(driver)));
    }

    /* enable or disable the mouse cursor */
    if (settings.mouseEnabled) {
        SDL_ShowCursor(SDL_ENABLE);
        cursors[0] = SDL_GetCursor();
        cursors[1] = screenInitCursor(w_xpm);
        cursors[2] = screenInitCursor(n_xpm);
        cursors[3] = screenInitCursor(e_xpm);
        cursors[4] = screenInitCursor(s_xpm);
    } else {
        SDL_ShowCursor(SDL_DISABLE);
    }

    filterScaler = scalerGet("point");

    screenRefreshThreadInit();
}

void screenDelete_sys() {
	screenRefreshThreadEnd();
    SDL_FreeCursor(cursors[1]);
    SDL_FreeCursor(cursors[2]);
    SDL_FreeCursor(cursors[3]);
    SDL_FreeCursor(cursors[4]);
    u4_SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/**
 * Attempts to iconify the screen.
 */
void screenIconify() {
    SDL_WM_IconifyWindow();
}

/**
 * Force a redraw.
 */

SDL_mutex *screenLockMutex = NULL;
int frameDuration = 0;

void screenLock() {
	SDL_mutexP(screenLockMutex);
}

void screenUnlock() {
	SDL_mutexV(screenLockMutex);
}

void screenRedrawScreen() {
	screenLock();
    SDL_UpdateRect(SDL_GetVideoSurface(), 0, 0, 0, 0);
    screenUnlock();
}

void screenRedrawTextArea(int x, int y, int width, int height) {
	screenLock();
	SDL_UpdateRect(SDL_GetVideoSurface(), x * CHAR_WIDTH * settings.scale, y * CHAR_HEIGHT * settings.scale, width * CHAR_WIDTH * settings.scale, height * CHAR_HEIGHT * settings.scale);
	screenUnlock();
}

void screenWait(int numberOfAnimationFrames) {
	SDL_Delay(numberOfAnimationFrames * frameDuration);
}

bool continueScreenRefresh = true;
SDL_Thread *screenRefreshThread = NULL;

int screenRefreshThreadFunction(void *unused) {

	while (continueScreenRefresh) {
		SDL_Delay(frameDuration);
		screenRedrawScreen();
	}
	return 0;
}

void screenRefreshThreadInit() {
	screenLockMutex = SDL_CreateMutex();;

	frameDuration = 1000 / settings.screenAnimationFramesPerSecond;

	continueScreenRefresh = true;
	if (screenRefreshThread) {
		xu4_error(XU4_LOG_WRN, "Screen refresh thread already exists.");
		return;
	}

	screenRefreshThread = SDL_CreateThread(screenRefreshThreadFunction, NULL);
	if (!screenRefreshThread) {
		xu4_error(XU4_LOG_WRN, SDL_GetError());
		return;
	}
}

void screenRefreshThreadEnd() {
	continueScreenRefresh = false;
	SDL_WaitThread(screenRefreshThread, NULL);
	screenRefreshThread = NULL;
}


/**
 * Scale an image up.  The resulting image will be scale * the
 * original dimensions.  The original image is no longer deleted.
 * n is the number of tiles in the image; each tile is filtered 
 * seperately. filter determines whether or not to filter the 
 * resulting image.
 */
Image *screenScale(Image *src, int scale, int n, int filter) {
    Image *dest = NULL;
	bool isTransparent;
	unsigned int transparentIndex;
	bool alpha = src->isAlphaOn();

	if (n == 0)
		n = 1;

	isTransparent = src->getTransparentIndex(transparentIndex);
	src->alphaOff();

	while (filter && filterScaler && (scale % 2 == 0)) {
		dest = (*filterScaler)(src, 2, n);
		src = dest;
		scale /= 2;
	}

	if (scale != 1)
		dest = (*scalerGet("point"))(src, scale, n);

	if (!dest)
		dest = Image::duplicate(src);

	if (isTransparent)
		dest->setTransparentIndex(transparentIndex);

	if (alpha)
		src->alphaOn();




    return dest;
}

/**
 * Scale an image down.  The resulting image will be 1/scale * the
 * original dimensions.  The original image is no longer deleted.
 */
Image *screenScaleDown(Image *src, int scale) {
    int x, y;
    Image *dest;
    bool isTransparent;
    unsigned int transparentIndex;
    bool alpha = src->isAlphaOn();

    isTransparent = src->getTransparentIndex(transparentIndex);

    src->alphaOff();

    dest = Image::create(src->width() / scale, src->height() / scale, src->isIndexed(), Image::HARDWARE);
    if (!dest) {
        return NULL;
    }

	if (!dest) {
		dest = Image::duplicate(src);
    }

    if (dest->isIndexed()) {
        dest->setPaletteFromImage(src);
    }

    for (y = 0; y < src->height(); y+=scale) {
        for (x = 0; x < src->width(); x+=scale) {
            unsigned int index;
            src->getPixelIndex(x, y, index);                
            dest->putPixelIndex(x / scale, y / scale, index);
        }
    }    

    if (isTransparent)
        dest->setTransparentIndex(transparentIndex);

    if (alpha)
        src->alphaOn();

    return dest;
}

/**
 * Create an SDL cursor object from an xpm.  Derived from example in
 * SDL documentation project.
 */
#define CURSORSIZE 32
SDL_Cursor *screenInitCursor(const char * const xpm[]) {
    int i, row, col;
    Uint8 data[(CURSORSIZE/8)*CURSORSIZE];
    Uint8 mask[(CURSORSIZE/8)*CURSORSIZE];
    int hot_x, hot_y;

    i = -1;
    for (row=0; row < CURSORSIZE; row++) {
        for (col=0; col < CURSORSIZE; col++) {
            if (col % 8) {
                data[i] <<= 1;
                mask[i] <<= 1;
            } else {
                i++;
                data[i] = mask[i] = 0;
            }
            switch (xpm[4+row][col]) {
            case 'X':
                data[i] |= 0x01;
                mask[i] |= 0x01;
                break;
            case '.':
                mask[i] |= 0x01;
                break;
            case ' ':
                break;
            }
        }
    }
    sscanf(xpm[4+row], "%d,%d", &hot_x, &hot_y);
    return SDL_CreateCursor(data, mask, CURSORSIZE, CURSORSIZE, hot_x, hot_y);
}

void screenSetMouseCursor(MouseCursor cursor) {
    static int current = 0;

    if (cursor != current) {
        SDL_SetCursor(cursors[cursor]);
        current = cursor;
    }
}
