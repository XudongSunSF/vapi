# Intex Cashflow Engine — 2027 Plan

**Objective.** A runnable C++ wrapper over the Intex CMO subroutine library, integrated with the existing behavioral model output, validated against PolyPaths on agency CMOs.

**Effort budget.** **40 delivered weeks** across calendar 2027, against two developers in sequence.

**Unit convention.** All estimates in delivered weeks (w). 1 person-month = 4 weeks. Estimates are delivered effort, not elapsed time; the gap absorbs PTO, meetings and interrupts.

**Definition of done.** Every sub-task names an artifact — code, test, memo or report. "Investigated it" is not an artifact.

---

## 1. Staffing model

| Period | Who | Focus |
|---|---|---|
| Q1 | Developer A — new to mortgage analytics and Intex | Research, assessment, spike, portfolio inventory, handover package |
| Q2–Q4 | Developer B — experienced hire | All production implementation, bridge, parity |

This split shapes the plan more than any technical decision in it.

**Q1 is deliberately weighted toward knowledge rather than code.** Developer A departs at the quarter boundary, so anything held only in their head is lost. Q1's job is to answer every open question in writing and prove one deal runs end to end — not to build the production wrapper. A throwaway prototype that resolves the injection-mode question is worth more than half-finished production code that Developer B will rewrite.

**Developer B starts with every blocking question already answered.** Injection mode, initialization semantics, thread-safety behavior, prior-work disposition, portfolio composition, pool-to-group reconciliation. That is what makes 30 weeks sufficient for Q2–Q4.

**Assumption:** Developer B is experienced in securitization analytics, not merely in C++. If the hire is a strong C++ engineer without mortgage background, add 2–3 w of ramp-up in Q2 and the June gate moves. State this explicitly in the job specification.

---

## 2. Scope

### Capability exclusions — only two

**Non-agency credit waterfall.** Overcollateralization, excess spread trapping, delinquency triggers, writedown ordering, severity and recovery lag. This is real implementation work — a trigger-state channel carrying delinquent balance and cumulative loss as *stocks*, plus loss allocation. Phase 2.

**Concurrency, GPU co-location, batching.** Throughput, not correctness. A wrapper that produces correct cashflows slowly is a valid year-one checkpoint. The interface is built to admit these without rework.

Everything else the injection path supports is supported. There is no third exclusion.

### Out of scope entirely

DevOps and packaging · licensing and vendor risk · MRM validation · portfolio-wide parity · as-of repricing · Java handle exposure · production cutover.

### Design commitments made on day one

Cheap to build in, expensive to retrofit:

- **Group-aware.** Group index in every bridge signature; group count read from the structural query, never assumed to be a single aggregate group. Group numbering is 1-based for real groups — group 0 is the synthetic whole-deal aggregate (`icmo_n_collat_grps` excludes it), so a single-group deal reports `icmo_n_collat_grps == 0`, not 1.
- **Index-aware.** Index rate forecasts are delivered through the vendor's per-index callback (`index_fcn` → `icmo_set_index_forecast()`), not as arrays plumbed alongside cashflows; the wrapper holds one curve per index per path and serves it from inside that callback, whether or not the portfolio currently holds floaters.
- **Granularity-agnostic input.** The bridge aggregates to group-level dollars. Whether the behavioral framework emits pool-level or loan-level results is an adapter concern, not a bridge concern — the input contract is defined on the aggregate.
- **No global or static state.** Handles move-only, owned per session. This is what makes phase 2 concurrency an addition rather than a rewrite.

### Input granularity

The behavioral framework emits pool-level results for some collateral, and possibly loan-level for fixed-rate collateral. Both are supported by the same bridge:

$$\text{loan or pool} \longrightarrow \text{group} \longrightarrow \text{deal}$$

Pool-level input is the simpler path — the pool→group mapping is defined in the offering document and exposed by the structural query, with no dependency on loan-level disclosure data or pool-reconstitution timing. Loan-level input requires a loan→pool→group chain, where the first arrow must come from the loan tape. Task 1.2 establishes which applies where.

---

## 3. Validation tiers

Validation effort is the scarce resource. Features differ in what they cost to validate, so the golden set grows in tiers rather than being fixed in advance.

| Tier | Content | Difference at the API call | Validation cost |
|---|---|---|---|
| **1** | Agency, fixed-rate collateral, single group — **any fixed-coupon class type**: sequential, PAC, TAC, support, Z | none | baseline |
| **2** | Multi-group deals | loop over a group index | ~0.5 w |
| **3** | Floating-rate and inverse classes | index rate callback per path (`icmo_set_index_forecast()`); coupon-cap (available-funds-cap) shortfall is vendor-side state to read back | ~1.5 w |
| **4** | ARM / hybrid collateral | none under injection | ~0.5 w |
| **5** | IO / PO classes | notional balance rather than principal balance on result extraction | ~0.5 w |

### Why class types collapse into tier 1

PAC, TAC, support and Z do not require different *calls* at the API boundary — the class type is readable from the result (`icmo_tranche_types` strings such as `PAC_FIX`), but it determines only how the waterfall allocates principal, and the waterfall is entirely internal to Intex — you set assumptions per group, run, and read tranche cashflows per class. The calls are identical. Clean-up call exercise is a run-time assumption flag (`icmo_do_optredeems[ICMO_OPTRTYPE_OPTIONAL] = ICMO_OPTREXEC_YES`, or an `icmo_optredeem_fcn` callback), not a feature class — with one parse-time caveat: for independent-group agency deals you must also set `icmomisc_alteruse.altu_cleanup_accuracy_required` to TRUE before parsing to get accurate cleanup-call timing.

