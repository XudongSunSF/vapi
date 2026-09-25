# Change Documentation — WFMCM API Code-Review Fixes

All **"Original lines"** reference the unmodified source files; **"Fixed lines"** reference the corrected files delivered with this document. Files not listed (`WfmcmApi.h`, `WfmcmApiComponents.cpp`, `WfmcmApiEnums.h/.cpp`, `WfmcmApiEnumsDefs.h`, `ModelConfigData.h`, `RequestContextData.h`, `ResultData.h`) required no changes.

---

## WfmcmApiInternal.h — 3 changes

**C-01 · CHECK_CONTENT null-deref/strlen(nullptr) — Critical**
Original lines 208–217 → fixed lines 208–217. Reordered the macro so the `content == nullptr || *content == 0` guard executes **before** the `-1` length normalization (`strlen(content)`) and before any dereference. Previously `content = nullptr, len = -1` crashed in `strlen`; it now returns `ApiErrorNullContent`. Behavior for valid inputs is unchanged.

**C-02 · CHECK_CONTENT2 null-deref + narrowing — Critical**
Original lines 221–230 → fixed lines 221–230. Same reordering as C-01 for the Handle-returning variant, plus added the missing `(int)` cast on `std::strlen(content)` (original line 226) to silence the `size_t`→`int` narrowing warning, matching CHECK_CONTENT.

**C-03 · Comment typo — Low**
Original line 95: "cldaring" → "clearing".

---

## WfmcmApiInternal.cpp — 5 changes

**C-04 · HandleData::getError() destructive move — High**
Original lines 107–111 → fixed lines 107–114. Changed `return move(error_);` to `return error_;` (with explanatory comment). The move emptied the stored error on every read, so `hasHandleError()` (WfmcmApi.cpp:3684) destroyed the error before `getHandleError()` could report it, and every internal `handleData(h).getError()` recovery path saw an empty error.

**C-05 · Variable typo `hasContex` — Low**
Original lines 270, 276, 280, 288 → fixed lines 273, 279, 283, 291. Renamed to `hasContext` (parameter, recursive call argument, and two uses), consistent with the caller at original line 301.

**C-06 · to_string(ApiModel) missing enumerators — High**
Original lines 675–709 (missing cases before `default:` at 705) → fixed lines 649–650 insert. Added `case ApiModel_BehavioralAdcoTuning: return "BehavioralAdcoTuning";` and `case ApiModel_Profitability: return "Profitability";`. Both enumerators exist in `ApiModel` (WfmcmApiComponents.h:56, 75) but previously threw `std::runtime_error`, which also corrupted the diagnostic built by `convert(ApiModel)`'s default branch (original line 835).

**C-07 · to_string(ApiSessionParameter) missing enumerators — High**
Original lines 735–766 (insert after `DiagLogForBaseScnOnly` at 761) → fixed lines 708–718. Added 11 cases: `RateHistory`, `EconScenario`, `ProfitabilityEquityType`, `ProfitabilityFalloutAdjust`, `ProfitabilityDataType`, `ProfitabilityFullCost`, `ProfitabilityRunWithABRD`, `ProfitabilityAssumptionsFolder`, `ProfitabilityInputCashflowFile`, `ProfitabilityPricingDate`, `ProfitabilityMonthlyCashflow`. Previously any model reporting these in `SupportedSpecParams()` made `specParamsOf<>()` (original line 527) throw at runtime.

**C-08 · streamSpecParams incomplete duplicate switch — High**
Original lines 603–673 (71 lines) → fixed lines 610–622 (13 lines). Replaced the hand-maintained duplicate of `supportedSpecParams`'s model switch — which was missing `Cfpm`, `Capm`, `Gfpm`, and `BehavioralModelMap` and hit `assert(false)`/throw for them — with a delegation to `supportedSpecParams(model, isModelSpec)`. Output for previously working models is identical (`polyvar("parameters") << set` then stream).

---

## WfmcmApi.cpp — 15 changes

**C-09 · resolveModelParameterVersion null version strings — High**
Original: function at 151–243; inserted after line 161 → fixed lines 162–167. Added `ApiErrorNullArgument` guards for `modelVersion` and `modelParamVersion` before they reach `resolver->resolveVersion(...)` (original line 187/191), where a null pointer was undefined behavior.

**C-10 · resolveModelParameterVersion dead misspelled local — Low**
Original line 222 (`auto& innerModleType = get<InterestRateModelType>(modelType);`) removed. Unused variable (also a typo).

