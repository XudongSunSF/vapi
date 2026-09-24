## Summary

Fix two error-path defects identified in the API layer review:

- **RC4** — exception handlers took a payload write-lock (`objectMutex_`) that did not protect the field they were about to write (`error_`, guarded by `m_`). The lock was decorative: it blocked concurrent readers on the worst path for zero synchronization benefit, and was a latent self-deadlock hazard. Removed it.
- **RC5** — `resultGetError()` returned error text through the same thread-local `api_response` buffer that holds the payload returned by the `const char*` calc APIs. A caller that held a payload pointer and then called `resultGetError()` would see the payload overwritten. Error text now uses its own per-thread buffer.

## Motivation

These are two concrete findings from the error-path design review. Both are independent of the larger error-model redesign and are safe to land now:

- RC4 removes lock acquisition that provides no correctness guarantee — `setHandleError`/`HandleData::setError` already take the handle's `m_` mutex internally.
- RC5 removes an actual aliasing bug: `const char*` return values point into a thread-local buffer, and two different APIs were sharing one buffer for unrelated purposes.

## Changes

### `api/api/WfmcmApiInternal.h`

- Removed `WRITE_LOCK_HANDLE(handle)` from both catch arms of the `EXCEPTION_ERROR` macro.
- Updated the macro's doc comment: error state is written via `setHandleError`, which is self-synchronising (`HandleData::setError` takes `m_`), so no payload lock is taken.

### `api/api/WfmcmVasaraApi.cpp` (and top-level `WfmcmVasaraApi.cpp`)

- Removed the two `WRITE_LOCK_HANDLE(requestContext)` lines from the hand-rolled `catch` arms in `calcValueForInstrument`.
- Added `storeResultErrorString()`, backed by a dedicated `static thread_local std::string` buffer.
- Changed `resultGetError()` to return error text via `storeResultErrorString()` instead of `storeApiString()` (which writes the `api_response` slot shared with calc payloads).

Both copies of `WfmcmVasaraApi.cpp` are updated consistently.

## How tested

Not yet run (the mortgage library headers are not part of the workspace snapshot, so the translation units can't be fully compiled here).

Suggested tests once the full build is available:

- **RC5 regression:** hold the pointer returned by `calcValueForInstrument(ctx, csv, -1)`, call `resultGetError(result)` on the same thread, assert the held payload pointer still reads the original JSON. Fails before this change.
- **RC4:** exercise the exception path of an entry point (`LibException` and `std::exception` arms) under load with concurrent readers on the same context; confirm no deadlock and error text is correct.

## Out of scope

- The remaining `storeApiString` callers (`getRatePathComponentJson`, `resultGetInstrumentId`, `resultGetPricingResultJson`) still share `api_response` and can alias each other. Fixing that class of issue is part of the broader move to handle-based results, not this PR.
- The error-anchor policy redesign (RC1–RC3) is intentionally not addressed here.
