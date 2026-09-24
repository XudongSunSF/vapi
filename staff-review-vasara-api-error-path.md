# Staff Review — `vasara-api-error-path-review.md` (Principal Engineer's Error-Path Review)

- **Date:** 2026-09-23
- **Reviewed:** `vasara-api-error-path-review.md` (Part A: RC1–RC5; Part B: B1–B6)
- **Cross-referenced:** `error-and-concurrency-design-review.md`, `api/api/WfmcmApiInternal.{h,cpp}`, `api/api/WfmcmApi.cpp`, `api/api/WfmcmVasaraApi.cpp`, `api/api/WfmcmApi.h`, `api/api/WfmcmApiComponents.h`
- **Verdict:** Part A is high-quality and I concur with all five findings. **Part B I do not endorse as the target design** — it is an incremental patch that adds a third tier of macros onto an errno-style ambient-error foundation, when the correct move (and the one the author is already signalling with the `//TODO: convert this to std::error_code` at `WfmcmApiInternal.h:330`) is to replace the foundation: **errors as first-class values carried by a per-call result object, and zero ambient error state.**

---

## 1. What is right and must be preserved

The principal engineer's Part A is the strongest analysis of this file to date. Concurring wholesale:

- **RC1's two-store split** is correct and important: clearing the thread-local slot on success is mandatory (errno semantics), clearing the per-handle slot of a shared *input* handle is wrong. The insight that "the convention was imported without its precondition" is exactly right.
- **RC2/RC3** are correct: the clobber is real, is confined to the `getHandleError()` path, and is *also* broken single-threaded (stale error survives a fail→success transition on the handle APIs).
- **RC4** is a genuinely sharp catch: the `WRITE_LOCK_HANDLE` in the exception arms locks `objectMutex_` while `setError` re-locks `m_` itself — decorative, non-free, and a latent self-deadlock.
- **RC5** (payload and error text aliasing the same thread-local buffer) is correct and independently worth fixing.
- **R1 ("a call writes error state only to a handle it created or exclusively mutates, never to an input handle")** is the single most valuable invariant in the document and should be the load-bearing rule of whatever replaces the current model.
- **B1's taxonomy** (call status / object state / business error) is directionally correct and matches what the code actually does.

The disagreements below are therefore *not* about Part A's findings. They are about whether Part B's mechanism is the right end state.

---

## 2. The core disagreement: Part B patches the errno-style foundation instead of replacing it

The proposal keeps the two existing ambient stores (`globalError()` thread-local + `HandleData::error_`) and **adds a third tier of macros** (`RETURN_CALL_SUCCESS` / `RETURN_CALL_ERROR` / `CALL_EXCEPTION_ERROR` / `CHECK_HANDLE_CALL`) that routes Vasara call status through the thread-local store only. That is a *generalization of the existing machinery*, not a redesign. The tell-tale signs:

1. **`RETURN_CALL_ERROR` still writes ambient state.** `setCallError()` writes `globalError()`. It is thread-local so it is race-free, but it is still "read-after-the-fact" errno state: it is invisible to the return-value check, it is clobbered by any *nested* API call the caller makes before reading (a logging hook, a callback, a getter — several getters themselves terminate in a macro that clears the slot), and it cannot carry structure.

2. **`RETURN_CALL_SUCCESS(ret)` is still a *write* on the success path.** It calls `setCallError({})`. In a value-returning model, success is simply `return ret;` with no ambient write at all. The proposal treats "clearing ambient state on success" as the mechanism to fix, when the fix is to have no ambient state to clear.

3. **The per-handle `error_` slot survives.** The proposal re-scopes `getHandleError()`/`hasHandleError()` to "object state." But the author's own evidence kills this channel: Part A RC1 shows that *nothing intrinsic to the object ever lands in that slot* — and that generalizes. **There is no object-state error in the system today.** A `fill()`/`execute()` failure is a per-call outcome; the handle remains valid and retryable. The one piece of genuine object state that exists (`RequestHandleData::isValid()` — "future already consumed") is *not* in `error_`. The per-handle slot is per-call everywhere it is used, which is precisely why `RETURN_API_SUCCESS` clears it on every setter. It is dead weight.

4. **The macro layer is retained and extended.** B2 adds four more macros; B5 Phase 4 still requires auditing "~80 `RETURN_API_SUCCESS` sites." The macro layer with hidden `return` statements and hidden locks is itself a symptom. State of the art is functions returning values plus RAII lock guards, with a *single* exception-translation helper — not per-entry-point macro scaffolding.

