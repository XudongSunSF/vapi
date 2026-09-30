# Code Review — `spline.h`

## Verdict

The generic `Axis` / `Interpolatable` / `AxisFor` / `AxisDefaultInterpT` machinery is well-intentioned but is effectively dead code: every interpolation path collapses to `double` before the math happens. The shadow vectors flagged below are the mechanism — they're the *only* thing the interpolation actually reads, and they're hard-coded `double`.

---

## 1. Core issue: the concepts are bypassed by `double` shadows

`LinearSpline1d` stores two copies of the data:

```cpp
std::pmr::vector<x_type> xs;
std::pmr::vector<V>     ys;
std::pmr::vector<double> xCoords;   // shadow
std::pmr::vector<double> yValues;   // shadow
```

and `LinearSpline2d` does the same with `xCoords` / `yCoords` / `zValues`.

The interpolation reads only the shadows:

```cpp
// LinearSpline1d::operator()
const double xQuery = static_cast<double>(static_cast<V>(XAxis::difference(x, xs.front())));
return static_cast<V>(Interpolator::linear(xCoords, yValues, xQuery, true));
```

```cpp
// LinearSpline2d::operator() interior path
const double xQuery = static_cast<double>(static_cast<V>(XAxis::difference(x, xs.front())));
const double yQuery = static_cast<double>(static_cast<V>(YAxis::difference(y, ys.front())));
return Interpolator::bilinear<double, double, double>(xCoords, yCoords, zValues, xQuery, yQuery);
```

Consequences:

- `difference_type` is converted `→ V → double` at the only point it matters, so `std::int64_t` (from `LinearAxis<int>`) is truncated to 53 bits of precision before any weight is computed.
- `Interpolatable V` only works meaningfully when `V == double`. `float` / `long double` are accepted by the concepts but silently re-narrowed/widened through `double`.
- `xs` / `ys` / `zs` become mostly decorative — they feed getters, size guards, and the 2d boundary logic, while the real math runs on a duplicated `double` copy. That's two sources of truth.
- `ys` is `std::pmr::vector<V>` but `yValues` is `std::pmr::vector<double>`: `getYs()` returns `V` while interpolation consumes `double` — inconsistent representations of the same data.

In short: `AxisDefaultInterpT` and `AxesDefaultInterpT` are elaborate machinery that always resolves to `double` in practice.

---

## 2. Unenforced coordinate convention (potential latent bug)

`xQuery` is a **relative offset** from `xs.front()`, but it is passed to `Interpolator::linear(xCoords, ...)`. Nothing in this header documents whether `xCoords` holds absolute breakpoints or offsets from the front:

- If `xCoords` is absolute, the query is in the wrong coordinate system — a real bug.
- If `xCoords` is offset-based, that's an undocumented invariant that only the (out-of-header) builders can preserve.

The same ambiguity applies to the 2d interior path and to `zValues`. This should either be documented with an assertion, or eliminated by removing the shadows entirely.

---

## 3. Secondary issues

| # | Location | Issue |
|---|----------|-------|
| 1 | `throwIfNotIntegralRawAxisValue` / `LinearAxis::parse` | Checks finiteness/integrality but **not range**. `static_cast<value_type>(raw)` on e.g. `1e20` for `T = int` is UB. Use `std::in_range<T>(raw)`. |
| 2 | `LinearAxis::raw_type` | `raw_type` for integral `T` is `double` — the "external" form of an integer axis is itself a lossy, UB-prone `double`, the very thing the axis is meant to sanitize. `raw_type = T` would be more consistent. |
| 3 | `io::buildLinearSpline*` | Builder signatures hard-code `std::pmr::vector<double> y` / `z` even though `V` is a template parameter. The value type is fixed to `double` at the API boundary too. |
| 4 | `LinearSpline1d` | `using d_type` is unused. |
| 5 | `interpRow` / `interpCol` | `std::lower_bound` used but `<algorithm>` is not included — compiles only via transitive includes. |
| 6 | 1d vs 2d clamping | 1d delegates clamping to `Interpolator::linear(..., true)`; 2d clamps explicitly in `interpRow`/`interpCol`. The boundary-behavior model is split across two places. |
| 7 | Builder signatures | Inconsistent: 1d takes `x`/`y` by value, 2d by `const&`/value. |
| 8 | `Axis` concept | Checks `std::totally_ordered<typename A::value_type>` before the `requires` clause that establishes `value_type` exists. Reorder so existence is verified first (SFINAE-friendliness). |

---

## 4. Recommended direction

**Option A — drop `Interpolator` and the shadows (recommended).** The correct interpolation logic already exists in `interpRow` / `interpCol`. Factor it into a shared 1d helper and use it everywhere:

```cpp
// Shared 1d helper operating on the real vectors, weights in V
template<Axis XAxis, Interpolatable V>
V lerp(const std::pmr::vector<typename XAxis::value_type>& xs,
       const std::pmr::vector<V>& ys,
       typename XAxis::value_type x)
{
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back())  return ys.back();

    const auto it = std::lower_bound(xs.begin(), xs.end(), x);
    const std::size_t i = static_cast<std::size_t>(it - xs.begin()) - 1;

    const auto dx  = XAxis::difference(xs[i + 1], xs[i]);
    const auto dxq = XAxis::difference(x, xs[i]);
    const V t = static_cast<V>(dxq) / static_cast<V>(dx);
    return ys[i] + (ys[i + 1] - ys[i]) * t;
}
```

Then `LinearSpline1d::operator()` becomes a one-liner over `xs`/`ys`, and `LinearSpline2d` composes it for rows/columns. No shadow vectors, no `double` round-trip, single source of truth.

**Option B — if `Interpolator` must be reused**, template it over the coordinate/value types (e.g. `Interpolator::linear<V>(xs, ys, x, clamp)`) and feed it `xs`/`ys` directly, converting only when an axis's `difference_type` genuinely needs promotion.

**Either way:**

- Make builders take `std::pmr::vector<V>` instead of `std::pmr::vector<double>`.
- Add `std::in_range` to the integral-axis parse guard, and `#include <algorithm>`.
- Reconsider `raw_type = double` for integral axes.
- If the true intent is "double only," say so: drop `AxisDefaultInterpT`/`AxesDefaultInterpT` and constrain `V` explicitly (e.g. `requires std::same_as<V, double>`). Right now the concepts advertise generality the implementation doesn't deliver, which is worse than being honestly `double`-only.

---

## Bottom line

The fix isn't to defend the shadow vectors with more comments or sync invariants — it's to delete them and interpolate directly on `xs`/`ys`/`zs` with weights computed in `V`. That single change makes the concept layer meaningful instead of decorative.
