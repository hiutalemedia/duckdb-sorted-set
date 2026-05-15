#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <functional>
#include <unordered_map>

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// Bloom filter — 256-bit fingerprint for fast subset pre-filtering.
//
// For AND mode:  candidate must be SUPERSET of target
//                bloom_candidate ⊇ bloom_target  →  target_bloom & ~cand_bloom == 0
//
// For OR mode:   candidate must be SUBSET of target
//                bloom_candidate ⊆ bloom_target  →  cand_bloom & ~target_bloom == 0
// ─────────────────────────────────────────────────────────────────────────────

struct Bloom256 {
	uint64_t w[4] = {0, 0, 0, 0};

	Bloom256() = default;
	Bloom256(uint64_t a, uint64_t b, uint64_t c, uint64_t d) : w {a, b, c, d} {
	}

	// Compute from a sorted pixel list using xxhash-style mixing per pixel ID.
	// The caller can optionally supply precomputed blooms from a salt table;
	// this path is used when no salt is provided (self-contained mode).
	static Bloom256 from_pixels(const std::vector<int32_t> &pixels);

	bool is_superset_of(const Bloom256 &sub) const {
		return ((sub.w[0] & ~w[0]) | (sub.w[1] & ~w[1]) | (sub.w[2] & ~w[2]) | (sub.w[3] & ~w[3])) == 0;
	}
	bool is_subset_of(const Bloom256 &sup) const {
		return sup.is_superset_of(*this);
	}
	bool overlaps(const Bloom256 &other) const {
		return (w[0] & other.w[0]) || (w[1] & other.w[1]) || (w[2] & other.w[2]) || (w[3] & other.w[3]);
	}
	Bloom256 operator|(const Bloom256 &o) const {
		return {w[0] | o.w[0], w[1] | o.w[1], w[2] | o.w[2], w[3] | o.w[3]};
	}
	Bloom256 operator&(const Bloom256 &o) const {
		return {w[0] & o.w[0], w[1] & o.w[1], w[2] & o.w[2], w[3] & o.w[3]};
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// CoverageEntry — one row in the candidate set.
// Pixels must be a SORTED list of non-negative integers.
// ─────────────────────────────────────────────────────────────────────────────

struct CoverageEntry {
	int64_t id;
	std::vector<int32_t> px; // sorted, unique
	Bloom256 bloom;

	CoverageEntry() = default;
	CoverageEntry(int64_t id_, std::vector<int32_t> px_, Bloom256 bl) : id(id_), px(std::move(px_)), bloom(bl) {
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// SearchResult — one found combination.
// ─────────────────────────────────────────────────────────────────────────────

struct SearchResult {
	std::vector<int64_t> ids; // IDs of the combined entries
	std::string op;           // "AND" | "AND_NOT" | "OR"
	int32_t depth;            // number of entries combined

	// Explicit constructor required for C++11 — the DuckDB build system
	// enforces -std=c++11 at link time regardless of CMakeLists settings,
	// so aggregate brace-init of structs containing std::string/std::vector
	// does not compile without it.
	SearchResult(std::vector<int64_t> ids_, std::string op_, int32_t depth_)
	    : ids(std::move(ids_)), op(std::move(op_)), depth(depth_) {
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// Search modes
// ─────────────────────────────────────────────────────────────────────────────

enum class SearchMode { AND, AND_NOT, OR, AUTO };

// ─────────────────────────────────────────────────────────────────────────────
// SetSearchEngine — the core BFS engine.
//
// Design decisions:
//   - Pure C++ with no DuckDB dependencies. Can be unit-tested independently.
//   - Coverage arrays must be sorted on input; sorted order maintained throughout.
//   - Deduplication via 64-bit fingerprint of the coverage array (FNV-1a on bytes).
//     Collision probability negligible for typical ARC problem sizes.
//   - Bloom pre-filter runs before every array operation.
//   - max_results controls early termination; default 10 for efficiency.
// ─────────────────────────────────────────────────────────────────────────────

class SetSearchEngine {
public:
	SetSearchEngine() = default;

	// Run search. Returns up to max_results combinations.
	std::vector<SearchResult> search(const std::vector<int32_t> &target, const std::vector<CoverageEntry> &candidates,
	                                 SearchMode mode = SearchMode::AUTO, int32_t max_depth = 5,
	                                 int32_t max_results = 10);

private:
	// AND: find combinations whose intersection = target
	std::vector<SearchResult> search_and(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
	                                     const std::vector<CoverageEntry> &candidates, int32_t max_depth,
	                                     int32_t max_results);

	// OR: find combinations whose union = target
	std::vector<SearchResult> search_or(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
	                                    const std::vector<CoverageEntry> &candidates, int32_t max_depth,
	                                    int32_t max_results);

	// AND_NOT: find (P1 ∩ P2 ∩ ...) \ N = target
	std::vector<SearchResult> search_and_not(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
	                                         const std::vector<CoverageEntry> &candidates, int32_t max_depth,
	                                         int32_t max_results);

	// ── Sorted array primitives ──────────────────────────────────────────
	static std::vector<int32_t> intersect(const std::vector<int32_t> &a, const std::vector<int32_t> &b);

	static std::vector<int32_t> union_(const std::vector<int32_t> &a, const std::vector<int32_t> &b);

	static std::vector<int32_t> subtract(const std::vector<int32_t> &a, const std::vector<int32_t> &b);

	static bool is_subset(const std::vector<int32_t> &sub, const std::vector<int32_t> &sup);

	static bool arrays_equal(const std::vector<int32_t> &a, const std::vector<int32_t> &b);

	// 64-bit fingerprint for dedup (FNV-1a over the integer bytes)
	static uint64_t fingerprint(const std::vector<int32_t> &v);

	// BFS state: (current_coverage, ids_used, last_candidate_index)
	struct BFSState {
		std::vector<int32_t> px;
		Bloom256 bloom;
		std::vector<int64_t> ids;
		int32_t last_idx; // prevents duplicate pairs
		uint64_t fp;      // fingerprint for dedup
	};
};

// ─────────────────────────────────────────────────────────────────────────────
// Bloom computation without salt table (self-contained mode)
// Uses two rounds of xxhash-style mixing to spread bits across 256 bits.
// ─────────────────────────────────────────────────────────────────────────────

inline Bloom256 Bloom256::from_pixels(const std::vector<int32_t> &pixels) {
	Bloom256 b;
	for (int32_t p : pixels) {
		// Mix the pixel ID into 4 different bit positions
		// Using different multipliers per word to decorrelate
		uint32_t h = static_cast<uint32_t>(p);
		h ^= h >> 16;
		h *= 0x45d9f3b;
		h ^= h >> 16;
		b.w[0] |= (UINT64_C(1) << (h & 63));

		h = static_cast<uint32_t>(p) * 0x9e3779b9;
		h ^= h >> 16;
		h *= 0x85ebca6b;
		h ^= h >> 16;
		b.w[1] |= (UINT64_C(1) << (h & 63));

		h = static_cast<uint32_t>(p) * 0xc4ceb9fe;
		h ^= h >> 16;
		h *= 0x94d049bb;
		h ^= h >> 16;
		b.w[2] |= (UINT64_C(1) << (h & 63));

		h = static_cast<uint32_t>(p) * 0x6c62272e;
		h ^= h >> 16;
		h *= 0xbf58476d;
		h ^= h >> 16;
		b.w[3] |= (UINT64_C(1) << (h & 63));
	}
	return b;
}

} // namespace duckdb
