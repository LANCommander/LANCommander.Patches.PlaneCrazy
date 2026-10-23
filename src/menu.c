/* Frontend menu extensions, shared by display.c and net.c (docs/graphics_options_research.md section 3,
 * docs/network_lobby_research.md section 4).
 *
 * The frontend menus are defined in data\DataMin\pcmenu.tct (plain text), loaded once by MENU_LoadTct 0x48e420
 * (called at 0x428b16 from FE_Construct). The load is redirected to a copy written to the saves folder with the
 * modules' additions: new DEF*ACTION names (inserted after the file's last one; action id = order of the DEF
 * lines), lines inserted before / replacing existing lines, and new menus and option lists appended at the end.
 * Actions on the new ids reach MENU_DoAction 0x42a84f, conditions MENU_EvalCondition 0x427f45 (unknown ids
 * evaluate to 1), and FE_Update 0x429e9f runs every frontend frame: all three are hooked and offered to the
 * modules first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcpatch.h"

#define LOAD_TCT_CALL 0x428b16u      /* call MENU_LoadTct (thiscall menusys, const char *file) */
#define LOAD_TCT      0x48e420u
#define DO_ACTION     0x42a84fu      /* MENU_DoAction (thiscall fe, int id), ret 4 */
#define EVAL_COND     0x427f45u      /* MENU_EvalCondition (stdcall int id) */
#define FE_UPDATE     0x429e9fu      /* FE_Update (thiscall fe) */

/* menu system object ([game+0x7c]) */
#define MS_NAMES      0x0000         /* action/condition names, 16 bytes each */
#define MS_NNAMES     0x16c0
#define MS_REBUILD    0x198c         /* menu changed */
#define MS_REDRAW     0x1990
#define MS_MENUS      0x199c         /* menus, 0xae4 bytes each */
#define MENU_SIZE     0xae4
#define MS_STACK      0x2d29c        /* navigation stack (menu indices) */
#define MS_DEPTH      0x2d39c
#define MS_NMENUS     0x2d3a0
#define MS_CURRENT    0x2d3a4
#define MS_LISTS      0x2d3a8        /* option lists, 0x1b0 bytes each */
#define LIST_SIZE     0x1b0
#define MS_NLISTS     0x309a8

#define MAX_NAMES     364            /* room in the action name table (MS_NNAMES follows it; no bounds check) */

#define MAX_EDITS 16
#define MAX_HOOKS 4

typedef struct {
    int kind;                        /* 0 insert before, 1 replace */
    const char *match;               /* line prefix */
    const char *text;
} edit_t;

static char g_defs[2048], g_body[8192];
static edit_t g_edits[MAX_EDITS];
static int g_nedits;
static char *g_ms;                   /* menu system object, once loaded */

static pc_menu_action_fn g_actions[MAX_HOOKS];
static pc_menu_cond_fn g_conds[MAX_HOOKS];
static pc_menu_frame_fn g_frames[MAX_HOOKS];
static pc_menu_loaded_fn g_loaded[MAX_HOOKS];
static int g_nactions, g_nconds, g_nframes, g_nloaded;

static int (__attribute__((thiscall)) *g_do_action)(void *fe, int id);
static int (__attribute__((stdcall)) *g_eval_cond)(int id);
static void (__attribute__((thiscall)) *g_fe_update)(void *fe);

void pc_menu_add_defs(const char *lines) { strncat(g_defs, lines, sizeof g_defs - strlen(g_defs) - 1); }
void pc_menu_append(const char *lines) { strncat(g_body, lines, sizeof g_body - strlen(g_body) - 1); }

static void add_edit(int kind, const char *match, const char *text)
{
    if (g_nedits < MAX_EDITS) {
        g_edits[g_nedits].kind = kind;
        g_edits[g_nedits].match = match;
        g_edits[g_nedits].text = text;
        g_nedits++;
    }
}

void pc_menu_insert_before(const char *match, const char *lines) { add_edit(0, match, lines); }
void pc_menu_replace(const char *match, const char *lines) { add_edit(1, match, lines); }

#define ADD_HOOK(list, n, fn) do { if ((n) < MAX_HOOKS) (list)[(n)++] = (fn); } while (0)
void pc_menu_on_action(pc_menu_action_fn fn) { ADD_HOOK(g_actions, g_nactions, fn); }
void pc_menu_on_condition(pc_menu_cond_fn fn) { ADD_HOOK(g_conds, g_nconds, fn); }
void pc_menu_on_frame(pc_menu_frame_fn fn) { ADD_HOOK(g_frames, g_nframes, fn); }
void pc_menu_on_loaded(pc_menu_loaded_fn fn) { ADD_HOOK(g_loaded, g_nloaded, fn); }

