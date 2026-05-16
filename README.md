# sorted_set

A DuckDB extension providing O(n+m) merge-based set operations on sorted integer arrays, plus a BFS-powered combinatorial search for boolean combinations of precomputed sets.

---

## Overview

DuckDB's built-in `list_intersect`, `list_union`, and related functions use O(n×m) algorithms designed for unsorted lists. When your arrays are sorted and deduplicated — coverage maps, pixel ID sets, document term lists, permission bitsets — this extension provides the same operations in O(n+m) by exploiting the sorted invariant.

Storage is identical to DuckDB's standard `INTEGER[]` type, so sorted sets interoperate freely with all existing list functions and tooling.

The extension also includes `set_search`, a BFS engine that finds minimal boolean combinations (AND, OR, AND_NOT) of precomputed sets whose result equals a target set exactly.

---

## The contract

**All operations assume sorted, deduplicated input.** Call `sorted_set()` once at write time; every subsequent operation is O(n+m) with no sorting overhead.

```sql
-- Write once: O(n log n) to sort and dedup
INSERT INTO coverage SELECT id, sorted_set(list(pixel_id)) FROM raw;

-- Read many: all operations are O(n+m)
SELECT set_intersect(a.px, b.px) FROM coverage a, coverage b ...
```

Passing unsorted arrays to any function other than `sorted_set()` produces incorrect results. Use `set_is_valid()` to validate data during development.

---

## Installation

```bash
git clone --recurse-submodules https://github.com/jaripekkatalgraf/duckdb-sorted-set.git
cd duckdb-sorted-set
GEN=ninja make
```

```sql
LOAD 'build/release/extension/sorted_set/sorted_set.duckdb_extension';
```

---

## Functions

### Construction

**`sorted_set(arr INTEGER[]) → INTEGER[]`**
Sort and deduplicate any integer array. O(n log n). This is the only function that accepts unsorted input.

```sql
SELECT sorted_set([3, 1, 2, 1, 3]::INTEGER[]);  -- [1, 2, 3]
SELECT sorted_set([]);                            -- []
```

**`set_from_range(lo INTEGER, hi INTEGER) → INTEGER[]`**
Construct {lo, lo+1, ..., hi}. Returns empty if lo > hi.

```sql
SELECT set_from_range(1, 5);  -- [1, 2, 3, 4, 5]
SELECT set_from_range(5, 3);  -- []
```

**`set_is_valid(arr INTEGER[]) → BOOLEAN`**
True if the array is sorted and has no duplicates. Use this to validate data during development, not to enforce it at runtime.

```sql
SELECT set_is_valid([1, 2, 3]::INTEGER[]);  -- true
SELECT set_is_valid([3, 1, 2]::INTEGER[]);  -- false
SELECT set_is_valid([1, 1, 2]::INTEGER[]);  -- false
```

---

### Predicates

All return NULL if either input is NULL.

**`set_contains(s INTEGER[], x INTEGER) → BOOLEAN`** — binary search, O(log n)

**`set_equal(a INTEGER[], b INTEGER[]) → BOOLEAN`** — memcmp after size check, O(n)

**`set_subset_of(a INTEGER[], b INTEGER[]) → BOOLEAN`** — a ⊆ b, merge walk O(n+m)

**`set_superset_of(a INTEGER[], b INTEGER[]) → BOOLEAN`** — a ⊇ b, equivalent to `set_subset_of(b, a)`

**`set_disjoint(a INTEGER[], b INTEGER[]) → BOOLEAN`** — no shared elements, O(n+m)

```sql
SELECT set_contains([1,3,5,7]::INTEGER[], 3);               -- true
SELECT set_subset_of([1,2]::INTEGER[], [1,2,3]::INTEGER[]); -- true
SELECT set_disjoint([1,2]::INTEGER[], [3,4]::INTEGER[]);    -- true
```

---

### Set operations

All return a new sorted set. Both inputs must be valid sorted sets.

**`set_intersect(a, b) → INTEGER[]`** — elements in both. O(n+m)

**`set_union(a, b) → INTEGER[]`** — elements in either. O(n+m)

**`set_subtract(a, b) → INTEGER[]`** — a \ b. O(n+m)

**`set_symmetric_diff(a, b) → INTEGER[]`** — elements in exactly one. O(n+m)

```sql
SELECT set_intersect([1,2,3,4]::INTEGER[], [2,4,6]::INTEGER[]);   -- [2, 4]
SELECT set_union([1,3,5]::INTEGER[], [2,4,6]::INTEGER[]);          -- [1, 2, 3, 4, 5, 6]
SELECT set_subtract([1,2,3,4]::INTEGER[], [2,4]::INTEGER[]);       -- [1, 3]
SELECT set_symmetric_diff([1,2,3]::INTEGER[], [2,3,4]::INTEGER[]); -- [1, 4]
```

---

### Aggregates

Inputs must be valid sorted sets. Pass unsorted data through `sorted_set()` first.

