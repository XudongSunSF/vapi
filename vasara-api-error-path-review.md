# Code Review — Error Anchoring on the Success Path (`WfmcmVasaraApi.cpp:1227`)

- **Date:** 2026-09-22
- **Reviewer focus:** `RETURN_SUCCESS(requestContext, out);` at `api/api/WfmcmVasaraApi.cpp:1227`
- **Files read:** `api/api/WfmcmVasaraApi.cpp`, `api/api/WfmcmApiInternal.h`, `api/api/WfmcmApiInternal.cpp`, `api/api/WfmcmApi.cpp`
- **Related:** `error-and-concurrency-design-review.md` (architecture-level treatment of the same error model; §4.3/F6 handle TOCTOU is *not* re-derived here)

This document is in two parts: **Part A** is line-anchored review comments; **Part B** proposes the target error-handling mechanism for the Vasara entry points.

---

# Part A — Review comments

## Background: `RETURN_SUCCESS` writes two unrelated stores

`RETURN_SUCCESS` (`WfmcmApiInternal.h:87`) → `clearHandleError` (`WfmcmApiInternal.cpp:314`) → `setHandleError(h, {})` (`WfmcmApiInternal.cpp:268`):

```cpp
void setHandleError(Handle h, HandleError error) {
    if (isValidHandle(h)) {
        handleData(h).setError(error);   // (1) shared, per-handle slot
    }
    globalError().second = move(error);  // (2) thread-local slot
}
```

| | Store | Scope | Public reader |
|---|---|---|---|
| **(1)** | `HandleData::error_` (`WfmcmApiInternal.h:413`) | **shared** across threads, guarded by `m_` (`:412`) | `getHandleError(h)` / `hasHandleError(h)` (`WfmcmApi.cpp:3623`) |
| **(2)** | `globalError()` (`WfmcmApiInternal.cpp:258`) | **thread-local** (`store<>` from `thread_local.h`) | `getError()` / `hasError()` (`WfmcmApi.cpp:3613`) |

Nearly every conclusion below follows from the fact that one macro writes both, and that the two have opposite correctness requirements on the success path.

---

## RC1 — `WfmcmVasaraApi.cpp:1227` — success must not clear the **thread-local** store's counterpart on a shared input handle

> **Comment 1 under review:** *"the successful API call should not be responsible to clean up the request's error."*

**Partially agreed — the claim must be split by store.**

**For the thread-local store (2), clearing on success is mandatory, not optional.** This is errno-style "status of my last call on this thread." If success did not clear it, the first failure would latch `hasError() == true` forever and every later `getError()` would report a stale message. No other thread can observe it, so clearing is also race-free. Removing the clear here would be a regression.

**For the per-handle store (1) on `requestContext`, the objection holds**, and for a stronger reason than convention. Enumerate everything that ever writes an error onto a `RequestContextData` handle:

- `WfmcmVasaraApi.cpp:1214` — `buildBehavioralRequestFromContext` failure
- `WfmcmVasaraApi.cpp:1258` — same, `*Obj` variant
- `WfmcmVasaraApi.cpp:1362` — empty/error response in `calcValueForInstrument`
- `WfmcmVasaraApi.cpp:1393`, `:1398` — the hand-rolled catch arms
- the `EXCEPTION_ERROR(requestContext, …)` arms at `:1229`, `:1269`, `:1474`, `:1526`, `:1619`
- `CHECK_HANDLE2(…, requestContext, …)` wrong-type rejections

Every one of these is a **per-invocation outcome**. Nothing intrinsic to the context object — nothing set at construction (`createRequestContextFromHandles`, `:1142`–`:1179`), nothing from `fill()` — ever lands in that slot. So `requestContext`'s error cell is not modelling object state at all; it is a per-call status channel bolted onto a **shared, explicitly reusable input object** (read-locked at `:1202`, holding an immutable `shared_ptr<const ModelConfigDataT>` precisely so it *can* be reused). Clearing it on success is the second half of that mistake, not an independent defect.

**Why the legacy convention does not transfer.** The ~80 `RETURN_API_SUCCESS(request)` sites in `WfmcmApi.cpp` (e.g. `:519`, `:554`, `:587`) clear the error on a handle the call *mutates* — builder-style `setX(request, …)`. There, "handle error == outcome of the last mutation of this object" is coherent and the object has one owner while being built. `requestContext` is the inverse: a read-only input, shared by design. The convention was imported without its precondition.

