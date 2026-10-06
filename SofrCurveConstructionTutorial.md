# SOFR Curve Construction — A Practical Tutorial

How to build a SOFR discount curve from **SOFR futures + OIS swaps**, using the standard
"bootstrap → interpolate" recipe. This is the canonical approach and matches what this codebase
does behind `SofrSwapCurveFactory` / `SOFRBootstrappingHelper` / `YieldTermStructure`
([yield_curve_construction](../../../.github/pando_context/pando_context/modules/yield_curve_construction.md)).

---

## 1. What you are building

One curve, three equivalent views:

| View | Symbol | Definition |
|---|---|---|
| Discount factors | $DF(t)$ | Present value of $1 paid at $t$ |
| Zero (spot) rates | $z(t)$ | $DF(t) = e^{-z(t)\,t}$ (cont. comp.) |
| Forward rates | $f(t_1,t_2)$ | $DF(t_2) = DF(t_1)\cdot e^{-f(t_1,t_2)\,(t_2-t_1)}$ |

The discount factor is the fundamental object; everything else is a re-labeling of it. All
derivatives (swaps, futures, options) are priced off $DF(t)$.

---

## 2. SOFR conventions (they matter for the math)

- **SOFR** = Secured Overnight Financing Rate: a backward-looking *overnight* rate computed from
  repo transactions, published with a one-day lag.
- **Day count:** Act/360 — accrual over a period is $\Delta = \text{days}/360$.
- **Compounding:** the floating leg of an OIS compounds daily SOFR over each accrual period:
  $$
  1 + \Delta_{\text{period}}\cdot R_{\text{period}} \;=\; \prod_{d \in \text{period}} \left(1 + \frac{r_d \cdot n_d}{360}\right)
  $$
- **No credit term structure** (unlike old LIBOR): SOFR is a risk-free proxy, so there is no
  "bank credit" premium baked in.

---

## 3. The two instrument legs

### 3.1 SOFR futures — the short end

A SOFR future covers a reference quarter and is quoted as price $P = 100 - R$, where $R$ is the
implied annualized rate (%). For the reference period $[T_1, T_2]$ with accrual
$\Delta = (T_2-T_1)/360$, the futures-implied forward rate is $R$. It gives you:

$$
DF(T_2) = \frac{DF(T_1)}{1 + f\,\Delta}
\qquad\text{where } f \approx R \text{ (before convexity adjustment)}
$$

### 3.2 OIS swaps — the long end

An OIS swap exchanges a **fixed rate $S_n$** against the **compounded SOFR** floating leg for
maturity $T_n$. At par (market convention), the fixed leg's present value equals the floating
leg's:

$$
S_n \underbrace{\sum_{i=1}^{n} \Delta_i\,DF(T_i)}_{A_n \;=\; \text{annuity}}
\;=\; 1 - DF(T_n)
$$

The fixed rate $S_n$ is what the market quotes. The equation has one unknown, $DF(T_n)$, once
all earlier $DF(T_i)$ are known.

---

## 4. Bootstrapping, step by step

"Bootstrapping" = solving for discount factors **one maturity at a time**, always using only
instruments whose earlier cash-flow dates have already been resolved.

```
DF(0) = 1
for each instrument in maturity order:
    if instrument is a FUTURE over [T1, T2]:
        f  = impliedRate - convexityAdjustment        # see §5
        DF(T2) = DF(T1) / (1 + f * accrual(T1,T2))
    if instrument is an OIS SWAP with rate S_n and maturity T_n:
        DF(T_n) = (1 - S_n * sum_{i<n} accrual_i * DF(T_i)) / (1 + S_n * accrual_n)
```

### Worked example (illustrative, Act/360, no convexity/lag)

Take a flat 1-year run with quarterly instruments:

| Instrument | Price/Rate | Implied forward $f$ |
|---|---|---|
| 3M future | 98.25 | $1.75\%$ |
| 6M future | 98.00 | $2.00\%$ |
| 9M future | 98.10 | $1.90\%$ |
| 1Y OIS swap | $S=2.10\%$ | — |

1. $DF(0.25) = \dfrac{1}{1 + 0.0175 \times 0.25} = 0.995640$
2. $DF(0.50) = \dfrac{0.995640}{1 + 0.0200 \times 0.25} = 0.990687$
3. $DF(0.75) = \dfrac{0.990687}{1 + 0.0190 \times 0.25} = 0.986003$
4. $DF(1.00) = \dfrac{1 - 0.0210 \times 0.25\,(0.995640 + 0.990687 + 0.986003)}{1 + 0.0210 \times 0.25} = 0.979252$

That's the entire algorithm: futures chain the short end, swaps extend the long end, and each
step solves one number.

---

## 5. Futures convexity adjustment (don't skip it)

Futures are **daily-settled**, so their price is linear in the rate; a forward is not. The
futures-implied rate is therefore **higher** than the true forward rate by a convexity bias. A
common first-order correction (one-factor normal model, constant vol $\sigma$):

$$
f \;=\; R \;-\; \tfrac12\,\sigma^2\,T_1\,T_2
$$

