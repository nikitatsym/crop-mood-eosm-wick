#!/usr/bin/env python3
"""DIAG (temporary) analyzer for the Crop Mood FilmGate diagnostic build.

  diag_analyze.py log  ML/LOGS/DIAG01.TXT      summarize a camera log
  diag_analyze.py clip frame1.dng [frame2.dng] banding / noise of uncompressed DNG frames

Runs on any computer with Python 3 and numpy; the clip mode also needs tifffile.
"""
import re
import sys
from collections import OrderedDict, defaultdict

KV = re.compile(r'(\w+)=("[^"]*"|\S+)')

# a few registers worth naming in snapshot diffs
REGNAMES = {
    0xC0F06008: 'TimerA (line time)', 0xC0F06014: 'TimerB (frame time)',
    0xC0F06804: 'RAW H/V size', 0xC0F06824: 'RAW readout', 0xC0F0819C: 'LV black level',
    0xC0F08184: 'RAW V-1', 0xC0F42744: 'LV brighten (AeWb method)',
    0xC0F37AE4: 'LV gain R (EVF method)', 0xC0F37AF0: 'LV gain G1', 0xC0F37AFC: 'LV gain G2',
    0xC0F37B08: 'LV gain B',
}


def kv(line):
    out = {}
    for k, v in KV.findall(line):
        out[k] = v.strip('"')
    return out


def num(d, k, default=None):
    v = d.get(k)
    if v is None:
        return default
    m = re.match(r'-?\d+(\.\d+)?', v)
    return float(m.group(0)) if m else default


def parse(path):
    recs = []                     # (time_s, type, text)
    with open(path, errors='replace') as f:
        for ln in f:
            ln = ln.rstrip('\n')
            m = re.match(r'(\d+)\.(\d{3}) (\w) (.*)', ln)
            if m:
                recs.append((int(m.group(1)) + int(m.group(2)) / 1000.0, m.group(3), m.group(4)))
            else:
                m = re.match(r'([GA]) (.*)', ln)
                if m:
                    recs.append((None, m.group(1), m.group(2)))
    return recs


def stats(v):
    v = [x for x in v if x is not None]
    if not v:
        return None
    return min(v), sum(v) / len(v), max(v)


def fmt3(s, f='%.1f'):
    if not s:
        return '-'
    return ' / '.join(f % x for x in s)


