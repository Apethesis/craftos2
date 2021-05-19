/*
 * terminal/HardwareSDLTerminal.cpp
 * CraftOS-PC 2
 * 
 * This file implements the HardwareSDLTerminal class.
 * 
 * This code is licensed under the MIT license.
 * Copyright (c) 2019-2021 JackMacWindows.
 */

#include <configuration.hpp>
#include "HardwareSDLTerminal.hpp"
#include "RawTerminal.hpp"
#include "../gif.hpp"
#include "../main.hpp"
#include "../runtime.hpp"
#include "../termsupport.hpp"
#ifndef NO_PNG
#include <png++/png.hpp>
#endif
#define rgb(color) (((color).r << 16) | ((color).g << 8) | (color).b)

extern "C" {
    struct font_image {
        unsigned int 	 width;
        unsigned int 	 height;
        unsigned int 	 bytes_per_pixel; /* 2:RGB16, 3:RGB, 4:RGBA */
        unsigned char	 pixel_data[128 * 175 * 2 + 1];
    };
    extern struct font_image font_image;
#ifdef __EMSCRIPTEN__
    extern void syncfs();
#endif
}

#ifdef __APPLE__
extern float getBackingScaleFactor(SDL_Window *win);
#endif
#ifdef __EMSCRIPTEN__
SDL_Renderer *HardwareSDLTerminal::ren = NULL;
SDL_Texture *HardwareSDLTerminal::font = NULL;
SDL_Texture *HardwareSDLTerminal::pixtex = NULL;
#endif

void MySDL_GetDisplayDPI(int displayIndex, float* dpi, float* defaultDpi)
{
    const float kSysDefaultDpi =
#ifdef __APPLE__
        144.0f;
#elif defined(_WIN32)
        96.0f;
#else
        96.0f;
#endif
 
    if (SDL_GetDisplayDPI(displayIndex, NULL, dpi, NULL) != 0)
    {
        // Failed to get DPI, so just return the default value.
        if (dpi) *dpi = kSysDefaultDpi;
    }
 
    if (defaultDpi) *defaultDpi = kSysDefaultDpi;
}

HardwareSDLTerminal::HardwareSDLTerminal(std::string title): SDLTerminal(title) {
#ifdef __EMSCRIPTEN__
    if (ren == NULL) {
#endif
    std::lock_guard<std::mutex> lock(locked); // try to prevent race condition (see explanation in render())
    float dpi, defaultDpi;
#ifdef __APPLE__
    dpi = getBackingScaleFactor(win), defaultDpi = 1.0;
#else
    MySDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(win), &dpi, &defaultDpi);
#endif
    dpiScale = (dpi / defaultDpi) - floor(dpi / defaultDpi) > 0.5f ? (int)ceil(dpi / defaultDpi) : (int)floor(dpi / defaultDpi);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (config.useVsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (ren == (SDL_Renderer*)0) {
        SDL_DestroyWindow(win);
        throw window_exception("Failed to create renderer: " + std::string(SDL_GetError()));
    }
    SDL_RendererInfo info;
    SDL_GetRendererInfo(ren, &info);
    if ((!overrideHardwareDriver.empty() && std::string(info.name) != overrideHardwareDriver) || 
        (overrideHardwareDriver.empty() && !config.preferredHardwareDriver.empty() && std::string(info.name) != config.preferredHardwareDriver))
        fprintf(stderr, "Warning: Preferred driver %s not available, using %s instead.\n", (overrideHardwareDriver.empty() ? config.preferredHardwareDriver.c_str() : overrideHardwareDriver.c_str()), info.name);
    if (std::string(info.name) == "software") dpiScale = 1;
    font = SDL_CreateTextureFromSurface(ren, bmp);
    if (font == (SDL_Texture*)0) {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        throw window_exception("Failed to load texture from font: " + std::string(SDL_GetError()));
    }
    pixtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, (int)(width * charWidth * dpiScale), (int)(height * charHeight * dpiScale));
    if (pixtex == (SDL_Texture*)0) {
        SDL_DestroyTexture(font);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        throw window_exception("Failed to create texture for pixels: " + std::string(SDL_GetError()));
    }
#ifdef __APPLE__
    // macOS has some weird scaling bug on non-Retina displays that this fixes for some reason?
    SDL_SetWindowSize(win, realWidth, realHeight);
#endif
#ifdef __EMSCRIPTEN__
    }
#endif
}

