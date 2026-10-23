/* Widescreen (docs/widescreen.md, research in docs/widescreen_research.md).
 *
 * The ZOOM release already offers every display mode (setup dialog, pcrazy.exe -s), and the game renders at the
 * full mode size, but:
 *  - the 3D view is Vert-: GP_SetProjection 0x4af3f0 builds the projection from a horizontal FOV (70 degrees in
 *    races, 45 in the frontend), so a wider screen shows less at the top and bottom. [Widescreen] HorPlusFov
 *    widens the FOV passed in by the two callers (0x428e04 frontend, 0x4599f3 race) so the 4:3 vertical view is kept.
 *  - the 2D layer (all text, HUD sprites, title cards) lives in a 640x480 space mapped to the full screen on each
 *    axis, so it stretches. Everything goes through FR_Transform2D 0x4e7e50, which is replaced
 *    ([Widescreen] Center2D) to scale x by 4/3 / aspect: the 2D layer is drawn at 4:3 in the middle of the screen. */
#include <math.h>
#include <string.h>
#include <ddraw.h>
#include "pcpatch.h"

#define GP_SET_PROJECTION 0x4af3f0u
#define FE_PROJ_CALL      0x428e04u
#define RACE_PROJ_CALL    0x4599f3u
#define GP_VIEW_W         0xb360         /* graphport: D3D viewport width / height (ints) */
#define GP_VIEW_H         0xb364

#define FR_TRANSFORM_2D   0x4e7e50u
#define FR_ASPECT         0x859048u      /* float h/w of the rasterizer rectangle */
#define FR_TL_OUT         0x6b8304u      /* D3DTLVERTEX output for vertex 0 (the buffer at 0x6b82e4 is 1-based) */
#define FR_TL_COUNT       0x83d014u

typedef void (__attribute__((thiscall)) *set_projection_fn)(void *gp, float near_, float far_, float fov);

static int g_hor_plus;

static void __attribute__((thiscall)) set_projection(void *gp, float near_, float far_, float fov)
{
    int w = *(int *)((char *)gp + GP_VIEW_W), h = *(int *)((char *)gp + GP_VIEW_H);
    if (g_hor_plus && w > 0 && h > 0 && w * 3 > h * 4) {
        double k = (double)w / h / (4.0 / 3.0);
        fov = (float)(2.0 * atan(tan(fov * M_PI / 360.0) * k) * 180.0 / M_PI);
    }
    ((set_projection_fn)PC_ADDR(GP_SET_PROJECTION))(gp, near_, far_, fov);
}

/* FR_Transform2D(rasterizer, verts, n): 640x480-space vertices (already in NDC x, and y scaled by h/W) to the TL
   buffer. Same as the original except x is scaled to a centred 4:3 area. */
typedef int (__attribute__((thiscall)) *transform_2d_fn)(void *rast, const float *in, int n);
static uint8_t g_t2d_tramp[16];          /* the original's first 9 bytes + jmp back */

static int __attribute__((thiscall)) transform_2d(void *rast, const float *in, int n)
{
    float s = *(float *)PC_ADDR(FR_ASPECT) * (4.0f / 3.0f);
    volatile float *out = (float *)PC_ADDR(FR_TL_OUT);
    int i, r;
    r = ((transform_2d_fn)(void *)g_t2d_tramp)(rast, in, n);
    if (s < 1.0f) {
        for (i = 0; i < n; i++) {
            out[i * 8] *= s;
        }
    }
    return r;
}

/* ---- full-screen pictures (splash, loading and championship screens) ----
   GP_ScaleImages 0x4a91b4 prescales each image to (img_w * W/640, img_h * H/480); GP_BlitImage 0x4b3fb0 and
   GP_BlitImageReveal 0x4b4380 draw it at (fx*W, fy*H), every caller with (0, 0). The width is scaled by H/480
   instead (keeps the 4:3 aspect), and a blit at x 0 is centred with the side bars filled black. */
#define SCALE_W_SITE   0x4a92c0u     /* mov ecx,280h / imul eax,ebp / div ecx: eax = img_w * W / 640 */
#define GP_BLIT_IMAGE  0x4b3fb0u
#define GP_BLIT_REVEAL 0x4b4380u
#define GP_MODE_W      0xb1cc        /* graphport: mode width / height, >> stretch shift (+0x7c3d0) */
#define GP_MODE_H      0xb1d0
#define GP_SHIFT       0x7c3d0
#define GP_BACKBUFFER  0xc494        /* IDirectDrawSurface */
#define GP_IMAGE_W     0x29c2c       /* + image * 0x40: prescaled width */

/* replaces the width computation: eax = img_w, [esp+4+0x1b4] = H (>> shift) */
__asm__(".text\n"
        "_scale_image_width:\n"
        "    imull 0x1b8(%esp), %eax\n"
        "    movl $480, %ecx\n"
        "    xorl %edx, %edx\n"
        "    divl %ecx\n"
        "    ret\n");