---

## RC2 — `WfmcmVasaraApi.cpp:1227` — the concurrent clobber is real, and it degrades without concurrency too

> **Comment 2 under review:** *"under concurrent API calls on the same request, the successful call might reset the error state before the failed API calls retrieve the error."*

**Agreed.** `m_` (`WfmcmApiInternal.h:412`) makes each individual access to `error_` atomic, but there is no *set → caller-read* transaction, and there cannot be one at this layer: the caller decides when to read, after the API call has returned and released every lock.

```
Thread A: calcValueForInstrument(ctx, …)          → fails  → setHandleError(ctx, err)
Thread B: calcBehavioralSpeedForInstrument(ctx, …) → succeeds → line 1227 clears ctx
Thread A: getHandleError(ctx)                      → "" (clean)  ← A's diagnostic is gone
```

Two refinements worth recording, because they bound the blast radius and point at the fix:

1. **The race is confined to the `getHandleError(h)` path.** Thread A's `getError()`/`hasError()` still work: `setHandleError` also wrote A's *thread-local* copy at `WfmcmApiInternal.cpp:273`, which B cannot touch. Callers already on the thread-local path are unaffected; callers on the per-handle path were already unsound. That asymmetry is the argument for standardising on the thread-local channel (Part B).

2. **It is not only a concurrency bug.** The same state is incoherent single-threaded, because of RC3: `calcBehavioralSpeedForInstrumentObj` fails → error on `ctx`; a subsequent *successful* `calcValueForInstrumentObj` clears `result` (`:1472`), not `ctx`; so `ctx` stays dirty indefinitely and a later `hasHandleError(ctx)` reports a failure that has long been superseded.

---

## RC3 — `WfmcmVasaraApi.cpp:1227` and `:1387` are the only two sites that mutate an **input** handle on success

Success-path anchors across this file:

| Function | Success anchor | Handle clarity |
|---|---|---|
| `createModelConfig` `:1088` | `h` (fresh) | inert — nobody else can reach it yet |
| `createRequestContext` `:1137` | `h` (fresh) | inert |
| `createRequestContextFromHandles` `:1179` | `h` (fresh) | inert |
| **`calcBehavioralSpeedForInstrument` `:1227`** | **`requestContext` (input, shared)** | **mutates caller-visible shared state** |
| **`calcValueForInstrument` `:1387`** | **`requestContext` (input, shared)** | **mutates caller-visible shared state** |
| `calcBehavioralSpeedForInstrumentObj` `:1267` | `result` (fresh) | inert |
| `calcValueForInstrumentObj` `:1472` | `result` (fresh) | inert |
| `genRatePaths` `:1524` | `result` (fresh) | inert |
| `calcValueForMortgageFromRates` `:1617` | `result` (fresh) | inert |

Six of eight clear a freshly created handle, which is harmless and meaningless. Two clear a shared input. **The majority convention in this file is already the correct one**; `:1227` and `:1387` are the outliers, and they are exactly the two that can lose another caller's diagnostic. This is the cheapest possible fix surface: two lines.

---

## RC4 — `EXCEPTION_ERROR`'s `WRITE_LOCK_HANDLE` guards the wrong mutex and buys nothing

`WfmcmApiInternal.h:152`:

```cpp
#define EXCEPTION_ERROR(handle, ret) } \
catch (const LibException& ex) { \
    WRITE_LOCK_HANDLE(handle)                                    // ← locks objectMutex_
    std::string troubleshoot = getGlobalTroubleShootInfo(ex); \
    RETURN_ERROR_WITH_TROUBLESHOOT(handle, …);                   // ← writes error_, locks m_
} \
catch (const std::exception& ex) { \
    WRITE_LOCK_HANDLE(handle)                                    // ← same
    RETURN_ERROR(handle, ApiErrorException, ex.what(), ret); \
}
```

The two mutexes are disjoint:

- `WRITE_LOCK_HANDLE(h)` → `handleWriteLock` (`WfmcmApiInternal.cpp:226`) → `handleData(h).getLock()` → **`objectMutex_`** (`WfmcmApiInternal.cpp:91`, `WfmcmApiInternal.h:415`) — guards the `std::any` payload.
- `RETURN_ERROR` → `setHandleError` → `HandleData::setError` (`WfmcmApiInternal.cpp:101`) → `scoped_lock<recursive_mutex>(m_)` — guards **`error_`** (`WfmcmApiInternal.h:412`).

