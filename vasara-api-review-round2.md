# Wfmcm Vasara API — Code Review (Round 2, consolidated)

**Scope.** This round reviews the full vasara implementation (`WfmcmVasaraApi.cpp`,
`WfmcmVasaraApi.h`) together with the SWIG core interface (`interface.i`), the Java
binding (`java_interface_i_in.cpp`, `SwigNative.java`, `SwigNativeAbstract.java`),
and the three tier data headers (`ModelSessionData.h`, `RequestContextData.h`,
`ResultData.h`). It supersedes the round-1 header-only review: where the two
disagree, this document governs. Findings are ordered by severity; each entry
states the context, the defect, and the fix applied (or, for the final section,
why it was left in place).

The fixes are contained to the files above and preserve the platform's stated
ownership contract: Java tracks every created handle on a LIFO stack and deletes
all of them explicitly at end of session; garbage collection must never be the
thing that frees a native object handle.

---

## Correction to Round 1

Round 1 recommended making `ModelSessionData` move-only (finding F5). **That was
wrong and must not be applied.** `ModelSessionData` is stored inside `HandleData`'s
`std::any`, and `std::any` requires a *copy-constructible* value type; a move-only
type does not compile there. The construction path compounds this — request-context
creation copies the session via `make_shared<const ModelSessionData>(modelObj)` — so
copyability is load-bearing, not incidental.

Round 2 therefore keeps `ModelSessionData` copyable and solves the problem F5 was
groping at — ownership of the internally created behavioral-map spec handle — with a
`std::shared_ptr<const Handle>` member carrying a custom deleter (see H1). Copies of
the session share ownership of that one native handle; the last owner to be destroyed
releases it. This is the correct model given the `std::any` copy requirement.

---

## Critical

### C1 — Result-retrieval sequence misclassifies successes and destroys error detail

`calcValueForInstrument` and `calcValueForInstrumentObj` decided how to read the
result by calling `hasResultError(request)` *before* `getResult`. The trap is that
`RequestHandleData::hasError()` reports "a response is ready," not "the response is
an error" — it is true for every completed request, success or failure. The
consequences depended on timing:

On the fast path (worker finished before the check), a perfectly good pricing result
took the error branch. `getResultError` then consumed the future, found no error
slices, and returned an empty string, so a correct valuation was reported to Java as
`ApiErrorException` with an empty message — a *silently wrong* result, the worst
possible failure mode for a pricing library.

On the slow path, genuine errors survived but their detail was frequently lost to the
same empty-response drain.

The object-returning variant had an additional defect: because the pre-check's
`getResultError` consumed the future into the slicer, the subsequent `response_.get()`
threw `std::future_error` (no shared state), turning any completed request into an
exception.

**Fix.** Both functions now call `getResult` first and treat an empty return as the
only signal to drain `getResultError` for a message. The object variant uses
`waitForResult` (which does not consume the future into the slicer), guards on
`reqData.isValid()` to avoid a double-`get`, then takes the response with
`response_.get()` and wraps it — success *or* error — via `makeResultHandle`, so error
detail is never discarded. The reasoning is captured in-code so the ordering is not
"cleaned up" back into a bug later.

### C2 — Behavioral-map construction failures were swallowed, producing silent mispricing

`createBehavioralModelMapSpecFromCsv` created the model spec and set its parameters but
ignored failures from `createModelSpec` / `setModelParameter`, returning `NullHandle`
on error indistinguishably from the legitimate empty-CSV ("no behavioral map") case.
`createModelSession` then built a session with no map, and every downstream calc path
quietly skipped `mapBehavioralModel`. The model ran with default behavior and returned
numbers that looked fine and were wrong, with no error anywhere.

**Fix.** `createBehavioralModelMapSpecFromCsv` now takes a `HandleError*` out-parameter.
An empty CSV returns `NullHandle` with the error left as success (a real "no map"
session); any spec/parameter failure is reported through the out-parameter with the
underlying spec's error, and `createModelSession` propagates it and fails the session
rather than constructing a silently map-less one.

### C3 — Use-after-free when a behavioral map fails on the spec handle

In `buildBehavioralRequestFromContext`, when `mapBehavioralModel` failed it could set
its error on the *model spec* handle rather than the portfolio handle. The caller
checked `getError(portfolio)`, saw a success-state `HandleError`, concluded all was
well, and proceeded to take a read lock and dereference the portfolio — which, on the
failure cleanup path, had already been deleted. That is a read of freed handle data.

**Fix.** The map failure is now detected from `mapBehavioralModel`'s own return code
and surfaced as an explicit `HandleError{ static_cast<ApiError>(mapRc),
"mapBehavioralModel failed" }`, independent of which handle the internal error landed
on. The portfolio is held in a `ScopedHandle` so the failure path unwinds it exactly
once with no manual delete, and no post-free access occurs.

