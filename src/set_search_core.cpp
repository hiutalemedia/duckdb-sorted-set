#include "set_search_core.hpp"
#include "sorted_set_type.hpp"
#include <algorithm>
#include <unordered_set>
#include <cstring>

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// Thin wrappers — delegate to sorted_set primitives (no duplication)
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

	// ── Pre-filter candidates ──────────────────────────────────────────
	// AND:          bloom overlap with target is a sound necessary condition.
	// OR:           must be a strict subset of target (exact check after bloom).
	// AND_NOT/AUTO: pass ALL non-empty candidates — negation candidates are
	//               disjoint from target and would be wrongly dropped by the
	//               AND overlap filter.
	std::vector<CoverageEntry> useful;
	useful.reserve(candidates.size());

	if (mode == SearchMode::OR) {
		for (const auto &c : candidates)
			if (!c.px.empty() && c.bloom.is_subset_of(target_bloom))
				if (is_subset(c.px, target))
					useful.push_back(c);
	} else if (mode == SearchMode::AND) {
		for (const auto &c : candidates)
			if (!c.px.empty() && target_bloom.overlaps(c.bloom))
				useful.push_back(c);
	} else {
		for (const auto &c : candidates)
			if (!c.px.empty())
				useful.push_back(c);
	}

	if (useful.empty())
		return {};

	if (mode == SearchMode::AND)
		return search_and(target, target_bloom, useful, max_depth, max_results);
	if (mode == SearchMode::OR)
		return search_or(target, target_bloom, useful, max_depth, max_results);
	if (mode == SearchMode::AND_NOT) {
		auto r = search_and(target, target_bloom, useful, max_depth, max_results);
		if (!r.empty())
			return r;
		return search_and_not(target, target_bloom, useful, max_depth, max_results);
	}
	// AUTO
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
// AND search
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search_and(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
                                                      const std::vector<CoverageEntry> &candidates, int32_t max_depth,
                                                      int32_t max_results) {
	std::vector<SearchResult> results;
	std::unordered_set<uint64_t> visited;
	std::vector<BFSState> current_level, next_level;

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
			results.push_back(SearchResult({c.id}, {}, "AND", 1));
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
				if (!c.bloom.is_superset_of(target_bloom))
					continue;

				auto inter = intersect(state.px, c.px);
				if (inter.size() < target.size())
					continue;

				uint64_t fp = fingerprint(inter);
				if (visited.count(fp))
					continue;
				visited.insert(fp);

				auto new_ids = state.ids;
				new_ids.push_back(c.id);

				if (arrays_equal(inter, target)) {
					results.push_back(SearchResult(new_ids, {}, "AND", depth));
					if ((int32_t)results.size() >= max_results)
						return results;
				} else if (depth < max_depth && inter.size() > target.size()) {
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
// AND_NOT search
//
// NOT terms are stored in negative_ids as plain positive BIGINT values,
// and the output schema has two separate list columns.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search_and_not(const std::vector<int32_t> &target,
                                                          const Bloom256 &target_bloom,
                                                          const std::vector<CoverageEntry> &candidates,
                                                          int32_t max_depth, int32_t max_results) {
	std::vector<SearchResult> results;

	// Collect supersets of target (but not exact matches — AND handles those)
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

	std::sort(supersets.begin(), supersets.end(),
	          [](const std::pair<std::vector<int32_t>, std::vector<int64_t>> &a,
	             const std::pair<std::vector<int32_t>, std::vector<int64_t>> &b) { return a.second[0] < b.second[0]; });

	for (size_t si = 0; si < supersets.size(); ++si) {
		if ((int32_t)results.size() >= max_results)
			break;

		const std::vector<int32_t> &sup_px = supersets[si].first;
		const std::vector<int64_t> &sup_ids = supersets[si].second;

		auto excess = subtract(sup_px, target);
		if (excess.empty())
			continue;

		Bloom256 excess_bloom = Bloom256::from_pixels(excess);

		for (const auto &neg : candidates) {
			if (!neg.bloom.is_superset_of(excess_bloom))
				continue;
			if (!arrays_equal(neg.px, excess))
				continue;

			// Verify neg is truly disjoint from target
			bool disjoint =
			    sorted_set::disjoint(neg.px.data(), (int32_t)neg.px.size(), target.data(), (int32_t)target.size());
			if (!disjoint)
				continue;

			// store neg.id in negative_ids as-is (plain positive value).
			std::vector<int64_t> neg_ids = {neg.id};

			results.push_back(SearchResult(sup_ids, neg_ids, "AND_NOT", (int32_t)(sup_ids.size() + neg_ids.size())));

			if ((int32_t)results.size() >= max_results)
				return results;
		}
	}
	return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// OR search
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> SetSearchEngine::search_or(const std::vector<int32_t> &target, const Bloom256 &target_bloom,
                                                     const std::vector<CoverageEntry> &candidates, int32_t max_depth,
                                                     int32_t max_results) {
	std::vector<SearchResult> results;
	std::unordered_set<uint64_t> visited;
	std::vector<BFSState> current_level, next_level;

	for (int32_t i = 0; i < (int32_t)candidates.size(); ++i) {
		const auto &c = candidates[i];

		uint64_t fp = fingerprint(c.px);
		if (visited.count(fp))
			continue;
		visited.insert(fp);

		if (arrays_equal(c.px, target)) {
			results.push_back(SearchResult({c.id}, {}, "OR", 1));
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
					continue;

				uint64_t fp = fingerprint(uni);
				if (visited.count(fp))
					continue;
				visited.insert(fp);

				auto new_ids = state.ids;
				new_ids.push_back(c.id);

				if (arrays_equal(uni, target)) {
					results.push_back(SearchResult(new_ids, {}, "OR", depth));
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