The catch arms take the payload lock and then write a field that the payload lock does not protect, using a different lock that `setError` acquires for itself anyway. **The write lock is decorative.** The same pattern is hand-rolled at `WfmcmVasaraApi.cpp:1393` and `:1398`.

Three reasons this is worth fixing rather than ignoring:

1. **It is not free.** `unique_lock` on a `shared_mutex` blocks until every concurrent reader of that handle drains. On a `requestContext` shared by N in-flight calls (all holding `READ_LOCK_HANDLE`, e.g. `:1202`, `:1246`, `:1421`, `:1491`), the *error* path serialises against all of them — worst latency on the worst path, for zero synchronisation benefit.
2. **It misleads.** It reads as "error writes are protected here," which invites someone to weaken or drop the `m_` lock inside `setError`, or to add a genuinely payload-touching statement to the catch arm and assume it is covered when the ordering guarantee they need was never established.
3. **It encodes a latent self-deadlock.** It is safe *today* only because `READ_LOCK_HANDLE` is always taken inside the `TRY` block, so unwinding releases the shared lock before the handler runs. Any future entry point that read-locks a handle *before* `TRY` — or in a scope enclosing it — self-deadlocks on a non-recursive `shared_mutex`. Nothing in the macro or its doc comment warns about this precondition.

**Recommendation:** delete `WRITE_LOCK_HANDLE(handle)` from both arms of `EXCEPTION_ERROR` and from the hand-rolled arms at `WfmcmVasaraApi.cpp:1393`/`:1398`. `setHandleError` is already self-synchronising. If a future arm needs the payload, it should take the lock explicitly with a comment saying what it protects.

*(This is a sharper instance of §4.4/F7 in `error-and-concurrency-design-review.md`, which covers writing errors while holding a **read** lock. The point here is the converse: the **write** lock in the catch arms is not synchronisation at all.)*

---

## RC5 — Error text and payload share one thread-local buffer

Found while tracing the error channel; independent of `:1227` but it constrains any redesign.

- `serializeResponseToApiBuffer` (`WfmcmVasaraApi.cpp:260`) writes `store<std::string, utility_id::api_response>()`.
- `storeApiString` (`:300`) writes **the same slot**, and `resultGetError` (`:1652`) goes through it.

So the documented-lifetime payload pointer returned by `calcValueForInstrument` is invalidated by a subsequent `resultGetError()` on the same thread:

```cpp
const char* payload = calcValueForInstrument(ctx, csv, -1);  // → api_response
const char* err     = resultGetError(result);                // → overwrites api_response
// payload now aliases err
```

`getError()`/`getHandleError()` are safe here — they use their own slots (`id::api_global_error`, `id::api_handle_error`) — but `resultGetError` is not. Give error text its own `utility_id` slot before routing more diagnostics through it.

---

## Verdict on `WfmcmVasaraApi.cpp:1227`

**The clear should not be applied to `requestContext`.** The convention it follows is sound for the handle the call *produces* or *mutates*, and unsound for a shared input. Both of the reviewer's comments identify real defects; comment 1 needs the two-store split to be stated correctly (the thread-local clear must stay).

The proposed alternative in comment 1 — *failed calls retrieve and clean up* — trades a lost-update race for a sticky-error leak: if a caller never reads, the error poisons the next `hasHandleError(ctx)`, and consume-on-read semantics were already a bug here once (`WfmcmApiInternal.cpp:110`–`:118`, where `move(error_)` in an observer emptied the message during `hasHandleError`). The fix is not to change *who* cleans up the shared slot; it is to stop using the shared slot as a per-call channel.

---

# Part B — Proposed error-handling mechanism

## B1. Principle: three error kinds, three channels

Today one `HandleError` cell carries all three. Separate them by **lifetime and owner**:

| Kind | Question it answers | Lifetime | Channel | Reader |
|---|---|---|---|---|
| **Call status** | "did *my* invocation fail?" | one call, one thread | return value + **thread-local** `globalError()` | `hasError()` / `getError()` |
| **Object state** | "is this handle unusable?" | as long as the handle | **per-handle** `HandleData::error_` | `hasHandleError(h)` / `getHandleError(h)` |
| **Business error** | "did the engine reject the request?" | as long as the result | the `ResultData` payload | `resultIsError(h)` / `resultGetError(h)` |

