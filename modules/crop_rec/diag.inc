/* ==================================================================================================
 * DIAG (temporary): diagnostic log, included by crop_rec.c.  Remove by deleting this file,
 * diag_math.h, tests/diag_test.c, the "DIAG" lines in crop_rec.c and mlv_lite_diag_get() in mlv_lite.c.
 *
 * It only READS: camera state, Canon's register shadow copy, the live-view picture and the
 * recorder's counters.  It never changes a setting.  Output: ML/LOGS/DIAGnn.TXT (one per boot).
 *
 *   ML/DIAG.OFF   (empty file)  switches the log off
 *   ML/DIAGLIVE   (empty file)  also writes to the card every 2 s while recording (for crashes;
 *                               adds a little card traffic, so it is off unless asked for)
 *
 * Line types:  H header | E event | C configuration | R recording/second stats | S streak
 *              measurement of the live-view picture | A/G register snapshots
 * ================================================================================================== */

#include "diag_math.h"
#include <stdarg.h>
#include <version.h>

/* mlv_lite (temporary export) */
extern WEAK_FUNC(ret_0) void mlv_lite_diag_get(int *o);

#define DG_BUF_SIZE   (96 * 1024)
#define DG_ADTG_SLOTS 512
#define DG_MAX_SNAPS  24

static char *dg_buf = 0;
static int   dg_len = 0;
static int   dg_state = 0;          /* 0 = not started, 1 = running, 2 = off / failed */
static int   dg_live = 0;
static int   dg_overflow = 0;
static char  dg_name[48];
static int   dg_snaps = 0;
static int   dg_last_flush = 0;

/* latest ADTG / CMOS values as applied by crop_rec (after its overrides) */
struct dg_adtg_slot { uint16_t reg; uint16_t val; uint8_t dst; uint8_t used; };
static struct dg_adtg_slot dg_adtg_tab[DG_ADTG_SLOTS];
static uint16_t dg_cmos[8];
static volatile int dg_cmos_seen = 0;
static volatile int dg_adtg_calls = 0;
static volatile int dg_cmos_calls = 0;

/* live-view timing, from the vsync callback */
static volatile int dg_vs_count = 0;
static volatile int dg_vs_gap_min = 0x7FFFFFFF;
static volatile int dg_vs_gap_max = 0;
static volatile int dg_vs_total = 0;
static volatile uint64_t dg_vs_last_us = 0;

/* the line time stamps are ms since boot */
static void dg_printf(const char *fmt, ...)
{
    if (!dg_buf) return;
    int room = DG_BUF_SIZE - dg_len;
    if (room < 200) { dg_overflow = 1; return; }
    int now = get_ms_clock();
    int n = snprintf(dg_buf + dg_len, room, "%d.%03d ", now / 1000, now % 1000);
    if (n < 0 || n >= room) return;
    dg_len += n;
    room -= n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(dg_buf + dg_len, room, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= room) n = room - 1;
    dg_len += n;
}

static void dg_flush(void)
{
    if (!dg_buf || !dg_len) return;
    FILE *f = FIO_CreateFileOrAppend(dg_name);
    if (f)
    {
        FIO_WriteFile(f, dg_buf, dg_len);
        FIO_CloseFile(f);
    }
    dg_len = 0;
    dg_last_flush = get_ms_clock();
}

/* ---- hooks: called from the ADTG / CMOS hooks with the final register lists ------------------ */
static void dg_capture_adtg(int dst, const uint32_t *list)
{
    dg_adtg_calls++;
    for (int n = 0; list[n] != 0xFFFFFFFF && n < 512; n++)
    {
        uint16_t reg = list[n] >> 16;
        uint16_t val = list[n] & 0xFFFF;
        unsigned h = ((reg * 31u) + dst * 7u) & (DG_ADTG_SLOTS - 1);
        for (int k = 0; k < 8; k++, h = (h + 1) & (DG_ADTG_SLOTS - 1))
        {
            struct dg_adtg_slot *s = &dg_adtg_tab[h];
            if (!s->used || (s->reg == reg && s->dst == (uint8_t)dst))
            {
                s->reg = reg; s->val = val; s->dst = dst; s->used = 1;
                break;
            }
        }
    }
}

static void dg_capture_cmos(const uint16_t *list)
{
    dg_cmos_calls++;
    for (int n = 0; list[n] != 0xFFFF && n < 64; n++)
    {
        int r = (list[n] >> 12) & 7;
        dg_cmos[r] = list[n] & 0xFFF;
        dg_cmos_seen |= 1 << r;
    }
}