Z classes deserve one fixture, not a tier. They are the case where an interest-waterfall item lands in the principal waterfall — accrual capitalizes to the Z balance while an equal amount pays principal to the accretion-directed class. That matters for *interpreting* a parity difference, not for making the call.

### Tier ordering is set by the portfolio, not by this table

Tiers 2–5 are ranked here by validation cost because that is what can be assessed before the inventory exists. **Task 1.2 replaces this ordering with one driven by actual holdings.** Validating what the book holds most of is worth more than validating what is structurally simplest.

If the portfolio holds no floaters or inverse floaters, tier 3 drops out entirely and 1.5 w returns to the triage budget. If ARM collateral is a large share, tier 4 moves ahead of tier 2. The commitment for 2027 is **tier 1 plus whatever tiers the Q3 capacity review supports**, in inventory-driven order.

---

## 4. Q1 — Research, assessment, prototype — 10 w

**Developer A.** Output is knowledge, decisions and a working prototype. Production code is explicitly not the goal.

### 1.1 Domain and platform ramp-up — 2 w

Target: able to hand-compute a two-tranche waterfall period and explain why a subordinate class differs from a senior one.

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 1.1.1 | Securitization fundamentals: amortization, prepayment, default, waterfall mechanics, agency vs. non-agency. Work through the four-stage tutorial including the hand arithmetic. | Completed exercises | 0.5 |
| 1.1.2 | Object model: loan, pool, group, deal, class; RMBS vs. CMO; where each lives in the platform. | Glossary note | 0.25 |
| 1.1.3 | Current PolyPaths integration: every call site, what the pricer consumes, what the behavioral models emit at what granularity. | Call-site inventory | 0.75 |
| 1.1.4 | Intex documentation and sample code review. | Reading notes | 0.5 |

**Comprehension gate.** A short written "how this fits together" note — collateral side, boundary, waterfall, pricer — reviewed by the architect. Cheapest available check on whether ramp-up worked, and it doubles as the first page of the handover package.

### 1.2 Portfolio composition inventory — 1 w

The tier ordering and much of the Q3 plan depend on knowing what is actually held. This is data work Developer A can do with SME support, and it is the highest-leverage week in the quarter.

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 1.2.1 | Extract Investment Portfolio holdings by CUSIP from the position system. | Holdings extract | 0.25 |
| 1.2.2 | Classify each holding: agency vs. non-agency; collateral type (fixed / ARM / hybrid); class type (fixed-coupon / floater / inverse / IO / PO); deal group count. | Classification table | 0.5 |
| 1.2.3 | Weight by market value and by position count; cross-reference against which collateral the behavioral framework currently models. | **Composition report** | 0.25 |

**This report sets the tier order in §3 and the golden-set composition in §6.** It answers directly whether floaters, inverse floaters, multi-group deals or ARM collateral exist in the book at material weight — none of which is currently known.

### 1.3 Prior work assessment — 2 w

Existing Intex wrapper work must be evaluated before any decision to build.

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 1.3.1 | Locate and inventory: repositories, branches, authorship, date, Intex version targeted, current owner. | Inventory note | 0.25 |
| 1.3.2 | Build it. Does it compile and run against the current SDK and a current deal file? | Build report | 0.5 |
| 1.3.3 | Functional inventory: which API calls are covered, which deal types were exercised, what was validated and against what. | Coverage matrix | 0.5 |
| 1.3.4 | Quality review against the checklist below. | Review findings | 0.5 |
| 1.3.5 | **Adopt / refactor / rewrite recommendation**, with rationale and consequences for the Q2 budget. | Decision memo | 0.25 |

**Quality checklist:**

- Global or static state anywhere in the wrapper
- Handle ownership: move-only? defined null state? double-close safe?
- Error handling: are vendor codes translated, and is the borrowed last-error string copied immediately?
- Buffer protocol: is the size-query/retry pattern implemented, including retry on size change?
- Header hygiene: does the vendor header leak into the public API?
- Test coverage, and whether tests run without a license
- Documented convention findings — potentially the most valuable content in the codebase
- Thread-safety assumptions, stated or implied

**Disposition criteria:**

| Finding | Recommendation | Q2 cost |
|---|---|---|
| Builds, no global state, tests present, conventions documented | Adopt | 1.5 w |
| Builds, sound structure, gaps in errors / buffers / tests | Refactor | 3 w (planning assumption) |
| Does not build, global state, or targets an incompatible Intex version | Rewrite | 4 w, funded from the Q4 buffer |

Global state is a hard fail regardless of how much else works — it forecloses phase 2 concurrency. Note also that the *findings* embedded in prior work retain full value even if every line of code is discarded; harvest them either way.

### 1.4 API spike and prototype — 3.5 w

