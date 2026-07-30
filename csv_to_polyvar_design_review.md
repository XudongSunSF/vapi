# CSV → polyvar Reader — Design Review and Recommendation

**Subject:** `CsvToPolyvarBridge` (proposed, `cc/src/io/`), Wave 1 design doc
**Reference implementation reviewed against:** `wf::mortgage::utility::types::polyvar`
(`polyvar.h`, `types.h`, `storage.h`, `class_accessor.h`, `polyvar_util.h`,
`polyvar_macros.h`, `parser.h`, `json_parser.h`, `json_options.h`)

---

## Scope

**In scope:** the two proposed designs for reading behavioral-model parameter CSVs into
`polyvar`, evaluated as *designs* — contracts, typing, schema binding, and fit with the
existing polyvar/parser infrastructure.

**Out of scope:**

- `Curve1d` / `Curve2d`, axis policies, and the interpolation work. Reviewed separately.
- Line-level defects in the sample code. Both listings are illustrative; parsing
  mechanics (quoting, delimiter handling, numeric conversion, error line numbers) are
  not evaluated here and are not a basis for the recommendation below.

## Governing constraint

Most parameter files are JSON and load into `polyvar` through `json_parser`. A minority
are CSV. `polyvar` is retained as the intermediate representation specifically so that
tooling over loaded parameters — diffing, inspection, migration, conversion into the
calculator via `convert(polyvar&&, T*)` — is format-agnostic.

**This makes the canonical polyvar shape the contract, not the CSV reader's API.** A CSV
reader that produces a correct-looking tree which differs from the tree `json_parser`
produces for the same logical parameter set does not satisfy the requirement. Every
evaluation below is anchored to that.

---

# Part 1 — Review of the proposed options

## The requirement neither option addresses

Neither option references the shape `json_parser` produces. Both design a CSV→polyvar
mapping in isolation. Consequently neither can be shown to satisfy the format-agnostic
goal, and there is no stated criterion by which either could be accepted or rejected.

There are four axes on which two independently-designed readers silently diverge while
both look correct in isolation:

| Axis | JSON path today | Must be pinned for CSV |
|---|---|---|
| Numeric type | `json_interpreter_options` defaults: `type_int64` for integrals, `type_double` for floats | Which polyvar type each field lands in |
| Member order | Document order; polyvar objects are order-preserving `array_type<polyvar>`, so order is observable | Whether grouping may reorder, and whether comparison is order-sensitive |
| Absent / null / empty | JSON distinguishes missing key, explicit `null`, and `[]` | Which of the three a blank cell maps to |
| Top-level wrapping | `json_interpreter_options::unwrap_external_object` decides anonymous object vs named polyvar | Must match whichever the JSON path is configured for |

A mismatch on any one of these makes `operator==(const polyvar&, const polyvar&)` report
two semantically identical parameter sets as different. That is the failure mode the
format-agnostic design exists to prevent, and it is invisible until a diff tool is
pointed at a CSV-loaded tree and a JSON-loaded tree together.

---

## Option 1 — Generic passthrough reader

Read the stream with a general-purpose CSV library, emit one polyvar member per row
holding that row's cells, apply no interpretation.

### What it gets right

- **Delegated tokenization.** Using an established CSV library rather than hand-rolling
  the lexer is correct and should survive into the final design.
- **Format-independence at the reader.** The reader knows nothing about behavioral
  models. That instinct is right; it is applied at the wrong layer.
- **Minimal surface.** One class, no per-format subclassing.

### Why it fails as a design

**1. polyvar cannot recover numbers from strings.**

This is decisive and specific to this library. From `types.h`:

```cpp
template <typename T, polyvar_type Enum = select_enum_v<T>>
static constexpr bool is_castable =
    is_primitive_type<select_enum_v<T>> && is_primitive_type<Enum> &&
    (select_enum_v<T> != polyvar_type::type_string) && (Enum != polyvar_type::type_string);
```

`type_string` is excluded on both sides of the cast relation. A polyvar holding a
`std::string` therefore cannot be converted to `double` through `cast_as`, `value_or`, or
`member_value_or` — the runtime `is_castable_type(type_index())` guard rejects it, and
every `cast_as_impl<T, Enum>` overload falls through to the `!is_castable` specialization
that returns `false`. Downstream code must call `from_string` on every value by hand.

