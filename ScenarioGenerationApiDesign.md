# Scenario Generation API Design

**Status:** Proposal — exposes scenario (and horizon) rate-path generation to `wfmcm` C-API callers.
**Companion designs:** `RateGenerationModelSuiteRedesign.md` (`RateGenerationSpec` + `RateGenSuite` + §6B horizon), `MortgageValuationModelSuiteRedesign.md` (downstream consumers), `proposed_design.md` (framework baseline).

---

## 1. Purpose

Today scenario rate generation is reachable only through the CLI (`RunMode::GenerateScenarioRate` →
`calcGenerateScenarioRate`; see [mortval_app](.github/pando_context/pando_context/modules/mortval_app.md)).
This design adds a public `wfmcm` C-API request so external callers (C#, Java via SWIG) can request
scenario and horizon rate paths through the same handle API used by `GenRatePathsForMortgageValuation`
and `CalcValueForMortgage`.

Scenario generation is **not new business logic**: it is `RateGenSuite` executing over a set of
scenario (and/or horizon) contexts. This document covers only the API surface and the
application-layer adapter; the domain mechanics live in `RateGenerationModelSuiteRedesign.md`.

---

## 2. API Surface

### 2.1 New request type

Add `GenScenarioRatePaths` to the `ApiRequest` enum (currently 11 types + `UNKNOWN`).

### 2.2 Setters (`api/WfmcmApi.h`)

| Setter | Purpose |
|---|---|
| `setScenarioSource(request, type, content, len)` | Scenario definitions; `ApiScenarioSource { File, String, None }` |
| `setScenarioGroupMap(request, ...)` | Scenario group → scenario-key mapping |
| `setHorizonSpec(request, type, horizonDate, horizonMonths, subscribedRates)` | Maps to `HorizonGenerationSpec` (`RateGenerationModelSuiteRedesign.md` §6B.3); `type` ∈ `ApiHorizonScenarioType { Prescribed, Filebased, Forward }` |
| `setSimulationMonths(request, simMonths)` | Existing |
| `setOutputPaths(request, types, n)` | Existing `ApiPathOutputType` (`SwapRates`, `PrimaryRates`, `SecondaryRates`, `UstSwapRates`, `DiscountRates`, `Base*` variants) |
| `setPrimaryRateOverride` / `setGreekSpec` | Reused for scenario/override inputs |

### 2.3 Response

`ScenarioRatePathsResponse` — scenario key → `CalcRatePaths` (key / primary / secondary / discount
paths, base and scenario variants, plus the horizon namespace). Add a slicer for chunked delivery,
mirroring `GenRatePathsForMortgageValuationResponseSlicer`.

---

## 3. Application Adapter

Create in `cc/src/app-layer/`:

- `ScenarioGenerationApplication.h/.cpp`
- `ScenarioGenerationRequestAdapter.h/.cpp`
- `ScenarioGenerationOutputFormatter.h/.cpp`

The adapter maps the API request to:

- `WorkflowRequest::requestedSuites_ = {"RateGenSuite"}`
- `RateGenerationSpec` with scenario source (→ `ScenarioDataAccess`) and optional `horizon_` (§6B)
- `WorkflowDataSources` (scenario, market, vol, historical)

No file I/O or projection in the adapter. The formatter reuses
`printGenRatePathsForMortgageValuationResponse` and `outputScenarioMarketData`
([mortval_app](.github/pando_context/pando_context/modules/mortval_app.md)).

---

## 4. Data Access

`ScenarioDataAccess` loads scenario definitions from the `ScenarioDataSource` payload;
`RateDataAccess` loads the subscribed `[base, horizon]` segment (`RateGenerationModelSuiteRedesign.md`
§6B.2 "Subscribed"). Cache keys follow the `RateRequirementsKey` pattern; mark them forward-looking
(the `DataAccessLayer` cache is stubbed today — see
[data_access](.github/pando_context/pando_context/modules/data_access.md)).

---

## 5. Horizon Integration

Horizon scenario generation is the same request carrying a `HorizonGenerationSpec`
(`RateGenerationModelSuiteRedesign.md` §6B):

| Horizon mode | API `type` | Anchor segment `[base, horizon]` |
|---|---|---|
| Prescribed (subscribed/scenario) | `Prescribed` / `Filebased` | User-supplied; model projects forward |
| Realized | `Forward` | Fully model-projected |

---

## 6. Bindings (SWIG / Java)

A new request enum + response must be added to the SWIG typemaps, which already enumerate
`PathOutputType`, `SecondaryRateType`, `PrimaryRateType`, `MonthEndRollStep`, `WaterfallStep`, and
related enum arrays ([swig_bindings](.github/pando_context/pando_context/modules/swig_bindings.md)).

---

## 7. Compatibility & Testing

- Keep `RunMode::GenerateScenarioRate` CLI path; route the API path through the adapter behind
  `--use-legacy-rate-generation` until golden parity.
- Tests: request→spec mapping, response slicer, scenario DAG execution, horizon parity vs `ppbridge`
  (`IpHorizon`/`WfhlHorizon`), and subscribed-splice continuity at the horizon seam.

---

## 8. Component Summary

| Component | Location | Mirrors |
|---|---|---|
| `ScenarioGenerationApplication` | `cc/src/app-layer/` | `RateGenerationApplication` |
| `ScenarioGenerationRequestAdapter` | `cc/src/app-layer/` | `RateGenerationRequestAdapter` |
| `ScenarioGenerationOutputFormatter` | `cc/src/app-layer/` | `RateGenerationOutputFormatter` |
| `ScenarioRatePathsResponse` + slicer | `cc/src/app-common/messages/` | `GenRatePathsForMortgageValuationResponse` |
| `ScenarioDataAccess` | `cc/src/core/data-access/` | `RateDataAccess` |

---

> **Repository-context verification note:** the `ApiRequest` enum member count ("11 types") and the
> `ApiHorizonScenarioType` members are taken from the `app_common` context; confirm the exact enum
> values and the existing `calcGenerateScenarioRate` request shape in source before finalizing the
> new request/response types. The `pricing_model` module referenced by the context index has no
> corresponding `pricing_model.md`, so valuation-side line citations are source-verified, not
> context-verified.