static unsigned int dg_vsync_cbr(unsigned int unused)
{
    uint64_t now = get_us_clock();
    uint64_t last = dg_vs_last_us;
    dg_vs_last_us = now;
    dg_vs_count++;
    dg_vs_total++;
    if (last)
    {
        int gap = (int)(now - last);
        if (gap < dg_vs_gap_min) dg_vs_gap_min = gap;
        if (gap > dg_vs_gap_max) dg_vs_gap_max = gap;
    }
    return 0;
}

/* ---- state description ------------------------------------------------------------------------ */
static int dg_output_type(void)
{
    return PathDriveMode ? (int)PathDriveMode->OutputType : -1;
}

static void dg_log_config(const char *tag)
{
    int mv[24] = {0};
    mlv_lite_diag_get(mv);
    int ta = shamem_read(0xC0F06008) & 0xFFFF;
    int tb = shamem_read(0xC0F06014) & 0xFFFF;
    dg_printf("C %s fmt=%d std=%s preset=%d res1x1=%d ar=%d fpsidx=%d bits=%d bright=%d a35short=%d "
              "lvdisp=%d out=%d lv=%d rec=%d outfmt=%d\n",
        tag, crop_rec_film_format(), slim_video_standard() ? "VIDEO" : "FILM", (int)crop_preset,
        (int)crop_preset_1x1_res, (int)crop_preset_ar, (int)crop_preset_fps, (int)bit_depth_analog,
        (int)brighten_lv_method, (int)a35_short_readout, (int)lv_dispsize, dg_output_type(), (int)lv,
        (int)RECORDING, mv[14]);
    dg_printf("C %s timerA_set=%u timerB_set=%u timerA_reg=%d timerB_reg=%d fps_x1000=%d readout_x10ms=%d "
              "raw=%ux%u preview=%ux%u\n",
        tag, TimerA, TimerB, ta, tb, fps_get_current_x1000(), slim_readout_x10(),
        (unsigned)RAW_H, (unsigned)RAW_V, (unsigned)Preview_H, (unsigned)Preview_V);
    dg_printf("C %s iso=%d iso_analog_raw=%d shutter_raw=%d shutter_ms_x10=%d aperture=%d wb=%d kelvin=%d "
              "picstyle=%d lens=\"%s\" focal=%d lens_id=%d\n",
        tag, lens_info.iso, lens_info.iso_analog_raw, lens_info.raw_shutter,
        raw2shutter_ms(lens_info.raw_shutter) / 100, lens_info.aperture, lens_info.wb_mode, lens_info.kelvin,
        lens_info.picstyle, lens_info.name, lens_info.focal_len, lens_info.lens_id);
    dg_printf("C %s mlv: outfmt=%d small_hacks=%d preview=%d pre_rec=%d span=%d srm=%d kill_gd=%d res=%dx%d "
              "max_frame=%d\n",
        tag, mv[14], mv[15], mv[16], mv[17], mv[18], mv[19], mv[21], mv[9], mv[10], mv[20]);
}