Throwaway code. The deliverable is answers plus a demonstration that one deal runs end to end.

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 1.4.1 | **Injection mode.** Enumerate every way collateral behavior can be supplied — prepayment/loss assumption vectors, forced collateral cashflows (`icmo_override_coll_cf()`, and the per-pool `icmo_override_pool_cf_fcn` field), and callbacks (`speed_fcn`, `index_fcn`). Run one deal through each available path. Note the vendor strongly discourages forced cashflows and recommends letting the subroutines generate collateral cashflows; forced cashflows also do not carry index assumptions (indexes are always handled normally). | **Decision memo** | 1.0 |
| 1.4.2 | Initialization semantics: per-process or per-session, static-init order, license check timing, fork behavior. | Findings memo §1 | 0.25 |
| 1.4.3 | Thread-safety probe: two sessions, two threads, same and different deals. Sanitizers plus deliberate interleaving. Do not trust the documentation. | Findings memo §2 | 0.25 |
| 1.4.4 | Structural query: does it expose group definitions, each group's pool composition, and the group→class map? | Findings memo §3 | 0.25 |
| 1.4.5 | **Pool→group reconciliation.** For one multi-group deal, do modeled balances per group match the reported group balance at a fixed factor date? | Reconciliation report | 0.5 |
| 1.4.6 | Per-deal memory footprint and open/close latency across ~10 sample deals. | Measurement table | 0.25 |
| 1.4.7 | **End-to-end prototype**: one agency CMO, model output in, tranche cashflows out, compared informally against PolyPaths. | Prototype repo + note | 1.0 |

Task 1.4.5 determines whether multi-group is a loop over an index or an upstream data project. If it fails to reconcile, the cost sits in the data pipeline and no amount of wrapper work recovers it.

### 1.5 Interface draft — 0.5 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 1.5.1 | Draft the abstract engine interface from the 1.1.3 call-site inventory. Python pseudocode first, then a C++ header. Group index and the per-path index-curve store (served through the vendor's index callback) present from v1. | `cashflow_engine.h` draft + contract doc | 0.5 |

Draft, not frozen. Developer B finalizes it in Q2 — they will implement against it and should own it.

### 1.6 Handover package — 1 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 1.6.1 | Consolidate all Q1 memos into a single onboarding document, ordered for a reader who has not seen any of it. | Handover document | 0.5 |
| 1.6.2 | Annotate the prototype: what it proves, what it fakes, what to keep and what to discard. | Prototype README | 0.25 |
| 1.6.3 | Open questions register — everything Q1 could not resolve, with what was tried. | Open questions log | 0.25 |

**Q1 exit:** injection mode known; portfolio composition known; prior work dispositioned; pool→group reconciliation answered; one deal running end to end in prototype form; everything written down for a developer who has not met Developer A.

---

## 5. Q2 — Production wrapper, bridge, first parity — 12 w

**Developer B.**

### 2.0 Onboarding — 1 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.0.1 | Work through the handover package; run the prototype; reproduce the 1.4.7 result independently. | Reproduction note | 0.5 |
| 2.0.2 | Review and finalize the interface draft; freeze v1 with the architect and pricer owner. | Frozen `cashflow_engine.h` | 0.5 |

Independent reproduction is the check on whether the handover succeeded. If Developer B cannot reproduce the prototype result from the written material alone, the gap surfaces in week one rather than in Q3 triage.

If Developer A and B can overlap for a few days at the quarter boundary, take it — but the plan does not assume it.

### 2.1 `intex_core` — 3 w

Scope per the 1.3.5 disposition; refactor assumed.

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.1.1 | Handle wrappers: move-only, custom deleters, defined null state. Tests for move, self-move, double-close, use-after-move. | `handles.h` + tests | 0.75 |
| 2.1.2 | Error translation: vendor code → `std::error_category`; copy the borrowed last-error string immediately on failure. | `errors.h` + tests | 0.5 |
| 2.1.3 | Buffer protocol helper: size-query/retry including retry on size change. | `buffer.h` + tests | 0.5 |
| 2.1.4 | Header hygiene: pimpl plus a compile-firewall test that fails if vendor symbols reach the public API. | Firewall test | 0.25 |
| 2.1.5 | Structural query wrappers, including group enumeration and the group→class map. | `structure.h` + tests | 0.75 |
| 2.1.6 | No-global-state review; ownership rule documented. | Review checklist | 0.25 |

### 2.2 Supporting infrastructure — 1 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.2.1 | Deal cache keyed by (deal id, deal-file version); eviction budget from the 1.4.6 measurements. | `deal_cache.h` + tests | 0.5 |
| 2.2.2 | Record/replay fixtures so the test suite runs without a license. Intex ships a native recorder (`icmomisc_isr_fn` writes `.isr` files) and playback via `icmo_isr_play()` — assess it before building a custom harness. | Replay harness | 0.5 |

### 2.3 `intex_bridge` — aggregation — 2.5 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.3.1 | Input contract from the model layer, defined on the aggregate: units, sign conventions, period timing. Adapters for pool-level and loan-level sources. | Input contract doc | 0.5 |
| 2.3.2 | Aggregation to group level: balances, scheduled principal, prepayment, buyout, interest. Assert the aggregate reconciles to the component sum. | `aggregate.cpp` + tests | 0.75 |
| 2.3.3 | Group membership resolution from the structural query; assert every deal pool is modeled and every modeled pool maps to exactly one group. | Validation + tests | 0.5 |
| 2.3.4 | Buyout handling: fold into prepayment or carry separately, per the 1.4.1 findings. Agency defaults are par repurchases, so this is a prepayment-side decision. | Implementation + note | 0.25 |
| 2.3.5 | Settlement and factor-date alignment; off-by-one tests in both directions. | Alignment logic + tests | 0.5 |

$$\bigcup_g \text{pools}(g) = \text{deal pools}, \qquad \text{pools}(g_i) \cap \text{pools}(g_j) = \emptyset$$