**2. The resulting tree is not comparable with a JSON-loaded tree.**

Everything is `type_string`; the JSON path produces `type_int64` / `type_double` /
arrays. `operator==` fails on every scalar. Diffing, round-tripping, and CSV→JSON
migration — the reasons polyvar is in the middle at all — do not work.

**3. Structure is discarded.**

Row-indexed members (`"0"`, `"1"`, `"2"`, …) are an array wearing an object's clothes.
polyvar has `as_object_array()` and `as_string_array()` for sequences; using
`type_object` with numeric string keys gives linear member lookup and a nonsensical
serialization. More importantly, the long-format key columns are never interpreted, so
nothing groups rows into curves and the helper layer has no tree to read.

**4. No schema hook.**

There is nowhere to say "this field's X axis is a `year_month`." The design forecloses
typing rather than deferring it.

**5. All error context is destroyed.**

Row and column position is known only inside the reader. Pushing conversion downstream
means every type error is reported without the location that would make it actionable.

### Verdict

Not viable as the parameter-loading path. The delegated-tokenization idea should be
carried forward; the untyped string-blob output should not.

---

## Option 2 — Format-aware bridge

Parse the known behavioral-model layout directly: a metadata header/value pair, then a
data header, then rows keyed by `prod` / `from` / `to` / `name` / `type` carrying `X`,
`Y`, `Z` values, grouped into a nested polyvar.

### What it gets right

- **Recognizes the file is long-format.** The core insight — that key columns must fold
  into tree structure rather than staying as columns — is correct and is the thing
  Option 1 misses entirely.
- **Groups before emitting.** Accumulating rows by key and writing complete series is the
  right shape for curve data.
- **Fails loudly on missing required columns.** Required-column resolution up front, with
  a named error, is the right posture for a config format.
- **Separates metadata from data conceptually.** `VERSION` / `FORMAT_VERSION` /
  `COMMENTS` are read as their own thing rather than treated as data rows.

### Why it fails as a design

**1. The reader/builder contract is self-contradictory.**

The class declares `ReadType = polyvar` and `read()` returns a single `polyvar`. The
sequence diagram, the usage snippet, and `ModelCalculatorBuilder::withParameter` all
specify `std::map<SubModelType, polyvar>`. These cannot both hold.

Worth noting when resolving it: since `prod` is already the outermost nesting level, the
polyvar *is* the map. `polyvar::begin()/end()` provides member iteration. A separate
`std::map` adds a string→enum parse and a subtree copy in exchange for enum-keyed
lookup — a defensible trade, but it must be chosen rather than left ambiguous.

**2. Effective dating is unresolved.**

The bridge produces five nesting levels
(`prod` → `from` → `to` → `name` → `type`). The helper layer takes two:

```cpp
Curve1d readCurve1d(polyvar&, std::string_view key);
c_.per_[subModel].Turbo = readCurve1d(pv, "rf_tb");
```

`from` and `to` — presumably effective-date bounds, so multiple time-versioned parameter
sets can coexist in one file — appear nowhere in the builder or the helpers. A file
carrying two effective ranges produces two sibling subtrees and nothing selects between
them. This is a semantic gap, not a signature mismatch: either the reader collapses to an
as-of date at load time (and must take that date), or the canonical tree carries all
ranges and the builder selects.

**3. Erasing every value to `double` contradicts the design's own goal.**

The Curve section states the intent plainly — breakpoints stored in their native type,
"no pre-encoding, no lossy conversion at load time," with `MonthlyCurve1d` holding
`vector<year_month>`. The bridge forces all X/Y/Z through numeric conversion into
`double`. `readMonthlyCurve1d` must then reconstruct `year_month` from a `double`, which
is exactly the round-trip the design says it eliminates.

polyvar already models the target type: `type_year_month_array` /
`as_year_month_array()` exist under `POLYVAR_CHRONO_SUPPORT`, backed by
`traits::array_type<std::chrono::year_month>` in `storage.h`. The capability is present
and unused.

**4. The schema is hardcoded into the reader.**

Column names, nesting order, and value typing are compiled into one class describing one
file layout. A second CSV parameter format requires a second class duplicating the same
grouping and metadata logic. The reader owns knowledge that belongs in data.

**5. Blank cells have no defined meaning.**

