#!/usr/bin/env python3
"""e2sar_loss_timeline.py RUN OUT.png RECV_LOG1 NIC_LOG1 [RECV_LOG2 NIC_LOG2 ...]: timeline of receiver losses for one run.

Each worker (k-th RECV/NIC pair = worker k) has an e2sar_daos_recv log (10 s progress lines:
'<epoch> in N evts ... | failed F | reassembly loss R, enqueue loss E') and an e2sar_nicmon.sh log
(1 s samples of the NIC and kernel counters of the data port).

Every 10 s interval in which a worker's reassembly loss rose is matched with that worker's counters over
(previous progress line - 1 s, this progress line]. A lost event needs at least one lost packet, so a drop
counter that rose by n explains at most n of the interval's lost events; the rest have no counter. Causes:
  IP->UDP drop     Kernel IP→UDP drop: Ip.InReceives - Ip.InDelivers > 2000 (taken in at IP, never handed to UDP;
                   no drop counter rises)
  NIC port discard rx_discards_phy rose (the NIC's port buffer overflowed)
  NIC ring full    rx_out_of_buffer rose;  softnet backlog  softnet.dropped rose;  socket buffer full  Udp.RcvbufErrors rose
  upstream         Upstream (no counter change): no drop counter rose on this host, and another worker lost
                   events within 10 s too, so the packets most likely never reached either node
  unexplained      Missing (no counter change): no drop counter rose, and only this worker lost events
Enqueue loss and failed DAOS writes are their own causes.

Bars (left y): events lost per minute, two bars per minute (worker 1 plain, worker 2 hatched), stacked by cause.
Line (right y): cumulative loss rate of all workers, lost / (received + lost), scaled by 10^n.
Prints the per-interval table and the totals per worker and cause.
Needs Python 3 with numpy and matplotlib (the laptop; not installed on the ebpf nodes). Example, from the logs
that e2sar_live_pipeline.sh stop leaves in LOGDIR/<run>/ (copied to the laptop):
  scripts/e2sar_loss_timeline.py <run> loss.png recv_ebpf2201.log nic_ebpf2201.log recv_ebpf2203.log nic_ebpf2203.log"""
import sys, re, numpy as np, matplotlib
matplotlib.use('Agg'); import matplotlib.pyplot as plt, matplotlib.dates as md
from matplotlib.patches import Patch
from matplotlib.lines import Line2D
from datetime import datetime, timezone

run, out, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
pairs = list(zip(rest[0::2], rest[1::2]))
pat = re.compile(r'^(\d+\.\d+) in (\d+) evts.*\| failed (\d+) \| reassembly loss (\d+), enqueue loss (\d+)')

def read_nic(fn):
    rows = []
    for line in open(fn):
        f = line.split()
        if len(f) < 2: continue
        d = {'t': float(f[0])}; d.update({k: float(v) for k, v in (x.split('=') for x in f[1:])}); rows.append(d)
    return {k: np.array([r[k] for r in rows]) for k in rows[0]}

W = []   # per worker: progress array (t, in, failed, reasm, enq) and nic dict
for rl, nl in pairs:
    P = np.array([tuple(float(x) for x in m.groups()) for m in map(pat.match, open(rl)) if m])
    P = np.vstack([[P[0, 0] - 10, 0, 0, 0, 0], P])   # counters start at 0 one interval before the first line
    W.append({'P': P, 'N': read_nic(nl), 'name': f'Worker {len(W) + 1}'})

def delta(N, k, a, b):   # counter increase between the last samples at or before a and b
    i = max(np.searchsorted(N['t'], a, 'right') - 1, 0); j = max(np.searchsorted(N['t'], b, 'right') - 1, 0)
    return N[k][j] - N[k][i]