A pool counted in two groups double-pays; a pool in none silently shrinks the collateral. Neither raises an error anywhere downstream, so both are asserted here.

### 2.4 Injection — 2 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.4.1 | Implement the chosen injection path; marshal aggregate arrays into vendor buffers, indexed by group. | `inject.cpp` | 0.75 |
| 2.4.2 | Round-trip verification: inject cashflows, read back the collateral cashflow the engine reports, assert equality. Catches convention mismatch immediately rather than at portfolio parity. | Round-trip test | 0.5 |
| 2.4.3 | Index rate callback wiring: register `index_fcn` and serve each path's curve from inside it via `icmo_set_index_forecast()`. | Curve wiring + tests | 0.25 |
| 2.4.4 | Error paths: bad group id, wrong vector length, unmodeled pool, unsupported deal. | Error tests | 0.5 |

### 2.5 `intex_result` — 1 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.5.1 | Extract tranche cashflows into the pricer's expected layout. | `result.cpp` | 0.5 |
| 2.5.2 | Class ↔ CUSIP mapping against the security master; handle both mismatch directions. | Mapping + tests | 0.25 |
| 2.5.3 | Tests: period totals reconcile, no dropped periods, correct first and last period. | Unit tests | 0.25 |

### 2.6 Convention fixtures — 1 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.6.1 | Hand-compute a 3-period, 2-class sequential deal; encode as a fixture with expected values to the cent. | Golden fixture | 0.25 |
| 2.6.2 | Fixtures per convention: default vs. scheduled ordering, buyout treatment, prepayment interest shortfall, 30/360 with delay days, Z accrual into the accretion-directed class, interest basis on injection (gross or net of servicing and g-fee). | Fixture suite | 0.5 |
| 2.6.3 | Reconcile against observed engine behavior; document divergences from PolyPaths. | Conventions memo | 0.25 |

The architect reviews all expected values. A wrong fixture is worse than no fixture — it certifies a defect.

### 2.7 First parity — 0.5 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 2.7.1 | One sequential agency CMO through both engines, one deterministic scenario. Diff, resolve to tolerance, document residuals. | Parity note | 0.5 |

**Q2 exit — the schedule gate:** one agency CMO, one deterministic scenario, matching PolyPaths within tolerance.

---

## 6. Q3 — Golden set parity — 10 w

### 3.1 Golden set — 1 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 3.1.1 | Select 20–30 deals from the 1.2.3 composition report, weighted toward what the book actually holds. Tier 1 covered at minimum twice per structural feature; higher tiers included per the capacity review. | Deal list + coverage matrix | 0.5 |
| 3.1.2 | Freeze deal files and factor date; snapshot inputs for reproducibility across the quarter. | Frozen input set | 0.5 |

### 3.2 Parity harness — 2.5 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 3.2.1 | Batch runner driving both engines from the frozen inputs. | `parity_run` tool | 1.0 |
| 3.2.2 | Diff at three levels: per-period per-tranche cashflow, summary statistics (WAL, total P&I), price. | Diff module | 0.75 |
| 3.2.3 | Report ranked by materiality rather than deal name. | Report generator | 0.25 |
| 3.2.4 | **Tolerance policy**: what tolerance at what level, and what constitutes an accepted engine difference versus a defect. Encoded as thresholds. | Policy doc + config | 0.5 |

### 3.3 Triage — 4 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 3.3.1 | First full run; classify exceptions into buckets by symptom. | Exception log | 0.5 |
| 3.3.2 | Bucket — timing and date alignment. | Fixes + regression tests | 1.0 |
| 3.3.3 | Bucket — convention differences. | Fixes + fixture additions | 1.0 |
| 3.3.4 | Bucket — structural features (Z, calls, PAC band behavior). | Fixes + tests | 1.0 |
| 3.3.5 | Disposition log: every exception carries a root cause and a decision. | Disposition log | 0.5 |

**Capacity review at the midpoint of 3.3.** Measure burn against the 4 w budget and decide which higher tiers join the golden set. This is the decision point for tiers 2–5, made on evidence rather than guessed in January.

### 3.4 Remediation and tier extension — 2.5 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 3.4.1 | Fix bridge and result defects surfaced in triage. | Code changes | 1.5 |
| 3.4.2 | Extend the golden set to the tiers approved at the capacity review; validate. | Extended coverage | 0.5 |
| 3.4.3 | Full regression run. | Regression report | 0.5 |

**Q3 exit:** golden set within tolerance; every exception dispositioned in writing; validated tier coverage recorded.

**3.2.4 must complete before 3.3.1 begins.** Parity triage without a written stopping rule is the most common way a single-developer effort overruns — there is always one more basis point to chase.

---

## 7. Q4 — Stabilization and phase 2 scoping — 8 w