Long-format value columns are sparse by construction — a 1-D curve has no `Z`. The design
does not state whether a blank cell means absent, null, or empty, and the choice is
consequential: `class_accessor::convert` throws on a missing non-optional member but
accepts a present-but-empty one, and `member_value_or` will not fall back to its default
for an empty array. polyvar offers `type_opt_double_array` precisely for "present but
some elements unset"; the design uses neither that nor a stated convention.

**6. It sits outside the library's parser abstraction.**

`parser` defines `deserialize` / `serialize` / `stream_in` / `stream_out` plus a
`std::error_code`. `json_parser` implements it. A CSV reader is the natural second
implementation; deriving from a separate CRTP reader base instead means CSV and JSON
parameter sources are not interchangeable behind one type, which is the interchangeability
the format-agnostic goal requires.

**7. No allocator or options object.**

`json_parser` takes `json_options` carrying a `polymorphic_allocator<polyvar>` and a
monotonic buffer size, and `json_interpreter_options` carrying type-coercion settings.
The bridge exposes neither. In a library built end-to-end on pmr, and where the JSON path
already has both, the omission is a consistency break and forecloses tuning.

### Verdict

Right instincts about structure — long-format recognition and grouping are the hard part
and it gets them. Wrong binding of the schema, no canonical-shape contract, and unresolved
effective dating. The structural work is reusable; the class as scoped is not.

---

## Comparison

| | Option 1 | Option 2 | Required |
|---|---|---|---|
| Tokenization | Delegated to CSV library | Hand-rolled | Delegated |
| Output typing | All `type_string` | All `double` | Per-field, schema-driven |
| Long-format grouping | None | Yes, hardcoded | Yes, schema-declared |
| Comparable with JSON-loaded tree | No | Unspecified | Yes — acceptance criterion |
| Extensible to a second CSV format | Trivially (does nothing) | New class | New schema document |
| Implements `parser` | No | No | Yes |
| Allocator / options | No | No | Yes, mirroring `json_options` |
| Effective dating (`from`/`to`) | N/A | Emitted, never consumed | Resolved explicitly |

## What both options omit

- No schema concept — typing is either absent (1) or hardcoded (2).
- No relationship to the tree `json_parser` produces.
- No `parser` implementation, so no interchangeability with the JSON path.
- No `csv_options` / allocator, breaking the pmr consistency the rest of the library holds.
- No acceptance test that would detect divergence between the two load paths.

---

# Part 2 — Recommended solution

## Decision

**One `csv_parser` implementing the library's `parser` interface, taking its schema as a
runtime `polyvar` — the same mechanism `json_parser(json_options, const polyvar& schema)`
already uses.**

The parser owns tokenization, grouping, validation, and emission. The schema owns column
names, key columns, nesting order, per-field types, and optionality. One schema document
per parameter family, consumed by both readers.

### Why runtime schema rather than a template parameter

A compile-time schema (`template <CsvSchema S> class csv_parser`) is attractive: per-cell
dispatch collapses to a `std::index_sequence` fold resolving `select_enum_v<T>`
statically, the same pattern `polyvar_impl.h` already uses in
`cast_as(std::index_sequence<Enum...>)`. It buys dispatch speed and compile-time
diagnostics.

It loses on the constraint that actually governs here. `json_parser`'s schema is a runtime
polyvar. A compile-time CSV schema means two descriptions of one parameter set, maintained
separately, free to drift — and drift surfaces as a false diff in the tooling, not as a
compile error. The dispatch speed is irrelevant at config-file scale.

A compile-time schema becomes the right answer if the JSON path never adopts schemas, or
if a future format is hot-path enough to matter. In that case, keep the template and
generate the runtime polyvar from it (`static polyvar to_schema_polyvar()`) so there is
still one source of truth. Define `CsvSchema` as a concept either way — a readable
diagnostic beats a 400-line instantiation trace when a schema is malformed.

---

## Step 1 — Establish the canonical shape (do this first)

Load an existing JSON parameter file with `json_parser` and serialize the result. **That
tree is the contract.** Everything downstream is defined against it, including whether
`from`/`to` are representable at all.

Write it down as a short document — half a page — pinning:

1. **Where curve data lives.** Breakpoint and value array names and nesting.
2. **Per-field numeric type.** Must agree with `json_interpreter_options` coercion
   settings, or both paths must consume the same schema.
