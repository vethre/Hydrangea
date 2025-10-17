#include "game.h"
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>
#include "cJSON.h"
#include <string.h>
#define BALANCE_BIPOLAR 1

static void scene_show_immediate(Game* g, int idx);
static void save_config(Game* g);
static void draw_text_col(SDL_Renderer* r, TTF_Font* f, SDL_Color col, const char* txt, int x, int y);
static char* assets_full_path2(const char* rel);
static void set_fullscreen(Game* g, bool fs);

static const int RES_LIST[][2] = {
    {1280,720}, {1366,768}, {1600,900}, {1920,1080}, {2560,1440}
};
static const int RES_COUNT = (int)(sizeof(RES_LIST)/sizeof(RES_LIST[0]));
static const char* LANGS[] = {"ua","ru","en"};

static int lang_to_idx(const char* s) {
    for (int i=0;i<3;i++) if (SDL_strcasecmp(s, LANGS[i]) == 0) return i;
    return 0;
}

typedef struct {
    char path[128];
    SDL_Texture* tex;
} TexCacheEntry;
static TexCacheEntry g_tex_cache[16];
static int g_tex_cache_n = 0;

static SDL_Texture* load_texture_cached(SDL_Renderer* r, const char* relpath) {
    if (!relpath || !*relpath) return NULL;

    // === prefix "assets/" якщо його немає ===
    char path[256];
    if (SDL_strncasecmp(relpath, "assets/", 7) == 0) {
        SDL_snprintf(path, sizeof(path), "%s", relpath);
    } else {
        SDL_snprintf(path, sizeof(path), "assets/%s", relpath);
    }

    // кеш
    for (int i=0;i<g_tex_cache_n;i++)
        if (SDL_strcasecmp(g_tex_cache[i].path, path) == 0)
            return g_tex_cache[i].tex;

    SDL_Surface* s = IMG_Load(path);
    if (!s) { SDL_Log("IMG_Load(%s): %s", path, IMG_GetError()); return NULL; }
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (!t) return NULL;

    if (g_tex_cache_n < (int)(sizeof g_tex_cache/sizeof g_tex_cache[0])) {
        SDL_snprintf(g_tex_cache[g_tex_cache_n].path, sizeof(g_tex_cache[g_tex_cache_n].path), "%s", path);
        g_tex_cache[g_tex_cache_n].tex = t; g_tex_cache_n++;
    }
    return t;
}

// ---------- math helpers ----------
static inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}
static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// Плавне «підтягування» до цілі зі швидкістю rate (од./сек)
static inline float approachf(float cur, float tgt, float rate, float dt) {
    float diff = tgt - cur;
    float step = rate * dt;
    if (diff >  step) return cur + step;
    if (diff < -step) return cur - step;
    return tgt;
}
static inline int approachi(int cur, int tgt, float rate, float dt) {
    int diff = tgt - cur;
    int step = (int)(rate * dt + 0.5f);
    if (step < 1) step = 1;
    if (diff >  step) return cur + step;
    if (diff < -step) return cur - step;
    return tgt;
}

static int parse_cmp(const cJSON* j, int* op, int* val) {
    *op = -1; *val = 0;
    if (!cJSON_IsString(j)) return 0;
    const char* s = j->valuestring;
    if (s[0]=='<' && s[1]=='=') { *op=1; *val=atoi(s+2); return 1; }
    if (s[0]=='>' && s[1]=='=') { *op=3; *val=atoi(s+2); return 1; }
    if (s[0]=='<')             { *op=0; *val=atoi(s+1); return 1; }
    if (s[0]=='>')             { *op=4; *val=atoi(s+1); return 1; }
    if (s[0]=='=' )            { *op=2; *val=atoi(s+1); return 1; }
    /* без оператора трактуємо як >= */
    *op = 3; *val = atoi(s);
    return 1;
}

static int test_cmp(int op, int lhs, int rhs) {
    switch(op){
        case 0: return lhs <  rhs;
        case 1: return lhs <= rhs;
        case 2: return lhs == rhs;
        case 3: return lhs >= rhs;
        case 4: return lhs >  rhs;
        default:return 1;
    }
}

static int game_has_flag(Game* g, const char* name) {
    if (!name) return 1;
    for (int i = 0; i < g->flags_count; ++i)
        if (strcmp(g->flags[i], name) == 0) return 1;
    return 0;
}
static void game_add_flag(Game* g, const char* name) {
    if (!name || game_has_flag(g, name) || g->flags_count >= MAX_FLAGS) return;
    g->flags[g->flags_count++] = SDL_strdup(name);
}
static void game_remove_flag(Game* g, const char* name) {
    if (!name) return;
    for (int i = 0; i < g->flags_count; ++i) {
        if (strcmp(g->flags[i], name) == 0) {
            free(g->flags[i]);
            g->flags[i] = g->flags[g->flags_count - 1];
            g->flags_count--;
            return;
        }
    }
}

static void push_notif(Game* g, int row, const char* txt, SDL_Color col, float life) {
    if (g->notif_count >= (int)(sizeof g->notifs / sizeof g->notifs[0])) return;
    Notif* n = &g->notifs[g->notif_count++];
    SDL_snprintf(n->text, sizeof(n->text), "%s", txt);
    n->col = col; n->t = life; n->row = row;
}