**C-11 · resolveGreekSpec null version strings — High**
Original: function at 245–266; inserted after line 253 → fixed lines 259–264. Added the same two null guards; original line 258 executed `string(modelVersion)`, which is UB for a null pointer.

**C-12 · Signed/unsigned loop indices — Low**
Original lines 530 (`setOutputPaths`), 563 (`setRequiredSofrSwapTenors`), 596 (`setRequiredTreasuryTenors`), 635 (`setRequiredPrimaryRates`), 730 (`setRequiredSecondaryRates`), 795 (`setValueMortgageRequestDebugInfo`), 817 (`setValueMortgageFromRatesRequestDebugInfo`). Changed `for (size_t i = 0; i < <intCount>; ++i)` to `for (int i = 0; ...)` in all seven loops (fixed lines 541, 574, 607, 646, 742, 807, 829). No behavior change (counts are validated `> 0` first); removes -Wsign-compare/C4018 warnings.

**C-13 · setPrimaryKeyRatesforBehavioralSpeed rejects documented `-1` length — High**
Original line 653: `CHECK_ARRAY(request, content, contentLen);` → fixed line 664: `CHECK_CONTENT(request, content, contentLen);`. The public header (WfmcmApi.h:302) documents `contentLen == -1` for null-terminated content; CHECK_ARRAY rejected all `len <= 0`. CHECK_CONTENT also yields the correct `ApiErrorNullContent` for null content.

**C-14 · setRequiredSecondaryRates unreachable documented request type — High**
Original line 713 → fixed lines 724–725. `CHECK_HANDLE(request, HandleType::GenSecondaryMortgageRatePathsRequest);` now also accepts `HandleType::GenRatePathsForMortgageValuationRequest`, making the already-implemented switch case at original lines 720–722 reachable and matching the public docs (WfmcmApi.h:326–338).

**C-15 · setIndexRateMapping missing input validation — Critical**
Original: function at 825–856; inserted after line 833 → fixed line 846. Added `CHECK_CONTENT(request, content, contentLen);`. Previously null `content` flowed into `IndexMapperReader::readFromCsv` and the documented `-1` length became `(size_t)-1`.

**C-16 · bindRatesData destructive move under read lock — High**
Original lines 1414–1421 → fixed lines 1426–1437. The bind setter moved `keyRatePaths_`/`ratePaths_` out of the source `RatesData` handle while `fill()` holds only a shared lock on it — a mutation under a read lock that also emptied the handle, so any refill or second bound request silently received no rates. The setter now copies from the existing `ratesData` const reference (which also removes the unused-variable warning).

**C-17 · setMarketPrimaryRates out-of-bounds read — Critical**
Original lines 1581–1585 (deref at 1583) → fixed lines 1597–1607. `find_first_of` returns the end iterator when the content contains neither `'\t'` nor `','`; the code then executed `*it`. Added an explicit end-iterator check that returns `ApiErrorInvalidFormat` ("Primary rates content is neither tab- nor comma-delimited"), and introduced a named `contentEnd` for clarity.

**C-18 · addModelOptionsSpec wrong success/exception handle — Medium**
Original lines 2099 and 2101 → fixed lines 2121, 2123. `RETURN_API_SUCCESS(modelSpec)` → `RETURN_API_SUCCESS(modelOptions)` and `EXCEPTION_API_ERROR(modelSpec)` → `EXCEPTION_API_ERROR(modelOptions)`. The function's error paths target `modelOptions`; success/exception must clear/set the same handle, otherwise stale errors persisted on `modelOptions` and legitimate errors were wiped from `modelSpec`.

**C-19 · createPrimaryRateOverride dead local — Low**
Original line 2429 removed (unused placeholder `std::map<...> userPrimaryRateInputs0`).

**C-20 · execute2 pre-validation dereference + unlocked options read — Critical**
Original lines 3389–3403 → fixed lines 3410–3434. (a) Added `CHECK_HANDLE(request, REQUEST_HANDLES);` before `BIND_HANDLES2`, which previously performed `handleData(request)` (a `reinterpret_cast` of the raw value) on an unvalidated handle. (b) The exec options are now copied into a local `staging::ExecutionOptions` under `READ_LOCK_HANDLE(execOptions)` instead of passing an unsynchronized reference into `executeWithOptions`.

**C-21 · hasResult / hasResultError report Pending for invalid handles — High**
Original lines 3546 and 3559 → fixed lines 3577, 3590. `CHECK_HANDLE2(false, ...)` → `CHECK_HANDLE2(ApiResultStatus_UNKNOWN, ...)` in both functions. `false` is `0` = `ApiResultStatus_Pending`, which made pollers with stale/wrong-type handles wait forever; UNKNOWN matches the functions' existing not-initialized branch.

