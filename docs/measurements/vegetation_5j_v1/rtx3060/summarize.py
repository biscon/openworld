import csv,collections,statistics,json,sys,pathlib
root=pathlib.Path(sys.argv[1])
with (root/'paired.csv').open() as f:pairs=[r for r in csv.DictReader(f) if r.get('timestamps')=='1']
with (root/'timings.csv').open() as f:timings=[r for r in csv.DictReader(f) if r.get('bytes')]
with (root/'frames.csv').open() as f:frames=[r for r in csv.DictReader(f) if r.get('timestamps')=='1']
groups=collections.defaultdict(list)
for r in pairs:groups[(r['sequence'],r['representation'],r['comparison'],r['variant'])].append(r)
summary=[]
for key,rows in groups.items():
    delta=[float(r['gpu_delta_median_ms']) for r in rows]
    matching=[r for r in timings if tuple(r[k] for k in ['sequence','representation','comparison','variant'])==key and r['enabled']=='1']
    drawn=[r for r in frames if tuple(r[k] for k in ['sequence','representation','comparison','variant'])==key and r['enabled']=='1']
    item=dict(zip(['sequence','representation','comparison','variant'],key))
    item.update(repeats=len(rows),delta_ms=statistics.median(delta),delta_min=min(delta),delta_max=max(delta))
    for name in ['gpu_median_ms','gpu_p95_ms','prepare_median_ms','prepare_p95_ms','vegetation_gpu_median_ms']:
        item[name]=statistics.median(float(r[name]) for r in matching)
    for name in ['select_ms','depth_ms','colour_ms','shadows_ms','visible','triangles','casters']:
        item[name]=statistics.median(float(r[name]) for r in drawn)
    summary.append(item)
print(json.dumps(dict(cases=summary,frames=len(frames),pairs=len(pairs),bytes=max(int(r['bytes']) for r in frames),allocations=sum(int(r['allocations']) for r in frames),overflow=sum(int(r['overflow']) for r in frames)),indent=2))