static char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s)+1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char* read_file_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { SDL_Log("read_file_all: can't open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(sz+1);
    if (!buf){ fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static bool lang_load(Lang* out, const char* path) {
    memset(out, 0, sizeof(*out));
    char* json = read_file_all(path);
    if (!json) return false;

    cJSON* root = cJSON_Parse(json);
    if (!root){ free(json); return false; }

    int n = cJSON_GetArraySize(root); (void)n;
    int count = 0;
    for (cJSON* it = root->child; it; it = it->next)
        if (cJSON_IsString(it)) count++;

    out->kv = (StringKV*)calloc(count, sizeof(StringKV));
    out->count = 0;
    for (cJSON* it = root->child; it; it = it->next){
        if (!cJSON_IsString(it)) continue;
        out->kv[out->count].key = str_dup(it->string);
        out->kv[out->count].val = str_dup(cJSON_GetStringValue(it));
        out->count++;
    }

    cJSON_Delete(root);
    free(json);
    return true;
}

static const char* lang_get(const Lang* lang, const char* key) {
    if (!lang || !key) return NULL;
    for (int i=0; i<lang->count; ++i) {
        if (strcmp(lang->kv[i].key, key)==0) return lang->kv[i].val;
    }
    return NULL;
}

static void lang_free(Lang* lang) {
    if (!lang || !lang->kv) return;
    for (int i=0;i<lang->count;++i){ free(lang->kv[i].key); free(lang->kv[i].val); }
    free(lang->kv);
    memset(lang, 0, sizeof(*lang));
}

static char* assets_full_path2(const char* rel) {
    const char* with_assets = rel;
    char tmp[512];
    if (SDL_strncasecmp(rel, "assets/", 7) != 0) {
        SDL_snprintf(tmp, sizeof(tmp), "assets/%s", rel);
        with_assets = tmp;
    }

    char* base = SDL_GetBasePath();
    if (!base) return SDL_strdup(with_assets);

    size_t nb = SDL_strlen(base), nr = SDL_strlen(with_assets);
    char* p = (char*)malloc(nb + nr + 1);
    if (!p) { SDL_free(base); return NULL; }
    memcpy(p, base, nb);
    memcpy(p + nb, with_assets, nr + 1);
    SDL_free(base);
    return p;
}

typedef struct { char* id; } IdMap;

static int find_scene_index(Scene* arr, int count, const char* id) {
    for (int i=0;i<count;++i) if (arr[i].id && strcmp(arr[i].id, id)==0) return i;
    return -1;
}

// Підтягнути локалізований рядок: якщо "str:key" -> взяти з lang, інакше повернути як є.
static char* resolve_str(const Lang* lang, const cJSON* node){
    if (!cJSON_IsString(node)) return NULL;
    const char* v = node->valuestring;
    if (strncmp(v, "str:", 4) == 0){
        const char* k = v + 4;                      // ключ після "str:"
        const char* loc = lang_get(lang, k);        // пошук у словнику
        return str_dup(loc ? loc : k);              // копіюємо рядок
    }
    return str_dup(v);                               // звичайний текст
}

static bool scenes_load(Game* g, const char* scene_path)
{
    bool ok = false;
    char* json = read_file_all(scene_path);
    if (!json) return false;

    cJSON* root = cJSON_Parse(json);
    if (!root) { free(json); return false; }

    const cJSON* jstart = cJSON_GetObjectItemCaseSensitive(root, "start");
    const cJSON* jscenes = cJSON_GetObjectItemCaseSensitive(root, "scenes");
    if (!cJSON_IsString(jstart) || !cJSON_IsArray(jscenes)) {
        cJSON_Delete(root); free(json); return false;
    }

    const int count = cJSON_GetArraySize(jscenes);
    g->scenes = (Scene*)calloc(count, sizeof(Scene));
    g->scenes_count = count;

    // тимчасово збережемо next-id для кожного choice (макс 4 на сцену)
    char** next_ids = (char**)calloc(count * 4, sizeof(char*));
    if (!next_ids) { cJSON_Delete(root); free(json); return false; }

    // ---- 1) ПЕРШИЙ ПРОХІД: парсимо сцени
    int sidx = 0;
    const cJSON* it = NULL;
    cJSON_ArrayForEach(it, jscenes) {
        Scene* S = &g->scenes[sidx];

        const cJSON* jid    = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON* jsp    = cJSON_GetObjectItemCaseSensitive(it, "speaker");
        const cJSON* jtext  = cJSON_GetObjectItemCaseSensitive(it, "text");
        const cJSON* jbg    = cJSON_GetObjectItemCaseSensitive(it, "background");
        const cJSON* jmu    = cJSON_GetObjectItemCaseSensitive(it, "music");
        const cJSON* jtitle = cJSON_GetObjectItemCaseSensitive(it, "title");
        const cJSON* jauto  = cJSON_GetObjectItemCaseSensitive(it, "auto_time");
        const cJSON* jaNext = cJSON_GetObjectItemCaseSensitive(it, "auto_next");
        const cJSON* jchoices = cJSON_GetObjectItemCaseSensitive(it, "choices");
        const cJSON* jcin = cJSON_GetObjectItemCaseSensitive(it, "cinematic");
        S->cinematic = cJSON_IsTrue(jcin);


        if (!cJSON_IsString(jid)) {
            goto cleanup; // поганий json
        }

        // базові поля
        S->id      = str_dup(jid->valuestring);
        S->speaker = resolve_str(&g->lang, jsp);
        S->text    = resolve_str(&g->lang, jtext);

        if (cJSON_IsString(jbg)) S->background = str_dup(jbg->valuestring);
        if (cJSON_IsString(jmu)) S->music      = str_dup(jmu->valuestring);

        // титри/автоперехід
        S->title        = resolve_str(&g->lang, jtitle);
        S->auto_time    = cJSON_IsNumber(jauto) ? (float)jauto->valuedouble : 0.f;
        S->auto_next_id = cJSON_IsString(jaNext) ? str_dup(jaNext->valuestring) : NULL;
        S->auto_next    = -1;

        // checks (для автоматичного переходу без кнопок)
        S->checks_count = 0;
        const cJSON* jchecks = cJSON_GetObjectItemCaseSensitive(it, "checks");
        if (cJSON_IsArray(jchecks)) {
            int idx = 0;
            const cJSON* chk = NULL;
            cJSON_ArrayForEach(chk, jchecks) {
                if (idx >= 4) break;
                const cJSON* jif   = cJSON_GetObjectItemCaseSensitive(chk, "if");
                const cJSON* jgoto = cJSON_GetObjectItemCaseSensitive(chk, "goto");
                if (!cJSON_IsObject(jif) || !cJSON_IsString(jgoto)) continue;

                SceneCheck* C = &S->checks[idx++];
                memset(C, 0, sizeof(*C));
                C->op_cl = C->op_anx = C->op_bal = -1;
                C->goto_id = str_dup(jgoto->valuestring);
                C->goto_index = -1;

                parse_cmp(cJSON_GetObjectItemCaseSensitive(jif,"clarity"),  &C->op_cl,  &C->val_cl);
                parse_cmp(cJSON_GetObjectItemCaseSensitive(jif,"anxiety"),  &C->op_anx, &C->val_anx);
                parse_cmp(cJSON_GetObjectItemCaseSensitive(jif,"balance"),  &C->op_bal, &C->val_bal);

                const cJSON* jf  = cJSON_GetObjectItemCaseSensitive(jif,"flag");
                const cJSON* jf2 = cJSON_GetObjectItemCaseSensitive(jif,"flag2");
                const cJSON* jnf = cJSON_GetObjectItemCaseSensitive(jif,"not_flag");
                if (cJSON_IsString(jf))  C->flag     = str_dup(jf->valuestring);
                if (cJSON_IsString(jf2)) C->flag2    = str_dup(jf2->valuestring);
                if (cJSON_IsString(jnf)) C->not_flag = str_dup(jnf->valuestring);
                S->checks_count = idx;
            }
        }

        // choices
        S->num_choices = 0;
        if (cJSON_IsArray(jchoices)) {
            int cidx = 0;
            const cJSON* jc = NULL;
            cJSON_ArrayForEach(jc, jchoices) {
                if (cidx >= 4) break;
                SceneChoice* C = &S->choices[cidx];

                const cJSON* jt = cJSON_GetObjectItemCaseSensitive(jc, "text");
                const cJSON* je = cJSON_GetObjectItemCaseSensitive(jc, "effects");
                const cJSON* jn = cJSON_GetObjectItemCaseSensitive(jc, "next");

                C->text = resolve_str(&g->lang, jt);
                C->d_clarity = C->d_anxiety = C->d_balance = 0;
                if (cJSON_IsObject(je)) {
                    const cJSON* jcl = cJSON_GetObjectItemCaseSensitive(je, "clarity");
                    const cJSON* jan = cJSON_GetObjectItemCaseSensitive(je, "anxiety");
                    const cJSON* jba = cJSON_GetObjectItemCaseSensitive(je, "balance");
                    if (cJSON_IsNumber(jcl)) C->d_clarity = (int)jcl->valuedouble;
                    if (cJSON_IsNumber(jan)) C->d_anxiety = (int)jan->valuedouble;
                    if (cJSON_IsNumber(jba)) C->d_balance = (int)jba->valuedouble;
                }

                // тимчасово збережемо next id
                const int flat = sidx * 4 + cidx;
                next_ids[flat] = cJSON_IsString(jn) ? str_dup(jn->valuestring) : NULL;
                C->next = -1;

                // flags+ / flags-
                C->add_flags_n = C->rem_flags_n = 0;
                const cJSON* jadd = cJSON_GetObjectItemCaseSensitive(jc, "flags+");
                const cJSON* jrem = cJSON_GetObjectItemCaseSensitive(jc, "flags-");
                if (cJSON_IsArray(jadd)) {
                    const cJSON* itf = NULL;
                    cJSON_ArrayForEach(itf, jadd) {
                        if (!cJSON_IsString(itf)) continue;
                        if (C->add_flags_n < 4) C->add_flags[C->add_flags_n++] = str_dup(itf->valuestring);
                    }
                }
                if (cJSON_IsArray(jrem)) {
                    const cJSON* itf = NULL;
                    cJSON_ArrayForEach(itf, jrem) {
                        if (!cJSON_IsString(itf)) continue;
                        if (C->rem_flags_n < 4) C->rem_flags[C->rem_flags_n++] = str_dup(itf->valuestring);
                    }
                }

                cidx++; S->num_choices = cidx;
            }
        }

        sidx++;
    }

    // ---- 2) ДРУГИЙ ПРОХІД: резолвимо next / auto_next / checks.goto
    for (int i = 0; i < g->scenes_count; ++i) {
        for (int c = 0; c < g->scenes[i].num_choices; ++c) {
            const int flat = i * 4 + c;
            if (next_ids[flat]) {
                const int ni = find_scene_index(g->scenes, g->scenes_count, next_ids[flat]);
                g->scenes[i].choices[c].next = ni; // лишимо -1 якщо не знайдено
                free(next_ids[flat]);
                next_ids[flat] = NULL;
            }
        }
        if (g->scenes[i].auto_next_id) {
            g->scenes[i].auto_next =
                find_scene_index(g->scenes, g->scenes_count, g->scenes[i].auto_next_id);
        }
        if (g->scenes[i].checks_count > 0) {
            for (int k = 0; k < g->scenes[i].checks_count; ++k) {
                SceneCheck* C = &g->scenes[i].checks[k];
                if (C->goto_id) {
                    C->goto_index = find_scene_index(g->scenes, g->scenes_count, C->goto_id);
                }
            }
        }
    }

    // ---- 3) стартова сцена
    g->start_scene = find_scene_index(g->scenes, g->scenes_count, jstart->valuestring);
    ok = true;

cleanup:
    if (next_ids) {
        // прибери залишки, якщо були ранні виходи
        for (int i = 0; i < count * 4; ++i) free(next_ids[i]);
        free(next_ids);
    }
    cJSON_Delete(root);
    free(json);
    return ok;
}

static void scenes_free(Game* g) {
    if(!g->scenes) return;
    for (int i=0;i<g->scenes_count;i++){
        Scene* S = &g->scenes[i];
        free(S->id); free(S->speaker); free(S->text); free(S->background); free(S->music); free(S->title); free(S->auto_next_id);
        for (int c=0;c<S->num_choices;c++) free(S->choices[c].text);
        for (int k=0; k<S->checks_count; ++k) {
            free(S->checks[k].flag);
            free(S->checks[k].flag2);
            free(S->checks[k].not_flag);
            free(S->checks[k].goto_id);
        }
        for (int c = 0; c < S->num_choices; ++c) {
            for (int k = 0; k < S->choices[c].add_flags_n; ++k) free(S->choices[c].add_flags[k]);
            for (int k = 0; k < S->choices[c].rem_flags_n; ++k) free(S->choices[c].rem_flags[k]);
        }
    }
    free(g->scenes);
    g->scenes = NULL; g->scenes_count = 0; g->start_scene = -1;
}

static SDL_Texture* load_texture(SDL_Renderer* r, const char* path);

static void start_fade_to(Game* g, int next) { g->fade_dir = +1.f; g->fade_queued_scene = next; }

static void scene_show_immediate(Game* g, int idx) {
    if (idx < 0 || idx >= g->scenes_count){ g->dialog.visible=false; g->cur_scene=-1; return; }
    g->cur_scene = idx;
    g->dialog.visible = true;
    g->dialog.hovered = -1;

    Scene* s = &g->scenes[idx];
    // --- auto-branch за checks ---
    if (s->checks_count > 0) {
        for (int k=0; k<s->checks_count; ++k) {
            SceneCheck* C = &s->checks[k];
            if (C->goto_index < 0) continue;

            int cl  = (int)g->memory_clarity_t; // використовуй цільові значення
            int anx = (int)g->anxiety_t;
            int bal = g->balance_t;

            int cond =
                test_cmp(C->op_cl,  cl,  C->val_cl) &&
                test_cmp(C->op_anx, anx, C->val_anx) &&
                test_cmp(C->op_bal, bal, C->val_bal);

            if (cond) {
                if (C->flag  && !game_has_flag(g, C->flag))  cond = 0;
                if (C->flag2 && !game_has_flag(g, C->flag2)) cond = 0;
                if (C->not_flag && game_has_flag(g, C->not_flag)) cond = 0;
            }

            if (cond) {
                // одразу переходимо, кнопки не показуємо
                scene_show_immediate(g, C->goto_index);
                return;
            }
        }
    }
    g->dialog.speaker = s->speaker;
    g->dialog.text = s->text;
    g->dialog.num_choices = s->num_choices;
    for (int i=0;i<s->num_choices;i++){
        g->dialog.choices[i].text = s->choices[i].text;
        g->dialog.choices[i].d_clarity = s->choices[i].d_clarity;
        g->dialog.choices[i].d_anxiety = s->choices[i].d_anxiety;
        g->dialog.choices[i].d_balance = s->choices[i].d_balance;
        g->dialog.choices[i].rect = (SDL_Rect){0,0,0,0};
    }
    if (s->background) {
        SDL_Texture* newbg = load_texture_cached(g->renderer, s->background);
        if (newbg) { g->bg = newbg; } // не знищуємо — кеш володіє ресурсом
    }

    if (s->music) {
        if (SDL_strcasecmp(g->current_music, s->music) != 0) {
            if (g->music) { Mix_HaltMusic(); Mix_FreeMusic(g->music); g->music = NULL; }
            char* mpath = assets_full_path2(s->music);        // <<< ДОДАНО
            g->music = mpath ? Mix_LoadMUS(mpath) : NULL;
            if (!g->music) SDL_Log("Mix_LoadMUS(%s) failed: %s", s->music, Mix_GetError());
            else {
                SDL_snprintf(g->current_music, sizeof(g->current_music), "%s", s->music);
                Mix_VolumeMusic(g->music_volume);
                Mix_PlayMusic(g->music, -1);
            }
            if (mpath) free(mpath);
        }
    }
}

static SDL_Texture* load_texture(SDL_Renderer* r, const char* path) {
    char* full = assets_full_path2(path);
    SDL_Texture* tex = NULL;
    if (full) {
        SDL_Surface* s = IMG_Load(full);
        if (!s) SDL_Log("IMG_LoadTexture(%s) failed: %s", path, SDL_GetError());
        else {
            tex = SDL_CreateTextureFromSurface(r, s);
            SDL_FreeSurface(s);
        }
        free(full);
    }
    return tex;
}

static void render_bg_fit(SDL_Renderer* r, SDL_Texture* tex, int win_w, int win_h) {
    // завжди чистимо чорним (або темним бекґраундом)
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    if (!tex) return;

    int tw=0, th=0; SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    float s = SDL_min((float)win_w/tw, (float)win_h/th);
    int w = (int)(tw*s), h = (int)(th*s);
    SDL_Rect dst = { (win_w - w)/2, (win_h - h)/2, w, h };
    SDL_RenderCopy(r, tex, NULL, &dst);
}

static void play_music(Game* g, const char* rel) {
    if (!rel || !rel[0]) return;
    if (SDL_strcasecmp(g->current_music, rel)==0 && Mix_PlayingMusic()) return;

    if (g->music) { Mix_HaltMusic(); Mix_FreeMusic(g->music); g->music = NULL; }

    char* full = assets_full_path2(rel);
    g->music = full ? Mix_LoadMUS(full) : NULL;
    if (g->music) {
        SDL_snprintf(g->current_music, sizeof(g->current_music), "%s", rel);
        Mix_VolumeMusic(g->music_volume);
        Mix_PlayMusic(g->music, -1);
    }
    if (full) free(full);
}

static void render_settings(Game* g) {
    SDL_SetRenderDrawColor(g->renderer, 20,24,32,255);
    SDL_RenderClear(g->renderer);

    int cx = g->width/2;
    draw_text_col(g->renderer, g->font, (SDL_Color){234,239,244,255}, "SETTINGS", cx-60, g->height/2-160);

    // --- Resolution ---
    SDL_Rect r_res = { 40, g->height/2 - 100, 280, 40 };
    SDL_SetRenderDrawColor(g->renderer, 28,32,40,255); SDL_RenderFillRect(g->renderer, &r_res);
    SDL_SetRenderDrawColor(g->renderer, 36,42,51,190); SDL_RenderDrawRect(g->renderer, &r_res);
    char res_txt[64]; SDL_snprintf(res_txt, sizeof(res_txt), "Resolution: %dx%d",
           RES_LIST[g->set_sel_res][0], RES_LIST[g->set_sel_res][1]);
    draw_text_col(g->renderer, g->font, (SDL_Color){210,210,210,255}, res_txt, r_res.x+10, r_res.y+10);

    if (g->set_drop_res_open) {
        for (int i=0;i<RES_COUNT;i++) {
            SDL_Rect r = { r_res.x, r_res.y + 40 + i*36, r_res.w, 34 };
            SDL_SetRenderDrawColor(g->renderer, i==g->set_sel_res?40:28, i==g->set_sel_res?48:32, i==g->set_sel_res?60:40, 255);
            SDL_RenderFillRect(g->renderer, &r);
            SDL_SetRenderDrawColor(g->renderer, 36,42,51,190); SDL_RenderDrawRect(g->renderer, &r);
            char txt[32]; SDL_snprintf(txt, sizeof(txt), "%dx%d", RES_LIST[i][0], RES_LIST[i][1]);
            draw_text_col(g->renderer, g->font, (SDL_Color){234,239,244,255}, txt, r.x+10, r.y+8);
        }
    }

    // --- Fullscreen ---
    SDL_Rect r_fs = { 40, r_res.y + (g->set_drop_res_open ? 40+RES_COUNT*36 : 50), 220, 40 };
    SDL_SetRenderDrawColor(g->renderer, 28,32,40,255); SDL_RenderFillRect(g->renderer, &r_fs);
    SDL_SetRenderDrawColor(g->renderer, 36,42,51,190); SDL_RenderDrawRect(g->renderer, &r_fs);
    draw_text_col(g->renderer, g->font, (SDL_Color){210,210,210,255},
                  g->set_fullscreen? "Fullscreen: ON":"Fullscreen: OFF", r_fs.x+10, r_fs.y+10);

    // --- Language (циклічний дропдаун) ---
    SDL_Rect r_lang = { 40, r_fs.y + 50, 220, 40 };
    SDL_SetRenderDrawColor(g->renderer, 28,32,40,255); SDL_RenderFillRect(g->renderer, &r_lang);
    SDL_SetRenderDrawColor(g->renderer, 36,42,51,190); SDL_RenderDrawRect(g->renderer, &r_lang);
    char ltxt[64]; SDL_snprintf(ltxt, sizeof(ltxt), "Language: %s", LANGS[g->set_lang_idx]);
    draw_text_col(g->renderer, g->font, (SDL_Color){210,210,210,255}, ltxt, r_lang.x+10, r_lang.y+10);

    // --- Music Volume (slider) ---
    SDL_Rect r_vol_line = { 40, r_lang.y + 70, 340, 6 };
    g->set_vol_rect = r_vol_line;
    SDL_SetRenderDrawColor(g->renderer, 60,66,76,255); SDL_RenderFillRect(g->renderer, &r_vol_line);

    float t = SDL_clamp(g->music_volume/128.0f, 0.f, 1.f);
    int knob_x = r_vol_line.x + (int)(t*(r_vol_line.w-12));
    SDL_Rect knob = { knob_x, r_vol_line.y - 7, 12, 20 };
    SDL_SetRenderDrawColor(g->renderer, 110,178,191,255); SDL_RenderFillRect(g->renderer, &knob);

    char vt[32]; SDL_snprintf(vt, sizeof(vt), "Music: %d/128", g->music_volume);
    draw_text_col(g->renderer, g->font, (SDL_Color){170,178,190,255}, vt, r_vol_line.x+360, r_vol_line.y-8);

    // --- Apply / Back ---
    SDL_Rect r_apply = { g->width - 220, g->height - 60, 80, 36 };
    SDL_Rect r_back  = { g->width - 120, g->height - 60, 80, 36 };
    SDL_SetRenderDrawColor(g->renderer, 40,48,60,255); SDL_RenderFillRect(g->renderer, &r_apply);
    SDL_SetRenderDrawColor(g->renderer, 36,42,51,190); SDL_RenderDrawRect(g->renderer, &r_apply);
    SDL_SetRenderDrawColor(g->renderer, 28,32,40,255); SDL_RenderFillRect(g->renderer, &r_back);
    SDL_SetRenderDrawColor(g->renderer, 36,42,51,190); SDL_RenderDrawRect(g->renderer, &r_back);
    draw_text_col(g->renderer, g->font, (SDL_Color){234,239,244,255}, "Apply", r_apply.x+18, r_apply.y+8);
    draw_text_col(g->renderer, g->font, (SDL_Color){234,239,244,255}, "Back",  r_back.x+22,  r_back.y+8);
}

static void handle_settings_event(Game* g, const SDL_Event* e) {
    // зручні локальні прямокутники (мають збігатися з render_settings)
    SDL_Rect r_res = { 40, g->height/2 - 100, 280, 40 };
    int res_list_top = r_res.y + 40;
    SDL_Rect r_fs  = { 40, r_res.y + (g->set_drop_res_open? 40+RES_COUNT*36:50), 220, 40 };
    SDL_Rect r_lang= { 40, r_fs.y + 50, 220, 40 };
    SDL_Rect r_vol = g->set_vol_rect;
    SDL_Rect r_apply = { g->width - 220, g->height - 60, 80, 36 };
    SDL_Rect r_back  = { g->width - 120, g->height - 60, 80, 36 };

    if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT) {
        SDL_Point p = { e->button.x, e->button.y };

        // Resolution dropdown
        if (SDL_PointInRect(&p, &r_res)) {
            g->set_drop_res_open = !g->set_drop_res_open;
            return;
        }
        if (g->set_drop_res_open) {
            for (int i=0;i<RES_COUNT;i++) {
                SDL_Rect r = { r_res.x, res_list_top + i*36, r_res.w, 34 };
                if (SDL_PointInRect(&p, &r)) {
                    g->set_sel_res = i;
                    g->set_drop_res_open = false;
                    return;
                }
            }
        }

        // Fullscreen toggle
        if (SDL_PointInRect(&p, &r_fs)) { g->set_fullscreen = !g->set_fullscreen; return; }

        // Language (циклічно)
        if (SDL_PointInRect(&p, &r_lang)) { g->set_lang_idx = (g->set_lang_idx+1)%3; return; }

        // Volume slider begin drag
        if (SDL_PointInRect(&p, &r_vol) ||
            (p.x >= r_vol.x && p.x <= r_vol.x+r_vol.w && p.y >= r_vol.y-8 && p.y <= r_vol.y+16)) {
            g->set_drag_vol = true;
            float t = SDL_clamp((p.x - r_vol.x) / (float)SDL_max(r_vol.w,1), 0.f, 1.f);
            g->music_volume = (int)(t * 128.f);
            Mix_VolumeMusic(g->music_volume);
            return;
        }

        // Apply
        if (SDL_PointInRect(&p, &r_apply)) {
            // apply resolution
            int nw = RES_LIST[g->set_sel_res][0], nh = RES_LIST[g->set_sel_res][1];
            SDL_SetWindowSize(g->window, nw, nh);
            SDL_GetWindowSize(g->window, &g->width, &g->height);
            SDL_RenderSetViewport(g->renderer, NULL);
            SDL_RenderSetScale(g->renderer, 1.0f, 1.0f);

            // apply fullscreen
            if (g->set_fullscreen != g->fullscreen) {
                set_fullscreen(g, g->set_fullscreen);
            }

            // apply language: reload lang + scenes
            SDL_snprintf(g->lang_code, sizeof(g->lang_code), "%s", LANGS[g->set_lang_idx]);
            lang_free(&g->lang);
            char lp[128]; SDL_snprintf(lp,sizeof(lp),"assets/strings/%s.json", g->lang_code);
            if (!lang_load(&g->lang, lp)) lang_load(&g->lang, "assets/strings/ua.json");

            scenes_free(g);
            scenes_load(g, "assets/content/scenes_demo.json");

            Mix_VolumeMusic(g->music_volume);
            save_config(g);
            return;
        }

        // Back
        if (SDL_PointInRect(&p, &r_back)) {
            g->mode = MODE_MENU;
            return;
        }
    }
    if (e->type == SDL_MOUSEMOTION && g->set_drag_vol) {
        float t = SDL_clamp((e->motion.x - r_vol.x) / (float)SDL_max(r_vol.w,1), 0.f, 1.f);
        g->music_volume = (int)(t * 128.f);
        Mix_VolumeMusic(g->music_volume);
        return;
    }
    if (e->type == SDL_MOUSEBUTTONUP && e->button.button==SDL_BUTTON_LEFT) {
        g->set_drag_vol = false;
        return;
    }

    // ESC -> назад у меню
    if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE) {
        g->mode = MODE_MENU;
        return;
    }
}

static void draw_text(SDL_Renderer* r, TTF_Font* f, const char* txt, int x, int y) {
    SDL_Color col = { 210, 210, 210, 255 };
    SDL_Surface* s = TTF_RenderUTF8_Blended(f, txt, col);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_Rect dst = { x, y, s->w, s->h };
    SDL_FreeSurface(s);
    if (t) {
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
}

static int draw_text_right(SDL_Renderer* r, TTF_Font* f, const char* txt, int right, int y) {
    SDL_Color col = { 234, 239, 244, 255 }; // #EAEFF4
    SDL_Surface* s = TTF_RenderUTF8_Blended(f, txt, col);
    if (!s) return 0;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    int x = right - s->w;
    SDL_Rect dst = { x, y, s->w, s->h };
    SDL_FreeSurface(s);
    if (t) {
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    return dst.w;
}

static void draw_text_col(SDL_Renderer* r, TTF_Font* f, SDL_Color col, const char* txt, int x, int y) {
    if (!f || !txt) return;
    SDL_Surface* s = TTF_RenderUTF8_Blended(f, txt, col);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_Rect dst = { x, y, s->w, s->h };
    SDL_FreeSurface(s);
    if (t) {
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
}

static void draw_text_wrapped(SDL_Renderer* r, TTF_Font* f, SDL_Color col,
                              const char* txt, int x, int y, int max_w) {
    if (!f || !txt) return;
    SDL_Surface* s = TTF_RenderUTF8_Blended_Wrapped(f, txt, col, (Uint32)max_w);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_Rect dst = { x, y, s->w, s->h };
    SDL_FreeSurface(s);
    if (t) {
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
}

static void set_fullscreen(Game* g, bool fs) {
    SDL_SetWindowFullscreen(g->window, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    g->fullscreen = fs;
    save_config(g);
}

static void load_config(Game* g) {
    // def
    SDL_snprintf(g->lang_code, sizeof(g->lang_code), "ua");
    g->music_volume = 96; // 0..128
    g->sfx_volume = 128;

    char* json = read_file_all("assets/config.json");
    if (!json) return;
    cJSON* root = cJSON_Parse(json);
    if (!root) { free(json); return; }

    const cJSON* jlang = cJSON_GetObjectItemCaseSensitive(root, "lang");
    const cJSON* jmv = cJSON_GetObjectItemCaseSensitive(root, "music");
    const cJSON* jsv = cJSON_GetObjectItemCaseSensitive(root, "sfx");
    const cJSON* jres = cJSON_GetObjectItemCaseSensitive(root, "resolution");
    const cJSON* jfs = cJSON_GetObjectItemCaseSensitive(root, "fullscreen");
    g->fullscreen = cJSON_IsTrue(jfs);
    if (cJSON_IsArray(jres) && cJSON_GetArraySize(jres)==2) {
        g->width = cJSON_GetArrayItem(jres,0)->valueint;
        g->height = cJSON_GetArrayItem(jres,1)->valueint;
    }
    const cJSON* jmm = cJSON_GetObjectItemCaseSensitive(root, "menu_music");
    if (cJSON_IsString(jmm)) {
        SDL_snprintf(g->menu_music_path, sizeof(g->menu_music_path), "%s", jmm->valuestring);
    }
    const cJSON* jbg = cJSON_GetObjectItemCaseSensitive(root,"menu_bg");
    if (cJSON_IsString(jbg)) SDL_snprintf(g->menu_bg_path, sizeof(g->menu_bg_path), "%s", cJSON_IsString(jbg) ? jbg->valuestring : "assets/backgrounds/menu_bg.png");
    if (cJSON_IsString(jlang)) SDL_snprintf(g->lang_code, sizeof(g->lang_code), "%s", jlang->valuestring);
    if (cJSON_IsNumber(jmv)) g->music_volume = (int)(jmv->valuedouble*1.28);
    if (cJSON_IsNumber(jsv)) g->sfx_volume = (int)(jsv->valuedouble*1.28);

    cJSON_Delete(root); free(json);
}

static void save_config(Game* g) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "lang", g->lang_code);
    cJSON_AddNumberToObject(root, "music", (int)(g->music_volume));
    cJSON_AddBoolToObject(root, "fullscreen", g->fullscreen);

    cJSON* arr = cJSON_CreateIntArray((int[]){g->width,g->height},2);
    cJSON_AddItemToObject(root, "resolution", arr);

    // тільки відносно assets/
    const char* relative_bg = g->menu_bg_path[0] ? g->menu_bg_path : "backgrounds/menu_bg.png";
    cJSON_AddStringToObject(root, "menu_bg", relative_bg);

    const char* relative_mm = g->menu_music_path[0] ? g->menu_music_path : "music/main_menu.mp3";
    cJSON_AddStringToObject(root, "menu_music", relative_mm);

    char* txt = cJSON_Print(root);
    FILE* f = fopen("assets/config.json", "w");
    if (f) { fputs(txt, f); fclose(f); }
    free(txt); cJSON_Delete(root);
}

bool game_init(Game* g, const char* title, int w, int h) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    // SDL_Image (PNG)
    int img_flags = IMG_INIT_PNG;
    if ((IMG_Init(img_flags) & img_flags) == 0) {
        SDL_Log("IMG_Init failed: %s", IMG_GetError());
    }

    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
        // не фатальна помилка, можна продовжити
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) < 0) {
        SDL_Log("Mix_OpenAudio failed: %s", Mix_GetError());
    }
    Mix_AllocateChannels(8);

    load_config(g);
    Mix_VolumeMusic(g->music_volume);

    // menu hover = none
    g->menu_hover = -1;

    // settings state from current config
    g->set_fullscreen = g->fullscreen;
    g->set_lang_idx = lang_to_idx(g->lang_code);
    g->set_sel_res = 0;
    for (int i=0;i<RES_COUNT;i++) if (RES_LIST[i][0]==g->width && RES_LIST[i][1]==g->height) { g->set_sel_res = i; break; }
    g->set_drop_res_open = false;
    g->set_drag_vol = false;

    // capture config mtime (for hot-reload)
    struct stat st; g->cfg_mtime = 0;
    if (stat("assets/config.json",&st)==0) g->cfg_mtime = st.st_mtime;

    // ensure menu music plays if we start at menu
    if (g->mode == MODE_MENU) {
        play_music(g, g->menu_music_path[0]? g->menu_music_path : "music/main_menu.mp3");
    }

    int cw = g->width ? g->width : w;
    int ch = g->height ? g->height : h;

    g->window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cw, ch, SDL_WINDOW_RESIZABLE);
    g->width = cw; g->height = ch;
    if (!g->window) {
        SDL_Log("CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    g->renderer = SDL_CreateRenderer(g->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(g->renderer, SDL_BLENDMODE_BLEND);
    if (!g->renderer) {
        SDL_Log("CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    if (g->fullscreen) set_fullscreen(g, true);

    char lang_path[128];
    SDL_snprintf(lang_path, sizeof(lang_path), "assets/strings/%s.json", g->lang_code[0] ? g->lang_code : "ua");
    if (!lang_load(&g->lang, lang_path)) {
        SDL_Log("lang_load failed, fallback to ua");
        lang_load(&g->lang, "assets/strings/ua.json");
        // не фатальна помилка, можна продовжити
    }
    bool scenes_ok = scenes_load(g, "assets/content/scenes_demo.json");
    if (!scenes_ok) SDL_Log("scenes_load failed");

    g->mode = MODE_MENU;
    g->menu_index = 0;

    g->fade = 0.f; g->fade_dir = 0.f; g->fade_queued_scene = -1;

    g->flags_count = 0;

    g->cur_scene = -1;
    g->running = true;
    g->memory_clarity = g->memory_clarity_t = 20.0f;
    g->anxiety        = g->anxiety_t        = 12.0f;
    g->balance        = g->balance_t        = 5;
    g->notif_count = 0;

    // Resources
    g->bg = load_texture_cached(
        g->renderer,
        g->menu_bg_path[0] ? g->menu_bg_path : "backgrounds/menu_bg.png"
    );    
    g->music = NULL;
    g->current_music[0] = 0;
    g->font = TTF_OpenFont("assets/fonts/Inter-Medium.ttf", 20);
    if (!g->font) SDL_Log("TTF_OpenFont failed: %s", TTF_GetError());

    return true;
}

void game_shutdown(Game* g) {
    save_config(g);
    if (g->renderer) SDL_DestroyRenderer(g->renderer);
    if (g->window)   SDL_DestroyWindow(g->window);
    if (g->bg)      SDL_DestroyTexture(g->bg);
    if (g->font)    TTF_CloseFont(g->font);
    for (int i = 0; i < g->flags_count; ++i) free(g->flags[i]);
    g->flags_count = 0;
    for (int i=0;i<g_tex_cache_n;i++) if (g_tex_cache[i].tex) SDL_DestroyTexture(g_tex_cache[i].tex);
    g_tex_cache_n = 0;

    scenes_free(g);
    lang_free(&g->lang);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}

void game_handle_event(Game* g, const SDL_Event* e) {
    switch(g->mode) {
        case MODE_MENU:
            if (e->type == SDL_QUIT) { g->running = false; return; }

            if (e->type == SDL_MOUSEMOTION) {
                SDL_Point p = { e->motion.x, e->motion.y };
                g->menu_hover = -1;
                for (int i=0;i<4;i++) if (SDL_PointInRect(&p, &g->menu_btn_rects[i])) { g->menu_hover=i; break; }
                return;
            }
            if (e->type == SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT) {
                SDL_Point p = { e->button.x, e->button.y };
                for (int i=0;i<4;i++) if (SDL_PointInRect(&p, &g->menu_btn_rects[i])) {
                    g->menu_index = i;
                    if (i==0 || i==1) { // New/Continue -> старт сцени
                        if (g->start_scene >= 0) { start_fade_to(g, g->start_scene); g->mode = MODE_GAME; }
                    } else if (i==2) {
                        g->mode = MODE_SETTINGS;
                    } else if (i==3) {
                        g->running = false;
                    }
                    return;
                }
            }
            if (e->type == SDL_QUIT) g->running = false;
            if (e->type == SDL_KEYDOWN) {
                if (e->key.keysym.sym == SDLK_ESCAPE) g->running = false;
                if (e->key.keysym.sym == SDLK_UP) g->menu_index = (g->menu_index+3) % 4;
                if (e->key.keysym.sym == SDLK_DOWN) g->menu_index = (g->menu_index+1) % 4;
                if (e->key.keysym.sym == SDLK_F11 || (e->key.keysym.sym == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT))) { set_fullscreen(g, !g->fullscreen); break; }
                if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_SPACE) {
                    if (g->menu_index == 0) {
                        if (g->start_scene >= 0) { start_fade_to(g, g->start_scene); g->mode = MODE_GAME; }
                    } else if (g->menu_index == 1) {
                        if (g->start_scene >= 0) { start_fade_to(g, g->start_scene); g->mode = MODE_GAME; }
                    } else if (g->menu_index == 2) {
                        g->mode = MODE_SETTINGS;
                    } else if (g->menu_index == 3) {
                        g->running = false;
                    }
                }
            }
            return;
        case MODE_SETTINGS:
            handle_settings_event(g, e);
            return;
        default: break;
    }
    switch (e->type) {
        case SDL_QUIT:
            g->running = false;
            break;
        case SDL_KEYDOWN:
            if (e->key.keysym.sym == SDLK_ESCAPE) g->running = false;
            if (e->key.keysym.sym == SDLK_e) g->memory_clarity_t += 2.f;
            if (e->key.keysym.sym == SDLK_q) g->anxiety_t += 2.f;
            if (e->key.keysym.sym == SDLK_a) g->balance_t -= 5;
            if (e->key.keysym.sym == SDLK_d) g->balance_t += 5;
            if (g->dialog.visible && e->key.keysym.sym >= SDLK_1 && e->key.keysym.sym <= SDLK_4) {
                // по клавішах
                if (g->dialog.visible && e->key.keysym.sym >= SDLK_1 && e->key.keysym.sym <= SDLK_4) {
                    int idx = (int)(e->key.keysym.sym - SDLK_1);
                    if (idx >= 0 && idx < g->dialog.num_choices) {
                        Scene* S = &g->scenes[g->cur_scene];
                        SceneChoice* C = &S->choices[idx];

                        // стат-ефекти
                        g->memory_clarity_t += C->d_clarity;
                        g->anxiety_t        += C->d_anxiety;
                        g->balance_t        += C->d_balance;

                        // прапорці
                        for (int k = 0; k < C->add_flags_n; ++k) game_add_flag(g, C->add_flags[k]);
                        for (int k = 0; k < C->rem_flags_n; ++k) game_remove_flag(g, C->rem_flags[k]);

                        g->dialog.visible = false;

                        // нотифікації
                        char tmp[24];
                        if (C->d_clarity){ SDL_snprintf(tmp, sizeof(tmp), "%+d CL", C->d_clarity); push_notif(g,0,tmp,(SDL_Color){110,178,191,255},1.4f); }
                        if (C->d_anxiety){ SDL_snprintf(tmp, sizeof(tmp), "%+d ANX", C->d_anxiety); push_notif(g,1,tmp,(SDL_Color){205,63,69,255},1.4f); }
                        if (C->d_balance){ SDL_snprintf(tmp, sizeof(tmp), "%+d BAL", C->d_balance); push_notif(g,2,tmp,(SDL_Color){199,141,165,255},1.4f); }

                        // перехід
                        if (C->next >= 0) start_fade_to(g, C->next);
                        else { g->dialog.visible=false; g->cur_scene=-1; }
                    }
                }
            }
            if (e->key.keysym.sym == SDLK_r) {
                start_fade_to(g, g->start_scene);
            }
            break;
        case SDL_MOUSEMOTION:
            if (g->dialog.visible) {
                int mx = e->motion.x, my = e->motion.y;
                g->dialog.hovered = -1;
                for (int i=0;i<g->dialog.num_choices;++i) {
                    if (SDL_PointInRect(&(SDL_Point){mx,my}, &g->dialog.choices[i].rect)) {
                        g->dialog.hovered = i;
                        break;
                    }
                }
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (g->dialog.visible && e->button.button == SDL_BUTTON_LEFT) {
                int mx = e->button.x, my = e->button.y;
                for (int i=0;i<g->dialog.num_choices;i++){
                    if (SDL_PointInRect(&(SDL_Point){mx,my}, &g->dialog.choices[i].rect)) {
                        Scene* S = &g->scenes[g->cur_scene];
                        SceneChoice* C = &S->choices[i];

                        // стат-ефекти
                        g->memory_clarity_t += C->d_clarity;
                        g->anxiety_t        += C->d_anxiety;
                        g->balance_t        += C->d_balance;

                        // прапорці
                        for (int k = 0; k < C->add_flags_n; ++k) game_add_flag(g, C->add_flags[k]);
                        for (int k = 0; k < C->rem_flags_n; ++k) game_remove_flag(g, C->rem_flags[k]);

                        g->dialog.visible = false;

                        // нотифікації
                        char tmp[24];
                        if (C->d_clarity){ SDL_snprintf(tmp, sizeof(tmp), "%+d CL", C->d_clarity); push_notif(g,0,tmp,(SDL_Color){110,178,191,255},1.4f); }
                        if (C->d_anxiety){ SDL_snprintf(tmp, sizeof(tmp), "%+d ANX", C->d_anxiety); push_notif(g,1,tmp,(SDL_Color){205,63,69,255},1.4f); }
                        if (C->d_balance){ SDL_snprintf(tmp, sizeof(tmp), "%+d BAL", C->d_balance); push_notif(g,2,tmp,(SDL_Color){199,141,165,255},1.4f); }

                        // перехід
                        if (C->next >= 0) start_fade_to(g, C->next);
                        else { g->dialog.visible=false; g->cur_scene=-1; }

                        break;
                    }
                }
            }
            break;
        case SDL_WINDOWEVENT:
            if (e->window.event == SDL_WINDOWEVENT_RESIZED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_MAXIMIZED) {
                SDL_GetWindowSize(g->window, &g->width, &g->height);
                SDL_RenderSetViewport(g->renderer, NULL);
                SDL_RenderSetScale(g->renderer, 1.0f, 1.0f);
            }
            break;
        default: break;
    }
}

void game_update(Game* g, float dt) {
    // ---- hot-reload config.json ----
    static float cfg_timer = 0.f;
    cfg_timer += dt;
    if (cfg_timer > 0.5f) { // раз на ~0.5 c
        cfg_timer = 0.f;
        struct stat st;
        if (stat("assets/config.json", &st) == 0 && st.st_mtime != g->cfg_mtime) {
            g->cfg_mtime = st.st_mtime;

            char old_bg[128]; SDL_snprintf(old_bg, sizeof(old_bg), "%s", g->menu_bg_path);
            char old_lang[8]; SDL_snprintf(old_lang, sizeof(old_lang), "%s", g->lang_code);

            load_config(g);

            // reload language if changed
            if (SDL_strcasecmp(old_lang, g->lang_code)!=0) {
                lang_free(&g->lang);
                char lp[128]; SDL_snprintf(lp,sizeof(lp),"assets/strings/%s.json", g->lang_code[0]?g->lang_code:"ua");
                if (!lang_load(&g->lang, lp)) lang_load(&g->lang, "assets/strings/ua.json");

                // scenes must be reloaded for new language
                scenes_free(g);
                scenes_load(g, "assets/content/scenes_demo.json");
            }

            // reload background texture if path changed
            if (SDL_strcasecmp(old_bg, g->menu_bg_path)!=0) {
                SDL_Texture* newbg = load_texture(g->renderer, g->menu_bg_path[0]?g->menu_bg_path:"backgrounds/menu_bg.png");
                if (newbg) { if (g->bg) SDL_DestroyTexture(g->bg); g->bg = newbg; }
            }

            Mix_VolumeMusic(g->music_volume);

            if (g->mode == MODE_MENU) {
                play_music(g, g->menu_music_path[0]? g->menu_music_path : "music/main_menu.mp3");
            }
        }
    }

    float s = 4.5f;
    if (g->fade_dir != 0.f) {
        g->fade += g->fade_dir * s * dt;
        if (g->fade >= 1.f) { g->fade = 1.f; g->fade_dir = -1.f;
            if (g->fade_queued_scene >= 0) {
                scene_show_immediate(g, g->fade_queued_scene);
                g->fade_queued_scene = -1;
            }
        } else if (g->fade <= 0.f) { g->fade = 0.f; g->fade_dir = 0.f; }
    }
    //* 1) Дрейф балансу від тривоги (в пер-секундних одиницях)
    float anx = g->anxiety; // беремо згладжене current
    float drift = 0.f;
    if (anx > 60.f)       drift = -(anx - 60.f) * 0.20f;  // до -8.0/с при 100
    else if (anx < 30.f)  drift =  (30.f - anx) * 0.12f;  // до +3.6/с при 0

    //* Демпфування: баланс сам тягнеться до 0 (щоб не зашкалював до ±100)
    float damping = 0.20f;                 // 20%/с до нуля
    float bal_vel = drift - damping * g->balance_t;

    g->balance_t += bal_vel * dt;

    //* 2) Вплив балансу на ясність (повільно набираємо, швидше втрачаємо)
    float k_up = 0.30f, k_down = 0.60f;    // при |balance|==100
    if (g->balance_t >= 0.f) g->memory_clarity_t += (g->balance_t / 100.f) * k_up   * dt;
    else                     g->memory_clarity_t += (g->balance_t / 100.f) * k_down * dt;

    //* 3) Природний спад тривоги (дуже повільно)
    g->anxiety_t -= 0.05f * dt;

    //* 4) Клампи target’ів
    g->memory_clarity_t = clampf(g->memory_clarity_t, 0.f, 100.f);
    g->anxiety_t        = clampf(g->anxiety_t,        0.f, 100.f);
    g->balance_t        = clampf(g->balance_t,      -100.f, 100.f);

    //* 5) Плавний підхід current → target
    g->memory_clarity = approachf(g->memory_clarity, g->memory_clarity_t, 220.f, dt);
    g->anxiety        = approachf(g->anxiety,        g->anxiety_t,        220.f, dt);
    g->balance        = approachf(g->balance,        g->balance_t,        500.f, dt);

    //* 6) Нотифікації
    for (int i=0; i<g->notif_count; ){
        g->notifs[i].t -= dt;
        if (g->notifs[i].t <= 0.f){
            g->notifs[i] = g->notifs[g->notif_count-1];
            g->notif_count--;
        } else ++i;
    }

    //* 7) Autoscenes
    if (g->dialog.visible && g->cur_scene >= 0) {
        Scene* S = &g->scenes[g->cur_scene];
        if (S->auto_time > 0.f && S->auto_next >= 0 && S->num_choices == 0) {
            S->auto_time -= dt;
            if (S->auto_time <= 0.f) {
                g->dialog.visible = false;
                start_fade_to(g, S->auto_next);
            }
        }
    }
}

static void draw_bar(SDL_Renderer* r, float x, float y, float w, float h, float value01) {
    // рамка
    SDL_SetRenderDrawColor(r, 36, 42, 51, 180); // #232A33 ~60% opacity
    SDL_Rect border = { (int)x, (int)y, (int)w, (int)h };
    SDL_RenderDrawRect(r, &border);

    // заповнення
    int pad = 2;
    SDL_Rect fill = {
        (int)(x + pad), (int)(y + pad),
        (int)((w - 2*pad) * value01), (int)(h - 2*pad)
    };
    SDL_SetRenderDrawColor(r, 110, 178, 191, 255); // #6ED2BF
    SDL_RenderFillRect(r, &fill);
}

static float balance_to01(int bal) {
    if (bal < -100) bal = -100;
    if (bal > +100) bal = 100;
    return (bal + 100) / 200.0f; // -100..+100 -> 0..1
}

static void draw_balance_bar(SDL_Renderer* r, float x, float y, float w, float h, int bal) {
    SDL_SetRenderDrawColor(r, 36, 42, 51, 180);
    SDL_Rect border = { (int)x, (int)y, (int)w, (int)h };
    SDL_RenderDrawRect(r, &border);

    int pad = 2;
    float ix = x + pad, iy = y + pad;
    float iw = w - 2*pad, ih = h - 2*pad;

#if BALANCE_BIPOLAR
    float zero_x = ix + iw * 0.5f;

    // негатив ліворуч (червоний)
    if (bal < 0) {
        float t = (bal + 100) / 200.0f;        // -100..0 -> 0..0.5
        float left  = ix + iw * t;
        float right = zero_x;
        SDL_SetRenderDrawColor(r, 205, 63, 69, 255);
        SDL_Rect neg = { (int)left, (int)iy, (int)(right - left), (int)ih };
        SDL_RenderFillRect(r, &neg);
    }

    // позитив праворуч (рожевий)
    if (bal > 0) {
        float t = (bal + 100) / 200.0f;        // 0..+100 -> 0.5..1.0
        float left  = zero_x;
        float right = ix + iw * t;
        SDL_SetRenderDrawColor(r, 199, 141, 165, 255);
        SDL_Rect pos = { (int)left, (int)iy, (int)(right - left), (int)ih };
        SDL_RenderFillRect(r, &pos);
    }

    // маркер
    float cx = ix + iw * (bal + 100) / 200.0f;
#else
    // Ліво->право 0..100: просто рожевий філд
    int bal01 = SDL_clamp(bal, 0, 100);
    SDL_SetRenderDrawColor(r, 199, 141, 165, 255);
    SDL_Rect pos = { (int)ix, (int)iy, (int)(iw * (bal01/100.0f)), (int)ih };
    SDL_RenderFillRect(r, &pos);
    float cx = ix + iw * (bal01/100.0f);
#endif

    int cy = (int)(iy + ih/2), rpx = 5;
    SDL_SetRenderDrawColor(r, 234, 239, 244, 255);
    for (int dy = -rpx; dy <= rpx; ++dy)
      for (int dx = -rpx; dx <= rpx; ++dx)
        if (dx*dx + dy*dy <= rpx*rpx) SDL_RenderDrawPoint(r, (int)cx + dx, cy + dy);
}

static void text_size(TTF_Font* f, const char* txt, int* w, int* h) {
    int tw = 0, th = 0;
    if (f && txt) TTF_SizeUTF8(f, txt, &tw, &th);
    if (w) *w = tw;
    if (h) *h = th;
}

void game_render(Game* g) {
    // ===== MENЮ =====
    if (g->mode == MODE_MENU) {
        render_bg_fit(g->renderer, g->bg, g->width, g->height);

        // напівпрозорий оверлей
        SDL_SetRenderDrawColor(g->renderer, 0,0,0,160);
        SDL_Rect ov = {0,0,g->width,g->height};
        SDL_RenderFillRect(g->renderer, &ov);

        const char* items[4] = {
            lang_get(&g->lang, "menu.new_game") ?: "New Game",
            lang_get(&g->lang, "menu.continue") ?: "Continue",
            lang_get(&g->lang, "menu.settings") ?: "Settings",
            lang_get(&g->lang, "menu.exit")     ?: "Exit"
        };

        int cx = g->width/2, cy = g->height/2;
        int bw = (int)(g->width * 0.28f), bh = 48, gap = 14;
        int start_y = cy - (2*bh + 1*gap + bh);

        for (int i=0;i<4;i++){
            int x = cx - bw/2;
            int y = start_y + i*(bh+gap);
            SDL_Rect r = {x,y,bw,bh};
            g->menu_btn_rects[i] = r;

            bool sel = (g->menu_hover==i) || (g->menu_hover==-1 && g->menu_index==i);
            SDL_SetRenderDrawColor(g->renderer, sel?40:28, sel?48:32, sel?60:40, 255);
            SDL_RenderFillRect(g->renderer, &r);
            SDL_SetRenderDrawColor(g->renderer, 36,42,51,180);
            SDL_RenderDrawRect(g->renderer, &r);

            int tw=0,th=0; text_size(g->font, items[i], &tw, &th);
            draw_text_col(g->renderer, g->font, (SDL_Color){234,239,244,255},
                        items[i], x + (bw - tw)/2, y + (bh - th)/2);
        }

        if (g->fade > 0.f) {
            Uint8 a = (Uint8)SDL_clamp((int)(g->fade*255),0,255);
            SDL_SetRenderDrawColor(g->renderer,0,0,0,a);
            SDL_Rect full={0,0,g->width,g->height}; SDL_RenderFillRect(g->renderer,&full);
        }
        SDL_RenderPresent(g->renderer);
        return;
    }

    // ===== НАЛАШТУВАННЯ =====
    if (g->mode == MODE_SETTINGS) {
        render_settings(g);
        if (g->fade > 0.f) {
            Uint8 a = (Uint8)SDL_clamp((int)(g->fade*255),0,255);
            SDL_SetRenderDrawColor(g->renderer,0,0,0,a);
            SDL_Rect full={0,0,g->width,g->height}; SDL_RenderFillRect(g->renderer,&full);
        }
        SDL_RenderPresent(g->renderer);
        return;
    }

    // ===== ОСНОВНА СЦЕНА (MODE_GAME та ін.) =====
    // фон
    if (g->bg) {
        SDL_RenderClear(g->renderer);
        SDL_Rect dst = {0,0,g->width,g->height};
        render_bg_fit(g->renderer, g->bg, g->width, g->height);
    } else {
        SDL_SetRenderDrawColor(g->renderer, 18,20,24,255);
        SDL_RenderClear(g->renderer);
    }

    // Чи ми в синематику?
    bool cinematic = (g->cur_scene >= 0 && g->scenes[g->cur_scene].cinematic);

    // Адаптивні коефіцієнти (потрібні і для діалогів)
    float sW = g->width  / 1280.f;
    float sH = g->height /  720.f;
    float ui_scale = SDL_clamp(SDL_min(sW, sH), 0.75f, 1.0f);

    // ----- HUD (бари/нотіфки) показуємо тільки якщо НЕ cinematic -----
    if (!cinematic) {
        float pad   = 16.f * ui_scale;
        float bar_h = 18.f * ui_scale;
        float gap   = 8.f  * ui_scale;
        int   x     = (int)pad;
        int   y     = (int)pad;
        float panel_w = (float)g->width * 0.34f;

        char val_cl[32], val_anx[32], val_bal[32];
        SDL_snprintf(val_cl,  sizeof(val_cl),  "%.0f%%", g->memory_clarity);
        SDL_snprintf(val_anx, sizeof(val_anx), "%.0f",   g->anxiety);
        SDL_snprintf(val_bal, sizeof(val_bal), "%+d",    (int)g->balance);

        int w_lbl1, w_lbl2, w_lbl3, htmp;
        text_size(g->font, "Memory Clarity", &w_lbl1, &htmp);
        text_size(g->font, "Anxiety",        &w_lbl2, NULL);
        text_size(g->font, "Balance",        &w_lbl3, NULL);
        int label_col_w = SDL_max(SDL_max(w_lbl1, w_lbl2), w_lbl3) + (int)(12*ui_scale);

        int w_val1, w_val2, w_val3;
        text_size(g->font, val_cl,  &w_val1, NULL);
        text_size(g->font, val_anx, &w_val2, NULL);
        text_size(g->font, val_bal, &w_val3, NULL);
        int value_col_w = SDL_max(SDL_max(w_val1, w_val2), w_val3) + (int)(6*ui_scale);

        int bar_x   = x + label_col_w + (int)(8*ui_scale);
        int value_x = (int)(bar_x + panel_w + 10*ui_scale);

        // підкладка
        {
            int hud_left  = (int)(x - pad*0.6f);
            int hud_top   = (int)(y - pad*0.6f);
            int hud_right = value_x + value_col_w + (int)(pad*0.6f);
            int hud_h     = (int)(bar_h * 3.f + gap * 2.f + pad * 2.f);
            if (hud_right > g->width - (int)(pad*0.4f)) hud_right = g->width - (int)(pad*0.4f);
            SDL_SetRenderDrawColor(g->renderer, 20,24,32,180);
            SDL_Rect hud = { hud_left, hud_top, hud_right - hud_left, hud_h };
            SDL_RenderFillRect(g->renderer, &hud);
        }

        // рядки
        draw_text(g->renderer, g->font, "Memory Clarity", x, y);
        draw_bar(g->renderer, (float)bar_x, (float)y, panel_w, bar_h, g->memory_clarity/100.f);
        draw_text(g->renderer, g->font, val_cl, value_x, y);
        y += (int)(bar_h + gap);

        draw_text(g->renderer, g->font, "Anxiety", x, y);
        SDL_SetRenderDrawColor(g->renderer, 36,42,51,180);
        SDL_Rect border = { bar_x, y, (int)panel_w, (int)bar_h };
        SDL_RenderDrawRect(g->renderer, &border);
        int pad2 = 2;
        SDL_Rect fill = { bar_x+pad2, y+pad2,
                          (int)((panel_w-2*pad2) * (g->anxiety/100.f)),
                          (int)(bar_h-2*pad2) };
        SDL_SetRenderDrawColor(g->renderer, 205,63,69,255);
        SDL_RenderFillRect(g->renderer, &fill);
        draw_text(g->renderer, g->font, val_anx, value_x, y);
        y += (int)(bar_h + gap);

        draw_text(g->renderer, g->font, "Balance", x, y);
        draw_balance_bar(g->renderer, (float)bar_x, (float)y, panel_w, bar_h, (int)g->balance);
        draw_text(g->renderer, g->font, val_bal, value_x, y);

        // нотифікації (прив'язано до HUD — теж ховаємо у cinematic)
        for (int i=0; i<g->notif_count; ++i) {
            Notif* n = &g->notifs[i];
            int row_y = (int)(pad + (bar_h + gap) * n->row);
            float t01 = SDL_clamp(n->t / 1.4f, 0.f, 1.f);
            SDL_Color col = n->col; col.a = (Uint8)(255 * t01);
            int tw=0, th=0; text_size(g->font, n->text, &tw, &th);
            int tx = (int)(bar_x + panel_w - tw - 8);
            int ty = row_y - (int)(th + 6);
            if (ty < 2) ty = 2;
            draw_text_col(g->renderer, g->font, col, n->text, tx, ty);
        }
    }

    // ----- ДІАЛОГ (завжди відмальовуємо; вигляд залежить від cinematic) -----
    if (g->dialog.visible) {
        if (cinematic) {
            // затемнення і короткий текст посередині
            SDL_SetRenderDrawColor(g->renderer, 0,0,0,140);
            SDL_Rect full = {0,0,g->width,g->height};
            SDL_RenderFillRect(g->renderer, &full);

            const char* msg = g->dialog.text ? g->dialog.text : "";
            int tw=0, th=0; text_size(g->font, msg, &tw, &th);
            int x = (g->width - tw)/2;
            int y = (g->height - th)/2 + (int)(12*ui_scale);
            draw_text_col(g->renderer, g->font, (SDL_Color){234,239,244,255}, msg, x, y);
        } else {
            // стандартна панель з виборами
            int inner_pad = (int)(20 * ui_scale);
            int panel_wi  = (int)(g->width - 2 * (16 * ui_scale));
            int panel_hi  = (int)SDL_max(110.f * ui_scale, g->height * 0.18f);
            int panel_x   = (g->width - panel_wi)/2;
            int panel_y   = g->height - panel_hi - (int)(12 * ui_scale);

            SDL_SetRenderDrawColor(g->renderer, 20,24,32,200);
            SDL_Rect dlg = { panel_x, panel_y, panel_wi, panel_hi };
            SDL_RenderFillRect(g->renderer, &dlg);

            int cur_y = panel_y + inner_pad;
            int cur_x = panel_x + inner_pad;
            SDL_Color c_title = {234,239,244,255};
            SDL_Color c_text  = {210,210,210,255};

            if (g->dialog.speaker) {
                draw_text_col(g->renderer, g->font, c_title, g->dialog.speaker, cur_x, cur_y);
                int w,h; text_size(g->font, g->dialog.speaker, &w, &h);
                cur_y += h + (int)(6*ui_scale);
            }

            int text_w = panel_wi - inner_pad*2;
            int wrap_h = 0;
            {
                SDL_Color mcol = {210,210,210,255};
                const char* msg = g->dialog.text ? g->dialog.text : "";
                SDL_Surface* ms = TTF_RenderUTF8_Blended_Wrapped(g->font, msg, mcol, (Uint32)text_w);
                if (ms) { wrap_h = ms->h; SDL_FreeSurface(ms); }
            }

            // 2) геометрія кнопок (перш ніж малювати текст)
            int btn_h   = (int)(56 * ui_scale);
            int btn_gap = (int)(10 * ui_scale);
            int cols    = 2;
            int btn_w   = (text_w - btn_gap) / cols;
            int rows    = (g->dialog.num_choices + cols - 1) / cols;

            // 3) максимально допустима висота тексту, щоб усе влізло в панель
            int max_text_h = panel_hi - inner_pad*2
                        - rows * btn_h
                        - (rows ? (rows-1)*btn_gap : 0)
                        - (int)(12 * ui_scale);          // відступ між текстом і кнопками
            wrap_h = SDL_clamp(wrap_h, 0, SDL_max(0, max_text_h));

            // 4) малюємо ТІЛЬКИ в межах панелі
            SDL_Rect clip = { cur_x, cur_y, text_w, wrap_h };
            SDL_RenderSetClipRect(g->renderer, &clip);
            draw_text_wrapped(g->renderer, g->font, c_text, g->dialog.text, cur_x, cur_y, text_w);
            SDL_RenderSetClipRect(g->renderer, NULL);

            cur_y += wrap_h + (int)(12 * ui_scale);

            // 5) кнопки — рівно під текстом
            for (int i=0; i<g->dialog.num_choices; ++i) {
                int row = i / cols, col = i % cols;
                int bx = cur_x + col * (btn_w + btn_gap);
                int by = cur_y + row * (btn_h + btn_gap);

                SDL_Rect r = (SDL_Rect){ bx, by, btn_w, btn_h };
                g->dialog.choices[i].rect = r;

                bool hov = (g->dialog.hovered == i);
                SDL_SetRenderDrawColor(g->renderer, hov?40:28, hov?48:32, hov?60:40, 255);
                SDL_RenderFillRect(g->renderer, &r);
                SDL_SetRenderDrawColor(g->renderer, hov?110:36, hov?178:42, hov?191:51, hov?255:180);
                SDL_RenderDrawRect(g->renderer, &r);

                int tx = bx + (int)(14 * ui_scale);
                int ty = by + (int)(10 * ui_scale);
                draw_text_col(g->renderer, g->font, c_title, g->dialog.choices[i].text, tx, ty);

                char hint[64];
                SDL_snprintf(hint, sizeof(hint), "(%+d CL, %+d ANX, %+d BAL)",
                            g->dialog.choices[i].d_clarity,
                            g->dialog.choices[i].d_anxiety,
                            g->dialog.choices[i].d_balance);
                draw_text_col(g->renderer, g->font, (SDL_Color){170,178,190,255}, hint, tx, ty + (int)(20 * ui_scale));
            }
        }
    }

    if (g->cur_scene >= 0) {
        Scene* S = &g->scenes[g->cur_scene];
        if (S->title && S->num_choices == 0) {
            SDL_SetRenderDrawColor(g->renderer, 0, 0, 0, 200);
            SDL_Rect full = {0,0,g->width,g->height};
            SDL_RenderFillRect(g->renderer, &full);

            SDL_Color col = {234,239,244,255};
            int tw, th;
            text_size(g->font, S->title, &tw, &th);
            draw_text_col(g->renderer, g->font, col,
                        S->title, (g->width - tw)/2, (g->height - th)/2);

            // легке fade-in/out
            SDL_RenderPresent(g->renderer);
        }
    }

    // загальний fade
    if (g->fade > 0.f) {
        Uint8 a = (Uint8)SDL_clamp((int)(g->fade * 255), 0, 255);
        SDL_SetRenderDrawColor(g->renderer, 0,0,0, a);
        SDL_Rect full = (SDL_Rect){0,0,g->width,g->height};
        SDL_RenderFillRect(g->renderer, &full);
    }

    SDL_RenderPresent(g->renderer);
}