3. **Member ordering.** Whether the canonical tree has a defined order, and whether
   `operator==` on `type_object` is order-sensitive. *(Implemented in the .cpp — verify.
   If it is order-sensitive, both paths need canonical ordering, or the diff tool needs
   its own comparison.)*
4. **Blank / absent / null / empty.** One rule, applied by both readers.
5. **Top-level wrapping.** Match `unwrap_external_object` as configured for the JSON path.
6. **Metadata subtree.** A reserved node — `metadata` — carrying `version`,
   `formatVersion`, `comments` identically regardless of source format, so provenance
   tooling does not branch on origin. Reserved names should not sit as siblings of data
   groups.
7. **Effective dating.** Whether `from`/`to` are collapsed at load or carried in the tree.

This document is what the CSV reader is reviewed against, and it is the deliverable that
makes the format-agnostic claim checkable rather than aspirational.

---

## Step 2 — Component design

### `csv_options` — mirrors `json_options`

```cpp
class csv_options
{
public:
    csv_options& polyvar_allocator(std::pmr::polymorphic_allocator<polyvar>);
    std::pmr::polymorphic_allocator<polyvar> get_polyvar_allocator() const;

    csv_options& parse_buffer_size(size_t);          // monotonic resource for parse scratch
    size_t       get_parse_buffer_size() const;

    csv_options& delimiter(char);                    // explicit; sniffing is opt-in, not default
    csv_options& required_format_version(std::string);
};
```

### `csv_parser` — implements `parser`

```cpp
class csv_parser : public parser
{
public:
    csv_parser(csv_options, const polyvar& schema);
    explicit csv_parser(const polyvar& schema);      // default options

    // parser interface
    polyvar       deserialize(std::string_view) const override;
    std::istream& stream_in(std::istream&, polyvar&) const override;
    std::string   serialize(const polyvar&) const override;   // NotImplementedException
    std::ostream& stream_out(std::ostream&, const polyvar&) const override;

private:
    csv_options          _options;
    const polyvar* const _schema;
};
```

CSV is read-only for this use case; `serialize` throwing `NotImplementedException` is the
honest contract, and it keeps the type usable behind `std::unique_ptr<parser>` at the
config boundary. If bidirectional CSV is ever wanted, it slots in without an API change.

### Division of responsibility

| Concern | Owner |
|---|---|
| Tokenization (quoting, delimiters, encoding) | `csv_parser`, delegated to a CSV library |
| Which columns are grouping keys, and nesting order | Schema |
| Per-field target polyvar type and optionality | Schema |
| Grouping algorithm | `csv_parser` |
| Metadata block location and names | Canonical shape (shared) |
| Structural validation (monotonicity, grid completeness) | `csv_parser`, driven by schema-declared field kind |
| Error reporting with row/column context | `csv_parser` |

The critical line: the schema *declares* structure, the parser *implements* it. If the
schema carried the grouping algorithm, every schema would re-implement folding.

---

## Step 3 — Schema document sketch

Expressed as a polyvar (authored as JSON, loaded once, passed to either parser):

```json
{
  "formatVersion": "1",
  "layout": "long_grouped",
  "groupKeys": ["prod", "from", "to", "name"],
  "metadata": {
    "block": "leading_header_value_pair",
    "columns": { "VERSION": "version", "FORMAT_VERSION": "formatVersion", "COMMENTS": "comments" }
  },
  "fields": {
    "rf_scurve":     { "kind": "curve1d",        "x": "double",     "y": "double" },
    "rf_tb":         { "kind": "curve1d",        "x": "double",     "y": "double" },
    "co_period_mul": { "kind": "curve1d",        "x": "year_month", "y": "double" },
    "rf_lnsz_age":   { "kind": "curve2d",        "x": "year_month", "y": "double", "z": "double" },
    "elbow_state":   { "kind": "enum_weighted",  "key": "State",    "value": "double" }
  }
}
```

`kind` drives both the target polyvar types (`type_year_month_array` vs
`type_double_array`) and the structural validation applied. Adding a parameter family is a
schema edit, not a new class.

---

## Step 4 — Load-time validation

The reader must guarantee the invariants downstream consumers assume, because they are
cheap here and undiagnosable later:

- **`FORMAT_VERSION` accepted.** Reject unknown versions explicitly rather than parsing
  optimistically.