def report_log(path):
    recs = parse(path)
    if not recs:
        print('No lines found in', path)
        return
    out = []
    p = out.append
    for t, ty, tx in recs:
        if ty == 'H':
            p('  ' + tx)
    # ---- timeline of events
    p('\nEvents')
    for t, ty, tx in recs:
        if ty == 'E':
            p('  %8.1fs  %s' % (t, tx))

    # ---- recordings
    p('\nRecordings (R lines while recording)')
    sess, cur = [], None
    for t, ty, tx in recs:
        if ty == 'E' and tx.startswith('RECORDING START'):
            cur = {'start': t, 'R': [], 'S': []}
        elif ty == 'E' and tx.startswith('RECORDING STOP'):
            if cur:
                cur['stop'] = t
                cur['stoptext'] = tx
                sess.append(cur)
            cur = None
        elif cur is not None and ty == 'R':
            cur['R'].append(kv(tx))
        elif cur is not None and ty == 'S':
            cur['S'].append(kv(tx))
    if cur:
        cur['stop'] = None
        cur['stoptext'] = 'no stop line (camera stopped or crashed during this take)'
        sess.append(cur)
    if not sess:
        p('  none logged')
    for i, s in enumerate(sess, 1):
        R = s['R']
        p('  take %d: %.1fs to %s  (%d second-lines)' % (i, s['start'], '%.1fs' % s['stop'] if s['stop'] else '?', len(R)))
        p('    ' + s['stoptext'])
        if R:
            p('    live-view fps (min/avg/max):      %s' % fmt3(stats([num(r, 'vs_fps') for r in R]), '%.2f'))
            p('    vsync gap us, worst (max):         %d' % max(num(r, 'max', 0) for r in R))
            p('    poll gap ms, worst:                %d' % max(num(r, 'poll_gap_ms', 0) for r in R))
            p('    write speed MB/s (min/avg/max):    %s' % fmt3(stats([num(r, 'wr_speed') for r in R]), '%.1f'))
            fs = [num(r, 'free_slots') for r in R]
            p('    free buffer slots (min/avg/max):   %s   (of %s)' % (fmt3(stats(fs), '%.0f'), R[0].get('free_slots', '?/?').split('/')[-1]))
            p('    write queue depth (max):           %d' % max(num(r, 'queue', 0) for r in R))
            p('    frames recorded / skipped:         %d / %d' % (max(num(r, 'frames', 0) for r in R), max(num(r, 'skipped', 0) for r in R)))
            p('    buffer-full seconds:               %d' % sum(1 for r in R if num(r, 'buf_full', 0)))
            p('    free memory (min):                 %d' % min(num(r, 'mem_free', 0) for r in R))
            p('    temperature raw (first -> last):   %d -> %d' % (num(R[0], 'temp', 0), num(R[-1], 'temp', 0)))
            p('    battery bars (first -> last):      %d -> %d' % (num(R[0], 'bat_bars', 0), num(R[-1], 'bat_bars', 0)))
            # hiccups: seconds with a very late vsync or a stalled poll
            bad = [r for r in R if num(r, 'max', 0) > 80000 or num(r, 'poll_gap_ms', 0) > 400]
            if bad:
                p('    seconds with a late frame (>80 ms) or stalled task (>400 ms): %d' % len(bad))

    # ---- streak table
    p('\nLive-view streak measurement (rms of column-to-column / row-to-row variation, luma levels 0-255)')
    p('  lower is cleaner.  Compare lines taken of the SAME scene; best with the lens cap on.')
    groups = OrderedDict()
    for t, ty, tx in recs:
        if ty != 'S':
            continue
        d = kv(tx)
        key = (int(num(d, 'bits', -1)), int(num(d, 'bright', -1)), int(num(d, 'iso', -1)),
               int(num(d, 'fmt', -1)), int(num(d, 'fpsidx', -1)), int(num(d, 'rec', 0)))
        groups.setdefault(key, []).append(d)
    names = {0: '14', 1: '12', 2: '11', 3: '10'}
    p('  %-4s %-6s %-6s %-4s %-4s %-5s %5s | %-8s %-9s %-9s %-9s %-9s' %
      ('bits', 'bright', 'ISO', 'fmt', 'fps', 'rec', 'n', 'mean', 'col_rms', 'col_max', 'row_rms', 'row_max'))
    for key, L in groups.items():
        bits, br, iso, fm, fp, rec = key
        def avg(k):
            v = [num(x, k, 0) for x in L]
            return sum(v) / len(v)
        p('  %-4s %-6s %-6d %-4d %-4d %-5s %5d | %-8.1f %-9.2f %-9.2f %-9.2f %-9.2f' %
          (names.get(bits, bits), 'EVF' if br else 'AeWb', iso, fm, fp, 'yes' if rec else 'no', len(L),
           avg('mean'), avg('col_rms'), avg('col_max'), avg('row_rms'), avg('row_max')))

    # ---- snapshots
    snaps, cur = [], None
    for t, ty, tx in recs:
        if ty == 'E' and tx.startswith('snapshot #'):
            cur = {'t': t, 'why': tx, 'G': {}, 'A': {}, 'C': []}
            snaps.append(cur)
        elif cur is not None and ty == 'C':
            cur['C'].append(tx)
        elif cur is not None and ty == 'G':
            m = re.match(r'([0-9A-F]{8}) ([0-9A-F]{8})$', tx)
            if m:
                cur['G'][int(m.group(1), 16)] = int(m.group(2), 16)
        elif cur is not None and ty == 'A':
            m = re.match(r'(\d+) ([0-9A-F]{4}) ([0-9A-F]{4})$', tx)
            if m:
                cur['A'][(int(m.group(1)), int(m.group(2), 16))] = int(m.group(3), 16)
    p('\nSnapshots: %d' % len(snaps))
    for i, s in enumerate(snaps):
        p('  #%d  %s' % (i + 1, s['why']))
        for c in s['C'][:2]:
            p('       ' + c[:160])
    for i in range(1, len(snaps)):
        a, b = snaps[i - 1], snaps[i]
        diffs = [(addr, a['G'].get(addr), b['G'].get(addr)) for addr in sorted(set(a['G']) | set(b['G']))
                 if a['G'].get(addr) != b['G'].get(addr)]
        adiffs = [(k, a['A'].get(k), b['A'].get(k)) for k in sorted(set(a['A']) | set(b['A']))
                  if a['A'].get(k) != b['A'].get(k)]
        p('\n  Register changes snapshot #%d -> #%d: %d engine, %d ADTG' % (i, i + 1, len(diffs), len(adiffs)))
        for addr, x, y in diffs[:60]:
            p('    %08X %-26s %s -> %s' % (addr, REGNAMES.get(addr, ''),
                                          '%08X' % x if x is not None else '-', '%08X' % y if y is not None else '-'))
        if len(diffs) > 60:
            p('    ... %d more' % (len(diffs) - 60))
        for (dst, reg), x, y in adiffs[:40]:
            p('    ADTG%d %04X  %s -> %s' % (dst, reg, '%04X' % x if x is not None else '-', '%04X' % y if y is not None else '-'))
    print('\n'.join(out))