### C4 — GC finalization double-deletes handles and raises spurious exceptions

The SWIG `~Handle` destructor unconditionally called native `deleteHandle(*$self)` for
any non-null handle. Combined with the Java `deleteHandle` module wrapper — which calls
`deleteHandleNative(handle)` and then `handle.delete()` — every explicit deletion ran
the native delete twice. The second call hit `CHECK_HANDLE`, returned
`ApiErrorHandleNotFound`, and set the per-thread error indicator, which the `%exception`
handler then raised to Java as an uncatchable, spurious `RuntimeException`. This also
directly violates the "GC must never free a native handle" contract, since the
finalizer was an active native-delete path.

The subtlety that makes this worth care: `interface.i` is shared with the C# and
**Python** bindings, and Python has no explicit LIFO teardown — its finalizer is the
*only* thing that frees a handle. Stripping the native call outright (an option
considered) would leak every handle in Python.

**Fix.** `~Handle` now guards the native delete with `isValidHandle(*$self)` — the exact
idiom `ScopedHandle` already uses internally. This makes native cleanup idempotent:
after Java's explicit delete the handle is no longer valid, so the finalizer only frees
the proxy object (no second native delete, no error, contract honored); under Python the
handle is still valid at GC time and is freed as before. The now-misleading
`%delobject deleteHandle` was removed — it only disowns when the handle is a wrapped
pointer argument, and Pando passes `Handle` by value, so it never actually disowned and
implied a safety mechanism that did not exist. Delete-idempotence is now the single,
documented responsibility of the `isValidHandle` guard.

---

## High

### H1 — Spec-handle leak and a destroy API that does not exist

The behavioral-map spec handle was created natively inside session setup and never
returned to Java, so it was absent from the Java teardown stack and nothing ever deleted
it — a per-session native leak. Compounding the confusion, header comments referred to a
`destroyModelSession` / `destroyRequestContext` that the API does not define; the actual
lifecycle is `deleteHandle` on the session/context handle.

**Fix.** `ModelSessionData` owns the spec via `std::shared_ptr<const Handle>` whose
deleter removes it from the `HandleContainer` when the last session copy is destroyed —
which, under Java's LIFO teardown (contexts before their session), is when the session
handle itself is deleted. The deleter tolerates teardown ordering: if the container is
already gone, the spec's `HandleData` went with it and there is nothing to free. The
header docs were corrected to describe `deleteHandle`-based lifecycle and to state that
the internal spec is released automatically when the session handle is deleted, with no
separate destroy call.

### H2 — Write-only context registry leaked unboundedly

`g_contextsByModelSession` and `g_modelSessionByContext` were populated on every context
creation but never read and never pruned — an unbounded map of stale handle associations
with no consumer.

**Fix.** The registry was removed rather than wired up, since nothing depended on it. A
short comment records that it was dead bookkeeping and why it is gone, so it is not
reintroduced by habit. If a session→context reverse lookup is genuinely needed later, it
should be added with an owner and a removal path.

### H3 — Behavioral path leaked the portfolio handle on exceptions

The pricing path already used `ScopedHandle` for its temporary portfolio, but the
behavioral path created a raw portfolio handle and relied on a manual `deleteHandle`,
which is skipped on any intervening exception or early error return.

**Fix.** The behavioral path uses `ScopedHandle` for the portfolio, matching the pricing
path, so the handle is released on every exit including the throwing ones. (Note:
`ScopedHandle` has no `release()`; where a handle must outlive the scope it is created
without the guard rather than released from it.)

---

## Medium

### M1 — Self-aliasing response-buffer assignment

The result path assigned the thread-local response store to itself
(`store<string, api_response>() = result;` where `result` was that store's own
`c_str()`), a self-aliasing `std::string` assignment that is at best a no-op and at
worst reads a buffer mid-mutation. The return value already points into that
thread-local store — its documented lifetime — so the assignment was removed and the
existing pointer returned directly.

### M2 — Dead accessor returning a pointer after its lock was released

`getresultResponse` was unused and returned a pointer into locked data after the read
lock had already been released — a use-after-unlock waiting for a caller. It was removed.

### M3 — Null vs empty-string inconsistency across the `const char*` APIs

Some `const char*` error paths returned `nullptr` (via `CHECK_HANDLE2(nullptr, ...)`)
while others returned an empty string. A `nullptr` becomes a Java `null` `String` and
NPEs callers that only test `isEmpty()`. The two sites were normalized to
`emptyApiString()` so every `const char*` error path returns an empty string, giving
callers one convention.