- **Breakpoint monotonicity.** `Curve1d::operator()` calls
  `std::lower_bound(xs.begin(), xs.end(), x)`, which requires sorted `xs`. Either sort at
  load or reject unsorted input — but decide, and enforce it in the reader.
- **2-D grid completeness.** The design specifies `vals` as a row-major M×N grid. Long
  format supplies (x, y, z) triples, so the reader must derive unique sorted X (M) and
  Y (N) breakpoints and verify M×N cells are present. Without this, `readCurve2d` has no
  contract to un-flatten against.
- **Value-column consistency within a group.** Sparse value columns must not silently
  misalign parallel arrays. Either require a constant present/absent pattern per group, or
  emit `type_opt_double_array` and let optionality be explicit.
- **Typed exceptions.** `ArgumentException` / `InvalidTypeException` rather than a generic
  throw, so callers can distinguish "file malformed" from "value out of range". `parser`
  also carries `std::error_code _error` for non-throwing paths.

---

## Step 5 — Acceptance criteria

The format-agnostic claim is testable, cheaply:

```cpp
// Same logical parameter set expressed in both formats
auto fromCsv  = csv_parser(copts, schema).deserialize(csvText);
auto fromJson = json_parser(jopts, schema).deserialize(jsonText);
ASSERT_TRUE(fromCsv == fromJson);                  // library operator==

// Round-trip through the canonical format
auto text  = json_parser(jopts).serialize(fromCsv);
auto again = json_parser(jopts, schema).deserialize(text);
ASSERT_TRUE(fromCsv == again);
```

If both hold, CSV is genuinely just an input encoding: diffing, inspection, migration, and
`convert(polyvar&&, T*)` into the calculator behave identically regardless of origin. If
either fails, the divergence surfaces in CI rather than in a diff report.

Add one CSV→JSON conversion utility over the same reader. It makes the migration path for
the remaining CSV files explicit, and if it ever runs clean across all of them, the reader
becomes a compatibility shim rather than permanent infrastructure.

---

## Step 6 — Sequencing

1. **Dump the JSON tree.** Load an existing parameter file with `json_parser`, serialize,
   inspect. Unblocks everything else.
2. **Write the canonical shape document.** Pin the seven items in Step 1. Circulate before
   any code.
3. **Resolve `from`/`to`.** Collapse-at-load or carry-in-tree. Driven by whether the JSON
   files have the concept at all; if they don't, it is a conversation with the CSV format
   owner, not a coding decision.
4. **Author the schema for the first parameter family.**
5. **Implement `csv_parser`** against the canonical shape, with `csv_options` and load-time
   validation.
6. **Gate on the conformance test** from Step 5.
7. **Rewrite the helper layer once**, against the canonical shape, with no knowledge of
   origin format. Doing this before the shape is settled is wasted work — which is why the
   current `readCurve1d` / `readMonthlyCurve1d` signatures should not be corrected in
   place.

---

## Feedback to send back on the current design

Three questions, framed as blockers to resolve rather than defects to fix:

1. `read()` returns `polyvar`; `withParameter()` takes `std::map<SubModelType, polyvar>`.
   Which is intended? Note that `prod` is already the outer nesting level, so the polyvar
   is the map.
2. The helpers take a two-level key but the bridge emits five levels. Where do `from` and
   `to` get resolved — at load, or in the builder?
3. Forcing all values to `double` contradicts the doc's own "no pre-encoding, no lossy
   conversion at load time" for `MonthlyCurve1d`. polyvar has `type_year_month_array`.

Plus the architectural one: the design never references the shape `json_parser` produces,
so it cannot be shown to satisfy the format-agnostic requirement that motivates keeping
polyvar in the middle.

---

## Open items requiring source not reviewed here

- **Is `operator==(const polyvar&, const polyvar&)` order-sensitive for `type_object`?**
  Determines whether canonical member ordering is required in both readers. Implemented in
  the .cpp.
- **Does `polyvar`'s move constructor re-parent grandchildren?** Members live in
  `array_type<polyvar>`, which reallocates on growth; `fully_qualified_name()` walks
  `_parent`. Relevant to any code holding references across member insertion.
- **What does `json_interpreter_options` resolve to on the current JSON load path?**
  Fixes the numeric types the CSV schema must match.