5. **The dual-write in `setHandleError` is left intact.** `setHandleError` (unchanged in B2) still writes both the per-handle slot and the thread-local slot, so the two-store coupling the whole document is about is never removed.

6. **A migration hazard is introduced silently.** After Phase 1, the Vasara handle APIs populate `getError()` but no longer populate `getHandleError(requestContext)` (B2's call-scoped macros touch no handle). The documented contract "if a handle error occurs, `getError()` returns the same value" (`WfmcmApi.h:1401`) then holds for the ~80 legacy sites and *fails* for the six Vasara sites. Callers checking `getHandleError(h)` on an input handle will read "no error" while `getError()` reports one. A partial cutover across one shared reader is worse than either end state.

The root cause is not the *anchors* at `:1227`/`:1387`. The root cause is that **the diagnostic (message) is divorced from the outcome (return value) at the boundary**: `int`-returning functions return a code but put the message in ambient state; handle-returning functions return `NullHandle` but put the message in ambient state. Every defect in this document — RC1, RC2, RC3, RC5, and the consume-on-read bug at `WfmcmApiInternal.cpp:110` — is a consequence of that divorce.

---

## 3. State of the art for a library error API

The modern practice, in C++ and across the C ABI boundary to Java, is:

1. **Errors are values, not ambient state.** The caller receives the error as part of the call's result. Thread-safety is then by construction: a per-call value cannot be clobbered by another thread or another call.
2. **One structured error type, not three stores.** `{ category, code, message, cause, context }`, where `cause` is the nested-exception chain and `context` is key/value detail (which handle, which instrument row, which model). This is what `HandleError` is halfway toward and what the `//TODO: convert this to std::error_code` at `WfmcmApiInternal.h:330` is already pointing at. The codebase already uses `std::error_code` elsewhere (the `json_parser`), so the pattern is established.
3. **Code and message travel together.** Never separate "which error" from "what happened."
4. **Business errors and library errors share the same carrier, discriminated by category.** The current `ResultData`/`resultIsError`/`resultGetError` machinery already does this for business errors; the correct move is to make *library* and *validation* errors ride the same per-call result object, so every outcome of a call is interrogated the same way.
5. **Exceptions are internal; translation happens once at the boundary.** The engine throws `LibException` with nested causes and a troubleshoot trace. That is fine and should stay. What must change is that the API layer translates exceptions into a structured `Error` value in exactly one place, instead of re-deriving `getGlobalTroubleShootInfo(ex)` in macro arms at every entry point.
6. **Concurrency is a non-issue for error state** because there is no shared error state — only the shared *input* handle, whose concurrency contract shrinks to "read-only input + not deleted while in flight."

---

## 4. Target design (from scratch)

### 4.1 One structured error type

```cpp
// api/error.h  (internal)
enum class ApiErrorCategory : int {
    Success    = 0,
    Validation,   // caller's input is bad; not a library fault
    Library,      // internal fault
    System,       // OS/resource/IO
    Business,     // engine rejected the request
};

struct ApiError {
    ApiErrorCategory category{ApiErrorCategory::Success};
    int              code{0};                 // stable, documented, ABI-facing
    std::string      message;                 // human-readable, free-form
    std::optional<ApiError> cause;            // nested (mirrors the LibException chain)
    std::vector<std::pair<std::string, std::string>> context; // "handle"/"row"/"model"…
    std::string      troubleshootTrace;       // the existing troubleshoot string

    bool ok() const noexcept { return category == ApiErrorCategory::Success; }
    std::error_code toErrorCode() const;      // std::error_category interop
};
```

This absorbs `HandleError` (the `troubleshoot_` field maps to `troubleshootTrace`), adds the category the current flat `ApiError` enum lacks, and adds `context` so downstream consumers can act programmatically instead of parsing strings.

### 4.2 Internal `Result<T>`

```cpp
template <class T> using Result = std::expected<T, ApiError>;   // C++23
// On the current C++20 toolchain: a ~40-line Result<T> shim, or tl::expected.
```

Entry points become value-returning:

```cpp
Result<Handle> calcValueForInstrumentObj(Handle requestContext, std::string_view csv) {
    auto ctx = readRequestContext(requestContext);       // Result<const RequestContextDataT&>
    if (!ctx) return std::unexpected(std::move(ctx).error());
    auto req = buildPricingRequest(*ctx, csv);           // Result<PricingRequestInputs>
    if (!req) return std::unexpected(std::move(req).error());
    auto resp = dispatch(std::move(*req));               // Result<msg::Response>
    if (!resp) return std::unexpected(std::move(resp).error());
    return makeResultHandle(std::move(*resp));           // business error rides the handle
}
```

Properties that matter:

- **No lock in the error path.** Payload reads take a `std::shared_lock` in a narrow scope; error creation touches nothing shared.
- **No writes to any input handle.** `requestContext` is strictly read-only.
- **Success is `return value;`** — there is nothing to clear.
- **The nested `LibException` chain is captured into `cause` by one `translate()` function**, not by per-entry-point macro arms.

### 4.3 Public surface (the C ABI)

The ABI is `extern "C"` + SWIG to Java, so the *surface* stays C-flavored, but it becomes:

- **Every computation returns a handle; every outcome lives on that handle.** `NullHandle` means "library/validation error" — same convention as today — but the *diagnostic* now rides the returned handle, discriminated by category:

  ```c
  Handle calcValueForInstrumentObj(Handle requestContext, const char* data, int len);
  // callers:
  //   if (resultIsError(h))        -> category == Business? No? then library/validation
  //   resultGetErrorCode(h)        -> ApiErrorCategory + code
  //   resultGetError(h)            -> message (own buffer, see §4.6)
  //   resultGetErrorCause(h)       -> nested cause handle (or null)
  //   else resultGetCleanPrice(h), resultGetPricingResultJson(h), …
  ```

  This **unifies** business errors and library errors onto one per-call, unshared, immutable object — no clearing policy, no clobber, no TOCTOU on the error.

- **String-returning variants are deprecated, not fixed.** `const char*` into a thread-local buffer is the source of RC5 and the earlier self-aliasing bug, and it cannot be made safe. The `*Obj` handle variants already exist and are strictly better. If a string form is still required during migration, give it an explicit out-param:

  ```c
  int calcValueForInstrument(Handle requestContext, const char* data, int len,
                             char* out, size_t outLen, ApiError* outError);
  ```

  Caller-owned buffers and caller-owned error structs are the C-idiomatic, thread-safe answer.

- **`getError()` / `hasError()` / `getHandleError()` / `hasHandleError()` are removed from the new API** (kept only as deprecated shims during migration). They are ambient state, and ambient state is the disease.

### 4.4 Exception translation (one place)

```cpp
ApiError translateException(const std::exception_ptr&) noexcept;   // walks nested causes
ApiError translateCurrentException() noexcept;                     // catch(...) helper
```

`getGlobalTroubleShootInfo` becomes the internals of this function. Every entry point ends in `catch (...) { return std::unexpected(translateCurrentException()); }`. This deletes `EXCEPTION_ERROR`, `CALL_EXCEPTION_ERROR`, and the hand-rolled arms at `:1391`–`:1399` in one stroke (RC4 evaporates because there is no lock to take).

### 4.5 `requestContext` and handle lifetime

With no error slot and no writes, `requestContext` is a pure read-only input. Its remaining hazard is the delete-while-in-use TOCTOU (`error-and-concurrency-design-review.md` §4.3/F6). Two-part fix:

1. **Resolve `HandleData` through the container map, returning `shared_ptr<HandleData>`**, rather than the lock-free `reinterpret_cast` in `handleData()` (`WfmcmApiInternal.cpp:238`). The map already holds `shared_ptr`s under a `shared_mutex`.
2. **Keep-alive:** the result handle holds a `shared_ptr` to the `requestContext`'s `HandleData` for the duration of the call, so a concurrent `deleteHandle(requestContext)` cannot free it mid-call. Document: an input handle may be shared and read concurrently; it must not be *mutated* while in flight.

### 4.6 One buffer per purpose

Every public getter that returns `const char*` gets its own thread-local slot *or* becomes handle-based. Specifically `resultGetError` must not alias `api_response` (RC5). Under §4.3 most of these become handle-getters and the problem disappears.

### 4.7 Java binding

With structured errors, the SWIG wrapper should throw rather than poll ambient state:

- success handle → return value;
- error handle → `throw new ApiException(category, code, message, cause, context)`.

This matches how Java callers actually consume errors, and removes the current `getError()`-after-every-call boilerplate the review's B4 item 2 describes.

---

## 5. Disposition of the principal engineer's proposals

| Item | Assessment | Action |
|---|---|---|
| RC1 | Agreed | Subsumed by R1; survives as the invariant "never write to an input handle." |
| RC2 | Agreed | **Superseded**: with a per-call result handle there is no shared slot to clobber. |
| RC3 | Agreed on the fact | The fix is not `RETURN_CALL_SUCCESS(out)` — it is `return out;`. |
| RC4 | Agreed | Fix now, independent of the redesign: delete the `WRITE_LOCK_HANDLE` in the catch arms. |
| RC5 | Agreed | The stopgap is a dedicated buffer; the real fix is killing `const char*`-returning APIs. |
| B1 (three kinds, three channels) | Taxonomy right, carrier wrong | **Kind ≠ channel.** Keep the taxonomy, collapse the channels to one `ApiError` type + one per-call carrier. |
| B2 (macro set) | Rejected | Extends the macro/ambient layer. Replace with `Result<T>` + `translateException()`. |
| B3 (canonical shape) | Shape right, mechanics wrong | Narrow lock scope and no-input-writes are correct; implement with `Result`, not macros. |
| B4 (caller contract) | Mostly right | Item 2 ("call `getError()` before the next call on this thread") *disappears* — that is the point of the redesign. |
| B5 (migration) | Partial | Phases 1–3 are good stop-the-bleeding steps but do not converge to the target; see §6. |
| B6 (tests) | Good | Add: "new API never consults ambient readers," and a Java-side exception-mapping test. |

---

## 6. Migration that actually ends at the target

Two tracks, run in parallel so the fix is not a series of detours:

**Track A — stop the bleeding now (cheap, behavior-preserving for correct callers).**
1. `:1227`, `:1387`: replace `RETURN_SUCCESS(requestContext, x)` with `return x;`.
2. `:1214`, `:1258`, `:1362`, `:1391`–`:1399`, and the `*Obj`/`genRatePaths`/`calcValueForMortgageFromRates` failure arms: route through the *thread-local* path only (no handle write), pending the real type.
3. Delete the `WRITE_LOCK_HANDLE` from `EXCEPTION_ERROR` and the hand-rolled arms (RC4).
4. Give `resultGetError` its own buffer (RC5).
5. Document per-function semantics during the transition: after step 2, `getHandleError(inputHandle)` is **not** populated by the Vasara entry points — state this explicitly to avoid the §2.6 hazard.

**Track B — replace the foundation (the actual work).**
1. Introduce `ApiError` (§4.1) and `Result<T>` (§4.2). Absorb `HandleError`; mark the flat `ApiError` enum as legacy.
2. Write `translateCurrentException()` and collapse all exception arms to it.
3. Make every computation entry point return a result handle (§4.3); add `resultGetErrorCode` / `resultGetErrorCause`.
4. Resolve `HandleData` via the map (§4.5).
5. Deprecate `getError()` / `hasError()` / `getHandleError()` / `hasHandleError()` and the string-returning variants; delete them when the last legacy caller migrates.
6. SWIG: throw `ApiException` from structured errors (§4.7).

---

## 7. Risks and open questions

1. **ABI break.** This is a surface redesign. Decide explicitly: version the ABI (`apiV2_*` prefix or a namespace), or accept a breaking release. Do not silently re-semanticize the existing readers.
2. **`std::expected` availability.** The code is on C++20 (`std::format`, `std::chrono_literals`). Either adopt C++23 or vendor `tl::expected` / a 40-line `Result<T>`. Do not let this block the design — the shim is trivial.
3. **Error-code stability.** `code` values become ABI. Freeze and document them; do not reuse the current volatile `ApiError` enum values without a mapping table.
4. **`context` bloat.** Key/value context is powerful but must be bounded (a fixed max entries/bytes) so error construction on a hot path stays cheap.
5. **Engine exceptions remain.** This design deliberately keeps the engine's throw-based internals; the only contract change is the single translation point at the API boundary. If the team later wants error-code propagation *inside* the engine, that is a separate, much larger effort and should not be conflated with this fix.
6. **Legacy `WfmcmApi.cpp` (~80 macro sites).** Track B item 5 is the long pole. It is mechanical, but it is the difference between "patched" and "replaced." Budget for it.

---

## 8. Conclusion

The principal engineer's review correctly identifies every defect and correctly derives the one invariant that matters (R1). But its Part B is a *better errno*, not a replacement: it adds a call-scoped macro tier on top of the two stores instead of deleting the stores. The codebase's own `//TODO: convert this to std::error_code` is the admission that the current model is a placeholder.

The staff position: **adopt Part A's findings immediately, reject Part B's mechanism as the end state, and build the small, structured, value-based error API described in §4.** It is less code than the current macro layer, it makes the concurrency questions vanish rather than merely bound them, and it is the model the Java binding, the tests, and the next engineer will actually want to inherit.