**`set_union_agg(px INTEGER[]) → INTEGER[]`**
Union of all sets in a group. NULL rows are skipped; empty group returns NULL.

```sql
SELECT category, set_union_agg(px) AS total_coverage
FROM feature_coverage
GROUP BY category;
```

**`set_intersect_agg(px INTEGER[]) → INTEGER[]`**
Intersection of all sets in a group. NULL rows are skipped; empty group returns NULL.
The first non-null row seeds the state — there is no identity element for intersection.

```sql
SELECT group_id, set_intersect_agg(px) AS common_pixels
FROM experiment_features
GROUP BY group_id;
```

---

### Scalar accessors

**`set_size(s) → INTEGER`** — element count

**`set_min(s) → INTEGER`** — minimum element, O(1). NULL for empty set.

**`set_max(s) → INTEGER`** — maximum element, O(1). NULL for empty set.

**`set_intersect_size(a, b) → INTEGER`** — intersection size without materialising. O(n+m). Prefer this in WHERE/HAVING over `cardinality(set_intersect(a, b))`.

**`set_rank(s, x) → INTEGER`** — zero-based position of x. Returns -1 if absent. O(log n).

**`set_at(s, rank) → INTEGER`** — element at zero-based rank. NULL if out of bounds. O(1).

---

### Mutation

Both return a new set. O(n) due to shifting; for bulk changes prefer `sorted_set(array_append(...))`.

**`set_add(s, x) → INTEGER[]`** — insert x in sorted position. No-op if present.

**`set_remove(s, x) → INTEGER[]`** — remove x. No-op if absent.

```sql
SELECT set_add([1,3,5]::INTEGER[], 4);      -- [1, 3, 4, 5]
SELECT set_remove([1,2,3,4]::INTEGER[], 3); -- [1, 2, 4]
```

---

### set_search — combinatorial set search

```sql
set_search(
    target      INTEGER[],          -- the set to match exactly
    ids         BIGINT[],           -- parallel array of candidate IDs
    coverages   INTEGER[][],        -- parallel array of coverage arrays
    blooms      UBIGINT[][4] = NULL,-- optional: precomputed bloom fingerprints
    mode        VARCHAR = 'AUTO',   -- AND | OR | AND_NOT | AUTO
    max_depth   INTEGER = 5,        -- max candidates to combine
    max_results INTEGER = 10        -- stop after this many solutions
) → TABLE(
    positive_ids BIGINT[],  -- AND/OR terms, or positive side of AND_NOT
    negative_ids BIGINT[],  -- NOT terms for AND_NOT; empty for AND/OR
    op           VARCHAR,   -- 'AND' | 'OR' | 'AND_NOT'
    depth        INTEGER    -- total candidates combined
)
```

Finds minimal boolean combinations of candidates whose coverage equals `target` exactly.

**Modes:**
- `AND` — intersection of candidates = target
- `OR` — union of candidates = target
- `AND_NOT` — tries AND first; falls back to (superset) minus (excess) = target
- `AUTO` — tries AND → AND_NOT → OR, returns first success

**AND_NOT result encoding:**
`positive_ids` contains the AND terms; `negative_ids` contains the NOT terms. Both are plain BIGINT values — no sign encoding. For AND and OR results `negative_ids` is always an empty list.

```sql
-- AND: intersection of candidates 10 and 20 equals target
SELECT positive_ids, negative_ids, op, depth
FROM set_search(
    [2, 3]::INTEGER[],
    [10, 20]::BIGINT[],
    [[1, 2, 3], [2, 3, 4]]::INTEGER[][],
    mode := 'AND'
);
-- positive_ids=[10,20], negative_ids=[], op=AND, depth=2

-- OR: union of candidates 10 and 20 equals target
SELECT positive_ids, negative_ids, op, depth
FROM set_search(
    [1, 2, 3, 4]::INTEGER[],
    [10, 20, 30]::BIGINT[],
    [[1, 2], [3, 4], [99]]::INTEGER[][],
    mode := 'OR'
);
-- positive_ids=[10,20], negative_ids=[], op=OR, depth=2

-- AND_NOT: candidate 10 minus candidate 20 equals target
SELECT positive_ids, negative_ids, op, depth
FROM set_search(
    [1, 2, 3]::INTEGER[],
    [10, 20]::BIGINT[],
    [[1, 2, 3, 4], [4]]::INTEGER[][],
    mode := 'AND_NOT'
);
-- positive_ids=[10], negative_ids=[20], op=AND_NOT, depth=2
```

**Important:** DuckDB table functions only accept literals as parameters — subqueries and column references are not supported. To search against data stored in a table, fetch the arrays client-side and pass them as literals, or use the BFS CTE pattern with `set_intersect` (see Patterns below).

---

### bloom_of — precompute bloom fingerprints

```sql
bloom_of(pixels INTEGER[]) → UBIGINT[4]
```

