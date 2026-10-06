# How Rates Are Shocked in the Pando Library

A tour of the mechanisms this codebase uses to move a rate curve and observe the effect — from
deterministic "what-if" scenario shifts to finite-difference Greek bumps.

---

## 1. Two families of rate shocks

Everything in the library is one of two things:

| Family | Question it answers | Example |
|---|---|---|
| **Scenario / curve-modification shock** | "What is the value under a shifted market?" | `ScenarioDef` govt/libor shift, `ShockSofrSwapCurve`, CLI shift/twist/butterfly |
| **Finite-difference Greek** | "How sensitive is the value to a bump?" | `DV01`, `KeyRateDV01`, `CV01` via `GreekAlgo::generateScenarios` |

Both ultimately do the same low-level thing — **perturb the rate curve and reprice** — they just
differ in whether the perturbation is a named scenario or a ±ε sensitivity bump.

---

## 2. Representing a rate shock

The core shift type is `ScenarioShift` ([scenario_infrastructure](../../../.github/pando_context/pando_context/modules/scenario_infrastructure.md)):

- `TermRateTS` — a POD of `(term months, rate bps)`.
- `ScenarioShift` — an **interpolation method + term→rate map**; i.e., you specify the shift at a
  few tenors and an interpolation rule defines it everywhere else.

On `ScenarioDef` the shift is split into two curves:

- `getGovtShift()` — the **government/Treasury** curve shift,
- `getLiborShift()` — the **swap/IBOR** curve shift,

each addressable by term via `getGovtShiftByTerm(...)` / `getLiborShiftByTerm(...)`.

So a rate shock is a **per-curve, per-tenor, interpolation-defined parallel or non-parallel
shift**, measured in basis points.

---

## 3. Yield-level vs bootstrap-level shocks

This is the key design distinction:

| Approach | What is perturbed | Then | Used by |
|---|---|---|---|
| **Yield-level** | the *built curve's* rates | re-interpolate | `ScenarioShift`, `FittedBondDiscountCurveFactory::createForScenario` |
| **Bootstrap-level** | the *instrument inputs* | re-bootstrap | `LiborSwapCurveInput*Shock`, `ShockSofrSwapCurve` |

- **Bootstrap-level (LIBOR family):** `YieldTermStructureBuilder` ships shock operators that
  mutate a `LiborSwapCurveInput` before bootstrapping
  ([wfmcm_public_api](../../../.github/pando_context/pando_context/modules/wfmcm_public_api.md)):
  - `LiborSwapCurveInputParallelShock::apply(input, shock)` — shift all instruments by `shock` bps,
  - `LiborSwapCurveInputKeyShock::apply(input, index, shock)` — bump one key point,
  - `LiborSwapCurveInputNonparallelShock::apply(input, shocks[])` — a vector of per-point bumps,
  - `LiborSwapCurveInputDatedKeyShock` — a key-rate bump by **absolute date** (for time-shifted
    scenarios).

- **Yield-level (Treasury family):** `FittedBondDiscountCurveFactory::createForScenario(date,
  terms, yields)` builds a scenario curve directly from a term-yield vector — the Treasury analog
  of a `ScenarioShift`.

The choice matters numerically: shocking instruments and re-bootstrapping moves *every* tenor
slightly (including off-grid ones), whereas shocking a built curve only moves what the shift says.

---

## 4. The parallel-shock task (`ShockSofrSwapCurve`)

For SOFR, the workhorse is a task struct ([yield_curve_construction](../../../.github/pando_context/pando_context/modules/yield_curve_construction.md)):

```
ShockSofrSwapCurve :: execute
  inputs:  AsOf, Shocks[], SofrCurveInput, DailySofrRate
  outputs: BaseSofrCurve, ShockedSofrCurves[]
```

The BAU app drives it with a ladder of parallel bumps — `−200, −100, −50, −25, −12, 0, +12, +25,
+50, +100, +200` bps — and writes `ShockedSofrZeroCurve.csv`
([bau](../../../.github/pando_context/pando_context/modules/bau.md)). This is the canonical
"re-price under a ladder of parallel rate shocks" pattern.

---

## 5. Key-rate / non-parallel / dated shocks

Beyond parallel:

- **Key-rate shock** — one tenor bucket bumped, others pinned (`KeyRateDV01Algo` supports a
  **tent** (triangle) shift and a **wave** shift via `shiftMethod_`).
- **Non-parallel shock** — an arbitrary `shocks[]` vector per point
  (`LiborSwapCurveInputNonparallelShock`).
- **Dated key shock** — a bump anchored to a calendar date rather than a relative tenor
  (`LiborSwapCurveInputDatedKeyShock`), used for month-end-roll / horizon-style shifts.

`GreekAlgoOptions` carries the knobs: `shock_` (value + direction), `scale_`, `shockedTenors_`,
and `shiftMethod_` ([app_common](../../../.github/pando_context/pando_context/modules/app_common.md)).

---

## 6. CLI curve modification (shift / twist / butterfly)

