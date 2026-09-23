# Error-Handling & Concurrency Design Review — `WfmcmApi.cpp` / `WfmcmVasaraApi.cpp`

- **Date:** 2026-09-22
- **Scope:** `api/api/WfmcmApi.cpp`, `api/api/WfmcmApiInternal.cpp`, `api/api/WfmcmApiInternal.h`, `api/api/WfmcmApi.h`, `api/api/WfmcmVasaraApi.cpp`
- **Focus:** the per-handle / per-thread error model, the `requestContext` error anchor, and concurrent correctness.

---

## 1. Executive summary

The API layer uses a **shared, last-writer-wins error slot** on each `HandleData` (plus a thread-local *global* error) to report the outcome of API calls. This model is coherent for the documented single-threaded, "one handle owned by one caller" contract, and the macro set (`RETURN_SUCCESS` / `RETURN_ERROR` / `EXCEPTION_ERROR`) implements it consistently **inside each function**.

However, three design tensions surface when the same handle — in particular a reusable `requestContext` — is shared across *different* API calls, and worse across *threads*:

1. **The success path of the string-returning Vasara APIs clears the shared `requestContext` error slot** (`RETURN_SUCCESS(requestContext, out)`). This implements the documented "resets on next call" contract, but it erases diagnostics produced by *other* failing calls (`genRatePaths`, `calcValueForMortgageFromRates`, the `*Obj` variants) that anchored their error on the same `requestContext`.
2. **The handle-returning Vasara APIs do not reset `requestContext` on success** (they clear the returned `result` handle instead). This is inconsistent with the string APIs and leaves stale errors on `requestContext` after a failure → success transition of the same API.
3. **The shared per-handle slot is a single cell.** Under concurrent use of one handle, reads/writes are individually atomic (recursive mutex `m_`) but there is no *set → caller-read* transaction. A success on one thread can clobber a failure on another thread before it is read.

**Bottom line:** the current design is defensible for sequential, single-owner-per-handle usage, but the error *anchor* policy is inconsistent across the six Vasara entry points and is not safe under concurrent sharing of a handle. The race-free per-call channels are the **return value** and the **thread-local** `getError()`/`hasError()` — not `getHandleError(requestContext)` under concurrency.

---

## 2. Architecture overview

### 2.1 Handle model

- A `Handle` is an opaque 64-bit value whose bits are a **raw pointer to a `HandleData`** (`reinterpret_cast`, `WfmcmApiInternal.cpp:238`).
- `HandleData` owns: `type_`, the payload `object_` (a `std::any`), `bindings_`, and the error slot `error_`.
- A global `std::unique_ptr<HandleContainer> handles` maps `Handle → shared_ptr<HandleData>` under a `std::shared_mutex` (`WfmcmApiInternal.h:458`).
- `ScopedHandle` (RAII) deletes its handle on destruction, so many internal temporaries are auto-released.

### 2.2 Error model — two stores

Every error write goes through `setHandleError` (`WfmcmApiInternal.cpp:268`):

```cpp
void setHandleError(Handle h, HandleError error) {
    if (isValidHandle(h)) {
        handleData(h).setError(error);      // (1) per-handle slot
    }
    globalError().second = move(error);     // (2) thread-local global slot
}
```

- **(1) Per-handle `HandleData::error_`** — shared across threads; guarded by a `recursive_mutex m_` (`WfmcmApiInternal.cpp:101`).
- **(2) Thread-local `globalError()`** — the store used by `getError()`/`hasError()`; documented as per-thread (`WfmcmApi.h`, `getError()` doc: *"This error is per-thread…"*).

`clearHandleError(h)` is just `setHandleError(h, {})` — i.e. it clears **both** stores.

### 2.3 Public error readers (all pure observers)

From `WfmcmApi.cpp:3613`:

| Function | Behavior |
|---|---|
| `getError()` | serializes `globalError()` — **does not reset** |
| `hasError()` | `globalError().second.isError()` — **does not reset** |
| `getHandleError(h)` | copies `handleData(h).getError()` into a thread-local buffer — **does not reset the handle** |
| `hasHandleError(h)` | `handleData(h).getError().isError()` — **does not reset** |

`HandleData::getError()` explicitly returns a **copy** and documents that reset happens only via `clearHandleError()`/`setError({})` — *"no consume-on-read is needed here"* (`WfmcmApiInternal.cpp:107`).

> Implication for the "future enhancement" discussed earlier: if `getError()` were changed to consume-on-read, every internal peek listed in §3.3 would silently mutate state, and the two stores would need a synchronized *take* that clears both — see §8.3.