HardwareSDLTerminal::~HardwareSDLTerminal() {
#ifndef __EMSCRIPTEN__
    if (!overridden) {
        SDL_DestroyTexture(pixtex);
        SDL_DestroyTexture(font);
        SDL_DestroyRenderer(ren);
    }
#endif
}

extern bool operator!=(Color lhs, Color rhs);

bool HardwareSDLTerminal::drawChar(unsigned char c, int x, int y, Color fg, Color bg, bool transparent) {
    SDL_Rect srcrect = getCharacterRect(c);
    SDL_Rect destrect = {
        (int)(x * charWidth * dpiScale + 2 * charScale * dpiScale), 
        (int)(y * charHeight * dpiScale + 2 * charScale * dpiScale), 
        (int)(fontWidth * charScale * dpiScale), 
        (int)(fontHeight * charScale * dpiScale)
    };
    SDL_Rect bgdestrect = destrect;
    if (config.standardsMode || config.extendMargins) {
        if (x == 0) bgdestrect.x -= (int)(2 * charScale * dpiScale);
        if (y == 0) bgdestrect.y -= (int)(2 * charScale * dpiScale);
        if (x == 0 || (unsigned)x == width - 1) bgdestrect.w += (int)(2 * charScale * dpiScale);
        if (y == 0 || (unsigned)y == height - 1) bgdestrect.h += (int)(2 * charScale * dpiScale);
        if ((unsigned)x == width - 1) bgdestrect.w += realWidth - (int)(width*charWidth*dpiScale+(4 * charScale * dpiScale));
        if ((unsigned)y == height - 1) bgdestrect.h += realHeight - (int)(height*charHeight*dpiScale+(4 * charScale * dpiScale));
    }
    if (!transparent && bg != palette[15]) {
        if (gotResizeEvent) return false;
        bg = grayscalify(bg);
        if (SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 0xFF) != 0) return false;
        if (gotResizeEvent) return false;
        if (SDL_RenderFillRect(ren, &bgdestrect) != 0) return false;
    }
    if (c != ' ' && c != '\0') {
        if (gotResizeEvent) return false;
        fg = grayscalify(fg);
        if (SDL_SetTextureColorMod(font, fg.r, fg.g, fg.b) != 0) return false;
        if (gotResizeEvent) return false;
        if (SDL_RenderCopy(ren, font, &srcrect, &destrect) != 0) return false;
    }
    return true;
}

extern SDL_Rect * setRect(SDL_Rect * rect, int x, int y, int w, int h);