At the application layer, `Util::fillCurveModificationSpec(modelOpts, runConfig)` populates
`messages::ModelOptions` with a **curve-modification spec** — a shift / twist / butterfly applied
to the curve before pricing, separate from the scenario machinery
([mortval_app](../../../.github/pando_context/pando_context/modules/mortval_app.md)). It pairs with
`fillCurveInterpolationSpec` (interpolation method) and the date-shift setting below.

---

## 7. Date-shift handling

When a curve is *date-shifted* (e.g. forward/horizon scenarios), you must decide which rate stays
fixed. `CurveDateShiftOption` encodes the four choices
([wfmcm_public_api](../../../.github/pando_context/pando_context/modules/wfmcm_public_api.md)):

`KeepZeroRateSame` · `KeepSwapRateSame` · `KeepForwardRateSame` · `KeepDiscountFactorSame`

The choice changes what a "no shock" forward scenario means, so it is set alongside the shock spec.
`HorizonScenarioUtils::applyHorizonShift(context, horizonDate)` is the runtime hook that applies
this shift ([app_common](../../../.github/pando_context/pando_context/modules/app_common.md)).

---

## 8. Scenario shocks (`ScenarioDef` → `ScenarioManager`)

A full scenario bundles the rate shift with everything else that moves:

- rate-curve shifts (`getGovtShift` / `getLiborShift`),
- vol-cube shocks (`getVolCubeShifts`),
- behavioral speed multipliers (`getPrepayMult` / `getDefaultMult` / `getLossSevMult`),
- analytic flags (`getCalcOAD`, `getCalcOAS`, …).

`ScenarioManager::createBaseContexts(...)` builds the **base (unshocked)** context;
`ScenarioManager::createGreekScenarioContexts(...)` builds the **shocked** contexts
([app_common](../../../.github/pando_context/pando_context/modules/app_common.md)). Each scenario
context is a fully re-constructed market environment — the shock is applied at curve construction,
then the whole pipeline (behavioral → cashflow → discounting) runs against it.

---

## 9. Greeks: bump-and-reprice

Rate Greeks are computed by finite differences over prices
([app_common](../../../.github/pando_context/pando_context/modules/app_common.md)):

- `Shock(value, direction)` — direction ∈ {`Up`, `Down`, `Both`}; `makeShocks(shock)` expands to
  `{upShock, downShock}`.
- `GreekAlgo::generateScenarios(base)` — returns `{ScenarioKey, context}` pairs for **base, up,
  down**.
- `GreekAlgo::aggregate(basePrice, scenarioPrices)` — turns those prices into the Greek.

Standard formulas (central difference, per 1bp bump ε):

$$
DV01 = \frac{P(-\varepsilon) - P(+\varepsilon)}{2\varepsilon}
\qquad
CV01 = P(+\varepsilon) + P(-\varepsilon) - 2P_0
$$

$$
\text{KeyRateDV01}_i = \frac{P_i(-\varepsilon) - P_i(+\varepsilon)}{2\varepsilon}
$$

Concrete algorithms: `DV01Algo`, `CV01Algo`, `SofrDV01Algo` (SOFR curve), `UstDV01Algo` (Treasury
curve), `KeyRateDV01Algo` (+ tent/wave variants), `KeyRateCV01Algo`. `GreekPriceLocator` finds the
up/down shocked prices in a scenario price map. The `CalcGreeksFromPrices` request is the
prices-in → Greeks-out path (`calcGreek` in `mortval`).

---

## 10. Sibling shocks (vol, HPI/unemployment)

Rates aren't the only thing that gets bumped; the same pattern extends to:

- **Vol shocks** — `VolCubeShift` / `ScenarioDef::getVolCubeShifts`; `PartialVegaAlgo`,
  `ParallelVegaAlgo`, `PartialVolgaAlgo`, `ParallelVolgaAlgo` (see
  `VolSurfaceUsageTutorial.md`).
- **HPI/unemployment shocks** — `StateLookupDataShock::shockHpi` (±1%), `shockUnemp` (±1pp),
  `overrideHpi`/`overrideUnempl`, and `GreekScenarioDataBuilder::buildHpiShiftContext` /
  `buildUnempShiftContext` (level shock from a `shockSince` date onward)
  ([greeks](../../../.github/pando_context/pando_context/modules/greeks.md)).

These are the `HPI01` / `Unemployment01` Greeks and the state-shock scenario machinery — same
bump-and-reprice shape, different target.

---

## 11. Treasury side

Treasury rate shocks use the fitted-curve path:

- `FittedBondDiscountCurveFactory::createForScenario(date, terms, yields)` — scenario/shock curve
  from a term-yield vector,
- `UstDV01Algo` — parallel Treasury DV01,
- `TreasuryFutureSensitivityCalculator` — Treasury futures Greeks (DV01, CV01, duration,
  convexity) via bumped prices ([fixed_income_instruments](../../../.github/pando_context/pando_context/modules/fixed_income_instruments.md)).

---

## 12. Component map