In practice you estimate $\sigma$ from caplet/swaption vols or a Hull-White / Ho-Lee calibration.
Skipping this biases the short end of the curve high, which propagates into every long-end
discount factor.

---

## 6. Interpolation — monotone convex

After bootstrapping you have $DF$ at a discrete set of knots. Interpolation fills the gaps. The
default here is **monotone convex** (Hagan–West):

- it is **local** (no far-away wiggle when one knot changes),
- it preserves **positivity** and **monotonicity** of forward rates,
- it avoids the "kinks → negative forward rates" artifacts of plain linear interpolation.

For a smooth, non-negative, arb-free short end, prefer monotone convex over cubic splines on
rates.

---

## 7. How this codebase wires it

| Step | Component |
|---|---|
| Input container | `SofrSwapCurveInput` = `SofrFuture[]` + `GenericVanillaSwap[]` |
| Front-end bootstrap | `SOFRBootstrappingHelper::bootstrapSofrFirstFuture` |
| Long-end bootstrap | `SOFRBootstrappingHelper::bootstrapSofrSwap` |
| Factory | `SofrSwapCurveFactory::create(date, input, interp) → YieldTermStructure` |
| Interpolation | `MONOTONECONVEX` (default) |
| Curve object | `YieldTermStructure` with `getDF(Date/double)` and optional OAS spread |
| Reader | `SofrSwapCurveInputReader` (JSON / DB) |
| Async tasks | `ConstructSofrFutureCurve`, `ConstructSofrSwapCurve`, `ShockSofrSwapCurve` |

The `ShockSofrSwapCurve` task is the scenario hook: it rebuilds the curve under parallel shocks,
which is how rate-shift scenarios feed the rest of the pipeline.

---

## 8. Where the curve goes next: the secondary mortgage rate model

The bootstrapped curve is a *discount* curve — but the secondary mortgage rate (basis) model does
not consume $DF(t)$ directly. It consumes the **SOFR swap rates** that come out of the same IR
model, because the current-coupon mortgage rate is modeled as

$$
r^{\text{mortgage}} \;=\; \underbrace{\text{weighted SOFR swap rate}}_{\text{WSR}} \;+\; \text{basis}
$$

([basis_model](../../../.github/pando_context/pando_context/modules/basis_model.md)).

### 8.1 What "weighted SOFR swap" means

Each `MortgageRateType` (e.g. FN30 current coupon) has a set of `RateWeights` expressed in
`KeyRate` (SOFR swap tenor) space. The model declares which tenors it needs via
`determineRequiredIndices(types)`, which returns `KeyRate{SofrSwap, tenor}` indices, and forms the
weighted swap:

$$
W\!S\!R_t = \sum_{\text{tenor}} w_{\text{tenor}} \cdot r^{\text{SOFR swap}}_{\text{tenor},\,t}
$$

`indexToWeight(keys, keyToWeight)` is the helper that maps those parameter-space tenors onto the IR
model's actual SOFR swap-rate columns.

### 8.2 Where SOFR swap rates enter the model

- **Calibration** — `MarketSnapshot::wsr_base` holds the weighted SOFR swap level at T0, and the
  session builder pulls **historical SOFR swap rates** to seed the rolling weighted-swap history
  (`calculateHistory` / `anchorPrevious`).
- **Projection** — `SecondaryMortgageRateModel::project(session, types, rates)` walks Monte-Carlo
  **SOFR swap-rate paths** and recomputes `weightedSwap` each step before adding the basis.
- **Basis dynamics** — the *change* in the weighted swap drives the spread through the regression
  term $\beta_{\text{wsr}}\,\Delta W\!S\!R$ in `StatisticalBasis::unadjustedBasis`.

### 8.3 Two different SOFR roles (don't confuse them)

| Role | What SOFR object | Consumer |
|---|---|---|
| Rate driver (WSR) | SOFR **swap (term)** rates | `SecondaryMortgageRateModel` (basis) |
| Discounting | SOFR **1M** rate path | `DiscountingModel::calculate(sofr1mRatePaths, ...)` |

The curve you bootstrap feeds the *swap-rate* side; the *discounting* side uses the 1M short rate
([discounting_model](../../../.github/pando_context/pando_context/modules/discounting_model.md)).

> Treasury note: `SecondaryMortgageRateModel::getSwapType()` returns `SofrSwap` today, which is why
> a Treasury-linked basis requires selecting `ProductType::Treasury` instead — the `basisIndex_`
> generalization in `RateGenerationModelSuiteRedesign.md` §6A.3.

---

## 9. Common pitfalls

1. **Ignoring convexity adjustment** — short-end forwards come out too high.
2. **Mixing day counts** — SOFR legs are Act/360; mixing Act/365 biases every accrual.
3. **Stale/illiquid futures** — beyond a few quarters the futures strip thins out; switch to
   OIS swaps (or SOFR-FRA) where the strip loses liquidity.
4. **Negative rates** — bootstrapping in log space breaks; use normal (bps) convention or a
   shifted/lognormal-normal hybrid.
5. **Interpolation artifacts** — linear-on-rates can create negative forwards at kinks; prefer
   monotone convex.
