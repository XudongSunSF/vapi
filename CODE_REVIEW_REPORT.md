# Code Review Report — WFMCM C API / Vasara Integration

**Scope:** All 14 files in the project (≈9,155 lines): `WfmcmApi.h/.cpp`, `WfmcmApiInternal.h/.cpp`, `WfmcmApiComponents.h/.cpp`, `WfmcmApiEnums.h/.cpp`, `WfmcmApiEnumsDefs.h`, `WfmcmVasaraApi.h/.cpp`, `ModelConfigData.h`, `RequestContextData.h`, `ResultData.h`.

**Method:** Static review (the code depends on external headers — QuantLib, `mortgage/utility`, `src/core`, `src/io`, message types — that are not part of the project, so the code could not be compiled here). All line numbers below refer to the **original** files. Fixed copies of the five modified files accompany this report; the exact edits are itemized in `CHANGES.md`.

**Overall assessment:** The codebase is a mature, macro-driven C API over a handle container, and the general architecture (opaque handles, per-handle rw-locks, bind/fill setters, thread-local return buffers) is sound and consistently applied. The recently added Vasara layer is noticeably more defensive and better documented than the legacy surface. The defects found fall into four clusters: (1) undefined behavior on hostile or edge-case inputs (null pointers, missing delimiters, `-1` lengths), (2) error-reporting channels that destroy or mis-encode the error they are supposed to report, (3) enum-to-string tables that lag behind the enums they serve and therefore throw for supported values, and (4) resource/contract slips (handle leak, destructive move under a shared lock, exceptions escaping the C ABI).

Severity legend: **Critical** = crash/UB or silent wrong results; **High** = incorrect behavior visible to API consumers; **Medium** = incorrect in edge cases, contract violations; **Low** = warnings, dead code, docs.

---

## 1. WfmcmApiInternal.h

### 1.1 [Critical — FIXED] `CHECK_CONTENT` / `CHECK_CONTENT2` dereference and `strlen` a possibly-null pointer before the null check (lines 208–217, 221–230)
Both macros execute `len = strlen(content)` (when `len == -1`) and later evaluate `*content == 0` — but the `content == nullptr` guard came **after** those uses. Any caller passing `content = nullptr` with `contentLen = -1` (a combination the public API documentation explicitly permits: *"May set to -1 if the content is null-terminated"*) crashed inside `strlen(nullptr)` instead of receiving `ApiErrorNullContent`. Because these macros guard nearly every content-accepting entry point (`setHistoricalData`, `setMarketRawCurveInput`, `createInstrumentPortfolio`, `createUserScenarios`, `setModelParameter`, `setSessionParameter`, and more), this was the single most widely exposed defect in the codebase. `CHECK_CONTENT2` additionally assigned `strlen`'s `size_t` result to an `int` without a cast (narrowing-conversion warning; `CHECK_CONTENT` already had the cast). **Fix:** the null/empty check is now performed first in both macros, and the missing `(int)` cast was added.

### 1.2 [Low — FIXED] Documentation typo (line 95)
"cldaring" → "clearing" in the `RETURN_API_SUCCESS` doc comment.

### 1.3 [Observation — no change] `HandleType::Unknown` / `HandleBinding::Unknown`
`HandleData::type_` is initialized with `HandleType::Unknown` (line 411) and `addModelOptionsSpec`/`getModelOptionsBinding` use `HandleBinding::Unknown`, but neither enumerator appears in `WfmcmApiEnumsDefs.h`. This only compiles if the `BEGIN_ENUM_CLASS`/`END_ENUM_CLASS` macro framework appends an `Unknown` (or similar sentinel) member automatically. Since the framework header is external and the code presumably builds today, this is flagged for confirmation rather than changed.

### 1.4 [Low — no change] Header hygiene
`using namespace app;` at global scope in a widely included header (line 38) pollutes every consumer's namespace; the `CHECK_*` macros are not `do { } while(false)`-wrapped (the code's own `{FIXME}` comments acknowledge this — an `if`-without-braces caller would misbehave); `MinDateInt` is a `Date`, not an int, despite its name. All pre-existing, all noted in the code's own FIXMEs or purely cosmetic; left untouched to avoid churn on a shared header's macro surface.

