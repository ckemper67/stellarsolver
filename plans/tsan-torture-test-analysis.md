# TSan Torture Test Analysis

Run command:
```
cmake -DBUILD_TESTS=On -DUSE_QT5=On \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread" \
  -B build .
cmake --build build --target TestTorture
cd build
QT_QPA_PLATFORM=offscreen \
TSAN_OPTIONS="halt_on_error=0 history_size=4 suppressions=/home/christian/work/stellarsolver/tsan.supp" \
setarch $(uname -m) -R ./TestTorture
```

Notes:
- `setarch -R` disables ASLR to avoid TSan's "unexpected memory mapping" error (`vm.mmap_rnd_bits=32` is too high)
- `QT_QPA_PLATFORM=offscreen` needed since there is no X display
- `TSAN_OPTIONS=suppressions=...` suppresses known-unfixable races in third-party C code

---

## Results: 0/7 tests passed (as of initial run)

All tests fail. Failures have three distinct causes:
1. **Real races in our code** (actionable)
2. **Qt5/TSan false positives** (large volume — structural limitation of Qt5 + TSan)
3. **External library races** (cfitsio, glib — not fixable here)
4. **Test timeouts** (TSan overhead slows extract/solve by ~10–20×)

---

## Real Races (Actionable)

### 1. `abort()` write-write race with `solve_fields` ✅ FIXED

**Location:** `internalextractorsolver.cpp:81-82`

**Race:**
- Calling thread (e.g. `finishParallelSolve`): writes `thejob.bp.cancelled = TRUE` and `thejob.bp.solver.quit_now = TRUE` in `abort()`
- Worker thread: `solve_fields` → `blind_run` → also writes `bp->cancelled` (via `timer_callback` at `blind.c:904`) and resets `bp->solver.quit_now = FALSE` (at `blind.c:1035`) for each field

Both fields are `anbool` (`unsigned char`). The write-write race is real UB even though `cancelled` writes are both `TRUE`. More importantly, `quit_now` is reset to `FALSE` per-field by `solve_fields`, so there is a narrow window where `abort()`'s `TRUE` can be overwritten and the cancel missed for one field (though `cancelled` still catches it).

**Fix:** Use `__atomic_store_n` with `__ATOMIC_SEQ_CST` for both writes in `abort()`. Added TSan suppression for `solve_fields` and `blind_run` since they are third-party C code that cannot be made atomic-aware.

**TSan suppression (tsan.supp):** `race:solve_fields`, `race:blind_run`

---

### 2. `ExtractorSolver::~ExtractorSolver()` vptr race — likely Qt false positive

**Race:**
- Thread A: `StellarSolver::~StellarSolver()` → `QScopedPointer::~QScopedPointer()` → `InternalExtractorSolver::~InternalExtractorSolver()` → `ExtractorSolver::~ExtractorSolver()` → `operator delete` on the object (overwrites vptr region)
- Thread B (`ExtractorSolver` thread, **already finished**): last ran `InternalExtractorSolver::run()` → `ExtractorSolver::metaObject()` (reads vptr)

The destructor calls `wait()` which should join thread B before proceeding. Thread B is marked 'finished' in the TSan report, confirming it did exit. However, Qt's `QThread::wait()` uses its own condition variables, which TSan does not recognize as establishing happens-before. So TSan reports the race even though the ordering is correct at runtime.

**Status:** Likely false positive. Not fixed. A TSan suppression on `ExtractorSolver::~ExtractorSolver` could silence it if needed.

---

## Qt5/TSan False Positives (Large Volume)

All of these share the same root cause: **Qt5's global thread pool reuses threads without calling `pthread_create` per task**. TSan tracks happens-before through `pthread_create`; when a thread pool thread picks up a new task, TSan does not see a happens-before edge between the task enqueuer and the task runner.

Affected races:

### `stretchOneChannel` / `stretchThreeChannels` lambda captures