**C-22 · cancelRequest handle leak + exceptions escaping the C ABI — High**
Original lines 3567–3599 → fixed lines 3598–3643. (a) The internally created cancel handle is now owned by `app::ScopedHandle` (with a null-creation guard) and is released on **every** return path; previously it leaked in the global container on all paths. (b) Wrapped the body in `TRY … EXCEPTION_API_ERROR(request)`; previously `getResult`/`from_string`/string construction could throw C++ exceptions through the `extern "C"` boundary.

**C-23 · isValidDate excludes the stated minimum date — Medium**
Original lines 3671–3674 → fixed lines 3712–3717. `return MinDateInt < date;` → `return !(date < MinDateInt);`. Error messages state dates "must start from 1900-01-01" but the strict comparison rejected exactly `19000101`.

---

## WfmcmVasaraApi.cpp — 5 changes

**C-24 · resultIsError mis-encodes ApiErrorLibSetup as "is error" — High**
Original lines 1116–1126 (CHECK_INIT at 1120) → fixed lines 1120–1125. Replaced `CHECK_INIT` (whose failure path returns the nonzero `ApiErrorLibSetup` enum through the `int` channel, which callers read as "the wrapped result is an error") with an explicit `if (!handles)` branch returning `0` with the per-thread error indicator set.

**C-25 · resultGetCleanPrice returns (double)ApiErrorLibSetup — High**
Original lines 1147–1166 (CHECK_INIT at 1151) → fixed lines 1155–1157. Same replacement returning `0.0`; the enum value cast to double (≈1.0) was a plausible price.

**C-26 · resultGetHolding — High**
Original lines 1168–1180 (CHECK_INIT at 1172) → fixed lines 1178–1180. Same replacement returning `0.0`.

**C-27 · resultGetOas — High**
Original lines 1182–1194 (CHECK_INIT at 1186) → fixed lines 1194–1196. Same replacement returning `0.0`.

**C-28 · resultGetSettleDate — High**
Original lines 1216–1231 (CHECK_INIT at 1220) → fixed lines 1230–1234. Same replacement returning `0` (the documented "unset" value), instead of the ApiErrorLibSetup enum value in the YYYYMMDD int channel.

---

## WfmcmApiComponents.h — 3 changes

**C-29 · `_declspec` → `__declspec` — Low**
Original lines 6 and 8 → fixed lines 6, 8. Standard MSVC spelling; the single-underscore alias is a legacy extension not accepted by all compilers/modes.

**C-30 · ApiModel_BehavioralGfpm doc comment — Low**
Original line 54: description said "Conventional Fixed Pool Level Prepayment Model (CFPM)" (copied from the line above); corrected to "Government Fixed Pool Level Prepayment Model (GFPM)".

**C-31 · ApiInterpolationMethod doc comments — Low**
Original lines 96–100: four members carried curve-input-format descriptions copy-pasted from `ApiCurveInputFormat`; replaced with correct interpolation descriptions (Linear, Log-linear, Cubic, Monotone-convex; Constant clarified as piecewise-constant).

---

## WfmcmVasaraApi.h — 1 change

**C-32 · Missing newline at end of file — Low**
Original line 162 (`#endif` with no trailing newline) → trailing CRLF appended.

---

## Items intentionally NOT changed (flagged in the review report)

1. `setPrimaryRateRefDate` (WfmcmApi.cpp:916) and `setSecondaryRateRefDate` (WfmcmApi.cpp:930) call the **AsOfDate** setters (`setPrimRateAsOfDate`/`setSecRateAsOfDate`), aliasing two documented-as-distinct APIs — suspected copy-paste bug, but the external `DateSpec` class may not expose separate ref-date setters and the Vasara layer's comments suggest the aliasing may be relied upon. Requires owner confirmation before changing (report §3.14).
2. `HandleType::Unknown` / `HandleBinding::Unknown` usage assumes the external enum macro framework appends an `Unknown` member (report §1.3).
3. The `>= 1976/01/01` date documentation vs. the 1900-01-01 implementation floor (report §3.10 / §5.3).
4. Unused `validateMarketDateSpec` (WfmcmApi.cpp:1592); thread-safety observations on `getResultWithTimeoutImpl`, `bindDateSpec`, and `HandleData::fill()`; recommended `static_assert` tripwires for `HandleTypeSelection` and cross-enum value alignment (report §2.8, §3.14, §6).
