#include "set_search_core.hpp"
#include "sorted_set_type.hpp" // ← NEW
#include <algorithm>
#include <unordered_set>
#include <cstring>

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// Thin wrappers — reuse sorted_set primitives (no duplication)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<int32_t> SetSearchEngine::intersect(const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
	std::vector<int32_t> out(std::min(a.size(), b.size()));
	int32_t n = sorted_set::intersect(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size(), out.data());
	out.resize(n);
	return out;
}

std::vector<int32_t> SetSearchEngine::union_(const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
	std::vector<int32_t> out(a.size() + b.size());
	int32_t n = sorted_set::union_(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size(), out.data());
	out.resize(n);
	return out;
}

std::vector<int32_t> SetSearchEngine::subtract(const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
	std::vector<int32_t> out(a.size());
	int32_t n = sorted_set::subtract(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size(), out.data());
	out.resize(n);
	return out;
}

bool SetSearchEngine::is_subset(const std::vector<int32_t> &sub, const std::vector<int32_t> &sup) {
	return sorted_set::subset_of(sub.data(), (int32_t)sub.size(), sup.data(), (int32_t)sup.size());
}

bool SetSearchEngine::arrays_equal(const std::vector<int32_t> &a, const std::vector<int32_t> &b) {
	return sorted_set::equal(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size());
}