static unsigned char circlePix[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

void HardwareSDLTerminal::render() {
#ifdef __EMSCRIPTEN__
    if (*renderTarget != this) return;
#endif
    // copy the screen data so we can let Lua keep going without waiting for the mutex
    std::unique_ptr<vector2d<unsigned char> > newscreen;
    std::unique_ptr<vector2d<unsigned char> > newcolors;
    std::unique_ptr<vector2d<unsigned char> > newpixels;
    Color newpalette[256];
    unsigned newwidth, newheight, newcharWidth, newcharHeight, newfontScale, newcharScale;
    int newblinkX, newblinkY, newmode;
    bool newblink, newuseOrigFont;
    unsigned char newcursorColor;
    {
        std::lock_guard<std::mutex> locked_g(locked);
        if (ren == NULL || font == NULL) return; // race condition since HardwareSDLTerminal() is called after SDLTerminal(), which adds the terminal to the render targets
                                                 // wait until the renderer and font are initialized before doing any rendering
        if (gotResizeEvent) {
            if (newWidth > 0 && newHeight > 0) {
                this->screen.resize(newWidth, newHeight, ' ');
                this->colors.resize(newWidth, newHeight, 0xF0);
                this->pixels.resize(newWidth * fontWidth, newHeight * fontHeight, 0x0F);
                changed = true;
            } else changed = false;
            this->width = newWidth;
            this->height = newHeight;
            gotResizeEvent = false;
        }
        if ((!changed && !shouldScreenshot && !shouldRecord) || width == 0 || height == 0) return;
        newscreen = std::make_unique<vector2d<unsigned char> >(screen);
        newcolors = std::make_unique<vector2d<unsigned char> >(colors);
        newpixels = std::make_unique<vector2d<unsigned char> >(pixels);
        memcpy(newpalette, palette, sizeof(newpalette));
        newblinkX = blinkX; newblinkY = blinkY; newmode = mode;
        newblink = blink; newuseOrigFont = useOrigFont;
        newwidth = width; newheight = height; newcharWidth = charWidth; newcharHeight = charHeight; newfontScale = fontScale; newcharScale = charScale;
        newcursorColor = cursorColor;
        changed = false;
    }
    std::lock_guard<std::mutex> rlock(renderlock);
    Color bgcolor = newmode == 0 ? newpalette[15] : defaultPalette[15];
    if (SDL_SetRenderDrawColor(ren, bgcolor.r, bgcolor.g, bgcolor.b, 0xFF) != 0) return;
    if (SDL_RenderClear(ren) != 0) return;
    SDL_Rect rect;
    if (newmode != 0) {
        void * pixels = NULL;
        int pitch = 0;
        SDL_LockTexture(pixtex, setRect(&rect, 0, 0, (int)(newwidth * newcharWidth * dpiScale), (int)(newheight * newcharHeight * dpiScale)), &pixels, &pitch);
        SDL_Surface * surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels, (int)(newwidth * newcharWidth * dpiScale), (int)(newheight * newcharHeight * dpiScale), 24, pitch, SDL_PIXELFORMAT_RGB888);
        for (unsigned y = 0; y < newheight * newcharHeight * dpiScale; y+=newcharScale*dpiScale) {
            for (unsigned x = 0; x < newwidth * newcharWidth * dpiScale; x+=newcharScale*dpiScale) {
                unsigned char c = (*newpixels)[y / newcharScale / dpiScale][x / newcharScale / dpiScale];
                if (gotResizeEvent) return;
                if (SDL_FillRect(surf, setRect(&rect, (int)x, (int)y, (int)(newcharScale * dpiScale), (int)(newcharScale * dpiScale)), rgb(newpalette[(int)c])) != 0) return;
            }
        }
        SDL_UnlockTexture(pixtex);
        SDL_RenderCopy(ren, pixtex, NULL, setRect(&rect, (int)(2 * newcharScale * dpiScale), (int)(2 * newcharScale * dpiScale), (int)(newwidth * newcharWidth * dpiScale), (int)(newheight * newcharHeight * dpiScale)));
    } else {
        for (unsigned y = 0; y < newheight; y++) {
            for (unsigned x = 0; x < newwidth; x++) {
                if (gotResizeEvent) return;
                if (!drawChar((*newscreen)[y][x], (int)x, (int)y, newpalette[(*newcolors)[y][x] & 0x0F], newpalette[(*newcolors)[y][x] >> 4])) return;
            }
        }
        if (gotResizeEvent) return;
        if (newblink && newblinkX >= 0 && newblinkY >= 0 && (unsigned)newblinkX < newwidth && (unsigned)newblinkY < newheight) if (!drawChar('_', newblinkX, newblinkY, newpalette[newcursorColor], newpalette[(*newcolors)[newblinkY][newblinkX] >> 4], true)) return;
    }
    currentFPS++;
    if (lastSecond != time(0)) {
        lastSecond = (int)time(0);
        lastFPS = currentFPS;
        currentFPS = 0;
    }
    if (config.showFPS) {
        // later
    }
    if (shouldScreenshot) {
        shouldScreenshot = false;
        int w, h;
        if (gotResizeEvent) return;
        if (SDL_GetRendererOutputSize(ren, &w, &h) != 0) return;
#ifdef PNGPP_PNG_HPP_INCLUDED
        if (screenshotPath == WS("clipboard")) {
            SDL_Surface * temp = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
            if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, temp->pixels, temp->pitch) != 0) return;
            copyImage(temp);
            SDL_FreeSurface(temp);
        } else {
            png::solid_pixel_buffer<png::rgb_pixel> pixbuf(w, h);
            if (gotResizeEvent) return;
            if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, (void*)&pixbuf.get_bytes()[0], w * 3) != 0) return;
            png::image<png::rgb_pixel, png::solid_pixel_buffer<png::rgb_pixel> > img(w, h);
            img.set_pixbuf(pixbuf);
            std::ofstream out(screenshotPath, std::ios::binary);
            img.write_stream(out);
            out.close();
        }
#else
        SDL_Surface *sshot = SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (gotResizeEvent) return;
        if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sshot->pixels, sshot->pitch) != 0) return;
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(sshot, SDL_PIXELFORMAT_RGB888, 0);
        SDL_FreeSurface(sshot);
        SDL_SaveBMP(conv, screenshotPath.c_str());
        SDL_FreeSurface(conv);
