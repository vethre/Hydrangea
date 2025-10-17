#include "game.h"
#include <SDL2/SDL.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv; // Unused
    
    Game g = {0};
    if (!game_init(&g, "The Hydrangea", 1280, 720)) return 1;

    Uint32 prev = SDL_GetTicks();
    while (g.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            game_handle_event(&g, &e);
        }

        Uint32 now = SDL_GetTicks();
        float dt = (now - prev) / 1000.f;
        prev = now;

        game_update(&g, dt);
        game_render(&g);

        SDL_Delay(1);
    }

    game_shutdown(&g);
    return 0;
}