/* ---------------------------------------------------------------- access to the loaded menus */

int pc_menu_id(const char *name)
{
    int i, n;
    if (!g_ms) {
        return -1;
    }
    n = *(int *)(g_ms + MS_NNAMES);
    for (i = 0; i < n; i++) {
        if (!strcmp(g_ms + MS_NAMES + i * 16, name)) {
            return i;
        }
    }
    return -1;
}

int pc_menu_index(const char *menu)
{
    int i, n;
    if (!g_ms) {
        return -1;
    }
    n = *(int *)(g_ms + MS_NMENUS);
    for (i = 0; i < n; i++) {
        if (!strcmp(g_ms + MS_MENUS + i * MENU_SIZE, menu)) {
            return i;
        }
    }
    return -1;
}

char *pc_menu_item(const char *menu, int item)
{
    int i = pc_menu_index(menu);
    if (i < 0 || item < 0 || item >= *(int *)(g_ms + MS_MENUS + i * MENU_SIZE + 0x34)) {
        return NULL;
    }
    return g_ms + MS_MENUS + i * MENU_SIZE + 0x64 + item * PC_ITEM_SIZE;
}

char *pc_menu_list(const char *list)
{
    int i, n;
    if (!g_ms) {
        return NULL;
    }
    n = *(int *)(g_ms + MS_NLISTS);
    for (i = 0; i < n; i++) {
        if (!strcmp(g_ms + MS_LISTS + i * LIST_SIZE, list)) {
            return g_ms + MS_LISTS + i * LIST_SIZE;
        }
    }
    return NULL;
}

int pc_menu_current(void)
{
    return g_ms ? *(int *)(g_ms + MS_CURRENT) : -1;
}

int pc_menu_cursor(const char *menu)
{
    int i = pc_menu_index(menu);
    return i < 0 ? -1 : *(int *)(g_ms + MS_MENUS + i * MENU_SIZE + 0x38);
}

void pc_menu_redraw(void)
{
    if (g_ms) {
        *(int *)(g_ms + MS_REDRAW) = 1;
    }
}

/* like following a LINK: make the menu current and push it on the navigation stack */
int pc_menu_goto(const char *menu)
{
    int i = pc_menu_index(menu), depth;
    if (i < 0) {
        return 0;
    }
    depth = *(int *)(g_ms + MS_DEPTH) + 1;
    *(int *)(g_ms + MS_DEPTH) = depth;
    *(int *)(g_ms + MS_STACK + depth * 4) = i;
    *(int *)(g_ms + MS_CURRENT) = i;
    *(int *)(g_ms + MS_REBUILD) = 1;
    return 1;
}

/* ---------------------------------------------------------------- the generated pcmenu.tct */

static int starts_with(const char *line, const char *prefix)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return !strncmp(line, prefix, strlen(prefix));
}

static void put_lines(FILE *f, const char *text)   /* "\n"-separated text → CRLF lines */
{
    for (; *text; text++) {
        if (*text == '\n') {
            fputc('\r', f);
        }
        fputc(*text, f);
    }
}

/* number of different DEF*ACTION names in text (counted into names[], up to max) */
static int add_def_names(const char *text, char (*names)[32], int n, int max)
{
    const char *p;
    for (p = text; *p; ) {
        const char *q;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!strncmp(p, "DEF", 3) && strncmp(p, "DEFAULT", 7) && (q = strchr(p, '"')) != NULL) {
            char name[32];
            size_t len = strcspn(q + 1, "\"\r\n");
            int i;
            snprintf(name, sizeof name, "%.*s", (int)len, q + 1);
            for (i = 0; i < n && strcmp(names[i], name); i++) {
            }
            if (i == n && n < max) {
                strcpy(names[n++], name);
            }
        }
        p = strchr(p, '\n');
        if (!p) {
            break;
        }
        p++;
    }
    return n;
}

