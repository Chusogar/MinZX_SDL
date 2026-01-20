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
	uint16_t val16 = 0x1;
	uint8_t* ptr8 = (uint8_t*)(&val16);
	return (*ptr8 == 1);
}

int main(int argc, char *argv[])
{
	if (isLittleEndian()) std::cout << "Running on little endian machine" << std::endl;
	else                  std::cout << "Running on big endian machine"    << std::endl;

	MinZX minZX;
	minZX.init();

	FileMgr fileMgr;

	if (argc > 1)
		fileMgr.loadSNA(argv[1], &minZX);

	SDL_Init(SDL_INIT_EVERYTHING);

	unsigned vsyncFlag = 0;
//	unsigned vsyncFlag = SDL_RENDERER_PRESENTVSYNC;

	SDL_Window* window = SDL_CreateWindow
		(
		"SDL2",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
//		960, 720,
//		1280, 960,
//		2560, 1920,
		320, 240,
		SDL_WINDOW_SHOWN
		);

	SDL_Renderer* renderer = SDL_CreateRenderer
		(
		window,
		-1,
		SDL_RENDERER_ACCELERATED | vsyncFlag
		);

	SDL_RendererInfo info;
	SDL_GetRendererInfo(renderer, &info);
	std::cout << "Renderer name: " << info.name << std::endl;
	std::cout << "Texture formats: " << std::endl;
	for (Uint32 i = 0; i < info.num_texture_formats; i++)
	{
		std::cout << SDL_GetPixelFormatName(info.texture_formats[i]) << std::endl;
	}

	const unsigned int texWidth = 320;
	const unsigned int texHeight = 240;
	SDL_Texture* texture = SDL_CreateTexture
		(
		renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING,
		texWidth, texHeight
		);

	std::vector< unsigned char > pixels(texWidth * texHeight * 4, 0);

	SDL_Event event;
	bool running = true;
	bool useLocktexture = false;

	unsigned int frames = 0;
	Uint64 start = SDL_GetPerformanceCounter();
	
	while (running)
	{

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
		SDL_RenderClear(renderer);

		while (SDL_PollEvent(&event))
		{
			if ((SDL_QUIT == event.type) ||
				(SDL_KEYDOWN == event.type && SDL_SCANCODE_ESCAPE == event.key.keysym.scancode))
			{
				running = false;
				break;
			}
			if (SDL_KEYDOWN == event.type && SDL_SCANCODE_L == event.key.keysym.scancode)
			{
				useLocktexture = !useLocktexture;
				std::cout << "Using " << (useLocktexture ? "SDL_LockTexture() + memcpy()" : "SDL_UpdateTexture()") << std::endl;
			}
			if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
			{
				bool press = (event.type == SDL_KEYDOWN);
				int row = -1;
				int bit = -1;
				switch (event.key.keysym.sym)
				{
				case SDLK_a: row = 1; bit = 0; break;
				case SDLK_b: row = 7; bit = 4; break;
				case SDLK_c: row = 0; bit = 3; break;
				case SDLK_d: row = 1; bit = 2; break;
				case SDLK_e: row = 2; bit = 2; break;
				case SDLK_f: row = 1; bit = 3; break;
				case SDLK_g: row = 1; bit = 4; break;
				case SDLK_h: row = 6; bit = 4; break;
				case SDLK_i: row = 5; bit = 2; break;
				case SDLK_j: row = 6; bit = 3; break;
				case SDLK_k: row = 6; bit = 2; break;
				case SDLK_l: row = 6; bit = 1; break;
				case SDLK_m: row = 7; bit = 2; break;
				case SDLK_n: row = 7; bit = 3; break;
				case SDLK_o: row = 5; bit = 1; break;
				case SDLK_p: row = 5; bit = 0; break;
				case SDLK_q: row = 2; bit = 0; break;
				case SDLK_r: row = 2; bit = 3; break;
				case SDLK_s: row = 1; bit = 1; break;
				case SDLK_t: row = 2; bit = 4; break;
				case SDLK_u: row = 5; bit = 3; break;
				case SDLK_v: row = 0; bit = 4; break;
				case SDLK_w: row = 2; bit = 1; break;
				case SDLK_x: row = 0; bit = 2; break;
				case SDLK_y: row = 5; bit = 4; break;
				case SDLK_z: row = 0; bit = 1; break;
				case SDLK_0: row = 4; bit = 0; break;
				case SDLK_1: row = 3; bit = 0; break;
				case SDLK_2: row = 3; bit = 1; break;
				case SDLK_3: row = 3; bit = 2; break;
				case SDLK_4: row = 3; bit = 3; break;
				case SDLK_5: row = 3; bit = 4; break;
				case SDLK_6: row = 4; bit = 4; break;
				case SDLK_7: row = 4; bit = 3; break;
				case SDLK_8: row = 4; bit = 2; break;
				case SDLK_9: row = 4; bit = 1; break;
				case SDLK_SPACE: row = 7; bit = 0; break;
				case SDLK_RETURN: row = 6; bit = 0; break;
				case SDLK_LSHIFT:
				case SDLK_RSHIFT: row = 0; bit = 0; break;  // Caps Shift
				case SDLK_LCTRL:
				case SDLK_RCTRL:
				case SDLK_LALT:
				case SDLK_RALT: row = 7; bit = 1; break;  // Symbol Shift
				}
				if (row >= 0 && bit >= 0)
				{
					minZX.keyPress(row, bit, press);
				}
			}
		}

		minZX.update(&pixels[0]);

		if (useLocktexture)
		{
			unsigned char* lockedPixels = nullptr;
			int pitch = 0;
			SDL_LockTexture
				(
				texture,
				NULL,
				reinterpret_cast< void** >(&lockedPixels),
				&pitch
				);
			std::memcpy(lockedPixels, pixels.data(), pixels.size());
			SDL_UnlockTexture(texture);
		}
		else
		{
			SDL_UpdateTexture
				(
				texture,
				NULL,
				pixels.data(),
				texWidth * 4
				);
		}

		SDL_RenderCopy(renderer, texture, NULL, NULL);
		SDL_RenderPresent(renderer);

		SDL_Delay(20);  // Aproxima 50Hz para ZX Spectrum

		frames++;
		const Uint64 end = SDL_GetPerformanceCounter();
		const static Uint64 freq = SDL_GetPerformanceFrequency();
		const double seconds = (end - start) / static_cast< double >(freq);
		if (seconds > 2.0)
		{
			std::cout
				<< frames << " frames in "
				<< std::setprecision(1) << std::fixed << seconds << " seconds = "
				<< std::setprecision(1) << std::fixed << frames / seconds << " FPS ("
				<< std::setprecision(3) << std::fixed << (seconds * 1000.0) / frames << " ms/frame)"
				<< std::endl;
			start = end;
			frames = 0;
		}
	}

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}