#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../modules/crop_rec/diag_math.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define W 960
#define H 540
static uint8_t img[W * H * 2];
static int cb[2048], rb[2048];
static void fill(int base, int colamp, int colperiod, int rowamp, int rowperiod, int noise)
{
    srand(1);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            int v = base + ((x % colperiod) == 0 ? colamp : 0) + ((y % rowperiod) == 0 ? rowamp : 0)
                  + (noise ? rand() % (2 * noise + 1) - noise : 0);
            img[(y * W + x) * 2] = 128; img[(y * W + x) * 2 + 1] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
}
int main(void)
{
    struct dg_streaks s;
    CHECK(dg_isqrt(0) == 0 && dg_isqrt(1) == 1 && dg_isqrt(99) == 9 && dg_isqrt(100) == 10 && dg_isqrt(4000000) == 2000, "isqrt");

    fill(100, 0, 7, 0, 7, 0);
    dg_streak_measure(img, W, W * 2, H, cb, rb, 2048, &s);
    CHECK(s.col_rms == 0 && s.row_rms == 0, "flat image should read 0, got %d %d", s.col_rms, s.row_rms);
    CHECK(s.mean_x100 == 10000, "mean %d", s.mean_x100);

    fill(100, 8, 5, 0, 7, 0);       /* every 5th column 8 levels brighter */
    dg_streak_measure(img, W, W * 2, H, cb, rb, 2048, &s);
    CHECK(s.col_rms > 200 && s.col_rms < 500, "column streak rms %d", s.col_rms);
    CHECK(s.col_max >= 600 && s.col_max <= 900, "column streak max %d", s.col_max);
    CHECK(s.row_rms == 0, "no row bands expected, got %d", s.row_rms);

    fill(100, 0, 7, 6, 3, 0);       /* every 3rd row 6 levels brighter */
    dg_streak_measure(img, W, W * 2, H, cb, rb, 2048, &s);
    CHECK(s.row_rms > 100 && s.row_max >= 400, "row band rms %d max %d", s.row_rms, s.row_max);
    CHECK(s.col_rms < 150, "col leak %d", s.col_rms);

    fill(100, 0, 7, 0, 7, 10);      /* pure random noise: both metrics small, equal-ish */
    dg_streak_measure(img, W, W * 2, H, cb, rb, 2048, &s);
    CHECK(s.col_rms < 120 && s.row_rms < 150, "noise only col %d row %d", s.col_rms, s.row_rms);

    fill(100, 16, 5, 0, 7, 0);      /* double amplitude -> about double rms */
    dg_streak_measure(img, W, W * 2, H, cb, rb, 2048, &s);
    int r2 = s.col_rms;
    fill(100, 8, 5, 0, 7, 0);
    dg_streak_measure(img, W, W * 2, H, cb, rb, 2048, &s);
    CHECK(r2 > s.col_rms * 18 / 10 && r2 < s.col_rms * 22 / 10, "linearity %d vs %d", r2, s.col_rms);

    dg_streak_measure(NULL, W, W * 2, H, cb, rb, 2048, &s);
    CHECK(s.col_rms == 0, "null");
    printf(fails ? "diag tests: %d FAILED\n" : "diag tests: all passed\n", fails);
    return fails != 0;
}
