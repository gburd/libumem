# one record per "BARN umem_alloc_*" line (the last line of each report)
/^BARN src=/ { for(i=3;i<=NF;i++){split($i,kv,"="); v[$2,kv[1]]=kv[2]} }
/^BARN stale_mags/ { for(i=2;i<=NF;i++){split($i,kv,"="); s[kv[1]]=kv[2]} }
/^BARN allocs=/ { for(i=2;i<=NF;i++){split($i,kv,"="); a[kv[1]]=kv[2]} }
/^BARN umem_alloc_\*/ { split($3,x,"="); SA=x[2]; split($4,y,"="); sub(";","",y[2]); SF=y[2];
  pm=v["src=exit","partial_mags"]+v["src=rseq","partial_mags"]+v["src=ptc","partial_mags"];
  po=v["src=exit","partial_objs"]+v["src=rseq","partial_objs"]+v["src=ptc","partial_objs"];
  fo=v["src=exit","full_drained_objs"]; fm=v["src=exit","full_drained_mags"];
  A=a["allocs"]; T=SA+SF; n++;
  printf "%-14s allocs=%11d  partial: mags=%5d objs=%6d (rseq %d/%d, exit %d/%d)  per-Malloc: mags=%.2f objs=%.1f | exitfull mags=%d objs=%d | stale objs=%d | drained->slab=%d | slab_alloc=%d slab_free=%d | partial_objs/slab_traffic=%.2f%% | slab_alloc/Malloc=%.0f | miss_with_depot_full=%d/%d\n",
    LABEL (n>1?"#"n:""), A, pm, po, v["src=rseq","partial_mags"], v["src=rseq","partial_objs"],
    v["src=exit","partial_mags"], v["src=exit","partial_objs"], pm/A*1e6, po/A*1e6, fm, fo,
    s["stale_objs"], s["drained_to_slab"], SA, SF, (T>0? po/T*100:0), SA/A*1e6,
    a["miss_slab_alloc_depot_nonempty"], a["miss_slab_alloc"] }