static int build_menu_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char *data, *p, *line, *last_def = NULL;
    long size;
    if (!in) {
        return 0;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    data = malloc(size + 1);
    if (!data || fread(data, 1, size, in) != (size_t)size) {
        fclose(in);
        free(data);
        return 0;
    }
    fclose(in);
    data[size] = '\0';
    for (p = data; *p; ) {      /* find the last DEF*ACTION line */
        if (!strncmp(p, "DEF", 3) && strncmp(p, "DEFAULT", 7)) {
            last_def = p;
        }
        p = strchr(p, '\n');
        p = p ? p + 1 : data + size;
    }
    {
        static char names[MAX_NAMES + 1][32];
        int n = add_def_names(g_defs, names, add_def_names(data, names, 0, MAX_NAMES + 1), MAX_NAMES + 1);
        if (n > MAX_NAMES) {
            pc_log("menu: more than %d action names, the additions don't fit", MAX_NAMES);
            free(data);
            return 0;
        }
    }
    out = fopen(dst, "wb");
    if (!out) {
        free(data);
        return 0;
    }
    for (line = data; *line; ) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        int i, replaced = 0;
        for (i = 0; i < g_nedits; i++) {
            if (starts_with(line, g_edits[i].match)) {
                put_lines(out, g_edits[i].text);
                replaced |= g_edits[i].kind == 1;
            }
        }
        if (!replaced) {
            fwrite(line, 1, len, out);
        }
        if (line == last_def) {
            put_lines(out, g_defs);
        }
        line += len;
    }
    put_lines(out, "\n#---------------------  pcpatch\n\n");
    put_lines(out, g_body);
    fclose(out);
    free(data);
    return 1;
}

static void __attribute__((fastcall)) load_tct(void *ms, void *unused, const char *file)
{
    char path[MAX_PATH];
    int i;
    (void)unused;
    snprintf(path, sizeof path, "%spcmenu.tct", pc_saves_dir());
    if (build_menu_file(file, path)) {
        pc_log("menu: loading %s", path);
        file = path;
    } else {
        pc_log("menu: could not write %s, using the original menus", path);
    }
    ((void (__attribute__((thiscall)) *)(void *, const char *))PC_ADDR(LOAD_TCT))(ms, file);
    g_ms = ms;
    pc_log("menu: %d names, %d menus, %d option lists", *(int *)(g_ms + MS_NNAMES), *(int *)(g_ms + MS_NMENUS),
           *(int *)(g_ms + MS_NLISTS));
    for (i = 0; i < g_nloaded; i++) {
        g_loaded[i]();
    }
}

/* ---------------------------------------------------------------- hooks */

static int __attribute__((thiscall)) do_action(void *fe, int id)
{
    int i;
    for (i = 0; i < g_nactions; i++) {
        if (g_actions[i](fe, id)) {
            return 0;
        }
    }
    return g_do_action(fe, id);
}

static int __attribute__((stdcall)) eval_cond(int id)
{
    int i, r;
    for (i = 0; i < g_nconds; i++) {
        if (g_conds[i](id, &r)) {
            return r;
        }
    }
    return g_eval_cond(id);
}

static void __attribute__((thiscall)) fe_update(void *fe)
{
    int i;
    for (i = 0; i < g_nframes; i++) {
        g_frames[i](fe);
    }
    g_fe_update(fe);
}

void pc_menu_install(void)
{
    static const uint8_t call_load[5] = { 0xe8, 0x05, 0x59, 0x06, 0x00 };
    static const uint8_t pro_action[9] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00 };
    static const uint8_t pro_cond[6] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x5c };
    static const uint8_t pro_update[9] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x14, 0x89, 0x4d, 0xf0 };
    if (!g_defs[0] && !g_body[0] && !g_nedits) {
        return;   /* no module added anything */
    }
    if (!pc_expect(LOAD_TCT_CALL, call_load, 5)) {
        pc_log("menu: MENU_LoadTct call differs, menu additions off");
        return;
    }
    if (g_nactions) {
        g_do_action = pc_hook(DO_ACTION, pro_action, 9, (void *)do_action);
    }
    if (g_nconds) {
        g_eval_cond = pc_hook(EVAL_COND, pro_cond, 6, (void *)eval_cond);
    }
    if (g_nframes) {
        g_fe_update = pc_hook(FE_UPDATE, pro_update, 9, (void *)fe_update);
    }
    if ((g_nactions && !g_do_action) || (g_nconds && !g_eval_cond) || (g_nframes && !g_fe_update)) {
        pc_log("menu: a menu hook failed");
    }
    pc_call(LOAD_TCT_CALL, (void *)load_tct);
}