---

## 2. WfmcmApiInternal.cpp

### 2.1 [High — FIXED] `HandleData::getError()` destructively moves the stored error out (lines 107–111)
`return move(error_);` empties the handle's stored error as a side effect of *reading* it. Concretely: `hasHandleError(h)` (WfmcmApi.cpp line 3684) calls `getError().isError()` to answer a yes/no question — and **wipes the error while doing so**, so the natural sequence `if (hasHandleError(h)) msg = getHandleError(h);` returned an empty/success error object. It also broke the documented contract of `getHandleError()` ("only resets when the next API call is made using this handle") and silently degraded the internal error-recovery paths in the Vasara layer (`handleData(spec).getError()` in `createBehavioralModelMapSpecFromCsv`, `handleErrFromHandle`, the `execute` failure paths), which read the error and fall back to a generic message when it appears empty. **Fix:** return a copy.

### 2.2 [High — FIXED] `to_string(ApiModel)` missing enumerators (lines 675–709)
`ApiModel_BehavioralAdcoTuning` and `ApiModel_Profitability` are live members of `ApiModel` (WfmcmApiComponents.h lines 56, 75; profitability requests are a first-class request type), yet `to_string` threw `std::runtime_error("Invalid model type")` for them. Worse, the `default:` branch of `convert(ApiModel)` (line 835) builds its diagnostic via `to_string(modelType)` — so for these values the intended "cannot convert" message was replaced by an unrelated exception thrown while formatting the message. **Fix:** both cases added.

### 2.3 [High — FIXED] `to_string(ApiSessionParameter)` missing eleven enumerators (lines 735–766)
`RateHistory`, `EconScenario`, and all nine `Profitability*` session parameters existed in the enum but not in the switch. `specParamsOf<Model>()` (line 527) iterates every parameter a model reports as supported and stringifies it — so registering or querying any model whose `SupportedSpecParams()` includes one of these values threw at runtime from `setSessionParameter`, `setModelParameter`, `getSupportedSessionParameters`, etc. **Fix:** all missing cases added.

### 2.4 [High — FIXED] `streamSpecParams` duplicated `supportedSpecParams`'s switch, incompletely (lines 603–673)
The duplicate switch was missing `Cfpm`, `Capm`, `Gfpm`, and `BehavioralModelMap` — all supported by the authoritative `supportedSpecParams` a page above — and hit `assert(false)` + throw for them. Two hand-maintained copies of the same dispatch is exactly how this drift happened. **Fix:** the function now delegates to `supportedSpecParams(model, isModelSpec)`, eliminating the second switch entirely.

### 2.5 [Low — FIXED] Parameter/variable typo `hasContex` (lines 270, 276, 280, 288)
Renamed to `hasContext` (all four occurrences, keeping it consistent with the caller at line 301).

### 2.6 [Low — no change] `HandleContainer::deleteHandleData` "invalidation" is a no-op (line 339)
`h.internal_ = 0;` mutates a by-value parameter; the caller's handle is untouched. Harmless but misleading — validity is actually established by `handles_` membership via `isValidHandle`. Left as-is (removing it changes nothing functionally) but worth cleaning up.

