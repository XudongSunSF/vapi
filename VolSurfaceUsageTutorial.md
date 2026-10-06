# Volatility Surface Usage — Secondary Mortgage Rates, Scenario Shocks, and Greeks

A tutorial on how the swaption **volatility surface** (a.k.a. vol cube) is consumed by the three
places this codebase uses it: the **secondary mortgage rate (basis) model**, **scenario vol
shocks**, and **vol Greeks** — plus the indirect role as the Monte-Carlo rate model's calibration
target.

---

## 1. What the vol surface is

A swaption is an option to enter a swap. Its volatility is indexed by three axes:

$$
\sigma(T_{\text{expiry}},\; T_{\text{tenor}},\; K)
$$

| Axis | Meaning |
|---|---|
| $T_{\text{expiry}}$ | when the option expires |
| $T_{\text{tenor}}$ | length of the underlying swap |
| $K$ | strike, usually expressed as moneyness relative to the ATM forward swap rate |

Two quote conventions coexist:

- **Normal / Bachelier** vol — in **basis points** (preferred near zero/negative rates).
- **Lognormal / Black** vol — in **percent**.

In this codebase the surface is `VolatilityStructureInput`, accessed through the
`VolatilityStructure` interface (`volatilityRelative`/`volatilityAbsolute`) with
`IAbsoluteExpiryVolatilityCube` / `IRelativeExpiryVolatilityCube` variants
([wfmcm_public_api](../../../.github/pando_context/pando_context/modules/wfmcm_public_api.md)).
It arrives over the API via `setMarketSwaptionVolatilityInput(..., atTheMoney, ...)` and through
the data provider as `normalSwaptionData` / `lognormalSwaptionData`
([app_common](../../../.github/pando_context/pando_context/modules/app_common.md)).

---

## 2. Use case 1 — the secondary mortgage rate (basis) model

The secondary mortgage rate (current-coupon, e.g. 30Y FNMA) is modeled as:

$$
r^{\text{mortgage}} = \underbrace{\text{weighted swap rate}}_{\text{from IR paths}} \;+\; \text{basis}
$$

The basis is not a constant. In the `StatisticalBasis` model it evolves as
([basis_model](../../../.github/pando_context/pando_context/modules/basis_model.md)):

$$
\text{basis}_t = \text{basis}_{t-1}
+ \beta_{\text{wsr}}\,\Delta W\!S\!R
+ \beta_{\text{vol}}\,\Delta \text{Vol}
+ \beta_{\text{trend}}\,\Delta \text{Trend}
+ \beta_{\text{hpa}}\,\Delta \text{HPA}
+ k\,(\text{LTL} - \text{basis}_{t-1})\,\Delta t
$$

with long-term level

$$
\text{LTL} = c_0 + c_{\text{hpa}}(\text{HPA}_{t-1} - \text{HpaCap}) + c_{\text{vol}}\,\text{Vol}_{t-1}
$$

**Where does the vol surface enter?**

- `MarketSnapshot::vol_base` — the *level* of swaption vol at calibration.
- The $\beta_{\text{vol}}\,\Delta\text{Vol}$ term — the *change* in vol drives basis changes.
- The **weighted-vol history** — `StatisticalBasis::calculateHistory` builds a 13-month
  weighted-vol series from the surface, and `setWeightedVol` feeds it into the time-slice state.

**Why vol at all?** The mortgage spread to swaps embeds a prepayment/refinancing option: higher
rate volatility increases the value of the borrower's refinancing option, which widens the
current-coupon spread. Vol is therefore a first-class *driver* of the basis, not a risk-free
afterthought.

---

## 3. Use case 2 — scenario vol shocks

Scenario definitions shock the vol surface explicitly
([scenario_infrastructure](../../../.github/pando_context/pando_context/modules/scenario_infrastructure.md)):

- `ScenarioDef::getVolCubeShifts()` / `getVolCubeShiftByTerms()`
- `VolCubeShift` = a shift on one **slice tenor**, as a map `(expiry, tenor) → shift (bps)`.