void scale_image_width(void);

typedef int (__attribute__((thiscall)) *blit_fn)(void *gp, float fx, float fy, int image);
static blit_fn g_blit_image, g_blit_reveal;

static void center_image(char *gp, float *fx, int image)
{
    int shift = *(int *)(gp + GP_SHIFT);
    int w = *(int *)(gp + GP_MODE_W) >> shift, h = *(int *)(gp + GP_MODE_H) >> shift;
    int iw = *(int *)(gp + GP_IMAGE_W + image * 0x40), x;
    IDirectDrawSurface *bb = *(IDirectDrawSurface **)(gp + GP_BACKBUFFER);
    if (*fx != 0.0f || iw <= 0 || iw >= w || !bb) {
        return;
    }
    x = (w - iw) / 2;
    *fx = (float)x / w;
    {
        DDBLTFX fx_fill;
        RECT l = { 0, 0, x, h }, r = { x + iw, 0, w, h };
        memset(&fx_fill, 0, sizeof fx_fill);
        fx_fill.dwSize = sizeof fx_fill;
        bb->lpVtbl->Blt(bb, &l, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx_fill);
        bb->lpVtbl->Blt(bb, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx_fill);
    }
}

static int __attribute__((thiscall)) blit_image(void *gp, float fx, float fy, int image)
{
    center_image(gp, &fx, image);
    return g_blit_image(gp, fx, fy, image);
}

static int __attribute__((thiscall)) blit_reveal(void *gp, float fx, float fy, int image)
{
    center_image(gp, &fx, image);
    return g_blit_reveal(gp, fx, fy, image);
}

static void images_install(void)
{
    static const uint8_t scale_w[10] = { 0xb9, 0x80, 0x02, 0x00, 0x00, 0x0f, 0xaf, 0xc5, 0xf7, 0xf1 };
    static const uint8_t prologue[10] = { 0x57, 0x56, 0x55, 0x53, 0x81, 0xec, 0xf4, 0x00, 0x00, 0x00 };
    static const uint8_t nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    if (!pc_expect(SCALE_W_SITE, scale_w, sizeof scale_w) || !pc_expect(GP_BLIT_IMAGE, prologue, 10)
        || !pc_expect(GP_BLIT_REVEAL, prologue, 10)) {
        pc_log("widescreen: image code differs, full-screen pictures left stretched");
        return;
    }
    pc_call(SCALE_W_SITE, (void *)scale_image_width);
    pc_write(SCALE_W_SITE + 5, nops, 5);
    g_blit_image = (blit_fn)pc_hook(GP_BLIT_IMAGE, prologue, 10, (void *)blit_image);
    g_blit_reveal = (blit_fn)pc_hook(GP_BLIT_REVEAL, prologue, 10, (void *)blit_reveal);
    pc_log("widescreen: full-screen pictures at 4:3");
}

void pc_widescreen_install(void)
{
    static const uint8_t fe_call[5] = { 0xe8, 0xe7, 0x65, 0x08, 0x00 };
    static const uint8_t race_call[5] = { 0xe8, 0xf8, 0x59, 0x05, 0x00 };
    static const uint8_t t2d[9] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x89, 0x4d, 0xf8 };

    g_hor_plus = pc_ini_int("Widescreen", "HorPlusFov", 1);
    if (g_hor_plus) {
        if (pc_expect(FE_PROJ_CALL, fe_call, 5) && pc_expect(RACE_PROJ_CALL, race_call, 5)) {
            pc_call(FE_PROJ_CALL, (void *)set_projection);
            pc_call(RACE_PROJ_CALL, (void *)set_projection);
            pc_log("widescreen: Hor+ field of view");
        } else {
            pc_log("widescreen: projection call sites differ, Hor+ skipped");
        }
    }
    if (pc_ini_int("Widescreen", "Center2D", 1)) {
        if (pc_expect(FR_TRANSFORM_2D, t2d, sizeof t2d)) {
            DWORD old;
            int32_t rel = (int32_t)(FR_TRANSFORM_2D + 9 - ((uintptr_t)g_t2d_tramp + 14));
            memcpy(g_t2d_tramp, t2d, 9);     /* push ebp / mov ebp,esp / sub esp,18 / mov [ebp-8],ecx: position independent */
            g_t2d_tramp[9] = 0xe9;
            memcpy(g_t2d_tramp + 10, &rel, 4);
            VirtualProtect(g_t2d_tramp, sizeof g_t2d_tramp, PAGE_EXECUTE_READWRITE, &old);
            pc_jmp(FR_TRANSFORM_2D, (void *)transform_2d);
            pc_log("widescreen: 2D layer centred at 4:3");
        } else {
            pc_log("widescreen: FR_Transform2D differs, 2D layer left stretched");
        }
        images_install();
    }
}
