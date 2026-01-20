#include <iostream>
#include <iomanip>
#include <vector>

#pragma comment(lib, "SDL2.lib")
#pragma comment(lib, "SDL2main.lib")

#include "SDL.h"
#include "minzx.h"
#include "filemgr.h"

bool isLittleEndian()
{
    uint16_t val = 0x0001;
    return *(uint8_t*)&val == 0x01;
}

int main(int argc, char* argv[])
{
    std::cout << (isLittleEndian() ? "Little endian" : "Big endian") << " machine\n";

    MinZX zx;
    zx.init();

    FileMgr fm;
    if (argc > 1) fm.loadSNA(argv[1], &zx);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    SDL_Window* window = SDL_CreateWindow("MinZX SDL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        320, 240, SDL_WINDOW_SHOWN);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = nullptr;

    if (SDL_OpenAudio(&want, &have) < 0)
        std::cerr << "Audio error: " << SDL_GetError() << "\n";
    SDL_PauseAudio(0);

    const int TEX_W = 320;
    const int TEX_H = 240;
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, TEX_W, TEX_H);

    std::vector<uint8_t> pixels(TEX_W * TEX_H * 4, 0);

    bool running = true;
    SDL_Event ev;

    uint32_t frames = 0;
    uint64_t start = SDL_GetPerformanceCounter();

    while (running)
    {
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
                running = false;

            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
            {
                bool press = (ev.type == SDL_KEYDOWN);
                int row = -1, bit = -1;

                switch (ev.key.keysym.sym)
                {
                case SDLK_z:      row = 0; bit = 1; break;
                case SDLK_x:      row = 0; bit = 2; break;
                case SDLK_c:      row = 0; bit = 3; break;
                case SDLK_v:      row = 0; bit = 4; break;
                case SDLK_a:      row = 1; bit = 0; break;
                case SDLK_s:      row = 1; bit = 1; break;
                case SDLK_d:      row = 1; bit = 2; break;
                case SDLK_f:      row = 1; bit = 3; break;
                case SDLK_g:      row = 1; bit = 4; break;
                case SDLK_q:      row = 2; bit = 0; break;
                case SDLK_w:      row = 2; bit = 1; break;
                case SDLK_e:      row = 2; bit = 2; break;
                case SDLK_r:      row = 2; bit = 3; break;
                case SDLK_t:      row = 2; bit = 4; break;
                case SDLK_1:      row = 3; bit = 0; break;
                case SDLK_2:      row = 3; bit = 1; break;
                case SDLK_3:      row = 3; bit = 2; break;
                case SDLK_4:      row = 3; bit = 3; break;
                case SDLK_5:      row = 3; bit = 4; break;
                case SDLK_0:      row = 4; bit = 0; break;
                case SDLK_9:      row = 4; bit = 1; break;
                case SDLK_8:      row = 4; bit = 2; break;
                case SDLK_7:      row = 4; bit = 3; break;
                case SDLK_6:      row = 4; bit = 4; break;
                case SDLK_p:      row = 5; bit = 0; break;
                case SDLK_o:      row = 5; bit = 1; break;
                case SDLK_i:      row = 5; bit = 2; break;
                case SDLK_u:      row = 5; bit = 3; break;
                case SDLK_y:      row = 5; bit = 4; break;
                case SDLK_RETURN: row = 6; bit = 0; break;
                case SDLK_l:      row = 6; bit = 1; break;
                case SDLK_k:      row = 6; bit = 2; break;
                case SDLK_j:      row = 6; bit = 3; break;
                case SDLK_h:      row = 6; bit = 4; break;
                case SDLK_SPACE:  row = 7; bit = 0; break;
                case SDLK_LCTRL: case SDLK_RCTRL:
                case SDLK_LALT:  case SDLK_RALT:  row = 7; bit = 1; break; // Symbol
                case SDLK_LSHIFT: case SDLK_RSHIFT: row = 0; bit = 0; break; // Caps
                }

                if (row >= 0 && bit >= 0)
                    zx.keyPress(row, bit, press);
            }
        }

        zx.update(pixels.data());

        const auto& abuf = zx.getAudioBuffer();
        if (!abuf.empty())
        {
            SDL_QueueAudio(1, abuf.data(), static_cast<uint32_t>(abuf.size() * sizeof(int16_t)));
            zx.clearAudioBuffer();
        }

        SDL_UpdateTexture(texture, nullptr, pixels.data(), TEX_W * 4);

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        SDL_Delay(20);

        frames++;
        uint64_t now = SDL_GetPerformanceCounter();
        double sec = static_cast<double>(now - start) / SDL_GetPerformanceFrequency();
        if (sec > 2.0)
        {
            printf("%.1f FPS   %.1f ms/frame\n", frames / sec, sec * 1000 / frames);
            start = now;
            frames = 0;
        }
    }

    SDL_CloseAudio();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    zx.destroy();
    return 0;
}