| # | Sub-task | Artifact | w |
|---|---|---|---|
| 4.1.1 | Single-threaded throughput: deals/sec and paths/sec at production path counts. | Benchmark results | 0.5 |
| 4.1.2 | Memory per session with M deals open; extrapolate to candidate worker counts. | Memory model | 0.5 |
| 4.1.3 | Profile the split: boundary crossing vs. waterfall execution vs. aggregation. Determines whether phase 2 batching or GPU co-location is the higher-value investment. | Profile report | 0.5 |
| 4.2.1 | Interface review against phase 2 requirements; confirm nothing blocks concurrency, multi-group or floating-rate work. | Review note | 0.5 |
| 4.3.1 | Conventions document, assembled from the 2.6 fixtures and memo. | Conventions doc | 0.75 |
| 4.3.2 | Integration guide: how to call it, what is validated, what is unvalidated, what is rejected and why. | Integration guide | 0.75 |
| 4.3.3 | Known gaps register: every unvalidated tier, with what it would take to close. | Gaps register | 0.5 |
| 4.4.1 | Size non-agency credit waterfall from measured Q2/Q3 effort. | Sizing analysis | 0.75 |
| 4.4.2 | Size concurrency and GPU co-location from the 4.1 measurements. | Sizing analysis | 0.75 |
| 4.4.3 | Phase 2 proposal with resourcing ask. | Proposal | 0.25 |
| 4.5 | **Buffer** | — | 2.5 |

Of the buffer, 1.5 w is pre-committed to the rewrite branch of the 1.3.5 disposition. The remainder covers ramp-up overrun and hiring slippage.

---

## 8. Effort roll-up

| Quarter | Owner | Block | w | Total |
|---|---|---|---:|---:|
| Q1 | Dev A | 1.1 Ramp-up | 2.0 | |
| | | 1.2 Portfolio inventory | 1.0 | |
| | | 1.3 Prior work assessment | 2.0 | |
| | | 1.4 API spike and prototype | 3.5 | |
| | | 1.5 Interface draft | 0.5 | |
| | | 1.6 Handover package | 1.0 | **10.0** |
| Q2 | Dev B | 2.0 Onboarding | 1.0 | |
| | | 2.1 `intex_core` | 3.0 | |
| | | 2.2 Supporting infrastructure | 1.0 | |
| | | 2.3 Bridge aggregation | 2.5 | |
| | | 2.4 Injection | 2.0 | |
| | | 2.5 `intex_result` | 1.0 | |
| | | 2.6 Convention fixtures | 1.0 | |
| | | 2.7 First parity | 0.5 | **12.0** |
| Q3 | Dev B | 3.1 Golden set | 1.0 | |
| | | 3.2 Parity harness | 2.5 | |
| | | 3.3 Triage | 4.0 | |
| | | 3.4 Remediation and tier extension | 2.5 | **10.0** |
| Q4 | Dev B | 4.1–4.4 Stabilization and scoping | 5.5 | |
| | | 4.5 Buffer | 2.5 | **8.0** |
| | | **Total** | | **40.0** |

---

## 9. Architect SME dependency

The plan assumes roughly **half a day per week of architect time**, concentrated in Q1 and early Q2:

| Purpose | Window |
|---|---|
| Ramp-up direction; comprehension gate review | Q1 |
| Portfolio classification support (1.2.2) | Q1 |
| Prior-work disposition review (1.3.5) | Q1 |
| Handover package review — is it sufficient for a stranger? | Q1 |
| Interface sign-off (2.0.2) | Q2 |
| Convention fixture expected-value review (2.6) | Q2 |
| Tolerance policy sign-off (3.2.4) | Q3 |
| Parity triage escalation on domain questions | Q3 |

This is not optional overhead. Fixture expected-values require someone who can hand-compute a waterfall period, and the handover package needs review by someone who can tell whether it is complete.

---

## 10. Dependencies

| Predecessor | Successor | Why |
|---|---|---|
| 1.1 ramp-up | 1.4 spike | Cannot evaluate injection modes without understanding what crosses the boundary |
| 1.1.3 call-site inventory | 1.5 interface draft | Interface derives from actual pricer requirements |
| 1.2.3 composition report | 3.1.1 golden set, §3 tier order | Golden set should reflect the book |
| 1.3.5 disposition | 2.1 `intex_core` | Scope of 2.1 depends on adopt / refactor / rewrite |
| 1.4.1 injection mode | 2.3, 2.4 | Determines bridge design; a late answer means building it twice |
| 1.4.5 reconciliation | tier 2 commitment | Determines whether multi-group is a loop or a data project |
| 1.6 handover | 2.0 onboarding | Q2 start quality is bounded by Q1 write-up quality |
| 2.3.1 input contract | 2.3.2 aggregation | Units and sign conventions settled first |
| 2.6 fixtures | 3.3 triage | Convention questions answered cheaply before appearing as portfolio diffs |
| 3.2.4 tolerance policy | 3.3.1 first run | Triage needs a stopping rule before it starts |
| 4.1 measurements | 4.4 sizing | Phase 2 estimates measured, not guessed |

---

## 11. Milestones

| Date | Gate | Evidence |
|---|---|---|
| Mid-Feb | Ramp-up complete; portfolio composition known | Comprehension note; composition report |
| End Feb | Prior work dispositioned | Decision memo (1.3.5) |
| End Mar | Injection mode known; one deal runs end to end | Findings memos; prototype |
| **Mid-Apr** | **Developer B productive** | Independent reproduction of the prototype result |
| **End Jun** | **One deal matches PolyPaths** | Parity note (2.7.1) |
| End Sep | Golden set parity; validated tiers recorded | Disposition log (3.3.5) |
| End Dec | Runnable slice documented | Integration guide, gaps register, phase 2 proposal |

**The June gate is the schedule.** Everything before it is serial work with no parallelism available to recover a slip. If it moves past July, Q3 parity compresses and the year-end deliverable degrades to a wrapper without validated parity. Surface that in July, not December.

---

## 12. Risks

