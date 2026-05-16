#define DUCKDB_EXTENSION_MAIN

#include "sorted_set_extension.hpp"
#include "sorted_set_type.hpp"
#include "set_search_core.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>

namespace duckdb {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<int32_t> read_int_list(const Value &v) {
	if (v.IsNull())
		return {};
	std::vector<int32_t> out;
	for (const auto &child : ListValue::GetChildren(v))
		if (!child.IsNull())
			out.push_back(child.GetValue<int32_t>());
	return out;
}

static Value write_int_list(const int32_t *data, int32_t n) {
	vector<Value> children;
	children.reserve(n);
	for (int32_t i = 0; i < n; ++i)
		children.emplace_back(Value::INTEGER(data[i]));
	return Value::LIST(LogicalType::INTEGER, std::move(children));
}

static Value write_int_list(const std::vector<int32_t> &v) {
	return write_int_list(v.data(), (int32_t)v.size());
}

static Value write_bigint_list(const std::vector<int64_t> &v) {
	vector<Value> children;
	children.reserve(v.size());
	for (int64_t id : v)
		children.emplace_back(Value::BIGINT(id));
	return Value::LIST(LogicalType::BIGINT, std::move(children));
}

using BinarySetOp = int32_t (*)(const int32_t *, int32_t, const int32_t *, int32_t, int32_t *);
using BinaryPred = bool (*)(const int32_t *, int32_t, const int32_t *, int32_t);

static void ExecuteBinarySetOp(DataChunk &args, ExpressionState &, Vector &result, BinarySetOp op) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		if (args.data[0].GetValue(i).IsNull() || args.data[1].GetValue(i).IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto a = read_int_list(args.data[0].GetValue(i));
		auto b = read_int_list(args.data[1].GetValue(i));
		std::vector<int32_t> out(a.size() + b.size());
		int32_t n = op(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size(), out.data());
		out.resize(n);
		result.SetValue(i, write_int_list(out));
	}
}