### M4 — Success paths left a stale global error for the SWIG layer to trip on

Some success paths returned without clearing the per-thread error indicator via
`RETURN_SUCCESS`, so a stale error from earlier in the call could still be pending when
the `%exception` handler ran, raising a spurious Java exception on an otherwise
successful call. The success paths now go through `RETURN_SUCCESS`.

---

## Low / code smells

Unused includes were dropped (`<unordered_set>` after the registry removal) and
`<string_view>` added where it is now used. Several identifiers were corrected for
consistency (`makeResultHandle`, `ResultDataT`). The `SwigNativeAbstract` base class was
made `abstract`: its `delete()` is a silent no-op meant only as the terminal of the
generated proxies' `super.delete()` chain, and leaving the class instantiable invited a
caller to hold a bare `SwigNativeAbstract`, call `delete()`, and free nothing while
believing otherwise. The concrete no-op body is retained because the generated `Handle`
proxy calls `super.delete()`. A guard was added at the top of `propagate_error` in the
Java binding: if a JNI exception is already pending (e.g. raised by an input array
typemap during marshalling), it must not issue further `FindClass`/`ThrowNew` calls or
stack a second exception over the original — it now clears the native error and returns,
letting the in-flight exception propagate.

---

## Pre-existing issues flagged but not changed

These are real defects found during the review but left untouched because each needs a
broader design decision or touches code outside the vasara layer. They are recorded here
so they are tracked rather than lost.

**P1 — Setup after teardown silently no-ops.** `setupApiLibrary` and `teardownApiLibrary`
are gated by `std::once_flag`s, so a `setup → teardown → setup` sequence leaves the
library torn down: the second setup's `call_once` does nothing and the library stays
uninitialized with no error. Any lifecycle that re-initializes (test harnesses, a worker
that recycles the library) is exposed. Fixing this means rethinking the once-flag gating
into a real state machine, which is a WfmcmApi-level change.

**P2 — Handle-assignment deadlock.** `HandleData`'s copy/move assignment takes two
sequential `scoped_lock`s. Concurrent `a = b` and `b = a` on two threads can deadlock
(each holds one, waits for the other). The fix is a single `std::scoped_lock` acquiring
both mutexes at once (deadlock-avoiding lock ordering), but it lives in core `HandleData`
and should be changed with that owner.

**P3 — Handle identity is a raw pointer (ABA).** A `Handle` carries the `HandleData*`
address, and validity is a container lookup by that address. If an address is reused by a
later `HandleData`, a stale handle compares "valid" and can address the wrong object —
the ABA hazard underlying the `isValidHandle` guard in C4. Robustly fixing this requires
a generation counter or slotmap-style handle, a platform-wide change worth its own
proposal.

**P4 — `hasError()` / `hasData()` are misnamed.** They report "a response is ready," not
"is an error" / "has data" (they resolve to `isResponseReady() || hasErrorSlices()` and
the data analogue). This naming is exactly what invited C1. Renaming to something like
`isResponseReady()` / `hasResponsePayload()` would remove the trap but ripples through
callers.

**P5 — `mapBehavioralModel` is O(n²) in pools.** `ensure_dates(pools, ...)` is called
inside the per-pool loop, re-walking all pools each iteration. Hoisting it out of the
loop makes it linear; left as-is pending confirmation that `ensure_dates` is safe to call
once up front.

**P6 — `getError()` is a destructive move.** It moves the error string out and leaves the
error enum behind as residue, so a second read sees an inconsistent half-cleared state.
Callers currently read once, so this is latent, but the destructive-read semantics should
be made explicit (or the enum cleared in the same step).

**P7 — Behavioral calc holds the context read lock across the whole request build.**
`calcBehavioralSpeedForInstrument` keeps the context read lock for the entire build,
whereas the pricing path snapshots the needed handles under a short lock and releases it
before building. The behavioral path is not wrong, but the inconsistency is worth
aligning to the pricing path's snapshot pattern.

**P8 — Session byte payloads are copied per context but unused.** `modelParams_` and
`modelMapCsv_` are carried in `ModelSessionData` and copied into every request context,
yet nothing downstream reads them once the spec is built. Until the Grid/bytes path is
wired, they are dead weight on every copy; a follow-up could hold them via a
`shared_ptr`-in-`any` or drop them from the per-context copy.

**P9 — `resultIsError` is ambiguous for invalid handles.** It returns `0` both for a
valid success result and for an invalid/wrong-type handle (the latter also setting the
error indicator). Callers that treat `0` as "definitely a success" can be misled; the
header now documents this, but a distinct sentinel or a separate validity check would be
cleaner.