# ---------------------------------------------------------------------------------------------
def load_dng(path):
    import numpy as np
    import tifffile
    with tifffile.TiffFile(path) as tf:
        page = tf.pages[0]
        comp = page.compression
        if int(comp) not in (1,):
            raise SystemExit('%s: compressed DNG (compression %s). Export the clip as UNCOMPRESSED DNG '
                             '(MLV App / mlv_dump -o x_ --dng without --compression).' % (path, comp))
        img = page.asarray().astype(np.float64)
        black = 0.0
        t = page.tags.get('BlackLevel')
        if t is not None:
            bl = np.atleast_1d(t.value)
            black = float(np.mean(bl))
        white = page.tags.get('WhiteLevel')
        white = float(np.atleast_1d(white.value)[0]) if white is not None else float(img.max())
    return img, black, white


def band_metrics(a):
    """Row and column banding of a flat/dark image, in raw levels."""
    import numpy as np
    out = {}
    # column means over rows, row means over columns; per CFA colour (even/odd) to ignore the Bayer pattern
    for name, axis in (('column', 0), ('row', 1)):
        vals = []
        for cy in (0, 1):
            for cx in (0, 1):
                ch = a[cy::2, cx::2]
                m = ch.mean(axis=axis)
                k = 7
                sm = np.convolve(m, np.ones(k) / k, mode='same')
                hp = (m - sm)[k:-k]
                vals.append(hp.std())
        out[name] = float(np.mean(vals))
    return out


def report_clip(paths):
    import numpy as np
    print('%-28s %9s %9s %9s %10s %10s' % ('file', 'black', 'mean', 'noise', 'col_band', 'row_band'))
    for p in paths:
        img, black, white = load_dng(p)
        h, w = img.shape
        crop = img[h // 10:h - h // 10, w // 10:w - w // 10]
        bm = band_metrics(crop)
        # temporal noise proxy: std of a high-passed 2x2-binned patch
        patch = crop[:256, :256]
        noise = float(np.std(patch - np.mean(patch)))
        print('%-28s %9.1f %9.1f %9.2f %10.3f %10.3f' % (p.split('/')[-1][:28], black, crop.mean(), noise, bm['column'], bm['row']))
    print('\ncol_band / row_band: rms of column-to-column / row-to-row variation, in raw levels.')
    print('Compare clips of the same scene (best lens cap on) at 12/11/10-bit and different frame rates.')


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'log':
        report_log(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'clip':
        report_clip(sys.argv[2:])
    else:
        print(__doc__)
        sys.exit(1)
