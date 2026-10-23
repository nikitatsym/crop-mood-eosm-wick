/* DIAG (temporary): pure integer math for the diagnostic log; no camera calls, so it is
 * unit-tested on the host (tests/diag_test.c).  Remove together with diag.inc. */
#ifndef DIAG_MATH_H
#define DIAG_MATH_H

#include <stdint.h>

static unsigned dg_isqrt(unsigned v)
{
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b)
    {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* High-pass of a[0..n-1] (values are luma x256): each element minus the mean of its 3 left and 3
 * right neighbours.  Returns the rms and the largest absolute deviation, both in luma x100. */
static void dg_hp_stats(const int *a, int n, int *rms_x100, int *max_x100)
{
    *rms_x100 = 0; *max_x100 = 0;
    if (n < 8) return;
    float sum = 0;
    unsigned mx = 0;
    int cnt = 0;
    for (int i = 3; i < n - 3; i++)
    {
        int nb = a[i-3] + a[i-2] + a[i-1] + a[i+1] + a[i+2] + a[i+3];
        int hp = a[i] - nb / 6;
        unsigned ah = hp < 0 ? -hp : hp;
        if (ah > mx) mx = ah;
        sum += (float)hp * (float)hp;
        cnt++;
    }
    float lvl2 = sum / (float)cnt / 65536.0f;               /* luma levels squared */
    *rms_x100 = (int)dg_isqrt((unsigned)(lvl2 * 10000.0f));  /* sqrt(levels^2 * 10^4) = levels x100 */
    *max_x100 = (int)(mx * 100 / 256);
}

struct dg_streaks
{
    int mean_x100;      /* mean luma of the sampled area */
    int col_rms, col_max;   /* column-to-column variation (vertical streaks), luma x100 */
    int row_rms, row_max;   /* row-to-row variation (horizontal bands), luma x100 */
    int ncols, nrows;
};

/* UYVY 4:2:2 picture, luma at odd bytes.  Central 80 %.  Columns are averaged over every 4th row,
 * rows over every 4th column, so the camera reads ~1/4 of the pixels twice. */
static void dg_streak_measure(const uint8_t *vram, int width, int pitch, int height,
                              int *colbuf, int *rowbuf, int bufn, struct dg_streaks *out)
{
    out->mean_x100 = out->col_rms = out->col_max = out->row_rms = out->row_max = 0;
    out->ncols = out->nrows = 0;
    if (!vram || width < 64 || height < 64) return;

    int x0 = width / 10, x1 = width - width / 10;
    int y0 = height / 10, y1 = height - height / 10;
    int nc = x1 - x0, nr = y1 - y0;
    if (nc > bufn) { nc = bufn; x1 = x0 + nc; }
    if (nr > bufn) { nr = bufn; y1 = y0 + nr; }

    int rstep = 4, cstep = 4;
    int nrs = (nr + rstep - 1) / rstep;
    int ncs = (nc + cstep - 1) / cstep;

    for (int x = 0; x < nc; x++) colbuf[x] = 0;
    for (int y = 0; y < nr; y++) rowbuf[y] = 0;
    unsigned total = 0;

    for (int y = y0; y < y1; y++)
    {
        const uint8_t *line = vram + (unsigned)y * pitch;
        if (((y - y0) % rstep) == 0)
            for (int x = x0; x < x1; x++)
                colbuf[x - x0] += line[2 * x + 1];
        for (int x = x0; x < x1; x += cstep)
        {
            int v = line[2 * x + 1];
            rowbuf[y - y0] += v;
            total += v;
        }
    }
    for (int x = 0; x < nc; x++) colbuf[x] = colbuf[x] * 256 / nrs;
    for (int y = 0; y < nr; y++) rowbuf[y] = rowbuf[y] * 256 / ncs;

    out->ncols = nc; out->nrows = nr;
    out->mean_x100 = (int)((total * 100u) / ((unsigned)nr * ncs));
    dg_hp_stats(colbuf, nc, &out->col_rms, &out->col_max);
    dg_hp_stats(rowbuf, nr, &out->row_rms, &out->row_max);
}

#endif
