#ifndef PCPATCH_H
#define PCPATCH_H
#include <windows.h>
#include <stdint.h>

#ifndef PC_VERSION
#define PC_VERSION "dev"             /* build.sh passes the release tag */
#endif

int pc_patch_start(void);            /* exe check + Enabled + not loaded twice; 1 = go ahead and install the modules */
void pc_install(void);               /* installs the patch modules */
void pc_log(const char *fmt, ...);
void pc_set_files(const char *ini, const char *log);

/* memory helpers (pcrazy.exe VAs, image base 0x400000) */
int pc_write(uint32_t addr, const void *bytes, size_t len);
int pc_expect(uint32_t addr, const void *bytes, size_t len);   /* 1 if memory matches */
int pc_patch(uint32_t addr, const void *expect, const void *bytes, size_t len);
int pc_patch32(uint32_t addr, uint32_t expect, uint32_t value);
int pc_jmp(uint32_t from, const void *to);    /* E9 rel32 at from */
int pc_call(uint32_t from, const void *to);   /* E8 rel32 at from */
void *pc_hook(uint32_t addr, const void *expect, size_t len, void *hook);   /* detour; returns the trampoline */

/* settings: [section] key in pcpatch.ini */
int pc_ini_int(const char *section, const char *key, int def);
int pc_ini_str(const char *section, const char *key, char *out, int size);   /* "" if missing */

void *pc_hook_import(const char *dll, const char *name, void *replacement);  /* returns the old target */
void *pc_hook_import_in(HMODULE module, const char *dll, const char *name, void *replacement);

/* patch modules */
void pc_sound_install(void);
void pc_saves_install(void);      /* saves.c: files the game writes go to the Saved Games folder */
const char *pc_saves_dir(void);   /* saves folder, trailing backslash (set by pc_saves_install) */
void pc_registry_install(void);   /* registry.c: settings in Settings.ini instead of the registry */
void pc_trace_install(void);
void pc_widescreen_install(void); /* widescreen.c: Hor+ FOV, 2D layer at 4:3 */
void pc_gamepad_install(void);
void pc_players_install(void);    /* players.c: [Players] MaxPlayers network players */
extern int pc_max_players;
void *pc_gamepad_wrap(void *direct_input);   /* gamepad.c: wraps the game's IDirectInputA (dinput_proxy.c) */
typedef void (*pc_config_fn)(char *game);
void pc_on_game_config(pc_config_fn fn);   /* runs after GAME_ReadConfig filled the game object (pcpatch.c) */
void pc_display_install(void);    /* display.c: resolution / colour depth in the Options menu */
void pc_net_install(void);        /* net.c: TCP/IP without dialogs, in-game game browser */
void pc_framerate_install(void);  /* framerate.c: frame rate limit, vsync, high refresh rates */

/* menu.c: additions to the frontend menus (pcmenu.tct) and hooks on them. Modules register before
   pc_menu_install; text is newline-separated pcmenu.tct lines. */
#define PC_ITEM_SIZE 0x54            /* menu item: +4 visible, +8 label[36], +0x2c action id */
typedef int (*pc_menu_action_fn)(void *fe, int id);     /* 1 = handled (the game's MENU_DoAction is skipped) */
typedef int (*pc_menu_cond_fn)(int id, int *result);    /* 1 = handled */
typedef void (*pc_menu_frame_fn)(void *fe);             /* every frontend frame (FE_Update) */
typedef void (*pc_menu_loaded_fn)(void);                /* after pcmenu.tct is loaded: resolve ids here */
void pc_menu_install(void);
void pc_menu_add_defs(const char *lines);                /* after the last DEF*ACTION line */
void pc_menu_append(const char *lines);                  /* at the end (new menus, option lists) */
void pc_menu_insert_before(const char *match, const char *lines);   /* match = line prefix */
void pc_menu_replace(const char *match, const char *lines);
void pc_menu_on_action(pc_menu_action_fn fn);
void pc_menu_on_condition(pc_menu_cond_fn fn);
void pc_menu_on_frame(pc_menu_frame_fn fn);
void pc_menu_on_loaded(pc_menu_loaded_fn fn);
int pc_menu_id(const char *name);        /* action/condition id of a DEF*ACTION name, -1 if none */
int pc_menu_index(const char *menu);     /* menu index by id, -1 if none */
char *pc_menu_item(const char *menu, int item);
char *pc_menu_list(const char *list);    /* option list: +0x20 count, +0x24 index, values at +0x30 (0x18 each, text[16]) */
int pc_menu_current(void);               /* index of the menu shown */
int pc_menu_cursor(const char *menu);    /* highlighted item */
int pc_menu_goto(const char *menu);
void pc_menu_redraw(void);

#define PC_ADDR(a) ((void *)(uintptr_t)(a))
#endif