| Concern | Component |
|---|---|
| Shift representation | `ScenarioShift`, `TermRateTS` |
| Scenario definition | `ScenarioDef` (govt/libor shift, vol shift, behavioral mults) |
| Scenario context building | `ScenarioManager::createBaseContexts` / `createGreekScenarioContexts` |
| LIBOR instrument shocks | `LiborSwapCurveInput{Parallel,Key,Nonparallel,DatedKey}Shock` |
| SOFR parallel shock task | `ShockSofrSwapCurve` |
| Treasury scenario curve | `FittedBondDiscountCurveFactory::createForScenario` |
| CLI curve modification | `Util::fillCurveModificationSpec` (shift/twist/butterfly) |
| Proxy curve | `SofrProxyCurve` builder, `setProxyCurveDefinition` (tenor grid), `BaseSofrProxyCurve` + shocked variants |
| Date shift | `CurveDateShiftOption`, `HorizonScenarioUtils::applyHorizonShift` |
| Greek bumps | `Shock`, `makeShocks`, `GreekAlgo::{generateScenarios, aggregate}` |
| Rate Greeks | `DV01Algo`, `CV01Algo`, `SofrDV01Algo`, `UstDV01Algo`, `KeyRateDV01Algo` |
| State shocks | `StateLookupDataShock`, `GreekScenarioDataBuilder` |

---

## 13. The proxy curve (and how shocks are applied to it)

A **proxy curve** is a re-parameterized SOFR yield curve defined on a **coarse, user-chosen tenor
grid** instead of the full bootstrap instrument set. It is built with monotone-convex interpolation
and can be **swap-only (no futures)** — the `SofrProxyCurveTest` cases are literally
"monotone-convex interp, no futures" for both the base and proxy builders
([test_valuation](../../../.github/pando_context/pando_context/tests/test_valuation.md)).

### 13.1 Why it exists

Re-bootstrapping the whole curve for every bump is expensive and moves off-grid tenors
unpredictably. The proxy curve fixes a **small set of anchor points** (the "proxy" tenors) and lets
you reshape the curve by moving only those points — the natural target for curve-modification
(shift/twist/butterfly) and for shock ladders.

### 13.2 Where it is configured

- `setProxyCurveDefinition(curveModSpec, tenors, n)` sets the **proxy tenor grid** on the curve
  modification spec ([app_common](../../../.github/pando_context/pando_context/modules/app_common.md)).
- `Util::fillCurveModificationSpec(modelOpts, runConfig)` builds the shift/twist/butterfly spec that
  operates on those points ([mortval_app](../../../.github/pando_context/pando_context/modules/mortval_app.md)).
- The interpolation method is set separately (`setSofrCurveInterpolationMethod` /
  `setUstCurveInterpolationMethod`).

### 13.3 How shocks are applied to the proxy curve

Because the proxy curve is just `{tenor → rate}` at the grid, a shock is a **yield-level
perturbation of the grid, then re-interpolation** (not re-bootstrap):

| Shock | What moves | Proxy-curve result |
|---|---|---|
| Parallel ±bump | every grid point | `ParallelUpSofrYts` / `ParallelDownSofrYts` (DV01) |
| Parallel ±bump (two-sided) | every grid point | `CV01ParallelUpSofrYts` / `CV01ParallelDownSofrYts` (CV01) |
| Key-rate bump | one grid point | `KeyRateUpSofrYts` / `KeyRateDownSofrYts` (per KRD term) |
| Next-day re-anchor | as-of shifted | `NextSofrYts` (carry) |

In stratecaster these live on `MarketDataSingleDay` as cached `YieldTermStructure` objects —
`BaseSofrProxyCurve` plus its shocked variants — and `constructAnalyticsYts(nextAsOf)` builds them
all for swaption analytics ([stratecaster_app](../../../.github/pando_context/pando_context/modules/stratecaster_app.md)).

This is the same **yield-level shock** from §3, applied on a coarse grid: fewer points to bump,
monotone-convex interpolation fills between them, and no re-bootstrap is needed per scenario.

> **Verification note:** the context documents the proxy curve via the API setter, the stratecaster
> fields, and the `SofrProxyCurveTest` cases; the precise `SofrProxyCurve` builder signature is not
> in the generated context, so confirm the grid-shock mechanics against source before relying on it.

---

## 14. Pitfalls

1. **Yield-level vs bootstrap-level mismatch** — a 10bp instrument shock ≠ a 10bp yield shift;
   don't compare DV01s computed the two ways.
2. **Parallel vs key-rate** — a key-rate bump is local; the tent/wave `shiftMethod_` determines
   how neighbors move, which changes the Greek.
3. **One-sided vs central difference** — one-sided DV01 embeds a half-order convexity error; use
   central difference when the platform supports `Both`.
4. **Negative rates** — lognormal shocks blow up near zero; the library's normal (bps) convention
   exists for exactly this reason.
5. **Date-shift leakage** — forgetting `CurveDateShiftOption` means forward scenarios silently
   re-anchor to the wrong rate.
6. **Re-bootstrap cost** — every shocked context re-runs curve construction and (for MC) vol
   calibration; cache the base context and perturb locally where possible.