# 1) host cause of each reassembly-loss rise
for w in W:
    P, N = w['P'], w['N']; ev = []
    for k in range(1, len(P)):
        dl = P[k, 3] - P[k - 1, 3]
        if dl <= 0: continue
        a, b = P[k - 1, 0] - 1, P[k, 0]
        g = lambda c: delta(N, c, a, b) if c in N else 0
        ipnd = g('Ip.InReceives') - g('Ip.InDelivers')
        drops = [('IP->UDP drop', ipnd if ipnd > 2000 else 0), ('NIC port discard', g('rx_discards_phy')),
                 ('NIC ring full', g('rx_out_of_buffer')), ('softnet backlog', g('softnet.dropped')),
                 ('socket buffer full', g('Udp.RcvbufErrors'))]
        info = {x: int(g(x)) for x in ('rx_discards_phy', 'rx_out_of_buffer', 'softnet.dropped', 'Udp.RcvbufErrors')} | {'ip_not_delivered': int(ipnd)}
        left = dl
        for c, n in drops:   # a lost event needs at least one lost packet: a counter explains at most n events
            if n > 0 and left > 0:
                k = min(left, n); ev.append([b, k, c, info]); left -= k
        if left > 0: ev.append([b, left, None, info])
    w['ev'] = ev
# 2) no host counter: upstream if another worker lost events within 10 s
for w in W:
    for e in w['ev']:
        if e[2] is None:
            other = any(abs(o[0] - e[0]) <= 10 for v in W if v is not w for o in v['ev'])
            e[2] = 'upstream' if other else 'unexplained'
# enqueue loss and failed writes as events too
for w in W:
    P = w['P']
    for col, c in ((4, 'enqueue loss'), (2, 'failed DAOS write')):
        d = np.diff(P[:, col]); w['ev'] += [[P[k + 1, 0], d[k], c, {}] for k in np.nonzero(d > 0)[0]]

causes = [('NIC port discard', '#2a78d6', 'NIC port discard (rx_discards_phy)'),
          ('upstream', '#1baf7a', 'Upstream (no counter change)'),
          ('IP->UDP drop', '#eb6834', 'Kernel IP→UDP drop'),
          ('unexplained', '#eda100', 'Missing (no counter change)'),
          ('NIC ring full', '#6250d6', 'NIC ring full (rx_out_of_buffer)'),
          ('socket buffer full', '#e87ba4', 'Socket buffer full (RcvbufErrors)'),
          ('softnet backlog', '#008300', 'softnet backlog dropped'),
          ('enqueue loss', '#e34948', 'Enqueue loss (receiver queue full)'),
          ('failed DAOS write', '#52514e', 'Failed DAOS write')]

# table
for w in W:
    print(f"== {w['name']}")
    for t, dl, c, v in sorted(w['ev'], key=lambda e: e[0]):
        print(datetime.fromtimestamp(t, timezone.utc).strftime('%H:%M:%S'), int(dl), c, ' '.join(f'{k}={x}' for k, x in v.items() if x))
tot = {(w['name'], c): sum(e[1] for e in w['ev'] if e[2] == c) for w in W for c, _, _ in causes}
for w in W: print(w['name'], {c: int(tot[(w['name'], c)]) for c, _, _ in causes if tot[(w['name'], c)]})

# rate line
t0 = min(w['P'][0, 0] for w in W); t1 = max(w['P'][-1, 0] for w in W)
def cum_at(P, col, tt):
    i = np.searchsorted(P[:, 0], tt, side='right') - 1
    return np.where(i >= 0, P[np.maximum(i, 0), col], 0)
grid = np.linspace(t0 + 60, t1, 800)   # from 1 min in: before that the rate is a few events over a tiny total
L = sum(cum_at(w['P'], 2, grid) + cum_at(w['P'], 3, grid) + cum_at(w['P'], 4, grid) for w in W)
Nn = sum(cum_at(w['P'], 1, grid) for w in W)
rate = np.where(Nn > 0, L / np.maximum(Nn + L, 1), np.nan)
exp = int(np.floor(np.log10(np.nanmax(rate)))) if np.nanmax(rate) > 0 else 0
sup = str(exp).translate(str.maketrans('-0123456789', '⁻⁰¹²³⁴⁵⁶⁷⁸⁹'))
tot_lost = sum(w['P'][-1, 2:5].sum() for w in W); tot_in = sum(w['P'][-1, 1] for w in W)

