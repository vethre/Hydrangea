#ifndef HYDRANGEA_GAME_H
#define HYDRANGEA_GAME_H
#define MAX_FLAGS 64

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>
#include <stdbool.h>

typedef struct {
    int op_cl,  val_cl;   // clarity
    int op_anx, val_anx;  // anxiety
    int op_bal, val_bal;  // balance
    char* flag;           // "flag"
    char* flag2;          // "flag2"
    char* not_flag;       // "not_flag"
    char* goto_id;        // id сцени для переходу
    int   goto_index;     // індекс (резолвимо після завантаження)
} SceneCheck;

typedef enum {
    MODE_MENU = 0,
    MODE_GAME = 1,
    MODE_SETTINGS = 2,
    MODE_END = 3,
} GameMode;

typedef struct {
    char* key;
    char* val;
} StringKV;

typedef struct {
    StringKV* kv;
    int count;
} Lang;

typedef struct {
    char* text;
    int d_clarity, d_anxiety, d_balance;
    int next;

    char* add_flags[4];   int add_flags_n;   // із "flags+"
    char* rem_flags[4];   int rem_flags_n;   // із "flags-"
} SceneChoice;

typedef struct {
    char* id;
    char* speaker;
    char* text;
    int num_choices;
    SceneChoice choices[4];

    char* background;
    char* music;

    char* title;
    float auto_time;
    char* auto_next_id;
    int   auto_next;

    SceneCheck checks[4];
    int checks_count;

    bool cinematic;
} Scene;

typedef struct {
    char text[24];
    SDL_Color col;
    float t;     // час анімації
    int row;    // 0=Clar, 1=Anx, 2=Bal 
} Notif;

typedef struct {
    const char* text; // текст кнопки
    int d_clarity; // ефекти на стати
    int d_anxiety;
    int d_balance;
    SDL_Rect rect; // область кнопки  
} Choice;

typedef struct {
    const char* speaker; // хто говорить can be NULL
    const char* text; // текст репліки
    int num_choices; // 0..4
    Choice choices[4];
    int hovered; // index hover or -1
    bool visible; // чи показаувати панель
} Dialog;

typedef struct {
    SDL_Window*   window;
    SDL_Renderer* renderer;
    bool          running;

    // Stats (0..100)
    float memory_clarity, memory_clarity_t;
    float anxiety,        anxiety_t;
    float balance,          balance_t;  // -100..+100

    int width, height;

    // Resources
    SDL_Texture* bg; // background
    TTF_Font*    font; // for text rendering

    Mix_Music* music; // actual song
    char current_music[256]; // that what now playin
    int music_volume; // 0..128
    int sfx_volume; // 0..128
    char lang_code[8]; // "ua"/"ru"/"en"

    Dialog dialog; // current dialog
    int cur_scene;

    Lang lang;
    Scene* scenes;
    int    scenes_count;
    int    start_scene;

    Notif notifs[6];
    int notif_count;

    GameMode mode; // MENU/SETTINGS/GAME/END
    int menu_index; // choose

    float fade;
    float fade_dir;
    int   fade_queued_scene;

    char menu_bg_path[128];

    SDL_Rect menu_btn_rects[4];
    int menu_hover;

    char* flags[MAX_FLAGS];
    int   flags_count;

    time_t cfg_mtime;

    bool fullscreen;

    char menu_music_path[128];

    int set_sel_res;
    bool set_drop_res_open;
    bool set_fullscreen;
    int set_lang_idx;
    bool set_drag_vol;
    SDL_Rect set_vol_rect;
} Game;

bool game_init(Game* g, const char* title, int w, int h);
void game_shutdown(Game* g);
void game_handle_event(Game* g, const SDL_Event* e);
void game_update(Game* g, float dt);
void game_render(Game* g);

#endif /* HYDRANGEA_GAME_H */