Computes a 256-bit bloom fingerprint for a sorted integer array. Use this to populate a `bloom` column at insert time so repeated searches don't recompute fingerprints.

```sql
CREATE TABLE feature_coverage (
    feat_id BIGINT PRIMARY KEY,
    px      INTEGER[],
    bloom   UBIGINT[4]
);

INSERT INTO feature_coverage VALUES
    (1, [0,1,4,5],   bloom_of([0,1,4,5]::INTEGER[])),
    (2, [0,1,4,5,9], bloom_of([0,1,4,5,9]::INTEGER[])),
    (3, [6,7,8],     bloom_of([6,7,8]::INTEGER[]));
```

The bloom hash is deterministic but not stable across extension versions. Recompute after upgrading.

---

## Patterns

### Superset queries

Find rows whose coverage contains all target elements.

```sql
SELECT feat_id FROM feature_coverage
WHERE set_subset_of([1,2,3]::INTEGER[], px);  -- O(n+m) per row
```

Compared to `WHERE list_has_all(px, [1,2,3])` which is O(n×m).

### Intersection size in HAVING

Avoid materialising the intersection when you only need the count.

```sql
SELECT feat_id, set_intersect_size(px, [1,2,3,4]::INTEGER[]) AS overlap
FROM feature_coverage
WHERE set_intersect_size(px, [1,2,3,4]::INTEGER[]) >= 3
ORDER BY overlap DESC;
```

### BFS through recursive CTEs

`set_intersect` produces a value that can be carried through recursive CTEs. The same intermediate intersection reached via different paths has the same sorted representation, enabling deduplication with `UNION`. This is the pure-SQL alternative to `set_search` for depth ≤ 2.

```sql
WITH RECURSIVE
seed AS (
    SELECT feat_id, px FROM feature_coverage
    WHERE set_subset_of($target, px)
),
bfs(feat_ids, coverage, depth) AS (
    SELECT [feat_id], px, 1 FROM seed
    UNION                                       -- deduplicates by coverage value
    SELECT bfs.feat_ids || [seed.feat_id],
           set_intersect(bfs.coverage, seed.px),
           bfs.depth + 1
    FROM bfs JOIN seed ON seed.feat_id > bfs.feat_ids[-1]
    WHERE bfs.depth < 5
      AND set_size(bfs.coverage) > set_size($target)
      AND set_subset_of($target, seed.px)
)
SELECT feat_ids, depth FROM bfs
WHERE set_equal(coverage, $target);
```

Use `set_search` instead when: you need AUTO mode, depth > 2 on large candidate sets, OR/AND_NOT search, or early termination via `max_results`.

### Interpreting AND_NOT results

```sql
WITH results AS (
    SELECT positive_ids, negative_ids, op, depth
    FROM set_search(
        [1, 2, 3]::INTEGER[],
        [10, 20]::BIGINT[],
        [[1, 2, 3, 4], [4]]::INTEGER[][],
        mode := 'AND_NOT'
    )
)
SELECT
    positive_ids AS and_terms,
    negative_ids AS not_terms,
    op,
    depth
FROM results;
```

### Residual computation

Compute which pixels remain uncovered after promoting some rows.

```sql
SELECT set_subtract(
    (SELECT set_union_agg(px) FROM target_pixels),
    (SELECT set_union_agg(px) FROM coverage WHERE promoted = true)
) AS residual;
```

### Common core of a group

Find pixels present in every instance of a slot.

```sql
SELECT slot_id, set_intersect_agg(instance_pixels) AS invariant_core
FROM instances
GROUP BY slot_id;
```

---

## Performance

| Operation | This extension | DuckDB built-in |
|---|---|---|
| Intersection | O(n+m) merge | O(n×m) hash |
| Union | O(n+m) merge | O(n×m) hash |
| Subset check | O(n+m) merge walk | O(n×m) nested loop |
| Intersection size | O(n+m), no allocation | O(n×m) + allocate |
| Min / Max | O(1) | O(n) scan |
| Sort+dedup | O(n log n) once at write | O(n log n) per query |

The O(n+m) bound is optimal — you must read every element of both inputs. Practical speedup is 10–50× for typical set sizes of 50–500 elements.

**set_search performance:**
- Bloom pre-filtering (256-bit, four independent 64-bit words) eliminates most candidates before any array operation.
- BFS deduplicates intermediate states by 64-bit FNV-1a fingerprint — the same intermediate set reached via different orderings is visited only once.
- `max_results := 1` enables early termination; useful when you only need the first/shallowest solution.
- Depth 1–2 covers the majority of practical cases. Depth 5+ on thousands of candidates may be slow.

**Known limitation:** AND_NOT search finds depth-1 positive candidates only — it finds `A \ N = target` but not `(A ∩ B) \ N = target`. Use the BFS CTE pattern for deeper AND_NOT combinations.

---

## Running the tests

```bash
make test
```

---

## License

MIT