Two flavors:

| Flavor | What moves | Use |
|---|---|---|
| **Parallel** vol shock | every $(T_{\text{expiry}}, T_{\text{tenor}})$ by the same bps | whole-surface scenarios |
| **Partial / key** vol shock | one expiry/tenor bucket | key-vol sensitivity |

Mechanically the shocked surface flows through **both** consumers at once: it re-anchors the
basis model's `vol_base`/$\Delta\text{Vol}$ terms **and** it changes the IR model's calibration
(§5), so the shock propagates: vol → rate paths → mortgage rates → cashflows → price.

---

## 4. Use case 3 — Greeks

Vol Greeks are finite-difference sensitivities of price to the surface:

$$
\text{Vega} = \frac{\partial P}{\partial \sigma}
\qquad
\text{Volga} = \frac{\partial^2 P}{\partial \sigma^2}
$$

Bump-and-reprice recipe:

1. Shift the surface by $\pm\varepsilon$ (e.g. $1$ bp normal).
2. Re-run the pipeline (re-calibrate → re-project → re-price) for each bump.
3. $\text{Vega} \approx \dfrac{P(+\varepsilon) - P(-\varepsilon)}{2\varepsilon}$.

The concrete algorithms here are:

- `ParallelVegaAlgo` / `ParallelVolgaAlgo` — the whole surface moves together.
- `PartialVegaAlgo` / `PartialVolgaAlgo` — bucketed (key-vol) shifts, one bucket at a time.

(`GreekType` also lists `HPI01`/`Unemployment01` as siblings — those shock the HPI/unemployment
state series via `StateLookupDataShock`, a parallel mechanism to the vol-cube shocks.)

---

## 5. Use case 4 (indirect) — the Monte-Carlo calibration target

The surface is also the **calibration target** of the Libor Market Model:

- `VolMatrixCalibrator::calibrate` (NL2SOL) fits the LMM vol matrix so that model-implied
  swaption vols match the market surface (`AnalyticalSwaption` + `calcSofrSwapRate`).
- `SkewCalibrator` fits the SABR-like skew across the `(expiry, tenor, moneyness)` grid.

So the surface doesn't just *level-shift* a scalar — it **determines the distribution of the
Monte-Carlo rate paths**. A vol shock is therefore a re-calibration event, not a cosmetic tweak
([interest_rate_model](../../../.github/pando_context/pando_context/modules/interest_rate_model.md)).

---

## 6. How this codebase wires it

| Purpose | Component |
|---|---|
| Surface input | `VolatilityStructureInput`, `VolatilityBasket` (subset) |
| Surface access | `VolatilityStructure::volatilityRelative/Absolute` |
| Basis model vol terms | `MarketSnapshot::vol_base`, `StatisticalBasis` $\beta_{\text{vol}}$, weighted-vol history |
| Scenario shocks | `VolCubeShift`, `ScenarioDef::getVolCubeShifts` |
| Greeks | `ParallelVegaAlgo`, `PartialVegaAlgo`, `ParallelVolgaAlgo`, `PartialVolgaAlgo` |
| LMM calibration | `VolMatrixCalibrator`, `SkewCalibrator` |

---

## 7. Pitfalls

1. **Normal vs lognormal units** — a "$1$ bp" bump means very different things in bps-vol vs
   %-vol; keep units explicit.
2. **ATM-only vs full cube** — many workflows only carry the ATM slice; partial/skew Greeks need
   the moneyness dimension.
3. **Basket selection** — calibrating to a `VolatilityBasket` subset changes the fit; a shock on
   a point outside the basket has no effect.
4. **Double counting** — the same surface feeds the basis model (level + delta) *and* the IR
   calibration (full grid); a scenario shock hits both, so don't also hand-apply a separate
   basis vol shift unless intended.
5. **Re-calibration cost** — partial-vega bumps that re-run LMM calibration are expensive; cache
   the base calibration and perturb locally where the model supports it.