static void ExecuteBinaryPredicate(DataChunk &args, ExpressionState &, Vector &result, BinaryPred pred) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		if (args.data[0].GetValue(i).IsNull() || args.data[1].GetValue(i).IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto a = read_int_list(args.data[0].GetValue(i));
		auto b = read_int_list(args.data[1].GetValue(i));
		FlatVector::GetData<bool>(result)[i] = pred(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size());
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Scalar functions
// ─────────────────────────────────────────────────────────────────────────────

static void sorted_set_ctor(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto v = args.data[0].GetValue(i);
		if (v.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(v);
		if (!data.empty()) {
			int32_t new_n = sorted_set::normalise(data.data(), (int32_t)data.size(), data.data());
			data.resize(new_n);
		}
		result.SetValue(i, write_int_list(data));
	}
}

static void set_is_valid(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto v = args.data[0].GetValue(i);
		if (v.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(v);
		result.SetValue(i, Value::BOOLEAN(sorted_set::is_sorted_unique(data.data(), (int32_t)data.size())));
	}
}

static void set_contains(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto sv = args.data[0].GetValue(i);
		auto ev = args.data[1].GetValue(i);
		if (sv.IsNull() || ev.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(sv);
		result.SetValue(
		    i, Value::BOOLEAN(sorted_set::contains(data.data(), (int32_t)data.size(), ev.GetValue<int32_t>())));
	}
}

static void set_intersect_size(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto av = args.data[0].GetValue(i);
		auto bv = args.data[1].GetValue(i);
		if (av.IsNull() || bv.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto a = read_int_list(av);
		auto b = read_int_list(bv);
		result.SetValue(
		    i, Value::INTEGER(sorted_set::intersect_size(a.data(), (int32_t)a.size(), b.data(), (int32_t)b.size())));
	}
}

static void set_size(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto v = args.data[0].GetValue(i);
		if (v.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		result.SetValue(i, Value::INTEGER((int32_t)read_int_list(v).size()));
	}
}

static void set_min(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto v = args.data[0].GetValue(i);
		if (v.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(v);
		if (data.empty()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		result.SetValue(i, Value::INTEGER(data.front())); // O(1): array is sorted
	}
}

static void set_max(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto v = args.data[0].GetValue(i);
		if (v.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(v);
		if (data.empty()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		result.SetValue(i, Value::INTEGER(data.back())); // O(1): array is sorted
	}
}

static void set_rank(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto sv = args.data[0].GetValue(i);
		auto ev = args.data[1].GetValue(i);
		if (sv.IsNull() || ev.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(sv);
		int32_t elem = ev.GetValue<int32_t>();
		auto it = std::lower_bound(data.begin(), data.end(), elem);
		result.SetValue(i, Value::INTEGER((it == data.end() || *it != elem) ? -1 : (int32_t)(it - data.begin())));
	}
}

static void set_at(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto sv = args.data[0].GetValue(i);
		auto rv = args.data[1].GetValue(i);
		if (sv.IsNull() || rv.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(sv);
		int32_t rank = rv.GetValue<int32_t>();
		if (rank < 0 || rank >= (int32_t)data.size())
			FlatVector::SetNull(result, i, true);
		else
			result.SetValue(i, Value::INTEGER(data[rank]));
	}
}

static void set_from_range(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto lv = args.data[0].GetValue(i);
		auto hv = args.data[1].GetValue(i);
		if (lv.IsNull() || hv.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		int32_t lo = lv.GetValue<int32_t>(), hi = hv.GetValue<int32_t>();
		std::vector<int32_t> data;
		data.reserve(std::max(0, hi - lo + 1));
		for (int32_t v = lo; v <= hi; ++v)
			data.push_back(v);
		result.SetValue(i, write_int_list(data));
	}
}

static void set_add(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto sv = args.data[0].GetValue(i);
		auto ev = args.data[1].GetValue(i);
		if (sv.IsNull() || ev.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(sv);
		int32_t elem = ev.GetValue<int32_t>();
		auto it = std::lower_bound(data.begin(), data.end(), elem);
		if (it == data.end() || *it != elem)
			data.insert(it, elem);
		result.SetValue(i, write_int_list(data));
	}
}

static void set_remove(DataChunk &args, ExpressionState &, Vector &result) {
	idx_t count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; ++i) {
		auto sv = args.data[0].GetValue(i);
		auto ev = args.data[1].GetValue(i);
		if (sv.IsNull() || ev.IsNull()) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto data = read_int_list(sv);
		int32_t elem = ev.GetValue<int32_t>();
		auto it = std::lower_bound(data.begin(), data.end(), elem);
		if (it != data.end() && *it == elem)
			data.erase(it);
		result.SetValue(i, write_int_list(data));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// set_union_agg
// ─────────────────────────────────────────────────────────────────────────────

struct SetUnionAggState {
	std::vector<int32_t> *data = nullptr;
};

struct SetUnionAgg {
	static void InitWrapper(const AggregateFunction &, data_ptr_t state) {
		reinterpret_cast<SetUnionAggState *>(state)->data = nullptr;
	}

	static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state_vector, idx_t count) {
		auto states = FlatVector::GetData<SetUnionAggState *>(state_vector);
		UnifiedVectorFormat idata;
		inputs[0].ToUnifiedFormat(count, idata);

		for (idx_t i = 0; i < count; ++i) {
			// Use selection-vector index for both validity and value lookup.
			idx_t idx = idata.sel->get_index(i);
			if (!idata.validity.RowIsValid(idx))
				continue;

			auto incoming = read_int_list(inputs[0].GetValue(idx));
			auto &st = *states[i];

			if (!st.data) {
				st.data = new std::vector<int32_t>(std::move(incoming));
			} else {
				std::vector<int32_t> merged(st.data->size() + incoming.size());
				int32_t n = sorted_set::union_(st.data->data(), (int32_t)st.data->size(), incoming.data(),
				                               (int32_t)incoming.size(), merged.data());
				merged.resize(n);
				*st.data = std::move(merged);
			}
		}
	}

	static void Combine(Vector &source_vector, Vector &target_vector, AggregateInputData &, idx_t count) {
		auto sources = FlatVector::GetData<SetUnionAggState *>(source_vector);
		auto targets = FlatVector::GetData<SetUnionAggState *>(target_vector);
		for (idx_t i = 0; i < count; ++i) {
			auto &src = *sources[i];
			auto &tgt = *targets[i];
			if (!src.data)
				continue;
			if (!tgt.data) {
				tgt.data = src.data;
				src.data = nullptr;
				continue;
			}
			std::vector<int32_t> merged(tgt.data->size() + src.data->size());
			int32_t n = sorted_set::union_(tgt.data->data(), (int32_t)tgt.data->size(), src.data->data(),
			                               (int32_t)src.data->size(), merged.data());
			merged.resize(n);
			*tgt.data = std::move(merged);
		}
	}

	static void Finalize(Vector &state_vector, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
		auto states = FlatVector::GetData<SetUnionAggState *>(state_vector);
		for (idx_t i = 0; i < count; ++i) {
			auto &st = *states[i];
			if (!st.data)
				FlatVector::SetNull(result, i + offset, true);
			else
				result.SetValue(i + offset, write_int_list(*st.data));
		}
	}

	static void Destroy(Vector &state_vector, AggregateInputData &, idx_t count) {
		auto states = FlatVector::GetData<SetUnionAggState *>(state_vector);
		for (idx_t i = 0; i < count; ++i) {
			delete states[i]->data;
			states[i]->data = nullptr;
		}
	}

	static AggregateFunction GetFunction() {
		AggregateFunction func({LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::LIST(LogicalType::INTEGER),
		                       AggregateFunction::StateSize<SetUnionAggState>, InitWrapper, Update, Combine, Finalize,
		                       nullptr, nullptr, Destroy);
		func.name = "set_union_agg";
		return func;
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// set_intersect_agg
//
// state.data == nullptr means no rows seen yet (nullptr, not empty vector).
// First non-null row seeds the state; empty intersection short-circuits.
// ─────────────────────────────────────────────────────────────────────────────

struct SetIntersectAggState {
	std::vector<int32_t> *data = nullptr;
};

struct SetIntersectAgg {
	static void InitWrapper(const AggregateFunction &, data_ptr_t state) {
		reinterpret_cast<SetIntersectAggState *>(state)->data = nullptr;
	}

	static void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state_vector, idx_t count) {
		auto states = FlatVector::GetData<SetIntersectAggState *>(state_vector);
		UnifiedVectorFormat idata;
		inputs[0].ToUnifiedFormat(count, idata);

		for (idx_t i = 0; i < count; ++i) {
			// Use selection-vector index for both validity and value lookup.
			idx_t idx = idata.sel->get_index(i);
			if (!idata.validity.RowIsValid(idx))
				continue;

			auto &st = *states[i];
			if (st.data && st.data->empty())
				continue; // already empty, short-circuit

			auto incoming = read_int_list(inputs[0].GetValue(idx));

			if (!st.data) {
				st.data = new std::vector<int32_t>(std::move(incoming));
			} else {
				std::vector<int32_t> inter(std::min(st.data->size(), incoming.size()));
				int32_t n = sorted_set::intersect(st.data->data(), (int32_t)st.data->size(), incoming.data(),
				                                  (int32_t)incoming.size(), inter.data());
				inter.resize(n);
				*st.data = std::move(inter);
			}
		}
	}

	static void Combine(Vector &source_vector, Vector &target_vector, AggregateInputData &, idx_t count) {
		auto sources = FlatVector::GetData<SetIntersectAggState *>(source_vector);
		auto targets = FlatVector::GetData<SetIntersectAggState *>(target_vector);
		for (idx_t i = 0; i < count; ++i) {
			auto &src = *sources[i];
			auto &tgt = *targets[i];
			if (!src.data)
				continue;
			if (!tgt.data) {
				tgt.data = src.data;
				src.data = nullptr;
				continue;
			}
			std::vector<int32_t> inter(std::min(tgt.data->size(), src.data->size()));
			int32_t n = sorted_set::intersect(tgt.data->data(), (int32_t)tgt.data->size(), src.data->data(),
			                                  (int32_t)src.data->size(), inter.data());
			inter.resize(n);
			*tgt.data = std::move(inter);
		}
	}

	static void Finalize(Vector &state_vector, AggregateInputData &, Vector &result, idx_t count, idx_t offset) {
		auto states = FlatVector::GetData<SetIntersectAggState *>(state_vector);
		for (idx_t i = 0; i < count; ++i) {
			auto &st = *states[i];
			if (!st.data)
				FlatVector::SetNull(result, i + offset, true);
			else
				result.SetValue(i + offset, write_int_list(*st.data));
		}
	}

	static void Destroy(Vector &state_vector, AggregateInputData &, idx_t count) {
		auto states = FlatVector::GetData<SetIntersectAggState *>(state_vector);
		for (idx_t i = 0; i < count; ++i) {
			delete states[i]->data;
			states[i]->data = nullptr;
		}
	}

	static AggregateFunction GetFunction() {
		AggregateFunction func({LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::LIST(LogicalType::INTEGER),
		                       AggregateFunction::StateSize<SetIntersectAggState>, InitWrapper, Update, Combine,
		                       Finalize, nullptr, nullptr, Destroy);
		func.name = "set_intersect_agg";
		return func;
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// set_search table function
//
// Output schema:
//   positive_ids BIGINT[]  — AND / OR terms, or the AND side of AND_NOT
//   negative_ids BIGINT[]  — NOT terms for AND_NOT; empty list for AND / OR
//   op           VARCHAR   — "AND" | "OR" | "AND_NOT"
//   depth        INTEGER   — total candidates combined
//
// ─────────────────────────────────────────────────────────────────────────────

static SearchMode parse_mode(const std::string &s) {
	if (s == "AND")
		return SearchMode::AND;
	if (s == "OR")
		return SearchMode::OR;
	if (s == "AND_NOT")
		return SearchMode::AND_NOT;
	return SearchMode::AUTO;
}

struct SetSearchBindData : public TableFunctionData {
	std::vector<SearchResult> results;
	mutable idx_t offset = 0;
};

static unique_ptr<FunctionData> set_search_bind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {
	    LogicalType::LIST(LogicalType::BIGINT), // positive_ids
	    LogicalType::LIST(LogicalType::BIGINT), // negative_ids
	    LogicalType::VARCHAR,                   // op
	    LogicalType::INTEGER                    // depth
	};
	names = {"positive_ids", "negative_ids", "op", "depth"};

	auto result = make_uniq<SetSearchBindData>();

	const auto &target_val = input.inputs[0];
	if (target_val.IsNull())
		return result;

	// Parse and normalise target
	std::vector<int32_t> target = read_int_list(target_val);
	if (!target.empty()) {
		int32_t new_n = sorted_set::normalise(target.data(), (int32_t)target.size(), target.data());
		target.resize(new_n);
	}

	// Parse ids
	const auto &ids_val = input.inputs[1];
	std::vector<int64_t> ids;
	for (const auto &v : ListValue::GetChildren(ids_val))
		ids.push_back(v.GetValue<int64_t>());

	// Parse and normalise coverages
	const auto &covs_val = input.inputs[2];
	std::vector<CoverageEntry> entries;
	const auto &cov_list = ListValue::GetChildren(covs_val);
	for (idx_t i = 0; i < cov_list.size() && i < ids.size(); ++i) {
		std::vector<int32_t> px = read_int_list(cov_list[i]);
		if (!px.empty()) {
			int32_t new_n = sorted_set::normalise(px.data(), (int32_t)px.size(), px.data());
			px.resize(new_n);
		}
		Bloom256 bl = Bloom256::from_pixels(px);
		entries.emplace_back(ids[i], std::move(px), bl);
	}

	// FIX: parse all named parameters 
	// Named param: blooms (optional precomputed bloom override)
	auto blooms_it = input.named_parameters.find("blooms");
	if (blooms_it != input.named_parameters.end() && !blooms_it->second.IsNull()) {
		const auto &bloom_list = ListValue::GetChildren(blooms_it->second);
		for (idx_t i = 0; i < bloom_list.size() && i < entries.size(); ++i) {
			const auto &bv = ListValue::GetChildren(bloom_list[i]);
			if (bv.size() == 4) {
				entries[i].bloom = Bloom256(bv[0].GetValue<uint64_t>(), bv[1].GetValue<uint64_t>(),
				                            bv[2].GetValue<uint64_t>(), bv[3].GetValue<uint64_t>());
			}
		}
	}

	// Named param: mode
	std::string mode_str = "AUTO";
	auto mode_it = input.named_parameters.find("mode");
	if (mode_it != input.named_parameters.end() && !mode_it->second.IsNull())
		mode_str = mode_it->second.GetValue<string>();

	// Named param: max_depth
	int32_t max_depth = 5;
	auto depth_it = input.named_parameters.find("max_depth");
	if (depth_it != input.named_parameters.end() && !depth_it->second.IsNull())
		max_depth = depth_it->second.GetValue<int32_t>();

	// Named param: max_results
	int32_t max_results = 10;
	auto res_it = input.named_parameters.find("max_results");
	if (res_it != input.named_parameters.end() && !res_it->second.IsNull())
		max_results = res_it->second.GetValue<int32_t>();

	SetSearchEngine engine;
	result->results = engine.search(target, entries, parse_mode(mode_str), max_depth, max_results);

	return result;
}

static void set_search_function(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<SetSearchBindData>();
	if (data.offset >= data.results.size()) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = std::min((idx_t)(data.results.size() - data.offset), (idx_t)STANDARD_VECTOR_SIZE);
	output.SetCardinality(count);

	auto &pos_col = output.data[0]; // positive_ids BIGINT[]
	auto &neg_col = output.data[1]; // negative_ids BIGINT[]
	auto &op_col = output.data[2];  // op VARCHAR
	auto &dep_col = output.data[3]; // depth INTEGER

	for (idx_t i = 0; i < count; ++i) {
		const auto &r = data.results[data.offset + i];
		pos_col.SetValue(i, write_bigint_list(r.positive_ids));
		neg_col.SetValue(i, write_bigint_list(r.negative_ids));
		op_col.SetValue(i, Value(r.op));
		dep_col.SetValue(i, Value::INTEGER(r.depth));
	}

	data.offset += count;
}

// ─────────────────────────────────────────────────────────────────────────────
// bloom_of scalar function
// ─────────────────────────────────────────────────────────────────────────────

static void bloom_of_function(DataChunk &args, ExpressionState &, Vector &result) {
	auto &input = args.data[0];
	idx_t count = args.size();

	UnifiedVectorFormat input_format;
	input.ToUnifiedFormat(count, input_format);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	for (idx_t i = 0; i < count; ++i) {
		idx_t idx = input_format.sel->get_index(i);
		if (!input_format.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		std::vector<int32_t> pixels = read_int_list(input.GetValue(idx));
		auto bloom = Bloom256::from_pixels(pixels);
		vector<Value> bloom_vals = {Value::UBIGINT(bloom.w[0]), Value::UBIGINT(bloom.w[1]), Value::UBIGINT(bloom.w[2]),
		                            Value::UBIGINT(bloom.w[3])};
		result.SetValue(i, Value::LIST(LogicalType::UBIGINT, std::move(bloom_vals)));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Load
// ─────────────────────────────────────────────────────────────────────────────

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("sorted_set", {LogicalType::LIST(LogicalType::INTEGER)},
	                                       LogicalType::LIST(LogicalType::INTEGER), sorted_set_ctor));

	loader.RegisterFunction(ScalarFunction("set_from_range", {LogicalType::INTEGER, LogicalType::INTEGER},
	                                       LogicalType::LIST(LogicalType::INTEGER), set_from_range));

	loader.RegisterFunction(
	    ScalarFunction("set_is_valid", {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::BOOLEAN, set_is_valid));

	loader.RegisterFunction(ScalarFunction("set_contains",
	                                       {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
	                                       LogicalType::BOOLEAN, set_contains));

	loader.RegisterFunction(ScalarFunction(
	    "set_equal", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::BOOLEAN,
	    [](DataChunk &args, ExpressionState &s, Vector &r) { ExecuteBinaryPredicate(args, s, r, sorted_set::equal); }));

	loader.RegisterFunction(ScalarFunction(
	    "set_subset_of", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::BOOLEAN, [](DataChunk &args, ExpressionState &s, Vector &r) {
		    ExecuteBinaryPredicate(args, s, r, sorted_set::subset_of);
	    }));

	loader.RegisterFunction(ScalarFunction(
	    "set_superset_of", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::BOOLEAN, [](DataChunk &args, ExpressionState &s, Vector &r) {
		    ExecuteBinaryPredicate(args, s, r, sorted_set::superset_of);
	    }));

	loader.RegisterFunction(ScalarFunction(
	    "set_disjoint", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::BOOLEAN, [](DataChunk &args, ExpressionState &s, Vector &r) {
		    ExecuteBinaryPredicate(args, s, r, sorted_set::disjoint);
	    }));

	loader.RegisterFunction(ScalarFunction(
	    "set_intersect", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::LIST(LogicalType::INTEGER),
	    [](DataChunk &args, ExpressionState &s, Vector &r) { ExecuteBinarySetOp(args, s, r, sorted_set::intersect); }));

	loader.RegisterFunction(ScalarFunction(
	    "set_union", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::LIST(LogicalType::INTEGER),
	    [](DataChunk &args, ExpressionState &s, Vector &r) { ExecuteBinarySetOp(args, s, r, sorted_set::union_); }));

	loader.RegisterFunction(ScalarFunction(
	    "set_subtract", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::LIST(LogicalType::INTEGER),
	    [](DataChunk &args, ExpressionState &s, Vector &r) { ExecuteBinarySetOp(args, s, r, sorted_set::subtract); }));

	loader.RegisterFunction(ScalarFunction(
	    "set_symmetric_diff", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::LIST(LogicalType::INTEGER), [](DataChunk &args, ExpressionState &s, Vector &r) {
		    ExecuteBinarySetOp(args, s, r, sorted_set::symmetric_diff);
	    }));

	loader.RegisterFunction(
	    ScalarFunction("set_size", {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_size));
	loader.RegisterFunction(
	    ScalarFunction("set_min", {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_min));
	loader.RegisterFunction(
	    ScalarFunction("set_max", {LogicalType::LIST(LogicalType::INTEGER)}, LogicalType::INTEGER, set_max));
	loader.RegisterFunction(ScalarFunction(
	    "set_intersect_size", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::LIST(LogicalType::INTEGER)},
	    LogicalType::INTEGER, set_intersect_size));
	loader.RegisterFunction(ScalarFunction("set_rank", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
	                                       LogicalType::INTEGER, set_rank));
	loader.RegisterFunction(ScalarFunction("set_at", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
	                                       LogicalType::INTEGER, set_at));
	loader.RegisterFunction(ScalarFunction("set_add", {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
	                                       LogicalType::LIST(LogicalType::INTEGER), set_add));
	loader.RegisterFunction(ScalarFunction("set_remove",
	                                       {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER},
	                                       LogicalType::LIST(LogicalType::INTEGER), set_remove));

	loader.RegisterFunction(SetUnionAgg::GetFunction());
	loader.RegisterFunction(SetIntersectAgg::GetFunction());

	// ── set_search table function ─────────────────────────────────────────────
	TableFunction set_search_func("set_search",
	                              {
	                                  LogicalType::LIST(LogicalType::INTEGER),
	                                  LogicalType::LIST(LogicalType::BIGINT),
	                                  LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER)),
	                              },
	                              set_search_function, set_search_bind);
	set_search_func.named_parameters["blooms"] = LogicalType::LIST(LogicalType::LIST(LogicalType::UBIGINT));
	set_search_func.named_parameters["mode"] = LogicalType::VARCHAR;
	set_search_func.named_parameters["max_depth"] = LogicalType::INTEGER;
	set_search_func.named_parameters["max_results"] = LogicalType::INTEGER;
	loader.RegisterFunction(set_search_func);

	// ── bloom_of scalar function ──────────────────────────────────────────────
	loader.RegisterFunction(ScalarFunction("bloom_of", {LogicalType::LIST(LogicalType::INTEGER)},
	                                       LogicalType::LIST(LogicalType::UBIGINT), bloom_of_function));
}

void SortedSetExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string SortedSetExtension::Name() {
	return "sorted_set";
}
std::string SortedSetExtension::Version() const {
	return "v0.2.0";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(sorted_set, loader) {
	duckdb::LoadInternal(loader);
}
}