| # | Risk | Mitigation | Window |
|---|---|---|---|
| 1 | **Developer B hired late** | Recruit during 2026 with a Q1 start-date target; every week of slippage comes directly off Q2 and pushes the June gate | Now |
| 2 | Developer B lacks mortgage background | State the requirement in the job specification; if unavoidable, add 2–3 w ramp-up and move the June gate | Hiring |
| 3 | Handover insufficient; Q1 knowledge lost | 1.6 package is a deliverable; 2.0.1 independent reproduction is the test; architect reviews the package for completeness | Q1–Q2 |
| 4 | Prior work requires full rewrite | 1.3 checklist decides on merit, not sunk cost; 1.5 w of Q4 buffer pre-committed to this branch | Q1 |
| 5 | Sunk-cost bias toward adopting unsound code | Global state is a hard fail regardless of what else works | Q1 |
| 6 | Injection mode is vector-only | Resolve in 1.4.1; escalate in January rather than absorbing | Q1 |
| 7 | Pool→group reconciliation fails | Tier 2 drops from the committed set; the cost is upstream data work, not wrapper work, and does not return by spending more weeks here | Q1 |
| 8 | Portfolio contains material floater / inverse holdings | 1.2 surfaces this in January; tier 3 is 1.5 w and must be funded from the triage budget or the buffer | Q1 |
| 9 | Convention drift between engines | Fixtures (2.6) before batch parity, not after | Q2 |
| 10 | Parity triage has no stopping rule | Tolerance policy (3.2.4) gates 3.3.1 | Q3 |
| 11 | Bus factor of one throughout | Fixture suite and 4.3 documentation are deliverables, not overhead; architect time per §9 | All |

Risk 1 is the largest and sits outside the project's control. Q2 cannot start without Developer B, and no amount of Q1 preparation substitutes. Recruiting should begin well before January.

---

## 13. What end-2027 delivers

**Delivered.** A single-threaded C++ wrapper producing tranche cashflows for agency CMOs from existing model output, validated against PolyPaths on a golden set weighted to actual holdings — tier 1 at minimum, higher tiers per the Q3 capacity review — with conventions documented and an interface that admits concurrency and the remaining tiers without rework.

**Not delivered.** Non-agency credit waterfall; production throughput and concurrency; portfolio-wide coverage; PolyPaths decommission. Coverage of tiers 2–5 is reported at year end, not promised in January.

Both halves belong in the stakeholder message. The value of this year is a proven integration path and a phase 2 estimate grounded in measurement, produced by one developer-quarter of research and three developer-quarters of implementation.

**Phase 2 outlook**, uncommitted, sized here for planning at roughly **15–18 p-m**: non-agency credit waterfall and trigger-state channel; validation of remaining tiers; concurrency; GPU co-location; portfolio parity; production hardening.

---

## Appendix A — Why generality is nearly free

Three design choices in §2 look like scope expansion and are not. This appendix is the argument to make to the team, because the instinct to "keep it simple, do one group / fixed-rate only / one model pair" produces a *narrower* wrapper at the *same* cost, and a rewrite later.

The common structure of all three arguments:

> The wrapper's job is to marshal numbers across a boundary. Everything that varies — group count, collateral rate type, which behavioral model produced the numbers — either varies *upstream* of the boundary, or is already a parameter of the vendor call. Neither situation is affected by how many distinct cases exist.

C API names below are illustrative; substitute the real entry points once the Q1 spike confirms them.

This appendix assumes the **forced-cashflow injection** path (`icmo_override_coll_cf()`), where the wrapper hands over collateral dollars. The vendor explicitly discourages that function and recommends letting the subroutines generate collateral cashflows from prepayment/loss assumptions — the alternative path 1.4.1 must decide between. If the recommended assumption-vector path is chosen instead, the wrapper marshals assumptions rather than dollars, and "rate-agnostic" no longer follows automatically (floating-rate collateral then needs the index callback). The group and model-pair arguments in A.1 and A.3 hold either way.

---

### A.1 Single group vs. multi-group

**The claim:** supporting N groups costs one loop.

The vendor API is group-indexed already, but the numbering is 1-based: group 0 is the synthetic whole-deal aggregate, real groups are `1..icmo_n_collat_grps`, and a single-group deal reports `icmo_n_collat_grps == 0`. So the choice is not "add group support or not" — it is "hardcode the aggregate (group 0) or read the group list."

The narrow version:

```cpp
// Hardcoded to the aggregate (group 0). Correct only while the deal has one group.
void submit(Deal& deal, const CollateralCashflows& cf) {
    check(ix_set_group_cashflows(deal.get(), /*group=*/0,
                                 cf.principal.data(), cf.interest.data(),
                                 cf.size()));
}
```

The general version:

```cpp
// by_group[0] is the whole-deal aggregate; real groups follow at 1..N.
void submit(Deal& deal, std::span<const CollateralCashflows> by_group) {
    for (int g = 0; g < std::ssize(by_group); ++g) {
        const auto& cf = by_group[g];
        check(ix_set_group_cashflows(deal.get(), g,
                                     cf.principal.data(), cf.interest.data(),
                                     cf.size()));
    }
}
```