uint64_t SetSearchEngine::fingerprint(const std::vector<int32_t> &v) {
	uint64_t h = 0xcbf29ce484222325ULL;
	const uint8_t *p = reinterpret_cast<const uint8_t *>(v.data());
	size_t n = v.size() * 4;
	for (size_t i = 0; i < n; ++i) {
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main entry: dispatch to mode
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search(const std::vector<int32_t> &target,
                                                  const std::vector<CoverageEntry> &candidates, SearchMode mode,
                                                  int32_t max_depth, int32_t max_results) {
	if (target.empty() || candidates.empty())
		return {};

	Bloom256 target_bloom = Bloom256::from_pixels(target);

	// ── Filter candidates to those useful for this search ──────────────
	// AND:          keep candidates whose bloom overlaps target bloom.
	// OR:           keep candidates that are strict subsets of target.
	// AND_NOT/AUTO: keep ALL non-empty candidates.
	//
	// AND_NOT requires two classes of candidate:
	//   positive: supersets of target (overlap with target bloom v)
	//   negative: disjoint from target (overlap with target bloom x)
	// Applying the AND overlap filter here would silently drop every valid
	// negation candidate before search_and_not ever sees it. Both search_and
	// and search_and_not do their own internal bloom/exact filtering, so
	// passing extra candidates through is safe -- just a small extra cost.

	std::vector<CoverageEntry> useful;
	useful.reserve(candidates.size());

	if (mode == SearchMode::OR) {
		for (const auto &c : candidates) {
			if (!c.px.empty() && c.bloom.is_subset_of(target_bloom)) {
				// Bloom passes; verify actual subset
				if (is_subset(c.px, target))
					useful.push_back(c);
			}
		}
	} else if (mode == SearchMode::AND) {
		// Bloom overlap is a sound pre-filter for pure AND: a candidate that
		// does not overlap target in the bloom cannot be a superset of target.
		for (const auto &c : candidates) {
			if (!c.px.empty() && target_bloom.overlaps(c.bloom))
				useful.push_back(c);
		}
	} else {
		// AND_NOT / AUTO: pass all non-empty candidates through.
		for (const auto &c : candidates) {
			if (!c.px.empty())
				useful.push_back(c);
		}
	}

	if (useful.empty())
		return {};

	if (mode == SearchMode::AND) {
		return search_and(target, target_bloom, useful, max_depth, max_results);
	}
	if (mode == SearchMode::OR) {
		return search_or(target, target_bloom, useful, max_depth, max_results);
	}
	if (mode == SearchMode::AND_NOT) {
		auto r = search_and(target, target_bloom, useful, max_depth, max_results);
		if (!r.empty())
			return r;
		return search_and_not(target, target_bloom, useful, max_depth, max_results);
	}

	// AUTO mode
	{
		auto r = search_and(target, target_bloom, useful, max_depth, max_results);
		if (!r.empty())
			return r;

		r = search_and_not(target, target_bloom, useful, max_depth, max_results);
		if (!r.empty())
			return r;

		return search_or(target, target_bloom, useful, max_depth, max_results);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// AND search: BFS over sorted-array intersections
//
// State: current coverage (intersection so far)
// Transition: intersect with one more candidate
// Goal: current coverage == target
//
// Deduplication: fingerprint of current coverage array.
// This prevents revisiting the same intermediate set via different candidate
// orderings — equivalent to USING KEY in the SQL version.
//
// Bloom pre-filter: before computing intersection, check that the candidate
// bloom is a superset of the target bloom. This is necessary (but not
// sufficient) for the intersection to equal target.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search_and(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
                                                      const std::vector<CoverageEntry> &candidates, int32_t max_depth,
                                                      int32_t max_results) {
	std::vector<SearchResult> results;
	std::unordered_set<uint64_t> visited; // dedup by coverage fingerprint

	// BFS queue
	std::vector<BFSState> current_level, next_level;

	// Seed: candidates that are supersets of target (bloom check first)
	for (int32_t i = 0; i < (int32_t)candidates.size(); ++i) {
		const auto &c = candidates[i];
		if (!c.bloom.is_superset_of(target_bloom))
			continue;
		if (!is_subset(target, c.px))
			continue;

		uint64_t fp = fingerprint(c.px);
		if (visited.count(fp))
			continue;
		visited.insert(fp);

		if (arrays_equal(c.px, target)) {
			// Depth-1 exact match
			results.push_back({{c.id}, "AND", 1});
			if ((int32_t)results.size() >= max_results)
				return results;
			continue;
		}

		current_level.push_back({c.px, c.bloom, {c.id}, i, fp});
	}

	// BFS depth 2..max_depth
	for (int32_t depth = 2; depth <= max_depth && !current_level.empty(); ++depth) {
		next_level.clear();

		for (const auto &state : current_level) {
			// Only try candidates with higher index (prevents duplicates)
			for (int32_t j = state.last_idx + 1; j < (int32_t)candidates.size(); ++j) {
				const auto &c = candidates[j];

				// Bloom: intersection can only shrink coverage.
				// P ∩ Q ⊇ target requires both P ⊇ target and Q ⊇ target.
				if (!c.bloom.is_superset_of(target_bloom))
					continue;

				// Compute intersection
				auto inter = intersect(state.px, c.px);
				if (inter.size() < target.size())
					continue; // too small

				uint64_t fp = fingerprint(inter);
				if (visited.count(fp))
					continue;
				visited.insert(fp);

				auto new_ids = state.ids;
				new_ids.push_back(c.id);

				if (arrays_equal(inter, target)) {
					results.push_back({new_ids, "AND", depth});
					if ((int32_t)results.size() >= max_results)
						return results;
				} else if (depth < max_depth && inter.size() > target.size()) {
					// Still has excess — keep exploring
					Bloom256 inter_bloom = c.bloom & state.bloom;
					next_level.push_back({std::move(inter), inter_bloom, std::move(new_ids), j, fp});
				}
			}
		}
		current_level = std::move(next_level);
	}

	return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// AND_NOT search: find (P1 ∩ P2 ∩ ...) \ N = target
//
// Strategy:
//   1. Find all AND candidates that are supersets of target (but not exact).
//   2. For each such candidate C, compute excess = C \ target.
//   3. Find a negation candidate N that exactly covers excess.
//   4. Result: AND(C) AND NOT N.
//
// Note: the negation candidate is found by exact array comparison only —
// no bloom pre-filter on disjointness. The earlier bloom-based disjoint
// gate (`target_bloom.overlaps(neg.bloom)`) was removed because bloom
// overlaps() has false positives: it can return true for actually-disjoint
// sets, causing valid AND_NOT solutions to be silently skipped. The exact
// binary-search disjoint check below is authoritative.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search_and_not(const std::vector<int32_t> &target,
                                                          const Bloom256 &target_bloom,
                                                          const std::vector<CoverageEntry> &candidates,
                                                          int32_t max_depth, int32_t max_results) {
	std::vector<SearchResult> results;

	std::vector<std::pair<std::vector<int32_t>, std::vector<int64_t>>> supersets;

	for (int32_t i = 0; i < (int32_t)candidates.size(); ++i) {
		const auto &c = candidates[i];
		if (!c.bloom.is_superset_of(target_bloom))
			continue;
		if (!is_subset(target, c.px))
			continue;
		if (arrays_equal(c.px, target))
			continue;

		supersets.push_back({c.px, {c.id}});
	}

	// Deterministic order
	std::sort(supersets.begin(), supersets.end(),
	          [](const std::pair<std::vector<int32_t>, std::vector<int64_t>> &a,
	             const std::pair<std::vector<int32_t>, std::vector<int64_t>> &b) { return a.second[0] < b.second[0]; });

	for (size_t si = 0; si < supersets.size(); ++si) {
		if ((int32_t)results.size() >= max_results)
			break;

		const auto &sup_px = supersets[si].first;
		const auto &sup_ids = supersets[si].second;

		auto excess = subtract(sup_px, target);
		if (excess.empty())
			continue;

		Bloom256 excess_bloom = Bloom256::from_pixels(excess);

		for (const auto &neg : candidates) {
			if (!neg.bloom.is_superset_of(excess_bloom))
				continue;
			if (!arrays_equal(neg.px, excess))
				continue;

			bool disjoint = true;
			for (int32_t t : target) {
				if (std::binary_search(neg.px.begin(), neg.px.end(), t)) {
					disjoint = false;
					break;
				}
			}
			if (!disjoint)
				continue;

			auto ids = sup_ids;
			ids.push_back(-neg.id);

			results.push_back({ids, "AND_NOT", (int32_t)(sup_ids.size() + 1)});

			if ((int32_t)results.size() >= max_results) {
				return results;
			}
		}
	}

	return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// OR search: BFS over sorted-array unions
//
// State: current coverage (union so far)
// Transition: union with one more candidate
// Goal: current coverage == target
//
// Candidates must be SUBSETS of target (pre-filtered in search()).
// Deduplication via fingerprint of union array.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search_or(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
                                                     const std::vector<CoverageEntry> &candidates, int32_t max_depth,
                                                     int32_t max_results) {
	std::vector<SearchResult> results;
	std::unordered_set<uint64_t> visited;

	std::vector<BFSState> current_level, next_level;

	// Seed: each individual candidate (all are subsets of target)
	for (int32_t i = 0; i < (int32_t)candidates.size(); ++i) {
		const auto &c = candidates[i];

		uint64_t fp = fingerprint(c.px);
		if (visited.count(fp))
			continue;
		visited.insert(fp);

		if (arrays_equal(c.px, target)) {
			results.push_back({{c.id}, "OR", 1});
			if ((int32_t)results.size() >= max_results)
				return results;
			continue;
		}
		current_level.push_back({c.px, c.bloom, {c.id}, i, fp});
	}

	for (int32_t depth = 2; depth <= max_depth && !current_level.empty(); ++depth) {
		next_level.clear();

		for (const auto &state : current_level) {
			for (int32_t j = state.last_idx + 1; j < (int32_t)candidates.size(); ++j) {
				const auto &c = candidates[j];

				auto uni = union_(state.px, c.px);
				if (uni.size() > target.size())
					continue; // overshot

				uint64_t fp = fingerprint(uni);
				if (visited.count(fp))
					continue;
				visited.insert(fp);

				auto new_ids = state.ids;
				new_ids.push_back(c.id);

				if (arrays_equal(uni, target)) {
					results.push_back({new_ids, "OR", depth});
					if ((int32_t)results.size() >= max_results)
						return results;
				} else if (depth < max_depth) {
					Bloom256 uni_bloom = state.bloom | c.bloom;
					next_level.push_back({std::move(uni), uni_bloom, std::move(new_ids), j, fp});
				}
			}
		}
		current_level = std::move(next_level);
	}

	return results;
}

} // namespace duckdb