#endif
#ifdef __EMSCRIPTEN__
        queueTask([](void*)->void* {syncfs(); return NULL; }, NULL, true);
#endif
    }
    if (shouldRecord) {
        if (recordedFrames >= config.maxRecordingTime * config.recordingFPS) stopRecording();
        else if (--frameWait < 1) {
            std::lock_guard<std::mutex> lock(recorderMutex);
            int w, h;
            if (SDL_GetRendererOutputSize(ren, &w, &h) != 0) return;
            SDL_Surface *sshot = SDL_CreateRGBSurface(0, w, h, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
            if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ABGR8888, sshot->pixels, sshot->pitch) != 0) return;
            uint32_t uw = static_cast<uint32_t>(w), uh = static_cast<uint32_t>(h);
            std::string rle = std::string((char*)&uw, 4) + std::string((char*)&uh, 4);
            uint32_t * px = ((uint32_t*)sshot->pixels);
            uint32_t data = px[0] & 0xFFFFFF;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    uint32_t p = px[y*w+x];
                    if ((p & 0xFFFFFF) != (data & 0xFFFFFF) || (data & 0xFF000000) == 0xFF000000) {
                        rle += std::string((char*)&data, 4);
                        data = p & 0xFFFFFF;
                    } else data += 0x1000000;
                }
            }
            rle += std::string((char*)&data, 4);
            SDL_FreeSurface(sshot);
            recording.push_back(rle);
            recordedFrames++;
            frameWait = config.clockSpeed / config.recordingFPS;
            if (gotResizeEvent) return;
        }
        SDL_Surface* circle = SDL_CreateRGBSurfaceWithFormatFrom(circlePix, 10, 10, 32, 40, SDL_PIXELFORMAT_BGRA32);
        if (circle == NULL) { fprintf(stderr, "Error creating circle: %s\n", SDL_GetError()); }
        SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, circle);
        if (SDL_RenderCopy(ren, tex, NULL, setRect(&rect, (int)(width * charWidth * dpiScale + 2 * charScale * (newuseOrigFont ? 2 : newfontScale) * dpiScale) - 10, (int)(2 * charScale * fontScale * dpiScale), 10, 10)) != 0) return;
        SDL_FreeSurface(circle);
    }
}

bool HardwareSDLTerminal::resize(unsigned w, unsigned h) {
    {
        std::lock_guard<std::mutex> lock(locked);
        float dpi, defaultDpi;
#ifdef __APPLE__
        dpi = getBackingScaleFactor(win), defaultDpi = 1.0;
#else
        MySDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(win), &dpi, &defaultDpi);
#endif
        dpiScale = (dpi / defaultDpi) - floor(dpi / defaultDpi) > 0.5f ? (int)ceil(dpi / defaultDpi) : (int)floor(dpi / defaultDpi);
        newWidth = w;
        newHeight = h;
        // not really a fan of having two tasks queued here, but there's not a whole lot we can do
        if (config.snapToSize && !fullscreen && !(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) queueTask([this, w, h](void*)->void*{SDL_SetWindowSize((SDL_Window*)win, (int)(w*charWidth+(4 * charScale * dpiScale)), (int)(h*charHeight+(4 * charScale * dpiScale))); return NULL;}, NULL);
        {
            std::lock_guard<std::mutex> lock2(renderlock);
            SDL_GetWindowSize(win, &realWidth, &realHeight);
            gotResizeEvent = (newWidth != width || newHeight != height);
            if (!gotResizeEvent) return false;
#ifdef __APPLE__
        }
        ren = (SDL_Renderer*)queueTask([this](void*win)->void*{
            SDL_DestroyRenderer(ren);
            return SDL_CreateRenderer((SDL_Window*)win, -1, SDL_RENDERER_ACCELERATED | (config.useVsync ? SDL_RENDERER_PRESENTVSYNC : 0));
        }, win);
#else
            SDL_DestroyRenderer(ren);
        }
        ren = (SDL_Renderer*)queueTask([](void*win)->void*{return SDL_CreateRenderer((SDL_Window*)win, -1, SDL_RENDERER_ACCELERATED | (config.useVsync ? SDL_RENDERER_PRESENTVSYNC : 0));}, win);
#endif
        font = SDL_CreateTextureFromSurface(ren, useOrigFont ? origfont : bmp);
        pixtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, (int)(w * charWidth * dpiScale), (int)(h * charHeight * dpiScale));