**The delta is a loop and a `span` instead of a reference.** Written this way from the first commit, multi-group support costs nothing — the aggregate slot is just index 0 and the real groups are `1..N`. (The vendor's forced-cashflow call also requires a separate call for the whole-deal aggregate, group 0, in a directed-cashflow deal.)

Written the first way, adding it later means changing the signature of `submit`, of everything that calls `submit`, of the aggregation layer that produces its argument, and of every test that constructs one. That is the 3-week retrofit — not because the loop is hard, but because a group index has to be threaded through call sites that were written assuming it did not exist.

**What genuinely changes upstream** is aggregation, and it is a `group_by` on a key already present in the structural query:

```cpp
std::vector<CollateralCashflows>
aggregate_by_group(std::span<const PoolProjection> pools,
                   const GroupMap& map,          // pool id -> group number, from pi_groupno / icmo_pool_is_in_group()
                   int n_groups, int n_periods)  // icmo_n_collat_grps (0 for single-group)
{
    std::vector<CollateralCashflows> out(n_groups + 1, CollateralCashflows{n_periods}); // [0]=aggregate
    for (const auto& p : pools) {
        auto& dst = out[map.group_of(p.pool_id)];   // 0 => aggregate, else 1..N
        auto& agg = out[0];
        for (int t = 0; t < n_periods; ++t) {
            dst.principal[t] += p.scheduled[t] + p.prepayment[t] + p.recovery[t];
            dst.interest[t]  += p.interest[t];
            dst.balance[t]   += p.balance[t];
            agg.principal[t] += p.scheduled[t] + p.prepayment[t] + p.recovery[t];
            agg.interest[t]  += p.interest[t];
            agg.balance[t]   += p.balance[t];
        }
    }
    return out;
}
```

For a single-group deal `icmo_n_collat_grps == 0`, so this loop runs with a single slot (index 0, the aggregate) and every pool maps to it. **The same code, unmodified, is the single-group implementation.** There is no simpler version to write.

Two invariants must be asserted regardless of N, so they are not extra work either:

```cpp
// A pool in two groups double-pays. A pool in no group silently
// shrinks the collateral. Neither raises an error downstream.
assert(all_pools_mapped(pools, map));
assert(no_pool_in_two_groups(map));
```

**What we do not implement:** cross-group cashflow rules, shared classes, group-level triggers. That logic is inside the vendor's deal model. We hand each group its dollars; the waterfall does the rest. Multi-group complexity in the *deal* is not multi-group complexity in the *wrapper*.

**Real cost of multi-group:** a few golden-set deals (~0.5 w validation), plus the Q1 reconciliation check that our pool→group mapping matches the deal's. If that reconciliation fails, the cost is in upstream data sourcing and no wrapper work recovers it.

---

### A.2 Fixed-rate vs. floating-rate collateral

**The claim:** under cashflow injection, the wrapper cannot tell the difference.

What crosses the boundary is dollars, not rates:

```cpp
struct CollateralPeriod {
    double begin_balance;
    double scheduled_principal;
    double prepayment;
    double default_amount;
    double recovery;
    double interest;          // dollars collected, not a coupon
};
```

There is no `wac`, no `margin`, no `index_id`, no `reset_date`, no `cap` in this struct — and therefore no place for rate-type-specific handling to live. The acceptance test is a grep:

```
$ grep -rn "arm\|hybrid\|reset\|is_floating" src/intex_bridge/
(no matches)
```

If that grep ever returns a hit, the boundary has been drawn in the wrong place.

**Where ARM complexity actually lives.** Resets, margins, periodic and lifetime caps, lookback conventions, recast schedules, payment shock, reset-driven refi incentive — all of it determines the *payment*, and the payment determines the *dollars*. That computation is the collateral engine's, upstream of us:

```cpp
// Upstream — the behavioral / collateral engine. Not our code.
//   fixed:  c_t = c_0
//   ARM:    c_t = clamp(index[t - lookback] + margin, c_{t-1} ± periodic, [floor, life])
// Either way, the output it hands us is the same struct.
std::vector<CollateralPeriod> project(const PoolRef&, const RatePath&);
```

The wrapper consumes `project()`'s output. It does not know, and must not know, which branch produced it.

**The one genuine residual: negative amortization.** Under the forced-cashflow call the caller may supply the balance vector (`vbalance`) directly; if it passes NULL instead, the engine derives future balances by reducing the opening balance by projected principal and losses — which cannot express a rising balance (option ARM, pay-option). So the wrapper must either supply `vbalance` for such collateral, or fail loudly. This is a single Q1 spike test and one guard, not a design axis:

```cpp
// If we don't supply vbalance, the engine derives future balances by subtracting
// principal — which cannot express a rising balance. Fail loudly rather than
// silently mis-allocating, until the Q1 spike confirms supplying vbalance works.
if (period.begin_balance > prev_balance && !supplying_balance_vector)
    throw unsupported_collateral{"negative amortization needs an explicit balance vector"};
```

**Do not confuse this with floating-rate *tranches*.** That is a separate axis and it is genuinely different: the vendor evaluates a floater's coupon internally from the block's formula — for a linear floater, `bi_flt_slope × index + bi_flt_const` (inverse floaters have negative slope), floored at `bi_flt_floor` and capped at `bi_flt_cap`, with an index lag `bi_flt_index_lag`; a complex formula is carried as the string `bi_flt_string`. Separately, the tranche's optimal pass-through rate is capped by the coupon cap / Available Funds Cap rate (`ICMOTV_couponcaprate`), and any shortfall is reported as coupon-cap shortfall (`ICMOTV_couponcapshort`):

$$c_{i,t} = \text{clamp}\big(s_i\,\text{idx}_{t-\ell_i} + b_i,\ \text{floor}_i,\ \text{cap}_i\big),\qquad \text{then capped by AFC}_t$$

so it needs the index path from us — the *same* path our prepayment model consumed for this Monte Carlo draw. Delivery is via a per-index callback: the engine invokes `index_fcn(icmop, ith_index)`, and the wrapper must call `icmo_set_index_forecast()` with an `ICMO_INDEX_FCAST` (`inf_vindex_rates`, `inf_nindex_rates`, `inf_index_fcast_method`). The wrapper holds one curve per index per path and serves it from inside that callback, which is why the scenario struct carries curves whether or not the current book holds floaters:

```cpp
struct PathInputs {
    std::vector<std::vector<CollateralPeriod>> by_group;
    std::unordered_map<IndexId, std::vector<double>> curves;  // empty for an all-fixed deal
};
```

An empty `curves` map is the fixed-rate case. Same code path, no branch. Adding floating-rate *classes* later costs validation effort (index-path alignment fixtures, coupon-cap / AFC-shortfall checks — ~1.5 w), not plumbing.

---

### A.3 One model pair vs. many

**The claim:** the wrapper depends on the output *format*, not on which model produced it. Supporting N model pairs therefore costs zero in the wrapper.

The entry point takes data, not a model:

```cpp
TrancheCashflows run(Deal& deal, const PathInputs& inputs);
```

`PathInputs` contains only numbers. There is no model handle, no model identifier, no policy template parameter. A wrapper written this way is model-agnostic by construction — not by effort.

Model selection belongs to the caller, one level up:

```cpp
// Caller's concern, not the wrapper's.
class ProjectorRegistry {
public:
    // Different pairs for agency vs. non-agency, fixed vs. ARM,
    // production vs. research, champion vs. challenger.
    const CollateralProjector& select(const PoolRef& pool) const;
};

PathInputs build_inputs(const Deal& deal,
                        std::span<const PoolRef> pools,
                        const RatePath& path,
                        const ProjectorRegistry& registry,
                        const GroupMap& map)
{
    std::vector<PoolProjection> projections;
    projections.reserve(pools.size());
    for (const auto& p : pools)
        projections.push_back(registry.select(p).project(p, path));   // <- the only variation

    return { aggregate_by_group(projections, map, ...), curves_for(path) };
}
```

Everything below `build_inputs` — aggregation, injection, execution, result extraction — is identical for every model pair. The variation is confined to one line.

**The condition that makes this work is the shared output contract.** As long as every projector returns `PoolProjection` with the same units, sign conventions and period alignment, the wrapper never learns how many implementations exist. Enforce it at compile time rather than by convention:

```cpp
template <class P>
concept CollateralProjectorLike = requires(const P& p, const PoolRef& r, const RatePath& path) {
    { p.project(r, path) } -> std::same_as<PoolProjection>;
};
```

**The anti-pattern to name explicitly in review.** Any of these means the boundary has leaked:

```cpp
run(deal, inputs, ModelId::CFPM);         // wrapper branching on model identity
template <class Model> class IntexRunner; // wrapper templated on the model
if (inputs.model_type == ModelType::Agency) { ... }   // in the bridge
```

Each of these converts "add a model" from a registry entry into a wrapper change, which is exactly the coupling that makes the second model pair expensive.

**Real cost of N model pairs:** each pair needs its own parity comparison against PolyPaths, because a difference could originate in the model or in the wrapper and only separate runs distinguish them. That is triage budget in Q3, and it is proportional to the number of pairs. The wrapper code is not.

---

### A.4 The cost asymmetry

| Axis | Build general now | Retrofit later | Real 2027 cost |
|---|---|---|---|
| Multi-group | loop + `span` — 0 w | signature change through bridge, callers, tests — ~1 w plus regression risk | ~0.5 w validation |
| Floating-rate collateral | nothing — the struct is already rate-agnostic | none needed | ~0.5 w validation, if held |
| Floating-rate classes | curves field, empty when unused — 0 w | scenario struct change through every call site | ~1.5 w validation |
| Multiple model pairs | registry lives in the caller — 0 w | de-coupling the wrapper from a model it learned about | triage, per pair |

The pattern is the same in every row: **generality is free at design time and expensive at retrofit time, while validation is the cost that is genuinely proportional to scope.** The 2027 plan therefore fixes the scope of *validation* and refuses to narrow the *code*.

### A.5 What is actually expensive, and why we are still deferring it

To keep the argument honest — these are not cases of free generality:

| Item | Why it costs real work |
|---|---|
| Non-agency credit waterfall | New data must cross the boundary: delinquent balance and cumulative loss as *stocks*, not flows. Under servicer advancing these are unrecoverable from the cashflow stream, so it is a new channel, new state, new fixtures. |
| Concurrency and GPU co-location | Session lifecycle, memory scaling across workers, work sharding, host/device pipelining. Deferred deliberately — throughput, not correctness. |
| Loan-level input granularity | Requires a loan→pool→group chain whose first arrow lives in the loan tape. A data-sourcing problem, not a code problem. |
| Every additional validated tier | Fixtures, golden-set entries, triage iterations. Proportional to scope by nature. |

The distinction to hold onto: **cheap when the variation is a parameter of an existing call or lives upstream of the boundary; expensive when it requires new information to cross the boundary.** Groups, rate type and model identity are the former. The credit waterfall is the latter.
