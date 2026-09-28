# gdb -p PID -batch -x scripts/ec2/r8rss_retention.py
# r8rss (O2): where do libumem's cached objects sit in a running process?
# Reads, with every thread stopped by ptrace: each PTC (bins + per-thread
# magazines, via the umem_ptc_list registry), and for every umem_alloc_*
# cache the per-CPU cc magazines, the rseq per-CPU magazines, the depot full
# lists (central + per-CPU stripes) and the slab layer's free count.
# Diagnosis only; changes nothing in the target.
import gdb


def ev(e):
    return gdb.parse_and_eval(e)


def ival(v):
    return int(v)


def try_ev(*exprs):
    for e in exprs:
        try:
            return ev(e)
        except gdb.error:
            pass
    return None


sizes = try_ev("'umem_ptc.c'::ptc_size_classes", "ptc_size_classes")
ptcs = try_ev("'umem_ptc.c'::umem_ptc_list", "umem_ptc_list")
nbins = 36
rows = []
p = ptcs
while p is not None and ival(p) != 0:
    t = p.dereference()
    bobj = bbytes = mobj = mbytes = 0
    for i in range(nbins):
        m = t['mags'][i]
        sz = ival(sizes[i]) if sizes is not None else 0
        msz = sz
        if ival(m['cache']) != 0:
            msz = ival(m['cache'].dereference()['cache_bufsize'])
        if sz == 0:
            sz = msz
        c = ival(t['bins'][i]['count'])
        bobj += c
        bbytes += c * sz
        r = (ival(m['rounds']) if ival(m['loaded']) != 0 else 0) + \
            (ival(m['prounds']) if ival(m['previous']) != 0 else 0)
        mobj += r
        mbytes += r * msz
    rows.append((bobj, bbytes, mobj, mbytes))
    p = t['reg_next']

print("R8RSS ptc_count %d" % len(rows))
active = [r for r in rows if r[0] + r[2] > 0]
tb = sum(r[1] for r in rows)
tm = sum(r[3] for r in rows)
print("R8RSS ptc_total bin_obj=%d bin_bytes=%d mag_obj=%d mag_bytes=%d" %
      (sum(r[0] for r in rows), tb, sum(r[2] for r in rows), tm))
print("R8RSS ptc_nonempty %d" % len(active))
for k, r in enumerate(sorted(rows, key=lambda r: -(r[1] + r[3]))):
    print("R8RSS ptc[%d] bin_obj=%d bin_bytes=%d mag_obj=%d mag_bytes=%d" %
          ((k,) + r))

nc = try_ev("'umem_rseq.c'::umem_rseq_ncpus", "umem_rseq_ncpus")
ncpus_rseq = ival(nc) if nc is not None else 0
null = ev("&umem_null_cache")
cp = null.dereference()['cache_next']
tot = dict(cpu=0, rseq=0, depot=0, slabfree=0, slabout=0, slabbytes=0)
print("R8RSS cache name bufsize magsize slab_out slab_free cpu_obj "
      "rseq_obj depot_full_mags depot_obj buftotal")
while ival(cp) != ival(null):
    c = cp.dereference()
    name = c['cache_name'].string()
    if name.startswith("umem_alloc_"):
        bs = ival(c['cache_bufsize'])
        mt = c['cache_magtype']
        ms = ival(mt.dereference()['mt_magsize']) if ival(mt) != 0 else 0
        out = ival(c['cache_slab_alloc']) - ival(c['cache_slab_free'])
        bt = ival(c['cache_buftotal'])
        cpu = 0
        cc0 = c['cache_cpu'][0].address
        for i in range(ival(c['cache_cpu_mask']) + 1):
            cc = (cc0 + i).dereference()
            if ival(cc['cc_loaded']) != 0 and ival(cc['cc_rounds']) > 0:
                cpu += ival(cc['cc_rounds'])
            if ival(cc['cc_ploaded']) != 0 and ival(cc['cc_prounds']) > 0:
                cpu += ival(cc['cc_prounds'])
        rs = 0
        try:
            rc = c['cache_rseq']
            if ival(rc) != 0:
                for i in range(ncpus_rseq):
                    r = (rc + i).dereference()
                    if ival(r['loaded_mag']) != 0:
                        rs += max(0, ival(r['rounds']))
                    if ival(r['previous_mag']) != 0:
                        rs += max(0, ival(r['prounds']))
        except gdb.error:
            pass
        fm = ival(c['cache_full']['ml_total'])
        dn = ival(c['cache_depot_ncpus'])
        for i in range(dn):
            fm += ival((c['cache_depot_full'] + i).dereference()['ml_total'])
        if bt > 0:
            print("R8RSS cache %s %d %d %d %d %d %d %d %d %d" %
                  (name, bs, ms, out, bt - out, cpu, rs, fm, fm * ms, bt))
        tot['cpu'] += cpu * bs
        tot['rseq'] += rs * bs
        tot['depot'] += fm * ms * bs
        tot['slabfree'] += (bt - out) * bs
        tot['slabout'] += out * bs
        tot['slabbytes'] += bt * ival(c['cache_chunksize'])
    cp = c['cache_next']
print("R8RSS bytes ptc_bins=%d ptc_mags=%d cpu_mags=%d rseq_mags=%d "
      "depot_full=%d slab_free=%d slab_out=%d slab_total_chunk_bytes=%d" %
      (tb, tm, tot['cpu'], tot['rseq'], tot['depot'], tot['slabfree'],
       tot['slabout'], tot['slabbytes']))
