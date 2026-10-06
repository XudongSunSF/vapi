# SOFR Curve Construction: A Practical Tutorial

How to build a SOFR discount curve from **SOFR futures + OIS swaps**, using the standard
"bootstrap, with interpolation inside the loop" recipe. This is the canonical approach and matches what this codebase
does behind `SofrSwapCurveFactory` / `SOFRBootstrappingHelper` / `YieldTermStructure`
([yield_curve_construction](../../../.github/lib_context/lib_context/modules/yield_curve_construction.md)).

---

## 1. What you are building

One curve, three equivalent views:

| View | Symbol | Definition |
|---|---|---|
| Discount factors | $DF(t)$ | Present value of \$1 paid at $t$ |
| Zero (spot) rates | $z(t)$ | $DF(t) = e^{-z(t)\,t}$ (cont. comp.) |
| Forward rates | $f(t_1,t_2)$ | $DF(t_2) = DF(t_1)\cdot e^{-f(t_1,t_2)\,(t_2-t_1)}$ |

The discount factor is the fundamental object; everything else is a re-labeling of it. All
linear products (swaps, futures, FRAs) are priced directly off $DF(t)$. Options additionally
need a volatility model.

---

## 2. SOFR conventions (they matter for the math)

- **SOFR** = Secured Overnight Financing Rate: a backward-looking *overnight* rate computed from
  Treasury repo transactions, published on the next business day.
- **Day count:** Act/360, so accrual over a period is $\Delta = \text{days}/360$.
- **No bank-credit term structure** (unlike old LIBOR): SOFR is a secured, near risk-free rate,
  so there is no "bank credit" premium baked in.

**Compounding:** the floating leg of an OIS compounds daily SOFR over each accrual period:

```math
1 + \Delta_{\mathrm{period}} \cdot R_{\mathrm{period}} = \prod_{d \in \mathrm{period}} \left(1 + \frac{r_d \cdot n_d}{360}\right)
```

where $r_d$ is the SOFR fixing for day $d$ and $n_d$ is the number of calendar days that
rate applies (e.g., 3 over a weekend).

---

## 3. The two instrument legs

### 3.1 SOFR futures: the short end

A 3M SOFR future settles on the compounded SOFR over its reference quarter and is quoted as
price $P = 100 - R$, where $R$ is the implied annualized rate (%). For the reference period
$[T_1, T_2]$ with accrual $\Delta = (T_2-T_1)/360$, the futures-implied rate is $R$. It gives you:

```math
DF(T_2) = \frac{DF(T_1)}{1 + f\,\Delta}, \qquad f \approx R \ \text{(before convexity adjustment)}
```

### 3.2 OIS swaps: the long end

An OIS swap exchanges a **fixed rate $S_n$** against the **compounded SOFR** floating leg for
maturity $T_n$. At par (market convention), and assuming the same curve is used for projection
and discounting, the fixed leg's present value equals the floating leg's:

```math
S_n \cdot A_n = 1 - DF(T_n), \qquad A_n = \sum_{i=1}^{n} \Delta_i \, DF(T_i)
```

Here $A_n$ is the swap annuity. The fixed rate $S_n$ is what the market quotes. The equation has
one unknown, $DF(T_n)$, once all earlier $DF(T_i)$ are known.

---

## 4. Bootstrapping, step by step

"Bootstrapping" = solving for discount factors **one maturity at a time**, always using only
instruments whose earlier cash-flow dates have already been resolved.

```python
df[0] = 1.0
for inst in instruments_sorted_by_maturity:
    if inst.is_future:                                  # covers [T1, T2]
        f = inst.implied_rate - convexity_adj(inst)     # see section 5
        df[T2] = df[T1] / (1 + f * accrual(T1, T2))
    elif inst.is_ois_swap:                              # rate S_n, maturity T_n
        annuity_prev = sum(accrual[i] * df[T[i]] for i in range(n - 1))
        df[T_n] = (1 - S_n * annuity_prev) / (1 + S_n * accrual[n])
```

Two practical notes:

- **Stub:** the first future starts at an IMM date, not today. $DF(T_1)$ for that first
  future must come from a stub (spot/overnight SOFR or 1M futures).
- **Interpolation is inside the loop:** a multi-year swap has fixed-leg dates that are not
  curve knots, so the intermediate $DF(T_i)$ come from the interpolator. Each swap is therefore
  a 1-D root solve for $DF(T_n)$ (the closed form above only holds when every $T_i$ is already a knot).

### Worked example (illustrative: Act/360, no convexity/lag, quarterly fixed leg)

Take a 1-year run with consecutive quarterly instruments (a real 1Y SOFR OIS pays a single
fixed payment at maturity; a quarterly fixed leg is assumed here for illustration):

| Instrument | Price/Rate | Implied forward $f$ |
|---|---|---|
| Future, 0–3M | 98.25 | $1.75\%$ |
| Future, 3–6M | 98.00 | $2.00\%$ |
| Future, 6–9M | 98.10 | $1.90\%$ |
| 1Y OIS swap | $S=2.10\%$ | n/a |

Values are computed at full precision and shown to 6 decimals.

1. $DF(0.25) = \dfrac{1}{1 + 0.0175 \times 0.25} = 0.995644$
2. $DF(0.50) = \dfrac{0.995644}{1 + 0.0200 \times 0.25} = 0.990691$
3. $DF(0.75) = \dfrac{0.990691}{1 + 0.0190 \times 0.25} = 0.986007$
4. For the 1Y swap:

```math
DF(1.00) = \frac{1 - 0.0210 \times 0.25 \times (0.995644 + 0.990691 + 0.986007)}{1 + 0.0210 \times 0.25} = 0.979254
```

That's the entire algorithm: futures chain the short end, swaps extend the long end, and each
step solves one number.

---

## 5. Futures convexity adjustment (don't skip it)

Futures are **daily-margined**, so their rate is the expected rate under the risk-neutral
measure, while a forward rate is not. The futures-implied rate is therefore **higher** than the
true forward rate by a convexity bias. A common first-order correction (Ho-Lee / one-factor
normal model, constant vol $\sigma$):

```math
f = R - \tfrac{1}{2}\,\sigma^2\,T_1\,T_2
```

In practice you estimate $\sigma$ from caplet/swaption vols or a Hull-White calibration. The bias
is negligible for the first quarters but grows roughly with $T^2$. Skipping it biases forwards
high, increasingly so for later contracts, and that propagates into every long-end discount factor.

---

## 6. Interpolation: monotone convex

After bootstrapping you have $DF$ at a discrete set of knots. Interpolation fills the gaps. The
default here is **monotone convex** (Hagan-West):

- it is **local** (no far-away wiggle when one knot changes),
- it keeps forward rates **positive** when the input forwards are positive, and preserves their **monotonicity**,
- it avoids the discontinuous, sometimes negative, forwards produced by plain linear interpolation on zero rates.

For a continuous, non-negative forward curve, prefer monotone convex over cubic splines on
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
| Async jobs | `ConstructSofrFutureCurve`, `ConstructSofrSwapCurve`, `ShockSofrSwapCurve` |

The `ShockSofrSwapCurve` job is the scenario hook: it rebuilds the curve under parallel shocks,
which is how rate-shift scenarios feed the rest of the pipeline.

---

## 8. Where the curve goes next: the secondary mortgage rate model

The bootstrapped curve is a *discount* curve, but the secondary mortgage rate (basis) model does
not consume $DF(t)$ directly. It consumes the **SOFR swap rates** that come out of the same IR
model, because the current-coupon mortgage rate is modeled as

```math
r^{\mathrm{mortgage}} = \mathrm{WSR} + \mathrm{basis}
```

where $\mathrm{WSR}$ is the weighted SOFR swap rate
([basis_model](../../../.github/lib_context/lib_context/modules/basis_model.md)).

### 8.1 What "weighted SOFR swap" means

Each `MortgageRateType` (e.g. FN30 current coupon) has a set of `RateWeights` expressed in
`KeyRate` (SOFR swap tenor) space. The model declares which tenors it needs via
`determineRequiredIndices(types)`, which returns `KeyRate{SofrSwap, tenor}` indices, and forms the
weighted swap:

```math
\mathrm{WSR}_t = \sum_{\mathrm{tenor}} w_{\mathrm{tenor}} \cdot r^{\mathrm{SOFR\ swap}}_{\mathrm{tenor},\,t}
```

`indexToWeight(keys, keyToWeight)` is the helper that maps those parameter-space tenors onto the IR
model's actual SOFR swap-rate columns.

### 8.2 Where SOFR swap rates enter the model

- **Calibration:** `MarketSnapshot::wsr_base` holds the weighted SOFR swap level at T0, and the
  session builder pulls **historical SOFR swap rates** to seed the rolling weighted-swap history
  (`calculateHistory` / `anchorPrevious`).
- **Projection:** `SecondaryMortgageRateModel::project(session, types, rates)` walks Monte-Carlo
  **SOFR swap-rate paths** and recomputes `weightedSwap` each step before adding the basis.
- **Basis dynamics:** the *change* in the weighted swap drives the spread through the regression
  term $\beta_{\mathrm{wsr}}\,\Delta\mathrm{WSR}$ in `StatisticalBasis::unadjustedBasis`.

### 8.3 Two different SOFR roles (don't confuse them)

| Role | What SOFR object | Consumer |
|---|---|---|
| Rate driver (WSR) | SOFR **swap (term)** rates | `SecondaryMortgageRateModel` (basis) |
| Discounting | SOFR **1M** rate path | `DiscountingModel::calculate(sofr1mRatePaths, ...)` |

The curve you bootstrap feeds the *swap-rate* side; the *discounting* side uses the 1M short rate
([discounting_model](../../../.github/lib_context/lib_context/modules/discounting_model.md)).

> Treasury note: `SecondaryMortgageRateModel::getSwapType()` currently returns `SofrSwap`, so a
> Treasury-linked basis is not supported by default. It requires selecting `ProductType::Treasury`
> via the `basisIndex_` generalization in `RateGenerationModelSuiteRedesign.md` §6A.3.

---

## 9. Common pitfalls

1. **Ignoring convexity adjustment:** forwards come out too high, increasingly for later futures.
2. **Mixing day counts:** SOFR legs are Act/360; mixing Act/365 biases every accrual.
3. **Illiquid futures:** past the liquid part of the strip (typically a few years), switch to
   OIS swaps (or SOFR FRAs).
4. **Negative rates:** positivity-preserving interpolation and lognormal vol assumptions break;
   use normal (bp) vol and an interpolator that allows negative forwards.
5. **Interpolation artifacts:** linear-on-rates creates discontinuous, possibly negative,
   forwards at kinks; prefer monotone convex.