### 2.7 [Observation — no change] `handleData()` trusts the handle bit-pattern (line 230)
`reinterpret_cast<HandleData*>(h.internal_)` performs no container membership check; every call site is responsible for validating first (see finding 3.10 for one that didn't). A debug-build assertion `assert(handles && handles->handleExists(h))` here would have caught 3.10 immediately.

### 2.8 [Observation — no change] `HandleData::fill()` locking
`fill()` holds this handle's `objectMutex_` (write) and `m_` while recursively calling `fill()` on bound handles, which take their own write locks. Two handles bound to each other (or a diamond with inconsistent order) could deadlock. The binding graph the API constructs is a DAG in practice (requests bind downward to data handles), so this is a design constraint to document rather than a live bug.

---

## 3. WfmcmApi.cpp

### 3.1 [Critical — FIXED] `setMarketPrimaryRates` reads one past the end of the buffer (lines 1568–1590, deref at 1583)
`find_first_of(content, content + contentLen, delimiters, delimiters + 2)` returns the end iterator when the content contains neither `'\t'` nor `','`, and the very next line dereferences it: `(*it == '\t')`. Any single-column or malformed primary-rates payload triggered an out-of-bounds read. **Fix:** the end case now returns `ApiErrorInvalidFormat` with a descriptive message.

### 3.2 [Critical — FIXED] `setIndexRateMapping` performed no content validation at all (lines 825–856)
Unlike every sibling setter, there was no `CHECK_CONTENT`: `content = nullptr` flowed straight into `IndexMapperReader::readFromCsv({ nullptr, ... })`, and the documented `contentLen == -1` became `(size_t)-1` ≈ 1.8×10¹⁹ — a wild-length string view over the process address space. **Fix:** `CHECK_CONTENT(request, content, contentLen)` added, which both rejects null content and normalizes `-1` to `strlen`.

### 3.3 [Critical — FIXED] `execute2` dereferences the request handle before validating it (lines 3389–3403)
`BIND_HANDLES2(request, …)` expands to `handleData(request).bindHandle(…)` — a raw `reinterpret_cast` of whatever value the caller passed — and ran **before** any handle check (`executeWithOptions` validates, but only after the bind). An invalid or stale request handle was undefined behavior instead of `ApiErrorInvalidHandle`. Additionally, `handleObject<ExecOptions>(execOptions)` was read and passed by reference without holding the options handle's lock while another thread could be mutating it via `setExecutionMode` etc. **Fix:** `CHECK_HANDLE(request, REQUEST_HANDLES)` added up front, and the options are now copied under a read lock before being handed to `executeWithOptions`.

### 3.4 [High — FIXED] `cancelRequest` leaks the internal cancel handle and lets exceptions escape the C ABI (lines 3567–3599)
The internally created cancel-request handle was never passed to `deleteHandle` on **any** path — every cancellation permanently leaked a `RequestHandleData` (including its request/response state) in the global container. The function was also the only non-trivial API entry point without a `TRY`/`EXCEPTION` guard: `getResult`, `from_string`, or the `std::string` construction could throw a C++ exception straight through the `extern "C"` boundary into Java/C#/Python callers. **Fix:** the cancel handle is now owned by `app::ScopedHandle` (released on every return path, including exceptional ones), a null-creation guard was added, and the body is wrapped in `TRY … EXCEPTION_API_ERROR(request)`.

### 3.5 [High — FIXED] `bindRatesData` setter moves data out of the source handle under a read lock (lines 1414–1421)
The bind lambda executed `opt.keyRatePaths_ = std::move(handleObject<RatesData>(from).keyRatePaths_)` (and same for `ratePaths_`). Setters run inside `HandleData::fill()`, which holds only a **shared** lock on the source handle — so this mutated shared state under a read lock (a data race if the rates handle is bound to two requests filled concurrently), and it left the `RatesData` handle empty, so a second `execute` on the same request, or a second request bound to the same rates handle, silently priced with **no rate paths**. The unused local `ratesData` was also dead. **Fix:** the setter now copies; the local is used for both reads.

### 3.6 [High — FIXED] `setRequiredSecondaryRates` type check contradicts its own switch and the public docs (lines 705–737)
`CHECK_HANDLE(request, HandleType::GenSecondaryMortgageRatePathsRequest)` rejected `GenRatePathsForMortgageValuationRequest`, yet the switch two lines later has a fully implemented (and therefore unreachable) case for it, and the public header (WfmcmApi.h lines 326–338) documents both request types as allowed. **Fix:** the check now accepts both types, making the existing case reachable.

### 3.7 [High — FIXED] `setPrimaryKeyRatesforBehavioralSpeed` rejects the documented `contentLen == -1` (line 653)
`CHECK_ARRAY` treats any `len <= 0` as an error, but the public header (WfmcmApi.h line 302) documents `-1` as valid for null-terminated content. It also produced the wrong error text ("Size must be > 0") for null content. **Fix:** switched to `CHECK_CONTENT`, matching the documented contract and every sibling content setter.

### 3.8 [High — FIXED] `hasResult` / `hasResultError` report *Pending* for invalid handles (lines 3546, 3559)
`CHECK_HANDLE2(false, …)` returns `false`, i.e. `0` — which in these functions' return domain is `ApiResultStatus_Pending`. A caller polling with a stale or wrong-type handle span forever "waiting" for a result that will never come. The functions' own not-initialized branch already returns `ApiResultStatus_UNKNOWN`, confirming intent. **Fix:** the check now returns `ApiResultStatus_UNKNOWN`.

### 3.9 [High — FIXED] `resolveModelParameterVersion` / `resolveGreekSpec` crash on null version strings (lines 151, 245)
`resolveGreekSpec` constructs `string(modelVersion)` and `string(modelParamVersion)` directly — `std::string(nullptr)` is undefined behavior (typically a crash) — and `resolveModelParameterVersion` forwards the raw pointers into `resolver->resolveVersion(...)`. Neither function validated them, while `histDataDir` (the third pointer) *was* handled. **Fix:** explicit null checks returning `ApiErrorNullArgument` were added to both, mirroring `setSuiteRegistryConfigFile`'s existing pattern.

### 3.10 [Medium — FIXED] `isValidDate` rejects the stated minimum date (line 3671–3674)
`MinDateInt < date` is strict, so `1900-01-01` itself — which every `CHECK_DATE` error message describes as the first valid date ("date must start from 1900-01-01") — was rejected. **Fix:** made inclusive (`!(date < MinDateInt)`). Related documentation inconsistency (not changed): `WfmcmApiComponents.h` line 499 and many API doc comments still say valid dates are `>= 1976/01/01`, while the implementation enforces 1900-01-01; the two sources of truth should be reconciled.

### 3.11 [Medium — FIXED] `addModelOptionsSpec` reports success/exception against the wrong handle (lines 2099, 2101)
The function's parameter-order primary handle is `modelOptions` (all its explicit errors target it), but `RETURN_API_SUCCESS(modelSpec)` cleared the error state of the *spec* handle instead — so a prior error on `modelOptions` survived a subsequent successful call, and a prior error on `modelSpec` was wrongly cleared — and `EXCEPTION_API_ERROR(modelSpec)` recorded exceptions on the spec. **Fix:** both now target `modelOptions`.

### 3.12 [Low — FIXED] Signed/unsigned loop mismatches (lines 530, 563, 596, 635, 730, 795, 817)
Seven loops compared `size_t i` against `int` counts (`-Wsign-compare`; on MSVC C4018). Counts are validated `> 0` beforehand so no functional bug, but the warnings mask real ones. **Fix:** loop indices changed to `int`, matching the count type and the `int i` loops elsewhere in the file.

### 3.13 [Low — FIXED] Dead/misspelled locals (lines 222, 2429)
`resolveModelParameterVersion` declared unused `auto& innerModleType` (also a typo) in the interest-rate branch; `createPrimaryRateOverride` declared an unused placeholder map. Both removed.

### 3.14 [Observations — no change]
* **Suspected copy-paste bug, needs owner confirmation:** `setPrimaryRateRefDate` (line 916) calls `setPrimRateAsOfDate(...)` — the *same* underlying setter as `setPrimaryRateAsOfDate` — and `setSecondaryRateRefDate` (line 930) likewise calls `setSecRateAsOfDate(...)`. The public docs describe RefDate and AsOfDate as distinct concepts, so calling one API silently overwrites the other's value. However, the Vasara comment at `WfmcmVasaraApi.cpp:398–401` suggests the as-of field is deliberately used as the historical reference date, and the `DateSpec` class (external) may not expose separate ref-date setters — so this was **flagged, not changed**. If `DateSpec` has `setPrimRateRefDate`/`setSecRateRefDate`, these two functions should call them.
* `validateMarketDateSpec` (line 1592) is never called (unused-function warning) and accesses two handles without locks; either wire it into `bindMarketData` (a commented "check to see if the start market was constructed with the same date spec" hints it was meant to be) or delete it.
* `setValueMortgageRequestRecalibratedBasis` / `setValueMortgageRequestUseBaseOAS` (lines 771–781) are live definitions for declarations that are `#if 0`'d in the header — dead exported-looking code with unused parameters.
* `bindDateSpec` (line 973) and `createSofrRatePaths2` read the array handle via `handleAtIndex` *before* taking the read lock; a concurrent `insertHandleAt` is a narrow race.
* `getResultWithTimeoutImpl` (line 3437) caches `auto& slicer` before `UPGRADE_TO_WRITE_LOCK_HANDLE` briefly drops the lock; a concurrent re-`execute` replaces `slicer_` and dangles the reference. The upgrade-lock pattern is inherently non-atomic (the macro's own comments say so); documenting "one reader thread until waitForResult returns" mitigates it, and the header already recommends this.
* `deleteHandle` on a request whose `std::async` future is still running will block in the future's destructor until the worker finishes — worth documenting for callers that cancel + delete.

---

## 4. WfmcmVasaraApi.cpp

This file is the newest and cleanest in the project; its comments capture several past bugs (self-aliasing string assignment, future-consumption race, use-after-free after `mapBehavioralModel` failure) and the code correctly avoids them. Two defect classes remained:

### 4.1 [High — FIXED] Numeric result getters mis-encode "library not initialized" through their value channel (lines 1116–1231)
`resultIsError`, `resultGetCleanPrice`, `resultGetHolding`, `resultGetOas`, and `resultGetSettleDate` all used `CHECK_INIT`, whose failure path `return`s the `ApiErrorLibSetup` enum value. Through an `int` return that value (a small nonzero integer) is indistinguishable from a real answer: `resultIsError` returned nonzero — i.e. "this result **is** an error" — merely because the library wasn't initialized, and `resultGetCleanPrice` returned `(double)ApiErrorLibSetup` (≈1.0), a perfectly plausible price. The string getters in the same file already avoided `CHECK_INIT` for exactly this reason. **Fix:** each now returns its documented neutral value (`0` / `0.0`) with the per-thread error indicator set, matching the string getters' pattern.

### 4.2 [Observations — no change]
* `calcBehavioralSpeedForInstrument`/`...Obj` hold a read lock on the request context across the whole build+execute; the pricing twins deliberately snapshot instead (per the `PricingRequestInputs` comment). Since the context is immutable after construction, this is a consistency nit, not a bug — but converging on the snapshot pattern would remove a long-held lock.
* `buildBehavioralRequestFromContext` validates `historicalData`/`dateSpec`/`modelOptions` and then re-reads the handles later without revalidation; if the Java side violated its LIFO teardown contract mid-call, the handles could die between check and use. The documented teardown ordering makes this acceptable; the comment block in `RequestContextData.h` already calls out the dependency.
* `resultIsError` returns from inside `TRY` without `RETURN_SUCCESS`, so it doesn't clear a stale handle error on the success path — inconsistent with the other getters but harmless.

---

## 5. WfmcmApiComponents.h

### 5.1 [Low — FIXED] `_declspec` → `__declspec` (lines 6, 8)
`_declspec` is a non-standard legacy alias that MSVC accepts but clang-cl and conforming modes may not; the documented spelling is `__declspec`.

### 5.2 [Low — FIXED] Wrong copy-pasted doc comments
`ApiModel_BehavioralGfpm` (line 54) was documented as "CFPM"; the four non-constant `ApiInterpolationMethod` members (lines 97–100) carried curve-input-format descriptions copied from the enum above. Both corrected.

### 5.3 [Observations — no change]
* `Handle`/`Date` are C-visible structs passed by value across the ABI — correct — but the `>= 1976/01/01` validity claim (line 499) conflicts with the implementation's 1900-01-01 floor (see 3.10).
* `ApiDebugInfoType`, `ApiPathOutputType` and the `wfmcm` enums they cast to must stay value-aligned; the casts in `WfmcmApi.cpp` (`static_cast<PathOutputType>` etc.) have no static assertion tying the two enum families together. A `static_assert((int)ApiPathOutputType_UNKNOWN == (int)PathOutputType::UNKNOWN)`-style tripwire per pair is recommended.

## 6. WfmcmVasaraApi.h, WfmcmApiEnums.h/.cpp, WfmcmApiEnumsDefs.h, WfmcmApiComponents.cpp, ModelConfigData.h, RequestContextData.h, ResultData.h

* **WfmcmVasaraApi.h [Low — FIXED]:** missing newline at end of file (line 162) — a warning under `-Wnewline-eof` and undefined behavior pre-C++11.
* **WfmcmApiEnums.h [Low — noted]:** include guard `BRIDGE_SWIG_ENUMS_H_` doesn't match the file name (`WFMCM_API_ENUMS_H` expected); risk of a future guard collision with another "bridge" header.
* **WfmcmApiEnumsDefs.h [noted]:** `HandleType` is explicitly marked "DO NOT REORDER" because its values index `HandleTypeSelection` (`WfmcmApiInternal.h:572`); the tuple currently has exactly 41 entries matching the 41 enumerators, verified by inspection. A `static_assert(std::tuple_size_v<HandleTypeSelection> == expected_count)` would make this drift-proof. Also note `FloatingPointMatrix`/`RatePathCollection` and `ModelSpec`/`SessionSpec` intentionally share underlying types — several call sites rely on this (`ratePathCollectionAdd` reads a `RatePathCollection` via the `FloatingPointMatrix` accessor; `bindModelSession` reads a `SessionSpec` via the `ModelSpec` accessor). This works today but is fragile; a comment at each site or a shared alias type would prevent an innocent-looking type change from turning into a `bad_any_cast`.
* **ModelConfigData.h / RequestContextData.h / ResultData.h:** No defects found. These are well-documented value types; the ownership commentary (RAII spec-handle owner, non-owning request handles, LIFO teardown contract) is accurate against the implementation reviewed, and the `std::variant` payload design in `RequestContextData` correctly makes the two storage modes mutually exclusive. The `NOTE` requiring copy-constructibility for `std::any` storage is respected.
* **WfmcmApiComponents.cpp:** Trivial enum-string instantiation TU; no issues.

---

## 7. Summary of disposition

**22 defects fixed** across 5 files (see `CHANGES.md` for line-by-line detail): 4 critical (null-deref/UB in the content macros, OOB read in `setMarketPrimaryRates`, missing validation in `setIndexRateMapping`, pre-validation dereference in `execute2`), 10 high (destructive `getError`, three incomplete enum-string tables, rates-data destructive move, handle leak + ABI-escaping exceptions in `cancelRequest`, contradictory type check in `setRequiredSecondaryRates`, `-1`-length rejection, Pending-for-invalid-handle status, null version strings, Vasara getter mis-encoding), and 8 medium/low (wrong success handle, inclusive date floor, sign-compare loops, dead locals, `__declspec`, doc comments, typos, missing EOF newline).

**Flagged for owner follow-up (not changed):** the RefDate/AsOfDate aliasing in `setPrimaryRateRefDate`/`setSecondaryRateRefDate` (3.14, highest priority of the flags), the `HandleType::Unknown` macro-framework assumption (1.3), the 1976-vs-1900 date-floor documentation conflict (3.10/5.3), the unused `validateMarketDateSpec`, and the recommended `static_assert` tripwires for the enum/tuple and cross-enum value alignments.