#ifdef __APPLE__
        queueTask([this](void*)->void*{SDL_SetWindowSize(win, realWidth, realHeight); return NULL;}, NULL);
#endif
    }
    while (gotResizeEvent) std::this_thread::yield();
    return true;
}

void HardwareSDLTerminal::init() {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
#ifdef __EMSCRIPTEN__
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");
#endif
#if SDL_VERSION_ATLEAST(2, 0, 8)
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (!overrideHardwareDriver.empty()) SDL_SetHint(SDL_HINT_RENDER_DRIVER, overrideHardwareDriver.c_str());
    else if (!config.preferredHardwareDriver.empty()) SDL_SetHint(SDL_HINT_RENDER_DRIVER, config.preferredHardwareDriver.c_str());
    SDL_StartTextInput();
    task_event_type = SDL_RegisterEvents(2);
    render_event_type = task_event_type + 1;
    renderThread = new std::thread(termRenderLoop);
    setThreadName(*renderThread, "Render Thread");
    SDL_Surface* old_bmp;
    std::string bmp_path = "built-in file";
#ifndef STANDALONE_ROM
    if (config.customFontPath == "hdfont") bmp_path = astr(getROMPath() + WS("/hdfont.bmp"));
    else 
#endif
    if (!config.customFontPath.empty())
        bmp_path = config.customFontPath;
    if (config.customFontPath.empty()) 
        old_bmp = SDL_CreateRGBSurfaceWithFormatFrom((void*)font_image.pixel_data, (int)font_image.width, (int)font_image.height, (int)font_image.bytes_per_pixel * 8, (int)font_image.bytes_per_pixel * (int)font_image.width, SDL_PIXELFORMAT_RGB565);
    else old_bmp = SDL_LoadBMP(bmp_path.c_str());
    if (old_bmp == (SDL_Surface*)0) {
        throw std::runtime_error("Failed to load font: " + std::string(SDL_GetError()));
    }
    bmp = SDL_ConvertSurfaceFormat(old_bmp, SDL_PIXELFORMAT_RGBA32, 0);
    if (bmp == (SDL_Surface*)0) {
        SDL_FreeSurface(old_bmp);
        throw std::runtime_error("Failed to convert font: " + std::string(SDL_GetError()));
    }
    SDL_FreeSurface(old_bmp);
    SDL_SetColorKey(bmp, SDL_TRUE, SDL_MapRGB(bmp->format, 0, 0, 0));
    if (config.customFontPath.empty()) origfont = bmp;
    else {
        old_bmp = SDL_CreateRGBSurfaceWithFormatFrom((void*)font_image.pixel_data, (int)font_image.width, (int)font_image.height, (int)font_image.bytes_per_pixel * 8, (int)font_image.bytes_per_pixel * (int)font_image.width, SDL_PIXELFORMAT_RGB565);
        origfont = SDL_ConvertSurfaceFormat(old_bmp, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(old_bmp);
        SDL_SetColorKey(origfont, SDL_TRUE, SDL_MapRGB(origfont->format, 0, 0, 0));
    }
}

void HardwareSDLTerminal::quit() {
    renderThread->join();
    delete renderThread;
    SDL_FreeSurface(bmp);
    if (bmp != origfont) SDL_FreeSurface(origfont);
    SDL_Quit();
}

#ifdef __EMSCRIPTEN__
#define checkWindowID(c, wid) (c->term == *renderTarget || findMonitorFromWindowID(c, (*renderTarget)->id, tmps) != NULL)
#else
#define checkWindowID(c, wid) ((wid) == ( c)->term->id || findMonitorFromWindowID((c), (wid), tmps) != NULL)
#endif

bool HardwareSDLTerminal::pollEvents() {
    SDL_Event e;
    std::string tmps;
#ifdef __EMSCRIPTEN__
    if (SDL_PollEvent(&e)) {
#else
    if (SDL_WaitEvent(&e)) {
#endif
        if (e.type == task_event_type) {
            while (!taskQueue->empty()) {
                auto v = taskQueue->front();
                try {
                    void* retval = std::get<1>(v)(std::get<2>(v));
                    if (!std::get<3>(v)) {
                        LockGuard lock2(taskQueueReturns);
                        (*taskQueueReturns)[std::get<0>(v)] = retval;
                    }
                } catch (...) {
                    if (!std::get<3>(v)) {
                        LockGuard lock2(taskQueueReturns);
                        LockGuard lock3(taskQueueExceptions);
                        (*taskQueueReturns)[std::get<0>(v)] = NULL;
                        (*taskQueueExceptions)[std::get<0>(v)] = std::current_exception();
                    }
                }
                taskQueue->pop();
            }
        } else if (e.type == render_event_type) {
#ifdef __EMSCRIPTEN__
            std::lock_guard<std::mutex> lock(((SDLTerminal*)*renderTarget)->renderlock);
            SDL_RenderPresent(HardwareSDLTerminal::ren);
            SDL_UpdateWindowSurface(SDLTerminal::win);
#else
            for (Terminal* term : renderTargets) {
                HardwareSDLTerminal * sdlterm = dynamic_cast<HardwareSDLTerminal*>(term);
                if (sdlterm != NULL) {
                    std::lock_guard<std::mutex> lock(sdlterm->renderlock);
                    if (sdlterm->gotResizeEvent || sdlterm->width == 0 || sdlterm->height == 0) continue;
                    SDL_RenderPresent(sdlterm->ren);
                    SDL_UpdateWindowSurface(sdlterm->win);
                }
            }
#endif
        } else {
            if (rawClient) {
                sendRawEvent(e);
            } else if (!computers.locked()) {
                LockGuard lock(computers);
                bool stop = false;
                if (eventHandlers.find((SDL_EventType)e.type) != eventHandlers.end()) {
                    Computer * comp = NULL;
                    Terminal * term = NULL;
                    for (Computer * c : *computers) {
                        if (c->term->id == lastWindow) {
                            comp = c;
                            term = c->term;
                            break;
                        } else {
                            monitor * m = findMonitorFromWindowID(c, lastWindow, tmps);
                            if (m != NULL) {
                                comp = c;
                                term = m->term;
                                break;
                            }
                        }
                    }
                    for (const auto& h : Range<std::unordered_multimap<SDL_EventType, std::pair<sdl_event_handler, void*> >::iterator>(eventHandlers.equal_range((SDL_EventType)e.type))) {
                        stop = h.second.first(&e, comp, term, h.second.second) || stop;
                    }
                }
                if (!stop) {
                    for (Computer * c : *computers) {
                        if (((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && checkWindowID(c, e.key.windowID)) ||
                            ((e.type == SDL_DROPFILE || e.type == SDL_DROPTEXT || e.type == SDL_DROPBEGIN || e.type == SDL_DROPCOMPLETE) && checkWindowID(c, e.drop.windowID)) ||
                            ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && checkWindowID(c, e.button.windowID)) ||
                            (e.type == SDL_MOUSEMOTION && checkWindowID(c, e.motion.windowID)) ||
                            (e.type == SDL_MOUSEWHEEL && checkWindowID(c, e.wheel.windowID)) ||
                            (e.type == SDL_TEXTINPUT && checkWindowID(c, e.text.windowID)) ||
                            (e.type == SDL_WINDOWEVENT && checkWindowID(c, e.window.windowID)) ||
                            e.type == SDL_QUIT) {
                            c->termEventQueue.push(e);
                            c->event_lock.notify_all();
                            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == c->term->id) {
                                if (c->requestedExit && c->L) {
                                    SDL_MessageBoxData msg;
                                    msg.flags = SDL_MESSAGEBOX_INFORMATION;
                                    msg.title = "Computer Unresponsive";
                                    msg.message = "The computer appears to be unresponsive. Would you like to force the computer to shut down? All unsaved data will be lost.";
                                    SDL_MessageBoxButtonData buttons[2] = {
                                        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Cancel"},
                                        {0, 1, "Shut Down"}
                                    };
                                    msg.buttons = buttons;
                                    msg.numbuttons = 2;
                                    msg.window = ((SDLTerminal*)c->term)->win;
                                    int id = 0;
                                    SDL_ShowMessageBox(&msg, &id);
                                    if (id == 1 && c->L) {
                                        // Forcefully halt the Lua state
                                        c->running = 0;
                                        lua_halt(c->L);
                                    }
                                } else c->requestedExit = true;
                            }
                        }
                    }
                }
                if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) lastWindow = e.window.windowID;
                for (Terminal * t : orphanedTerminals) {
                    if ((e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == t->id) || e.type == SDL_QUIT) {
                        orphanedTerminals.erase(t);
                        delete t;
                        break;
                    }
                }
            }
            if (e.type == SDL_QUIT) return true;
        }
    }
    return false;
}