# plot
ink, ink2, grd, surf = '#0b0b0b', '#52514e', '#e3e2dc', '#ffffff'
dt = lambda x: [datetime.fromtimestamp(v, timezone.utc) for v in x]
plt.rcParams.update({'font.family': 'sans-serif', 'font.size': 11, 'axes.edgecolor': grd, 'xtick.color': ink2,
                     'ytick.color': ink2, 'hatch.linewidth': 1.2})
fig, ax = plt.subplots(figsize=(13, 6), dpi=150, facecolor=surf); ax.set_facecolor(surf)
edges = np.arange(np.floor(t0 / 60) * 60, t1 + 60, 60)
width = 26 / 86400; top = 0; used = []
for k, w in enumerate(W):
    x = dt(edges[:-1] + 30 + (k - 0.5) * 28); bottom = np.zeros(len(edges) - 1)
    for c, col, _ in causes:
        ts = np.array([e[0] for e in w['ev'] if e[2] == c]); vs = np.array([e[1] for e in w['ev'] if e[2] == c])
        if not len(ts): continue
        v = np.histogram(ts, bins=edges, weights=vs)[0]
        ax.bar(x, v, width, bottom=bottom, color=col, hatch='///' if k else None, edgecolor=surf, linewidth=0.8)
        bottom += v; used.append(c)
    top = max(top, bottom.max())
ax.set_ylabel('Events lost per minute', color=ink); ax.set_ylim(0, max(top * 1.15, 1))
ax.grid(axis='y', color=grd, lw=0.8); ax.set_axisbelow(True)
for s in ('top', 'right'): ax.spines[s].set_visible(False)
ax2 = ax.twinx()
ax2.plot(dt(grid), rate / 10**exp, color=ink, lw=2)
ax2.set_ylabel(f'Cumulative loss rate (×10{sup})', color=ink); ax2.set_ylim(0, np.nanmax(rate) / 10**exp * 1.15)
ax2.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{v:g}'))
for s in ('top', 'left'): ax2.spines[s].set_visible(False)
ax.xaxis.set_major_formatter(md.DateFormatter('%H:%M', tz=timezone.utc)); ax.set_xlabel('UTC', color=ink2)
handles = [Patch(color=col, label=f'{lab}: {int(sum(tot[(w["name"], c)] for w in W)):,}') for c, col, lab in causes if c in used]
for k, w in enumerate(W):
    handles.append(Patch(facecolor='#9a9a96', edgecolor=surf, hatch='///' if k else None,
                         label=f"{w['name']}: {int(w['P'][-1, 2:5].sum()):,} lost of {int(w['P'][-1, 1] + w['P'][-1, 2:5].sum()):,}"))
handles.append(Line2D([], [], color=ink, lw=2, label='Cumulative loss rate (right axis)'))
ax.legend(handles=handles, loc='lower left', frameon=False, ncol=3, bbox_to_anchor=(0, 1.0), borderaxespad=0.2, fontsize=10)
start = datetime.fromtimestamp(t0, timezone.utc).strftime('%Y-%m-%d %H:%M'); end = datetime.fromtimestamp(t1, timezone.utc).strftime('%H:%M')
fig.suptitle(f'Receiver event loss by cause, {run}', x=0.05, ha='left', fontsize=14, fontweight='bold', color=ink)
ax.set_title(f'{start}–{end} UTC · {int(tot_lost):,} lost of {int(tot_in + tot_lost):,} events (loss rate {tot_lost / (tot_in + tot_lost) / 10**exp:.2f}×10{sup}) · '
             'cause = the host counters over each 10 s interval with loss', loc='left', fontsize=10.5, color=ink2, pad=62)
fig.tight_layout(); fig.savefig(out, facecolor=surf)
print(out)
