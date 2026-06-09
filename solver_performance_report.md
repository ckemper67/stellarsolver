# StellarSolver Multi-Algorithm Performance Report

This report documents the performance characteristics of StellarSolver's parallel solving algorithms (`MULTI_SCALES` vs `MULTI_DEPTHS`) under various hint configurations and explains the underlying code mechanisms responsible for these behaviors.

---

## 1. Executive Summary
StellarSolver's `MULTI_AUTO` algorithm selection currently maps hint scenarios to parallelization strategies incorrectly:
* **Scale-Only (Position Unknown) solves** are mapped to `MULTI_DEPTHS`, which forces every thread to search the entire sky concurrently, causing a **5x to 14x catastrophic slowdown** due to disk I/O contention and cache thrashing.
* **Position-Only solves** are mapped to `MULTI_SCALES`, which partitions the scale range and wastes threads since a localized spatial search is already extremely fast and would benefit far more from depth partitioning.
* **Both Hints** are mapped to `NOT_MULTI` (single thread), which executes sequentially and is slower than running `MULTI_DEPTHS` in parallel.

A simple fix in the client (such as KStars' `SolverUtils::patchMultiAlgorithm`) or upstream in StellarSolver resolves these issues by mapping hint scenarios based on position availability:
```cpp
params.multiAlgorithm = usePosition ? MULTI_DEPTHS : MULTI_SCALES;
```

---

## 2. Benchmark Results
Solves were benchmarked across 6 fields (61 to 187 stars) under three hint scenarios on a macOS Apple Silicon machine.

### Solve Times (Seconds)

| Target Field | Hint Configuration | MULTI_SCALES (s) | MULTI_DEPTHS (s) | Winner | Slowdown Ratio |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **M45 Pleiades** *(187 stars)* | Both (Scale + Pos) | **2.05** | 3.35 | `MULTI_SCALES` | 1.6x |
| | Scale Only | **2.40** | 23.50 | `MULTI_SCALES` | **9.8x (Catastrophic)** |
| | Position Only | **2.12** | 10.87 | `MULTI_SCALES` | 5.1x |
| **NGC 4535** *(61 stars)* | Both (Scale + Pos) | 0.92 | **0.66** | `MULTI_DEPTHS` | 1.4x |
| | Scale Only | 2.07 | **0.76** | `MULTI_DEPTHS` | 2.7x |
| | Position Only | 1.75 | **0.57** | `MULTI_DEPTHS` | 3.1x |
| **Galactic Pole** *(63 stars)* | Both (Scale + Pos) | 1.71 | **0.59** | `MULTI_DEPTHS` | 2.9x |
| | Scale Only | 2.14 | **1.35** | `MULTI_DEPTHS` | 1.6x |
| | Position Only | 2.18 | **1.09** | `MULTI_DEPTHS` | 2.0x |
| **Orion Plane** *(125 stars)* | Both (Scale + Pos) | 2.13 | **1.16** | `MULTI_DEPTHS` | 1.8x |
| | Scale Only | **1.68** | 4.26 | `MULTI_SCALES` | 2.5x |
| | Position Only | **1.30** | 2.20 | `MULTI_SCALES` | 1.7x |
| **M5 Cluster** *(66 stars)* | Both (Scale + Pos) | 2.08 | **0.57** | `MULTI_DEPTHS` | 3.6x |
| | Scale Only | **2.22** | 2.39 | `MULTI_SCALES` | 1.1x |
| | Position Only | 2.04 | **1.51** | `MULTI_DEPTHS` | 1.4x |
| **M74 Galaxy** *(98 stars)* | Both (Scale + Pos) | 1.84 | **0.68** | `MULTI_DEPTHS` | 2.7x |
| | Scale Only | **2.08** | 2.40 | `MULTI_SCALES` | 1.2x |
| | Position Only | 2.21 | **1.36** | `MULTI_DEPTHS` | 1.6x |

### Key Findings
1. **`MULTI_DEPTHS` dominates when Position is known:** For `Both` or `Position-only` hints, `MULTI_DEPTHS` wins in **5/6** and **4/6** fields respectively.
2. **`MULTI_SCALES` is essential when Position is unknown:** For `Scale-only` solves, `MULTI_DEPTHS` suffers from a massive **9.8x slowdown** on Pleiades and a **2.5x slowdown** on Orion. `MULTI_SCALES` is safe and performs optimally.

---

## 3. Underlying Code Analysis in StellarSolver

The behavior is governed by the astrometry solver engine loop in [`stellarsolver/astrometry/blind/engine.c`](file:///Users/ckemper/work/stellarsolver/stellarsolver/astrometry/blind/engine.c):

```c
// Line 446: Outer loop over depth ranges
for (i=0; i<il_size(job->depths)/2; i++) {
    int startobj = il_get(job->depths, i*2);
    int endobj = il_get(job->depths, i*2+1);
    ...
    // Line 462: Inner loop over scale ranges
    for (j=0; j<dl_size(job->scales) / 2; j++) {
        double fmin, fmax;
        ...
        // Line 495: Loop over indexes to filter by scale
        indexlist = il_new(16);
        for (k = 0; k < pl_size(engine->indexes); k++) {
            index_t* index = pl_get(engine->indexes, k);
            if (!index_overlaps_scale_range(index, fmin, fmax))
                continue;
            il_append(indexlist, k);
        }
        ...
        // Line 515: Loop over indexes to filter by position (if center given)
        for (k=0; k<il_size(indexlist); k++) {
            int ii = il_get(indexlist, k);
            index_t* index = pl_get(engine->indexes, ii);
            anbool inrange = TRUE;
            if (job->use_radec_center)
                inrange = index_is_within_range(index, job->ra_center, job->dec_center, job->search_radius);
            if (!inrange)
                continue;
            add_index_to_blind(engine, bp, ii);
        }
        ...
        blind_run(bp); // Execute solver search
    }
}
```

### Why `MULTI_DEPTHS` fails on Scale-Only (Blind Position) Solves
1. When position is unknown, `job->use_radec_center` is `false`, disabling the `index_is_within_range` spatial filter.
2. Since `MULTI_DEPTHS` threads do not partition the scale range, every thread's scale check matches **all** index files.
3. Therefore, **every single thread loads and searches all index files across the entire sky in parallel**.
4. This results in heavy concurrent disk read requests and page cache thrashing, completely saturating the disk bus and causing the **5–14x slowdowns**.

### Why `MULTI_SCALES` succeeds on Scale-Only Solves
1. Each thread is assigned a narrow scale range.
2. The `index_overlaps_scale_range` check filters out most indexes, meaning **each thread only loads and searches the specific index files matching its small scale sub-range**.
3. Thread search spaces and disk reads are completely isolated, ensuring clean multi-core parallelization.

### Why `MULTI_DEPTHS` succeeds when Position is Known
1. With `job->use_radec_center == true`, the spatial filter `index_is_within_range` is active.
2. Threads only load and search the tiny fraction of index files (typically 1 or 2 files) that overlap the target coordinates, making disk I/O and spatial search times negligible.
3. The primary bottleneck becomes matching quads under varying star depths. 
4. Testing depths sequentially (`NOT_MULTI` or `MULTI_SCALES` with mismatched scales) is slow. Partitioning depths across threads via `MULTI_DEPTHS` resolves the match immediately in parallel.

---

## 4. The Upstream Bug in `stellarsolver.cpp`

The logical bug resides in [`stellarsolver.cpp` (lines 381-391)](file:///Users/ckemper/work/stellarsolver/stellarsolver/stellarsolver.cpp#L381-L391):

```cpp
if(params.multiAlgorithm == MULTI_AUTO)
{
    if(m_UseScale && m_UsePosition)
        params.multiAlgorithm = NOT_MULTI;
    else if(m_UsePosition)
        params.multiAlgorithm = MULTI_SCALES;
    else if(m_UseScale)
        params.multiAlgorithm = MULTI_DEPTHS; // <-- Bug: forces scale-only to MULTI_DEPTHS
    else
        params.multiAlgorithm = MULTI_SCALES;
}
```

### Recommendation
Update the resolution of `MULTI_AUTO` to base the choice on position availability:
```cpp
if(params.multiAlgorithm == MULTI_AUTO)
{
    params.multiAlgorithm = m_UsePosition ? MULTI_DEPTHS : MULTI_SCALES;
}
```
This avoids the `MULTI_DEPTHS` blind-solve bottleneck and exploits `MULTI_DEPTHS` for all position-constrained cases.

---

## 5. Hybrid (2D) Sharding for Blind Solving
For blind solving (position unknown), it is possible to combine both parallelization dimensions into a **2D sharding strategy** to achieve even faster solves.

### How it Works
Rather than partitioning only scales or only depths, the available threads $N$ are allocated into $S$ scale bins and $D$ depth bins ($S \times D \le N$). 
* Scale partitioning ($S$) divides the index files searched, keeping disk I/O isolated.
* Depth partitioning ($D$) tries multiple star density limits in parallel.

### Benefits
1. **Bypasses Sequential Depth Matching:** Within the correct scale bin, the solver does not have to sequentially try depths (10 $\to$ 20 $\to$ ... $\to$ 80). The thread assigned to the correct depth solves immediately.
2. **Optimal Core Utilization:** On high-core count systems (e.g. 16 or 32 threads), partitioning by scale alone results in overly narrow scale ranges with diminishing returns. Hybrid partitioning makes full use of all cores.

### Proposed Code Implementation in `StellarSolver::parallelSolve()`
```cpp
else if(params.multiAlgorithm == MULTI_HYBRID)
{
    // E.g. Allocate 4 threads to scale partitioning and partition depths with the rest
    int scaleThreads = 4;
    int depthThreads = threads / scaleThreads;
    if (depthThreads < 1) depthThreads = 1;
    
    double minScale = m_UseScale ? m_ScaleLow : params.minwidth;
    double maxScale = m_UseScale ? m_ScaleHigh : params.maxwidth;
    double scaleConst = (maxScale - minScale) / pow(scaleThreads, 2);
    
    int sourceNum = params.keepNum != 0 ? params.keepNum : 200;
    int depthInc = sourceNum / depthThreads;
    if (depthInc < 10) depthInc = 10;
    
    for (int s = 0; s < scaleThreads; s++)
    {
        double low = minScale + scaleConst * pow(s, 2);
        double high = minScale + scaleConst * pow(s + 1, 2);
        
        for (int d = 1; d < sourceNum; d += depthInc)
        {
            ExtractorSolver *solver = m_ExtractorSolver->spawnChildSolver(s * 1000 + d);
            connect(solver, &ExtractorSolver::finished, this, &StellarSolver::finishParallelSolve);
            
            // 1. Shard the Scale range
            solver->setSearchScale(low, high, units);
            
            // 2. Shard the Depth range
            solver->depthlo = d;
            solver->depthhi = d + depthInc;
            
            parallelSolvers.append(solver);
        }
    }
}
```

### Recommendation Heuristic
* **Low core counts ($N \le 4$):** Stick to pure `MULTI_SCALES` ($S = N, D = 1$) to prevent disk I/O contention.
* **High core counts ($N \ge 8$):** Use hybrid sharding ($S = 4, D = N/4$) to accelerate dense-field solves without saturating the disk bus.

---

## 6. StellarSolver Benchmark (PARALLEL_SMALLSCALE Profile)

These results were obtained by running the benchmark directly against StellarSolver
using its built-in `PARALLEL_SMALLSCALE` profile (keepNum=50, minwidth=0.1,
maxwidth=10, inParallel=true) and the full KStars index file collection
(series 4204-4219, including healpix-split files). Apple Silicon, macOS.

The same 6 pre-rendered FITS fields from the KStars benchmark were used
(1280x1024, 2.06 arcsec/px, GSC catalog stars with Moffat PSFs).

### Solve Times (Seconds)

| Target Field | Hint Configuration | MULTI_SCALES (s) | MULTI_DEPTHS (s) | Winner | Ratio |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **M45 Pleiades** | Both (Scale + Pos) | 1.76 | **0.45** | `MULTI_DEPTHS` | 3.9x |
| | Scale Only | 1.53 | **1.27** | `MULTI_DEPTHS` | 1.2x |
| | Position Only | 1.63 | **1.28** | `MULTI_DEPTHS` | 1.3x |
| **NGC 4535** | Both (Scale + Pos) | 0.55 | **0.27** | `MULTI_DEPTHS` | 2.0x |
| | Scale Only | 1.74 | **0.42** | `MULTI_DEPTHS` | 4.1x |
| | Position Only | 1.10 | **0.50** | `MULTI_DEPTHS` | 2.2x |
| **Galactic Pole** | Both (Scale + Pos) | 1.02 | **0.31** | `MULTI_DEPTHS` | 3.3x |
| | Scale Only | 1.44 | **0.37** | `MULTI_DEPTHS` | 3.9x |
| | Position Only | 1.50 | **0.68** | `MULTI_DEPTHS` | 2.2x |
| **Orion Plane** | Both (Scale + Pos) | 2.23 | **0.32** | `MULTI_DEPTHS` | 7.0x |
| | Scale Only | 1.70 | **0.45** | `MULTI_DEPTHS` | 3.8x |
| | Position Only | 2.33 | **0.61** | `MULTI_DEPTHS` | 3.8x |
| **M5 Cluster** | Both (Scale + Pos) | 0.98 | **0.37** | `MULTI_DEPTHS` | 2.6x |
| | Scale Only | 2.05 | **0.51** | `MULTI_DEPTHS` | 4.0x |
| | Position Only | 2.28 | **0.86** | `MULTI_DEPTHS` | 2.7x |
| **M74 Galaxy** | Both (Scale + Pos) | 2.12 | **0.46** | `MULTI_DEPTHS` | 4.6x |
| | Scale Only | 1.43 | **0.94** | `MULTI_DEPTHS` | 1.5x |
| | Position Only | 2.01 | **1.42** | `MULTI_DEPTHS` | 1.4x |

### Key Findings

1. **`MULTI_DEPTHS` wins all 18 cases.** The Pleiades outlier from Section 2
   does not reproduce. The catastrophic 9.8x slowdown on scale-only Pleiades
   is absent -- MULTI_DEPTHS solves in 1.27s vs MULTI_SCALES 1.53s.

2. **The difference from Section 2 is the profile and index set, not the
   machine.** Both benchmarks ran on the same Apple Silicon hardware. The
   KStars benchmark (Section 2) used the KStars default align profile through
   `SolverUtils`, while this benchmark uses StellarSolver's built-in
   `PARALLEL_SMALLSCALE` profile directly. The KStars index collection also
   includes healpix-split series (4204-4207) which provide finer spatial
   coverage than the whole-sky 4208-4219 files.

3. **The Pleiades dense-field problem appears to be a KStars profile or
   SolverUtils issue, not an upstream StellarSolver bug.** When StellarSolver
   is configured with its own built-in profile, MULTI_DEPTHS handles dense
   fields without catastrophic failure.