Four rules follow:

- **R1 — A call writes error state only to a handle it created or exclusively mutates.** Never to an input handle. This single rule kills RC1, RC2 and RC3.
- **R2 — Call status is thread-local and total.** Every entry point terminates in exactly one success/failure macro, so "thread-local error == status of my last call on this thread" is an invariant with no exceptions.
- **R3 — Object state is set where the object is built** (construction, `fill()`, builder-style setters) and is the *only* legitimate use of `RETURN_SUCCESS(handle, …)`.
- **R4 — Business errors ride the result handle**, which is per-call and unshared, so they need no clearing policy at all.

Under R2 there is nothing to "clean up": the next call on the same thread overwrites the thread-local slot, and no other thread can see it. Consume-on-read becomes unnecessary rather than merely undesirable.

## B2. Macro set

Add a call-scoped tier alongside the existing handle-scoped one.

```cpp
// WfmcmApiInternal.cpp — factor out the thread-local write
void setCallError(HandleError e) { globalError().second = std::move(e); }

void setHandleError(Handle h, HandleError e) {   // unchanged behavior
    if (isValidHandle(h)) { handleData(h).setError(e); }
    setCallError(std::move(e));
}
```

```cpp
// WfmcmApiInternal.h
/** Return success. Clears this thread's call status. Touches no handle.
 *  Use when every handle argument is an input the function does not own. */
#define RETURN_CALL_SUCCESS(ret) \
    do { setCallError({}); return ret; } while (false)

/** Return failure on this thread's call status only. Touches no handle. */
#define RETURN_CALL_ERROR(err, text, ret) \
    do { setCallError({err, text}); return ret; } while (false)

#define RETURN_CALL_ERROR_WITH_TROUBLESHOOT(err, text, ctx, ret) \
    do { setCallError({err, text, ctx}); return ret; } while (false)

/** Exception arms for call-scoped functions. No lock: globalError() is thread-local. */
#define CALL_EXCEPTION_ERROR(ret) } \
catch (const LibException& ex) { \
    std::string troubleshoot = getGlobalTroubleShootInfo(ex); \
    RETURN_CALL_ERROR_WITH_TROUBLESHOOT(ApiErrorException, ex.what(), std::move(troubleshoot), ret); \
} \
catch (const std::exception& ex) { \
    RETURN_CALL_ERROR(ApiErrorException, ex.what(), ret); \
}
```

Also add `CHECK_HANDLE_CALL(ret, h, …)` mirroring `CHECK_HANDLE2` but routing through `RETURN_CALL_ERROR` — today `CHECK_HANDLE2`'s wrong-type branch writes an error onto the caller's handle, which violates R1 for the same reason `:1227` does. (Its invalid-handle branch is already a no-op on the handle, since `setHandleError` skips the write when `isValidHandle` is false.)

Existing `RETURN_SUCCESS` / `RETURN_ERROR` / `EXCEPTION_ERROR` stay, scoped by R3 to handles the function owns. Document that precondition in their doc comments; it is currently absent, which is how `:1227` happened.

## B3. Canonical entry-point shape

```cpp
Handle calcValueForInstrumentObj(Handle requestContext, const char* data, int len)
{
    TRY
    CHECK_INIT2;
    CHECK_HANDLE_CALL(NullHandle, requestContext, HandleType::RequestContextData);

    msg::Request req;
    {
        READ_LOCK_HANDLE(requestContext);            // payload lock, narrowest scope
        const auto& ctx = handleObject<HandleType::RequestContextData>(requestContext);
        if (auto e = buildRequestFromContext(ctx, data, len, req); e.isError()) {
            RETURN_CALL_ERROR(e.error_, e.reason_.c_str(), NullHandle);   // ctx untouched
        }
    }                                                 // lock released before dispatch

    msg::Response response = msg::RequestProcessor::handle(std::move(req));
    const Handle result = makeResultHandle(std::move(response));  // business error rides here
    RETURN_CALL_SUCCESS(result);

    CALL_EXCEPTION_ERROR(NullHandle)
}
```

