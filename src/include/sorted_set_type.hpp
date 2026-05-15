#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// SortedSet — a sorted, deduplicated sequence of int32_t values.
//
// Storage is identical to DuckDB's INTEGER[] — a flat array of int32_t with a
// length prefix — so sorted sets and integer arrays share the same memory
// layout. The extension validates and enforces the sorted+unique invariant at
// construction time. All set operations exploit the sorted order for O(n+m)
// merge-based algorithms rather than O(n*m) nested loops.
//
// Operations:
//   Construction:   from_unsorted(data, n)   — sort + dedup in O(n log n)
//                   from_sorted(data, n)     — validate + dedup in O(n)
//   Predicates:     contains(x)             — binary search O(log n)
//                   subset_of(other)         — merge walk O(n+m)
//                   superset_of(other)       — merge walk O(n+m)
//                   equal(other)             — memcmp O(n)
//                   disjoint(other)          — merge walk O(n+m)
//   Operations:     intersect(a, b)          — merge O(n+m)
//                   union_(a, b)             — merge O(n+m)
//                   subtract(a, b)           — merge O(n+m)
//                   symmetric_diff(a, b)     — merge O(n+m)
//   Aggregates:     count, min, max, sum
// ─────────────────────────────────────────────────────────────────────────────

namespace sorted_set {

// ── Core operations on sorted unique int32_t spans ──────────────────────────
// All functions take (data, size) pairs — no allocation inside.
// Callers allocate output buffers; returned size is the number of elements
// written. Maximum output size is always known upfront (max(a,b) for union,
// min(a,b) for intersect, a for subtract).

inline bool contains(const int32_t *data, int32_t n, int32_t x) {
	int32_t lo = 0, hi = n - 1;
	while (lo <= hi) {
		int32_t mid = (lo + hi) >> 1;
		if (data[mid] == x)
			return true;
		else if (data[mid] < x)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

inline int32_t intersect(const int32_t *a, int32_t na, const int32_t *b, int32_t nb, int32_t *out) {
	int32_t i = 0, j = 0, k = 0;
	while (i < na && j < nb) {
		if (a[i] == b[j]) {
			out[k++] = a[i];
			++i;
			++j;
		} else if (a[i] < b[j]) {
			++i;
		} else {
			++j;
		}
	}
	return k;
}

inline int32_t union_(const int32_t *a, int32_t na, const int32_t *b, int32_t nb, int32_t *out) {
	int32_t i = 0, j = 0, k = 0;
	while (i < na && j < nb) {
		if (a[i] == b[j]) {
			out[k++] = a[i];
			++i;
			++j;
		} else if (a[i] < b[j]) {
			out[k++] = a[i];
			++i;
		} else {
			out[k++] = b[j];
			++j;
		}
	}
	while (i < na)
		out[k++] = a[i++];
	while (j < nb)
		out[k++] = b[j++];
	return k;
}

inline int32_t subtract(const int32_t *a, int32_t na, const int32_t *b, int32_t nb, int32_t *out) {
	int32_t i = 0, j = 0, k = 0;
	while (i < na && j < nb) {
		if (a[i] == b[j]) {
			++i;
			++j;
		} else if (a[i] < b[j]) {
			out[k++] = a[i];
			++i;
		} else {
			++j;
		}
	}
	while (i < na)
		out[k++] = a[i++];
	return k;
}

inline int32_t symmetric_diff(const int32_t *a, int32_t na, const int32_t *b, int32_t nb, int32_t *out) {
	int32_t i = 0, j = 0, k = 0;
	while (i < na && j < nb) {
		if (a[i] == b[j]) {
			++i;
			++j;
		} else if (a[i] < b[j]) {
			out[k++] = a[i];
			++i;
		} else {
			out[k++] = b[j];
			++j;
		}
	}
	while (i < na)
		out[k++] = a[i++];
	while (j < nb)
		out[k++] = b[j++];
	return k;
}

// subset_of: returns true if every element of a appears in b.
// O(na + nb) merge walk — no binary search overhead.
inline bool subset_of(const int32_t *a, int32_t na, const int32_t *b, int32_t nb) {
	if (na > nb)
		return false;
	int32_t j = 0;
	for (int32_t i = 0; i < na; ++i) {
		while (j < nb && b[j] < a[i])
			++j;
		if (j >= nb || b[j] != a[i])
			return false;
		++j;
	}
	return true;
}

inline bool superset_of(const int32_t *a, int32_t na, const int32_t *b, int32_t nb) {
	return subset_of(b, nb, a, na);
}

inline bool equal(const int32_t *a, int32_t na, const int32_t *b, int32_t nb) {
	return na == nb && memcmp(a, b, na * sizeof(int32_t)) == 0;
}

inline bool disjoint(const int32_t *a, int32_t na, const int32_t *b, int32_t nb) {
	int32_t i = 0, j = 0;
	while (i < na && j < nb) {
		if (a[i] == b[j])
			return false;
		else if (a[i] < b[j])
			++i;
		else
			++j;
	}
	return true;
}

// ── Normalisation ────────────────────────────────────────────────────────────

// Sort and deduplicate. Returns the number of unique elements written to out.
// out may alias data (in-place operation).
inline int32_t normalise(const int32_t *data, int32_t n, int32_t *out) {
	if (n <= 0)
		return 0;
	if (out != data)
		memcpy(out, data, n * sizeof(int32_t));
	std::sort(out, out + n);
	return (int32_t)(std::unique(out, out + n) - out);
}

// Validate that data is sorted and unique. Returns true if valid.
inline bool is_sorted_unique(const int32_t *data, int32_t n) {
	for (int32_t i = 1; i < n; ++i)
		if (data[i] <= data[i - 1])
			return false;
	return true;
}

// Intersection size without materialising the result — for HAVING clauses.
inline int32_t intersect_size(const int32_t *a, int32_t na, const int32_t *b, int32_t nb) {
	int32_t i = 0, j = 0, k = 0;
	while (i < na && j < nb) {
		if (a[i] == b[j]) {
			++k;
			++i;
			++j;
		} else if (a[i] < b[j]) {
			++i;
		} else {
			++j;
		}
	}
	return k;
}

} // namespace sorted_set