---

## 3. The `requestContext` error anchor — the core issue

### 3.1 Anchor per entry point (`WfmcmVasaraApi.cpp`)

| Function | Returns | Error anchor on **failure** | Error anchor on **success** |
|---|---|---|---|
| `calcBehavioralSpeedForInstrument` | `const char*` | `requestContext` | **`requestContext`** (line 1227) |
| `calcValueForInstrument` | `const char*` | `requestContext` | **`requestContext`** (line 1387) |
| `calcBehavioralSpeedForInstrumentObj` | `Handle` | `requestContext` | `result` (line 1267) |
| `calcValueForInstrumentObj` | `Handle` | `requestContext` | `result` (line 1472) |
| `genRatePaths` | `Handle` | `requestContext` | `result` (line 1524) |
| `calcValueForMortgageFromRates` | `Handle` | `requestContext` | `result` (line 1617) |

### 3.2 What `RETURN_SUCCESS(requestContext, out)` actually does

```cpp
#define RETURN_SUCCESS(handle, ret)  do { clearHandleError(handle); return ret; } while(false)
```

On the success path of `calcBehavioralSpeedForInstrument`, nothing between entry and line 1227 has written `requestContext`'s error. Therefore the clear **always** wipes a *pre-existing* error — never one produced by this invocation. It is **not** "helping" the failing call; it is a **slot-alignment policy**: leave the shared slot reflecting *this call's* outcome (empty ⇒ error, non-empty ⇒ success, and `hasHandleError(requestContext) == false` on success).

This is consistent with the documented contract in `WfmcmApi.h`:

> `getHandleError()` … only resets when the next API call is made **using this handle**.

### 3.3 Who writes an error onto `requestContext`

Only failure paths that use `requestContext` as their `RETURN_ERROR`/`EXCEPTION_ERROR` anchor:

- `calcBehavioralSpeedForInstrument` — invalid context, build error, exception.
- `calcBehavioralSpeedForInstrumentObj` — invalid context, build error, exception.
- `calcValueForInstrument` — invalid context, empty CSV, bytes-variant context, build error, `execute` failure, empty result, exceptions.
- `calcValueForInstrumentObj` — invalid context, empty CSV, bytes-variant context, build error, `execute`/`waitForResult` failure, consumed response, exceptions.
- `genRatePaths` — invalid context, build error (no primary model / >1 model / empty required types / bad date / fill errors), engine error response, exceptions.
- `calcValueForMortgageFromRates` — invalid context, empty CSV, bad `pricingOutputType`, build error, engine error response, exceptions.

*Not* anchored on `requestContext`: the `!handles` early-outs (anchor `NullHandle`), `CHECK_HANDLE2(... ratePaths ...)` (anchor `ratePaths`), and `createRequestContext*` (anchor `modelConfig`). Those still set the **global** error but leave the `requestContext` slot untouched.

### 3.4 The inconsistency

The string APIs reset `requestContext` on **both** success and failure; the handle-returning APIs reset `requestContext` **only on failure** (their success clears `result`). Consequences:

- `genRatePaths` fails → error on `requestContext`. A later *successful* `genRatePaths` **leaves it there** (its success clears `result`, not `requestContext`). Only a later string API success (or another failure) clears it.
- So the documented "resets on next call using this handle" holds for the string APIs but **not** for the handle-returning APIs.

---

## 4. Concurrent correctness

### 4.1 Lock topology

| Resource | Mutex | Type |
|---|---|---|
| `HandleData::object_` payload | `objectMutex_` | `shared_mutex` |
| `HandleData::error_`, `bindings_` | `m_` | `recursive_mutex` |
| `HandleContainer::handles_` map | `m_` | `shared_mutex` |

- `READ_LOCK_HANDLE(h)` / `WRITE_LOCK_HANDLE(h)` guard the **payload** via `objectMutex_`.
- `setError`/`getError` guard the **error** via `m_`, which is independent of `objectMutex_`.
- Therefore calling `setHandleError` while a `READ_LOCK_HANDLE` is held (as `calcBehavioralSpeedForInstrument` does at line 1227) **cannot deadlock**, and there is no payload/data race from the error write itself.

### 4.2 The shared-slot race (the user's Q2)

Because `error_` is a single cell with last-writer-wins semantics, `m_` makes each *access* atomic but provides no *set → read* transaction. Sharing one `requestContext` across threads yields:

| # | Thread A | Thread B | Outcome |
|---|---|---|---|
| C1 | `genRatePaths(requestContext)` fails → sets error | `calcBehavioralSpeedForInstrument` succeeds → clears | A's error **lost** if A reads after B's clear |
| C2 | `calcValueForMortgageFromRates` fails → sets error | behavioral succeeds → clears | A's error lost |
| C3 | any `*Obj` / string variant fails → sets error | behavioral succeeds → clears | A's error lost |
| C4 | A's failure lands *after* B's clear | — | A's error survives (B cleared nothing) |

**Race-free per-call channels** remain: the **return value** (empty vs non-empty for strings; `NullHandle` vs handle for handles) and the **thread-local** `getError()`/`hasError()` on the calling thread.

### 4.3 Handle→`HandleData` resolution TOCTOU

`handleData(h)` is `*reinterpret_cast<HandleData*>(h.internal_)` with **no lock**. `isValidHandle(h)` separately checks the container map under a lock. The two are not atomic together:

```
isValidHandle(h)  →  true (map lookup, lock released)
                   [another thread: deleteHandle(h) → shared_ptr destroyed → memory freed]
handleData(h)     →  dereference of dangling pointer → UB
```

This is a latent use-after-free for any concurrent delete/use pattern, independent of the error model. `deleteHandleData` also only zeroes its *local* parameter copy, so the caller's `Handle` still holds the stale pointer after deletion.

> Mitigation today: the documented usage assumes a handle is not deleted while in use on another thread. Worth stating explicitly in API docs and, if possible, resolving `HandleData` through the container map (returning `shared_ptr`) instead of a raw `reinterpret_cast`.

### 4.4 Error writes under read locks (layering smell, not a bug)

`RETURN_SUCCESS`/`RETURN_ERROR` mutate `error_` (via `m_`) while the function still holds a `READ_LOCK_HANDLE` on the *payload* (`objectMutex_`). Because the two mutexes are disjoint this is safe today, but it means "read-locked" functions also mutate observable state — a layering hazard if the two mutexes are ever unified or if a future reviewer assumes read-lock ⇒ read-only.

### 4.5 `hasResultError` vs `hasHandleError` vs business errors

The header distinguishes:
- `hasHandleError`/`getHandleError` = **library fault** (handle/exec errors).
- `hasResultError`/`getResultError` = **business error** in the backend response.

The Vasara `Obj` paths already encode this: the comment before `waitForResult` (`calcValueForInstrumentObj`) explains why the code must **not** branch on `hasResultError()` before consuming the future — it reports "response ready", not "response is an error", and its `getResultError()` branch consumes the future. This is a correct but subtle contract that any refactor of the error paths must preserve.

---

## 5. Sequential scenario analysis (stale-error wipe)

Assuming single-threaded reuse of one `requestContext`, line 1227 wipes a leftover error from these prior failures whenever `calcBehavioralSpeedForInstrument` subsequently **succeeds**:

| # | Prior call (failed) | Error on `requestContext` before 1227 | Cleared? |
|---|---|---|---|
| S1 | `genRatePaths` | yes | yes |
| S2 | `calcValueForMortgageFromRates` | yes | yes |
| S3 | `calcValueForInstrumentObj` | yes | yes |
| S4 | `calcBehavioralSpeedForInstrumentObj` | yes | yes |
| S5 | `calcValueForInstrument` (string) | yes | yes (if caller didn't read it first) |
| S6 | `createRequestContext*` | no (anchor is `modelConfig`) | no (but thread-local global *is* cleared) |

Sequentially this is mostly benign **stale-error hygiene**; the problematic cases are C1–C3 (concurrent) and S5/S1–S4 when the caller legitimately still needs the prior diagnostic.

---

## 6. Findings

### F1 — Inconsistent error-anchor ownership (Medium)

The six entry points disagree on which handle owns the success-side reset. String APIs reset `requestContext`; handle APIs reset `result`. This causes exactly the clobber the discussion identified, and makes `getHandleError(requestContext)`'s value depend on *which* API ran last, not on any single operation.

### F2 — Shared per-handle slot used as a per-call outcome channel (High under concurrency)

A single `error_` cell cannot represent N concurrent invocations on the same handle. Last-writer-wins guarantees at most one thread's outcome survives. This is inherent to the slot design; it is not fixed by moving the clear earlier, later, or to the read side.

### F3 — Success-path clear can destroy a concurrent failure's diagnostic (High under concurrency)

`RETURN_SUCCESS(requestContext, out)` (and the `calcValueForInstrument` equivalent) can erase an unconsumed failure on a shared `requestContext` (§4.2). If concurrent use of a `requestContext` is a supported scenario, this must be fixed; if not, it must be documented and enforced.

### F4 — Pure-observer readers vs. a future consume-on-read (Design risk)

`getError()`/`getHandleError()`/`hasHandleError()` are observers. Internal logic (`fillModelOptionsHandle`, `handleErrFromHandleOrFallbackImpl`, the `execute`/`waitForResult` failure paths) relies on `HandleData::getError()` being non-consuming. Any move to consume-on-read requires a **peek vs. take** split and a single `take` that clears both `error_` and `globalError()` in sync.

### F5 — `getError()` "resets on each call" is not universal (Low)

The `getSwapRates`/`getPrimaryRates`/… getters return `emptyApiString()` for an *absent* component **without touching the error indicator** (deliberate, per the comment in `getRatePathComponentJson`). After a prior failing call, `getSwapRates(...)` can return `""` while a stale global error remains set. This is an intentional "absent ≠ failure" contract, but it weakens the blanket doc statement and is easy to misuse (callers checking `hasError()` after a getter can read a stale error).

### F6 — `HandleData` dereference is a raw pointer with a separate validity check (Medium)

TOCTOU between `isValidHandle` and `handleData` (§4.3); a `Handle` remains a dangling pointer after `deleteHandle`. Safe only under the "no concurrent delete vs. use" assumption.

### F7 — Error state mutated under read locks (Low)

Not a bug (separate mutexes), but a layering hazard and a future deadlock risk if the error mutex and payload mutex are ever merged (§4.4).

---

## 7. Recommendations

### 7.1 Decide who owns the error slot, and apply it uniformly

Pick one of:

- **(a) Last-writer-wins + single-owner-per-context (status quo, documented).** Keep `RETURN_SUCCESS(requestContext, …)`, but make **all six** entry points reset `requestContext` on success (change `genRatePaths`, `calcValueForMortgageFromRates`, and the two `*Obj` variants to also clear `requestContext` on success), so the documented "resets on next call using this handle" is actually true. Document that a handle must not be used concurrently.
- **(b) Per-call delivery (recommended for correctness).** Stop using the shared `requestContext` slot as the outcome channel for these functions. String APIs: signal errors via the return value + thread-local `getError()` only — replace `RETURN_SUCCESS(requestContext, out)` with `return out;` and `RETURN_ERROR(requestContext, …)` with a `NullHandle`/global-error variant. Handle APIs already return a handle (`NullHandle` = error) whose own error slot can carry the diagnostic. This removes the cross-call clobber entirely.

### 7.2 Make error reads either idempotent or consuming — never both, never mixed

Current readers are idempotent. If a consuming read is added, introduce `takeHandleError(h)` / `takeError()` that atomically returns-and-clears **both** stores, keep internal peeks non-consuming, and do not let `RETURN_SUCCESS` also clear (single point of reset).

### 7.3 Resolve `HandleData` through the container map

Prefer returning a `shared_ptr<HandleData>` (or an index) from a locked lookup over `reinterpret_cast`, to close the §4.3 TOCTOU. At minimum, document "a handle must not be deleted concurrently with its use."

### 7.4 Clarify the getter error contract

Either document that `getSwapRates`/`getPrimaryRates`/… do not touch the error indicator (already done in-code, but not in the public header docs), or have them clear the global error on entry to honor "resets on each new API call."

---

## 8. Conclusion

`WfmcmApi.cpp` and `WfmcmVasaraApi.cpp` implement a **simple, last-writer-wins error model** that is internally consistent within each function and safe under the intended single-threaded, single-owner-per-handle usage. The macro layer cleanly separates payload locking (`objectMutex_`) from error locking (`m_`), so the frequent pattern of writing errors while holding a read lock is deadlock-free.

The weaknesses are architectural, not line-level:

1. A **shared per-handle error slot** is asked to serve as both a persistent diagnostic store and a per-call outcome indicator across **six different entry points with inconsistent anchor policies**.
2. That shared slot is **fundamentally unsafe under concurrent sharing of one handle** — a success on one thread can erase a failure on another before it is read.
3. The only robust per-call signals today are the **return value** and the **thread-local** `getError()`; `getHandleError(requestContext)` is only reliable for sequential single-owner use.

The concrete, minimal first step is to **align all six entry points on one error-anchor policy** (§7.1) and, if concurrent reuse of a `requestContext` is ever allowed, to **move error delivery to per-call channels** rather than the shared slot.