static void dg_log_header(void)
{
    struct card_info *c = get_shooting_card();
    dg_printf("H diag v1 camera=\"%s\" fw=\"%s\" build_version=\"%s\" build_date=\"%s\" compiled=\"%s %s\"\n",
        camera_model, firmware_version, build_version, build_date, __DATE__, __TIME__);
    dg_printf("H card type=%s maker=%s model=%s cluster=%d free_32k=%d live_mode=%d\n",
        c && c->type ? c->type : "?", c && c->maker ? c->maker : "?", c && c->model ? c->model : "?",
        c ? c->cluster_size : 0, c ? get_free_space_32k(c) : 0, dg_live);
    struct tm tm;
    LoadCalendarFromRTC(&tm);
    dg_printf("H clock %04d-%02d-%02d %02d:%02d:%02d\n",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    dg_printf("H mem free_alloc=%d\n", GetFreeMemForAllocateMemory());
}

/* ---- register snapshot (written straight to the card; ~30-60 KB) ------------------------------- */
static void dg_snapshot(const char *why)
{
    static const uint32_t pages[] = {
        0xC0F04000, 0xC0F05000, 0xC0F06000, 0xC0F07000, 0xC0F08000, 0xC0F09000, 0xC0F11000,
        0xC0F1A000, 0xC0F26000, 0xC0F35000, 0xC0F37000, 0xC0F38000, 0xC0F3A000, 0xC0F3B000,
        0xC0F42000,
    };
    if (dg_snaps >= DG_MAX_SNAPS) return;
    dg_snaps++;

    dg_printf("E snapshot #%d (%s)\n", dg_snaps, why);
    dg_log_config("snap");
    dg_flush();

    const int size = 4096;
    char *b = malloc(size);
    if (!b) { dg_printf("E snapshot: no memory\n"); return; }
    FILE *f = FIO_CreateFileOrAppend(dg_name);
    if (!f) { free(b); return; }

    int len = snprintf(b, size, "G --- engine registers (shadow copy, non-zero) snapshot #%d ---\n", dg_snaps);
    for (unsigned k = 0; k < COUNT(pages); k++)
    {
        for (uint32_t off = 0; off < 0x1000; off += 4)
        {
            uint32_t a = pages[k] + off;
            uint32_t v = shamem_read(a);
            if (!v) continue;
            len += snprintf(b + len, size - len, "G %08X %08X\n", a, v);
            if (len > size - 64) { FIO_WriteFile(f, b, len); len = 0; }
        }
    }
    len += snprintf(b + len, size - len, "A --- ADTG registers as applied (dst reg value) ---\n");
    for (int i = 0; i < DG_ADTG_SLOTS; i++)
    {
        struct dg_adtg_slot *s = &dg_adtg_tab[i];
        if (!s->used) continue;
        len += snprintf(b + len, size - len, "A %d %04X %04X\n", s->dst, s->reg, s->val);
        if (len > size - 64) { FIO_WriteFile(f, b, len); len = 0; }
    }
    len += snprintf(b + len, size - len, "A cmos seen=%02X [0]=%03X [1]=%03X [2]=%03X [3]=%03X [4]=%03X [5]=%03X [6]=%03X [7]=%03X\n",
        dg_cmos_seen, dg_cmos[0], dg_cmos[1], dg_cmos[2], dg_cmos[3], dg_cmos[4], dg_cmos[5], dg_cmos[6], dg_cmos[7]);
    FIO_WriteFile(f, b, len);
    FIO_CloseFile(f);
    free(b);
}

/* ---- live-view picture streak measurement ------------------------------------------------------ */
static int dg_colbuf[1024];
static int dg_rowbuf[1024];

static void dg_streak(void)
{
    struct vram_info *v = get_yuv422_vram();
    if (!v || !v->vram || v->width < 64 || v->height < 64) return;
    int t0 = get_ms_clock();
    struct dg_streaks s;
    dg_streak_measure((const uint8_t *)v->vram, v->width, v->pitch, v->height, dg_colbuf, dg_rowbuf, 1024, &s);
    int ms = get_ms_clock() - t0;
    dg_printf("S mean=%d.%02d col_rms=%d.%02d col_max=%d.%02d row_rms=%d.%02d row_max=%d.%02d "
              "cols=%d rows=%d pic=%dx%d bits=%d bright=%d iso=%d fmt=%d fpsidx=%d rec=%d took_ms=%d\n",
        s.mean_x100 / 100, s.mean_x100 % 100, s.col_rms / 100, s.col_rms % 100, s.col_max / 100, s.col_max % 100,
        s.row_rms / 100, s.row_rms % 100, s.row_max / 100, s.row_max % 100, s.ncols, s.nrows,
        v->width, v->height, (int)bit_depth_analog, (int)brighten_lv_method, lens_info.iso,
        crop_rec_film_format(), (int)crop_preset_fps, (int)RECORDING, ms);
}

/* ---- the periodic task, called from crop_rec_polling_cbr ---------------------------------------- */
static void dg_tick(void)
{
    if (dg_state == 2) return;
    int now = get_ms_clock();

    if (dg_state == 0)
    {
        if (now < 5000) return;             /* let the camera finish booting */
        dg_state = 2;
        if (is_file("ML/DIAG.OFF")) return;
        dg_live = is_file("ML/DIAGLIVE");
        dg_buf = malloc(DG_BUF_SIZE);
        if (!dg_buf) return;
        FIO_CreateDirectory("ML/LOGS");
        if (get_numbered_file_name("ML/LOGS/DIAG%02d.TXT", 99, dg_name, sizeof(dg_name)) < 0)
            snprintf(dg_name, sizeof(dg_name), "ML/LOGS/DIAG00.TXT");
        dg_state = 1;
        dg_log_header();
        dg_flush();
    }

    static int last_poll = 0, poll_gap_max = 0;
    static int last_stat = 0, last_streak = 0;
    static int was_rec = 0, was_lv = 0, was_menu = 0;
    static int rec_t0 = 0, rec_snap_done = 0;
    static int settle_t0 = 0, settle_pending = 1;
    static int fp[12], fp_old[12];
    static int vs_last_total = 0, vs_t0 = 0;
    static int fr_last = 0;

    if (last_poll)
    {
        int gap = now - last_poll;
        if (gap > poll_gap_max) poll_gap_max = gap;
    }
    last_poll = now;

    int rec = RECORDING ? 1 : 0;
    int menu = gui_menu_shown() ? 1 : 0;

    if (menu != was_menu) { dg_printf("E menu %s\n", menu ? "open" : "closed"); was_menu = menu; }
    if (lv != was_lv)
    {
        dg_printf("E liveview %s\n", lv ? "on" : "off");
        was_lv = lv;
        settle_pending = 1; settle_t0 = now;
        vs_t0 = 0;
    }

    if (rec && !was_rec)
    {
        dg_printf("E RECORDING START\n");
        dg_log_config("rec_start");
        rec_t0 = now; rec_snap_done = 0; fr_last = 0;
        vs_t0 = 0; poll_gap_max = 0;
        dg_vs_gap_min = 0x7FFFFFFF; dg_vs_gap_max = 0;
    }
    if (!rec && was_rec)
    {
        int mv[24] = {0};
        mlv_lite_diag_get(mv);
        dg_printf("E RECORDING STOP after %d ms: frames=%d skipped=%d buffer_full=%d written_mb=%d\n",
            now - rec_t0, mv[1], mv[2], mv[3], mv[11]);
        dg_log_config("rec_stop");
        dg_flush();
    }
    was_rec = rec;

    /* what the picture depends on: when it changes (and stays still for 3 s) take a snapshot */
    fp[0] = crop_rec_film_format(); fp[1] = slim_video_standard(); fp[2] = crop_preset;
    fp[3] = crop_preset_1x1_res;    fp[4] = crop_preset_ar;        fp[5] = crop_preset_fps;
    fp[6] = bit_depth_analog;       fp[7] = brighten_lv_method;    fp[8] = lv_dispsize;
    fp[9] = lens_info.iso;          fp[10] = dg_output_type();     fp[11] = a35_short_readout;
    if (memcmp(fp, fp_old, sizeof(fp)))
    {
        memcpy(fp_old, fp, sizeof(fp));
        settle_pending = 1; settle_t0 = now;
    }
    if (settle_pending && lv && !menu && !rec && now - settle_t0 >= 3000)
    {
        settle_pending = 0;
        dg_snapshot("settings changed");
    }

    /* one snapshot shortly after recording started (this is the state while recording) */
    if (rec && !rec_snap_done && now - rec_t0 >= 3000)
    {
        rec_snap_done = 1;
        dg_snapshot("while recording");
    }

    /* per-second (recording) / 5-second (idle) statistics */
    int period = rec ? 1000 : 5000;
    if (lv && now - last_stat >= period)
    {
        last_stat = now;
        if (!vs_t0) { vs_t0 = now; vs_last_total = dg_vs_total; dg_vs_count = 0; }
        int dt = now - vs_t0;
        int vs = dg_vs_count;
        int vfps = dt > 0 ? vs * 100000 / dt : 0;       /* measured live-view frames per second x100 */
        int mv[24] = {0};
        mlv_lite_diag_get(mv);
        int dfr = mv[1] - fr_last; fr_last = mv[1];
        dg_printf("R rec=%d vs_fps=%d.%02d vsync_gap_us min=%d max=%d poll_gap_ms=%d | frames=%d (+%d) skipped=%d "
                  "buf_full=%d free_slots=%d/%d queue=%d wr_speed=%d.%02dMB/s comp=%d%% written_mb=%d wr_ms=%d idle_ms=%d | "
                  "mem_free=%d temp=%d bat_bars=%d card_free_32k=%d adtg_calls=%d cmos_calls=%d\n",
            rec, vfps / 100, vfps % 100,
            dg_vs_gap_min == 0x7FFFFFFF ? 0 : dg_vs_gap_min, dg_vs_gap_max, poll_gap_max,
            mv[1], dfr, mv[2], mv[3], mv[4], mv[5], mv[6], mv[7] / 100, mv[7] % 100, mv[8], mv[11], mv[12], mv[13],
            GetFreeMemForAllocateMemory(), efic_temp, battery_level_bars,
            get_free_space_32k(get_shooting_card()), dg_adtg_calls, dg_cmos_calls);
        dg_vs_count = 0; vs_t0 = now;
        dg_vs_gap_min = 0x7FFFFFFF; dg_vs_gap_max = 0; poll_gap_max = 0;
        dg_adtg_calls = 0; dg_cmos_calls = 0;
        (void)vs_last_total;
    }

    /* streak numbers of the live-view picture every 2 s idle / 4 s while recording */
    if (lv && !menu && now - last_streak >= (rec ? 4000 : 2000))
    {
        last_streak = now;
        dg_streak();
    }

    /* write to the card: when idle every 5 s; while recording only in "live" mode */
    if (dg_len)
    {
        if (!rec && now - dg_last_flush >= 5000) dg_flush();
        else if (rec && dg_live && now - dg_last_flush >= 2000) dg_flush();
        else if (dg_len > DG_BUF_SIZE * 3 / 4) dg_flush();   /* long take: one 70 KB write */
    }
    if (dg_overflow && !rec) { dg_overflow = 0; dg_printf("E log buffer overflowed during the last recording\n"); dg_flush(); }
}