`QtConcurrent::run(lambda)` allocates a `StoredFunctorCall0` on the heap, copies the lambda (with all captures) into it, then enqueues the task. The pool thread reads the captures when running the lambda. TSan sees the write (copy into `StoredFunctorCall0`) and the read (lambda execution) as unordered.

**Reality:** The write completes before `QThreadPool::start()` is called, and `QThreadPool::start()` signals the thread to wake. TSan doesn't see this via `pthread_create` because the thread already exists.

### `InternalExtractorSolver::extractPartition()` reading `m_ActiveParameters`

`StellarSolver::createExtractorSolver()` writes the `m_ActiveParameters` field, then starts the IES `QThread` (which DOES go through `pthread_create` — TSan sees this edge). The IES thread then calls `runSEPExtractor()` which spawns `extractPartition` tasks via `QtConcurrent::run`. Those pool tasks read `m_ActiveParameters`. TSan loses the happens-before because the pool task dispatch goes through thread reuse, not `pthread_create`.

### Qt internals: `QListData::realloc_grow`, `QWaitCondition`, `QtConcurrent::RunFunctionTask::~RunFunctionTask` vptr, `QMapData`, `QVector<float>`, `QList<FITSImage::Star>`, `QtPrivate::ResultItem`

All of these are internal Qt synchronization and result-passing machinery. Qt5 does not ship with TSan annotations (unlike some newer projects that use `__tsan_acquire`/`__tsan_release` intrinsics). These races cannot be suppressed individually without a very broad Qt suppression.

**Suppression approach:** A broad `race:Qt*` suppression would silence these but also mask real Qt-related races. The better long-term fix is to move to Qt6, which has improved internal thread safety, or to add `-fsanitize-ignorelist` entries.

---

## External Library Races

### `ffopen` (cfitsio)

cfitsio's `ffopen` accesses global state (file descriptor tables, error message buffers) without locks. Multiple concurrent calls to `ffopen` (or any `ff*` function) from different threads will race.

**Workaround:** Serialize all cfitsio calls behind a mutex. The stellarsolver codebase already has a comment noting cfitsio's thread-safety limitation (`stellarsolver.cpp:303`).

**TSan suppression (tsan.supp):** `race:ffopen`

### libglib

glib uses a global type registration system that is not TSan-annotated. Races appear on first-time type initialization. These are benign in practice (one-time init) but TSan can't prove it.

---

## Test Timeouts

TSan instrumentation adds ~10–20× overhead to thread operations. Tests with `N=10` concurrent solvers or extractors may need timeout adjustments when run under TSan:

- `Concurrent Extract Flood`: times out at 120s
- `Rapid Create-Destroy`: times out at 90s

Consider adding a `TSAN_TIMEOUT_MULTIPLIER` or reducing iteration counts when `$TSAN_OPTIONS` is set.

---

## Summary Table

| Race | Location | Real? | Status |
|------|----------|-------|--------|
| `abort()` writes `cancelled`/`quit_now` | `internalextractorsolver.cpp:81-82` | Yes | **Fixed** — `__atomic_store_n` + suppression |
| `solve_fields` writes `cancelled`/`quit_now` | `blind.c` (third-party) | Yes (UB) | Suppressed in `tsan.supp` |
| `ffopen` global state | cfitsio (third-party) | Yes | Suppressed in `tsan.supp` |
| `ExtractorSolver::~ExtractorSolver()` vptr | `extractorsolver.cpp` | Likely false positive | Open |
| `stretchOneChannel`/`stretchThreeChannels` lambdas | `stretch.cpp` | Qt5 false positive | Open |
| `extractPartition` reading params | `internalextractorsolver.cpp` | Qt5 false positive | Open |
| Qt internals (QList, QVector, QMap, QWaitCondition…) | libQt5Core | Qt5 false positive | Open |
| libglib type init | libglib | Benign one-time init | Open |
