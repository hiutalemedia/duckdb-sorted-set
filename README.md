# sorted_set

A DuckDB extension providing fast **O(n+m)** set operations on sorted integer arrays, plus powerful combinatorial search.

DuckDB's built-in `list_intersect`, `list_union`, etc. are general-purpose and use O(n×m) algorithms. When your integer arrays are sorted and deduplicated (e.g. coverage maps, document term lists, permission sets, feature IDs, pixel IDs, etc.), this extension delivers the same operations in linear time.

Storage is fully compatible with DuckDB's native `INTEGER[]` type — no custom types needed.

---

## The Contract

**All operations except `sorted_set()` assume sorted, deduplicated input.**

```sql
-- Write once
INSERT INTO coverage 
SELECT id, sorted_set(list(pixel_id)) FROM raw_data;

-- Query many times — all O(n+m)
SELECT set_intersect(a.px, b.px) FROM coverage a JOIN coverage b ...
```

Use `set_is_valid()` during development to verify data.

---

## Installation

```bash
git clone --recurse-submodules https://github.com/yourname/sorted_set.git
cd sorted_set
GEN=ninja make
```

```sql
LOAD 'build/release/extension/sorted_set/sorted_set.duckdb_extension';
```

---

## Core Functions

### Construction

**`sorted_set(arr INTEGER[]) → INTEGER[]`**  
Sort + deduplicate. O(n log n)

**`set_from_range(lo INTEGER, hi INTEGER) → INTEGER[]`**  
Create `[lo, lo+1, ..., hi]`

**`set_is_valid(arr INTEGER[]) → BOOLEAN`**  
Check if array is sorted and unique.

### Predicates

- `set_contains(s, x)` — O(log n) binary search
- `set_equal(a, b)` — O(n)
- `set_subset_of(a, b)` — O(n+m)
- `set_superset_of(a, b)`
- `set_disjoint(a, b)`

### Set Operations (all O(n+m))

- `set_intersect(a, b)`
- `set_union(a, b)`
- `set_subtract(a, b)`
- `set_symmetric_diff(a, b)`

### Scalar Accessors

- `set_size(s)`
- `set_min(s)` / `set_max(s)` — O(1)
- `set_intersect_size(a, b)` — count without materializing
- `set_rank(s, x)` — zero-based position (-1 if missing)
- `set_at(s, rank)`

### Mutation

- `set_add(s, x)`
- `set_remove(s, x)`

### Aggregates

- `set_union_agg(px)`
- `set_intersect_agg(px)`

---

## Advanced Search: `set_search`

**`set_search(target INTEGER[], ids BIGINT[], coverages INTEGER[][], ...)`**

Finds minimal combinations of candidate sets whose **intersection**, **union**, or **intersection-minus-one** exactly equals the target.

```sql
SELECT * FROM set_search(
    [1, 2, 3]::INTEGER[],           -- target
    [10, 20, 30]::BIGINT[],         -- candidate ids
    [[1,2,3,4], [2,3], [3,4]]::INTEGER[][],  -- coverages
    mode := 'AUTO'
);
```

**Returns columns:** `result_ids`, `op`, `depth`

**Supported modes:**
- `'AND'` — intersection of several sets = target
- `'OR'` — union of several sets = target
- `'AND_NOT'` — (intersection of positives) minus one negative = target
- `'AUTO'` (default) — tries AND → AND_NOT → OR

**Named parameters:**
- `blooms UBIGINT[][]` — optional precomputed bloom filters (see `bloom_of`)
- `max_depth INTEGER` — default 5
- `max_results INTEGER` — default 10

### `bloom_of(pixels INTEGER[]) → UBIGINT[4]`

256-bit bloom filter for fast pre-filtering.

---

## Performance

| Operation              | This extension     | DuckDB built-in     |
|------------------------|--------------------|---------------------|
| Intersection / Union   | O(n+m) merge       | O(n×m) hash         |
| Subset check           | O(n+m)             | O(n×m)              |
| Intersection size      | O(n+m), no alloc   | O(n×m) + alloc      |
| Min / Max              | O(1)               | O(n)                |
| Search (combinatorial) | Highly optimized   | Not available       |

Typical speedup: **10–50×** on medium-sized sets (50–1000 elements).

---

## Usage Patterns

### Superset queries
```sql
SELECT feat_id 
FROM features 
WHERE set_subset_of([1,2,3]::INTEGER[], px);
```

### Efficient overlap filtering
```sql
SELECT feat_id, set_intersect_size(px, target) AS overlap
FROM features 
WHERE set_intersect_size(px, target) >= 5;
```

### Combinatorial search
```sql
SELECT * FROM set_search(my_target, ids, coverages, mode := 'AND');
```

---

## Running tests

```bash
make test
```

## License

MIT