Properties: no write to any input handle; the payload lock covers only the payload read; the exception arms take no lock; success and failure both leave the thread-local status correct.

## B4. Caller contract (to be documented in `WfmcmVasaraApi.h`)

1. Check the **return value** first: `NullHandle` for handle APIs, empty string for `const char*` APIs.
2. On failure, call `getError()` **on the same thread, before the next API call on that thread**.
3. On success from a handle API, check `resultIsError(h)` / `resultGetError(h)` for engine-side business errors — a library success can carry a business failure.
4. `getHandleError(h)` reports **object state only**. It is not a per-call channel and must not be used to diagnose the call that just returned.
5. A handle must not be deleted concurrently with its use (see `error-and-concurrency-design-review.md` §4.3/F6).

Under R2 this also settles the getter ambiguity (F5 in that doc): `getSwapRates` and friends (`WfmcmVasaraApi.cpp:316`–`:337`) end in `RETURN_CALL_SUCCESS(emptyApiString())` for an absent component, so "absent" clears the status and "failed" sets it — an empty return is disambiguated by `hasError()` instead of being indistinguishable from a stale error.

## B5. Migration

**Phase 1 — stop writing to input handles (small, behaviour-preserving for correct callers).**
- `WfmcmVasaraApi.cpp:1227`, `:1387`: `RETURN_SUCCESS(requestContext, x)` → `RETURN_CALL_SUCCESS(x)`.
- `:1214`, `:1258`, `:1362`: `RETURN_ERROR(requestContext, …)` → `RETURN_CALL_ERROR(…)`.
- `:1229`, `:1269`, `:1474`, `:1526`, `:1619`: `EXCEPTION_ERROR(requestContext, r)` → `CALL_EXCEPTION_ERROR(r)`; likewise the hand-rolled arms at `:1391`–`:1399` (keep their `logVasaraDebug` calls).
- After this, no Vasara entry point writes to `requestContext`, and RC1/RC2/RC3 are closed.

**Phase 2 — drop the decorative write locks (RC4).** Remove `WRITE_LOCK_HANDLE` from both `EXCEPTION_ERROR` arms and from `:1393`/`:1398`.

**Phase 3 — split the thread-local string buffer (RC5).** Give error text its own `utility_id` so `resultGetError` stops aliasing `api_response`.

**Phase 4 — sweep `WfmcmApi.cpp`.** Audit the ~80 `RETURN_API_SUCCESS(request)` sites against R3: keep it where the call mutates `request` (the builder setters — legitimate object state), convert the rest.

## B6. Tests to add

- **Concurrency:** N threads sharing one `requestContext`, half issuing calls that fail deterministically and half calls that succeed; assert each thread's `getError()` matches its own last call. This fails today and passes after Phase 1.
- **Sequential hygiene:** fail → succeed → `hasHandleError(ctx) == false` and `hasError() == false`; fail → `hasError() == true` with a non-empty message (regression guard for the `move(error_)` bug at `WfmcmApiInternal.cpp:110`).
- **Buffer aliasing:** hold a payload pointer, call `resultGetError`, assert the payload is still readable (fails until Phase 3).
- **Absence vs failure:** `getSwapRates` on a rate-path set lacking swap rates returns `""` with `hasError() == false`, immediately after an unrelated failing call.

---

## Summary

| ID | Finding | Location | Severity |
|---|---|---|---|
| RC1 | Success clears the error slot of a shared **input** handle; correct for the thread-local store, wrong for the per-handle store | `WfmcmVasaraApi.cpp:1227` | Medium |
| RC2 | Concurrent success erases an unread failure on a shared `requestContext`; also leaves stale state single-threaded | `:1227`, `:1387` | High under concurrency |
| RC3 | Only 2 of 8 success paths mutate an input handle; the other 6 already follow the correct convention | file-wide | Medium |
| RC4 | `WRITE_LOCK_HANDLE` in the catch arms guards `objectMutex_` while the write targets `error_`/`m_` — no protection, real cost, latent self-deadlock | `WfmcmApiInternal.h:152`; `WfmcmVasaraApi.cpp:1393`, `:1398` | Low (correctness) / Medium (maintenance) |
| RC5 | `resultGetError` shares the `api_response` thread-local buffer with the payload | `WfmcmVasaraApi.cpp:260`, `:300`, `:1652` | Medium |
