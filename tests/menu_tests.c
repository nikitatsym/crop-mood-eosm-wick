/* Tests for the Film Format / Standard menu logic of crop_rec.  tests/run.sh pulls the real
 * functions out of modules/crop_rec/crop_rec.c, so this runs the code that is built into the camera. */
#include <stdio.h>
#include <string.h>
#include "film-formats.h"

#define COERCE(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* the menu state, same names as in crop_rec.c */
static int slim_mode_ui = 2, slim_1x1_ar = 2, slim_unified_preset = 0, crop_preset_ar_menu = 4;
static int slim_film_fmt = 0, slim_film_frames = 0, crop_preset_fps_menu = 0;
static int apply_calls = 0;
static int std_slim = 0;            /* the SLIM standard (crop.std_slim); the film tests run with it off */
static int INFO_button = 6;
#define MOD(x, m) ((((x) % (m)) + (m)) % (m))
#define MENU_SELECT_FUNC(name) void name(void * priv __attribute__((unused)), int delta)
/* backend state read by slim_crop_fps_mask() */
enum { CROP_PRESET_OFF, CROP_PRESET_1X1, CROP_PRESET_1X3, CROP_PRESET_3X3 };
static int g_preset = CROP_PRESET_3X3, crop_preset_1x1_res_menu = 3, crop_preset_1x3_res_menu = 1;
#define CROP_PRESET_MENU g_preset

#include "crop_rec_menu_snippets.h"

/* stand-in for the real mode switch (which writes the sensor settings): not under test here */
static void slim_crop_apply_mode(void) { apply_calls++; }
static void slim_crop_sync_from_backend(void) { }
/* real slim_film_apply is extracted after this point */
#include "crop_rec_apply_snippet.h"

static void set_state(int fmt, int frame)
{
    slim_film_fmt = fmt;
    slim_film_frames = 0;
    slim_film_frame_set(fmt, frame);
}

int main(void)
{
    /* 1. every format and frame: apply -> menu state -> sync gives back the same format and frame */
    for (int f = 0; f < FILM_FORMAT_COUNT; f++)
        for (int k = 0; k < film_formats[f].count; k++)
        {
            set_state(f, k);
            apply_calls = 0;
            slim_film_apply(f);
            CHECK(apply_calls == 1, "format %d frame %d: apply must switch the mode once", f, k);
            int ro = slim_film_menu_readout();
            CHECK(ro == film_frames[film_frame_index(f, k)].readout, "format %d frame %d: menu readout %d, table says %d",
                  f, k, ro, film_frames[film_frame_index(f, k)].readout);
            /* lose the saved choice, as after a reboot with a different group: sync keeps what it can */
            int fmt = slim_film_sync();
            CHECK(fmt == f && slim_film_frame_get(f) == k, "format %d frame %d: sync gives format %d frame %d", f, k, fmt, slim_film_frame_get(f));
            CHECK(slim_video_standard() == film_is_video(f), "format %d: standard flag wrong", f);
        }

    /* 2. a saved frame that belongs to another readout is repaired by sync */
    set_state(9, 0);                       /* 1/2.3": frame 0 is 16:9 (2.5K) */
    slim_mode_ui = 0; slim_1x1_ar = 3; slim_unified_preset = 0;     /* but the camera is on 1280p */
    CHECK(slim_film_sync() == 4, "1/2.3\" cannot use 1280p: the first format that can (S8)");
    set_state(8, 1);                       /* 1/2": both frames are on 1280p, so the saved frame is kept */
    CHECK(slim_film_sync() == 8 && slim_film_frame_get(8) == 1, "1/2\" 4:3 stays on 1280p");

    /* 3. leftover legacy modes are not film formats */
    slim_mode_ui = 0; slim_1x1_ar = 1; slim_unified_preset = 1;      /* 2.8K Higher */
    CHECK(slim_film_sync() == -1, "2.8K is not a film format");
    slim_mode_ui = 2; crop_preset_ar_menu = 0;                       /* 3x3 16:9 high fps */
    CHECK(slim_film_sync() == -1, "3x3 high fps is not a film format");
    slim_mode_ui = 1;
    CHECK(slim_film_sync() == -1, "1x3 is not a film format");
    slim_mode_ui = 3;
    CHECK(slim_film_sync() == -1, "Full-Res LV is not a film format");

    /* 3b. frame rates: bit0 = 23.976, bit1 = 25, bit2 = 30 (29.97 in VIDEO), bit3 = 18.
     *     FILM never offers 30 or 29.97; VIDEO never offers 18; VIDEO offers 29.97 only on the
     *     2.5K and 1280p frames (1440p / 1620p do not run it) */
    for (int f = 0; f < FILM_FORMAT_COUNT; f++)
        for (int k = 0; k < film_formats[f].count; k++)
        {
            set_state(f, k);
            slim_film_apply(f);
            g_preset = (slim_mode_ui == 2) ? CROP_PRESET_3X3 : CROP_PRESET_1X1;
            int mask = slim_crop_fps_mask();
            int ro = film_frames[film_frame_index(f, k)].readout;
            CHECK(mask & 1, "format %d frame %d: 23.976 must always be offered", f, k);
            /* 36 fps (bit 4): every A35 frame (3x3 readout), nothing else.
             * 48 fps (bit 5): only the A35 frames that fit in the 976-row readout. */
            int a35 = (ro == FILM_RO_3X3);
            int fits976 = a35 && film_frames[film_frame_index(f, k)].h <= 958;
            CHECK(!!(mask & 0x10) == a35, "format %d frame %d: 36 fps offered=%d, expected %d (mask %x)", f, k, !!(mask & 0x10), a35, mask);
            CHECK(!!(mask & 0x20) == fits976, "format %d frame %d: 48 fps offered=%d, expected %d (mask %x)", f, k, !!(mask & 0x20), fits976, mask);
            if (!film_is_video(f))
                CHECK(!(mask & 4), "FILM format %d frame %d must not offer 30 / 29.97 (mask %x)", f, k, mask);
            else
            {
                CHECK(!(mask & 8), "VIDEO format %d frame %d must not offer 18 fps (mask %x)", f, k, mask);
                /* 29.97 is offered on the 2.5K frames and on 1280p; 1440p does not run it */
                int ok30 = (ro == FILM_RO_25K || ro == FILM_RO_1280);
                CHECK(!!(mask & 4) == ok30, "VIDEO format %d frame %d: 29.97 offered=%d, expected %d (mask %x)", f, k, !!(mask & 4), ok30, mask);
            }
        }

    /* 3c. Readout Speed (tenths of ms): recorded height x line time, line time from the readout.
     *     Independent expectations worked out by hand: rows * clocks / 3200, rounded. */
    {
        static const struct { int w, h, fps, x10; } exp[] = {
            { 1696,  954, 0, 157 }, { 1696,  916, 0, 151 }, { 1696,  722, 0, 119 },   /* A35, 528 clocks */
            { 1376, 1152, 0, 190 }, { 1536, 1152, 0, 190 },                          /* anamorphic */
            { 1696,  954, 1, 191 },                                                  /* 25 fps: 640 clocks */
            { 1696,  954, 4, 157 },                                                  /* 36 fps: 528 clocks */
            { 1696,  954, 5, 155 }, { 1696,  722, 5, 117 },                          /* 48 fps: 520 clocks */
            { 2912, 1238, 0, 333 },                                                  /* S16, 3K 860 clocks */
            { 2384, 1340, 0, 307 },                                                  /* 16mm, 1440p 732 clocks */
            { 1344,  930, 0, 164 },                                                  /* S8, 1280p 566 clocks */
            { 3056, 1300, 0, 349 },                                                  /* 1", 3K */
            { 1424,  800, 0, 179 },                                                  /* 1/2.3", 2.5K 716 clocks */
        };
        for (unsigned n = 0; n < sizeof(exp) / sizeof(exp[0]); n++)
        {
            int found = 0;
            for (int f = 0; f < FILM_FORMAT_COUNT && !found; f++)
                for (int k = 0; k < film_formats[f].count && !found; k++)
                {
                    int i = film_frame_index(f, k);
                    if (film_frames[i].w != exp[n].w || film_frames[i].h != exp[n].h)
                        continue;
                    found = 1;
                    set_state(f, k);
                    slim_film_apply(f);
                    crop_preset_fps_menu = exp[n].fps;
                    CHECK(slim_readout_x10() == exp[n].x10, "%dx%d at fps index %d: readout %d, expected %d",
                          exp[n].w, exp[n].h, exp[n].fps, slim_readout_x10(), exp[n].x10);
                }
            CHECK(found, "readout test: no frame %dx%d", exp[n].w, exp[n].h);
        }
        crop_preset_fps_menu = 0;
        /* every format and frame has a readout time; none is zero or absurd */
        for (int f = 0; f < FILM_FORMAT_COUNT; f++)
            for (int k = 0; k < film_formats[f].count; k++)
            {
                set_state(f, k);
                slim_film_apply(f);
                CHECK(slim_readout_x10() >= 80 && slim_readout_x10() <= 400, "format %d frame %d: readout %d out of range", f, k, slim_readout_x10());
            }
    }

    /* 4. with SLIM off, Film Format 0 is the FILM standard */
    slim_film_fmt = 0; slim_film_frames = 0;
    CHECK(!slim_video_standard(), "format 0 is FILM");

    /* 5. SLIM: no menu state is a film format, and the saved film choice is left alone */
    std_slim = 1;
    set_state(9, 1);
    {
        int saved_frames = slim_film_frames;
        for (int m = 0; m <= 3; m++)
            for (int a = 0; a <= 4; a++)
                for (int p = 0; p <= 2; p++)
                {
                    slim_mode_ui = m; slim_1x1_ar = a; crop_preset_ar_menu = a; slim_unified_preset = p;
                    CHECK(slim_film_menu_readout() == -1, "SLIM mode %d ar %d preset %d: menu readout must not be a film readout", m, a, p);
                    CHECK(slim_film_sync() == -1, "SLIM mode %d ar %d preset %d: must not be a film format", m, a, p);
                    CHECK(slim_film_frame_height() == 0, "SLIM mode %d ar %d preset %d: no film frame height", m, a, p);
                }
        CHECK(slim_film_fmt == 9 && slim_film_frames == saved_frames, "SLIM must leave the saved film format and frame alone");
        CHECK(!slim_video_standard(), "SLIM is not the VIDEO standard, even with a VIDEO format saved");
    }

    /* 5b. SLIM frame rates are the slim GUI's: bit0 = 23.976, bit1 = 25, bit2 = 30, no 18 / 36 / 48 */
    {
        static const struct { int preset, mode, ar, tier, res1x3, mask; const char * what; } fr[] = {
            { CROP_PRESET_3X3, 2, 4, 0, 0, 0x7, "3x3 3:2: 23.976 / 25 / 30" },
            { CROP_PRESET_3X3, 2, 0, 0, 0, 0x0, "3x3 16:9 high fps: one fixed rate" },
            { CROP_PRESET_1X1, 0, 0, 0, 0, 0x7, "1x1 2.33:1 (2.5K): 23.976 / 25 / 30" },
            { CROP_PRESET_1X1, 0, 1, 0, 0, 0x1, "1x1 2.35:1 Highest (3K): 23.976" },
            { CROP_PRESET_1X1, 0, 1, 1, 0, 0x3, "1x1 2.35:1 Higher (2.8K): 23.976 / 25" },
            { CROP_PRESET_1X1, 0, 2, 0, 0, 0x3, "1x1 16:9 (1440p): 23.976 / 25" },
            { CROP_PRESET_1X1, 0, 3, 0, 0, 0x3, "1x1 3:2 (1280p): 23.976 / 25" },
            { CROP_PRESET_1X1, 0, 4, 0, 0, 0x1, "1x1 4:3 (1620p): 23.976" },
            { CROP_PRESET_1X3, 1, 0, 0, 0, 0x1, "1x3 Highest: 23.976 only" },
            { CROP_PRESET_1X3, 1, 0, 1, 1, 0x1, "1x3 Higher 16:9: 23.976 only" },
            { CROP_PRESET_1X3, 1, 3, 1, 1, 0x3, "1x3 Higher 2.35:1: 23.976 / 25" },
            { CROP_PRESET_1X3, 1, 3, 2, 2, 0x3, "1x3 Medium 2.35:1: 23.976 / 25" },
        };
        for (unsigned n = 0; n < sizeof(fr) / sizeof(fr[0]); n++)
        {
            g_preset = fr[n].preset; slim_mode_ui = fr[n].mode;
            slim_1x1_ar = fr[n].ar; crop_preset_ar_menu = fr[n].ar;
            slim_unified_preset = fr[n].tier; crop_preset_1x3_res_menu = fr[n].res1x3;
            CHECK(slim_crop_fps_mask() == fr[n].mask, "SLIM %s: mask %x, expected %x", fr[n].what, slim_crop_fps_mask(), fr[n].mask);
        }
    }

    /* 5c. SLIM Preset row: 1x3 has three tiers, 1x1 2.35:1 has two, everything else one */
    slim_mode_ui = 1;
    CHECK(slim_preset_choice_count() == 3, "SLIM 1x3: Highest / Higher / Medium");
    slim_mode_ui = 0; slim_1x1_ar = 1;
    CHECK(slim_preset_choice_count() == 2, "SLIM 1x1 2.35:1: Highest / Higher");
    slim_1x1_ar = 2;
    CHECK(slim_preset_choice_count() == 1, "SLIM 1x1 16:9: Highest only");
    slim_mode_ui = 2;
    CHECK(slim_preset_choice_count() == 1, "SLIM 3x3: Highest only");
    slim_mode_ui = 3;
    CHECK(slim_preset_choice_count() == 1, "SLIM LV: one size");

    /* 6. Standard row: FILM -> VIDEO -> SLIM -> FILM; FILM and VIDEO come back at their last format */
    std_slim = 0;
    set_state(3, 0);                          /* 16mm */
    slim_film_apply(3);
    slim_crop_standard_select(0, 1);
    CHECK(!std_slim && slim_film_fmt == FILM_FILM_COUNT, "FILM -> VIDEO starts at the first VIDEO size, got %d", slim_film_fmt);
    slim_crop_standard_select(0, 1);
    CHECK(std_slim, "VIDEO -> SLIM");
    CHECK(slim_film_fmt == FILM_FILM_COUNT, "entering SLIM keeps the saved format");
    slim_crop_standard_select(0, 1);
    CHECK(!std_slim && slim_film_fmt == 3, "SLIM -> FILM returns to 16mm, got %d", slim_film_fmt);
    slim_crop_standard_select(0, -1);
    CHECK(std_slim, "FILM -> SLIM backwards");
    slim_crop_standard_select(0, -1);
    CHECK(!std_slim && slim_film_fmt == FILM_FILM_COUNT, "SLIM -> VIDEO backwards returns to the VIDEO size, got %d", slim_film_fmt);

    /* 6b. a film-only frame rate does not survive the switch to SLIM */
    std_slim = 0;
    set_state(4, 0);                          /* S8: 1280p */
    slim_film_apply(4);
    g_preset = CROP_PRESET_1X1;
    crop_preset_fps_menu = 3;                 /* 18 fps */
    CHECK(slim_crop_fps_mask() & 8, "S8 offers 18 fps");
    slim_crop_standard_select(0, -1);         /* FILM -> SLIM */
    CHECK(std_slim && crop_preset_fps_menu == 0, "18 fps becomes 23.976 in SLIM, got index %d", crop_preset_fps_menu);
    std_slim = 0;

    /* 7. INFO Button: stepping skips the hidden Framing (5) and wraps through Dual ISO (7) */
    INFO_button = 4;
    slim_info_button_select(0, 1);
    CHECK(INFO_button == 6, "INFO False Color -> Quick Panel skips Framing, got %d", INFO_button);
    slim_info_button_select(0, 1);
    CHECK(INFO_button == 7, "INFO Quick Panel -> Dual ISO, got %d", INFO_button);
    slim_info_button_select(0, 1);
    CHECK(INFO_button == 0, "INFO Dual ISO -> OFF, got %d", INFO_button);
    slim_info_button_select(0, -1);
    CHECK(INFO_button == 7, "INFO OFF backwards -> Dual ISO, got %d", INFO_button);
    slim_info_button_select(0, -1);
    slim_info_button_select(0, -1);
    CHECK(INFO_button == 4, "INFO backwards skips Framing, got %d", INFO_button);

    printf("menu_tests: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
