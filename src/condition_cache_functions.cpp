#include "condition_cache_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

// ============================================================================
// condition_cache_build(table_name VARCHAR, predicate VARCHAR)
// ============================================================================

struct ConditionCacheBuildBindData : public TableFunctionData {
	string table_name;
	string predicate_sql;
};

struct ConditionCacheBuildState : public GlobalTableFunctionState {
	bool done = false;
	string status;
	idx_t total_qualifying_rows = 0;
	idx_t total_row_groups = 0;
	idx_t vectors_with_matches = 0;
	idx_t total_vectors = 0;
};

static unique_ptr<FunctionData> ConditionCacheBuildBind(ClientContext &context, TableFunctionBindInput &input,
                                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ConditionCacheBuildBindData>();
	result->table_name = input.inputs[0].GetValue<string>();
	result->predicate_sql = input.inputs[1].GetValue<string>();

	names.reserve(6);
	return_types.reserve(6);

	names.emplace_back("status");
	return_types.emplace_back(LogicalType {LogicalTypeId::VARCHAR});
	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType {LogicalTypeId::VARCHAR});
	names.emplace_back("predicate");
	return_types.emplace_back(LogicalType {LogicalTypeId::VARCHAR});
	names.emplace_back("qualifying_rows");
	return_types.emplace_back(LogicalType {LogicalTypeId::BIGINT});
	names.emplace_back("vectors_with_matches");
	return_types.emplace_back(LogicalType {LogicalTypeId::BIGINT});
	names.emplace_back("total_vectors");
	return_types.emplace_back(LogicalType {LogicalTypeId::BIGINT});

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ConditionCacheBuildInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	return make_uniq<ConditionCacheBuildState>();
}

static void ConditionCacheBuildExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<ConditionCacheBuildState>();
	if (gstate.done) {
		return;
	}
	gstate.done = true;

	auto &bind_data = data_p.bind_data->Cast<ConditionCacheBuildBindData>();

	// Set up thread-local capture for the optimizer to record the filter key
	tl_build_capture.active = true;
	tl_build_capture.target_table = bind_data.table_name;
	tl_build_capture.captured_filter_key.clear();

	// Build the internal query
	string query = StringUtil::Format("SELECT rowid FROM %s WHERE %s", bind_data.table_name, bind_data.predicate_sql);

	// Run on a new connection to avoid nested query issues
	auto &db = DatabaseInstance::GetDatabase(context);
	Connection con(db);
	auto result = con.Query(query);

	// Reset capture state
	tl_build_capture.active = false;

	if (result->HasError()) {
		throw InvalidInputException("condition_cache_build: internal query failed: %s", result->GetError());
	}

	// Build the bitvectors from row IDs
	auto entry = make_shared_ptr<ConditionCacheEntry>();
	entry->table_name = bind_data.table_name;
	entry->total_qualifying_rows = 0;

	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}

		auto &rowid_vec = chunk->data[0];
		UnifiedVectorFormat vdata;
		rowid_vec.ToUnifiedFormat(chunk->size(), vdata);
		auto rowids = UnifiedVectorFormat::GetData<int64_t>(vdata);

		for (idx_t i = 0; i < chunk->size(); i++) {
			auto idx = vdata.sel->get_index(i);
			if (!vdata.validity.RowIsValid(idx)) {
				continue;
			}
			int64_t row_id = rowids[idx];
			idx_t rg_idx = NumericCast<idx_t>(row_id) / CONDITION_CACHE_ROW_GROUP_SIZE;
			idx_t vec_idx = (NumericCast<idx_t>(row_id) % CONDITION_CACHE_ROW_GROUP_SIZE) / CONDITION_CACHE_VECTOR_SIZE;

			entry->bitvectors[rg_idx].SetVector(vec_idx);
			entry->total_qualifying_rows++;
		}
	}

	// Use the captured filter key if available, otherwise fall back to normalized predicate
	if (!tl_build_capture.captured_filter_key.empty()) {
		entry->filter_key = tl_build_capture.captured_filter_key;
	} else {
		// Fallback: use normalized predicate string
		string trimmed = bind_data.predicate_sql;
		StringUtil::Trim(trimmed);
		entry->filter_key = bind_data.table_name + "|" + StringUtil::Lower(trimmed);
	}

	// Compute statistics
	idx_t total_row_groups = entry->bitvectors.size();
	idx_t vectors_with_matches = 0;
	idx_t total_vectors = total_row_groups * VECTORS_PER_ROW_GROUP;
	for (auto &pair : entry->bitvectors) {
		vectors_with_matches += pair.second.CountSetBits();
	}

	// Store in cache
	auto store = ConditionCacheStore::GetOrCreate(context);
	store->Insert(entry);

	// Output summary
	output.SetCardinality(1);
	output.data[0].SetValue(0, Value("OK"));
	output.data[1].SetValue(0, Value(bind_data.table_name));
	output.data[2].SetValue(0, Value(bind_data.predicate_sql));
	output.data[3].SetValue(0, Value::BIGINT(NumericCast<int64_t>(entry->total_qualifying_rows)));
	output.data[4].SetValue(0, Value::BIGINT(NumericCast<int64_t>(vectors_with_matches)));
	output.data[5].SetValue(0, Value::BIGINT(NumericCast<int64_t>(total_vectors)));
}

TableFunction ConditionCacheBuildFunction() {
	TableFunction func("condition_cache_build", {LogicalType {LogicalTypeId::VARCHAR}, LogicalType {LogicalTypeId::VARCHAR}},
	                   ConditionCacheBuildExecute, ConditionCacheBuildBind, ConditionCacheBuildInit);
	return func;
}

// ============================================================================
// condition_cache_clear()
// ============================================================================

struct ConditionCacheClearState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> ConditionCacheClearBind(ClientContext &context, TableFunctionBindInput &input,
                                                        vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("status");
	return_types.emplace_back(LogicalType {LogicalTypeId::VARCHAR});
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> ConditionCacheClearInit(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	return make_uniq<ConditionCacheClearState>();
}

static void ConditionCacheClearExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<ConditionCacheClearState>();
	if (gstate.done) {
		return;
	}
	gstate.done = true;

	auto store = ConditionCacheStore::GetOrCreate(context);
	store->Clear();

	output.SetCardinality(1);
	output.data[0].SetValue(0, Value("Cache cleared"));
}

TableFunction ConditionCacheClearFunction() {
	TableFunction func("condition_cache_clear", {}, ConditionCacheClearExecute, ConditionCacheClearBind,
	                   ConditionCacheClearInit);
	return func;
}

// ============================================================================
// condition_cache_info()
// ============================================================================

struct ConditionCacheInfoBindData : public TableFunctionData {};

struct ConditionCacheInfoState : public GlobalTableFunctionState {
	idx_t current_idx = 0;
	vector<shared_ptr<ConditionCacheEntry>> entries;
};

static unique_ptr<FunctionData> ConditionCacheInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	names.reserve(5);
	return_types.reserve(5);

	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType {LogicalTypeId::VARCHAR});
	names.emplace_back("filter_key");
	return_types.emplace_back(LogicalType {LogicalTypeId::VARCHAR});
	names.emplace_back("qualifying_rows");
	return_types.emplace_back(LogicalType {LogicalTypeId::BIGINT});
	names.emplace_back("row_groups");
	return_types.emplace_back(LogicalType {LogicalTypeId::BIGINT});
	names.emplace_back("vectors_with_matches");
	return_types.emplace_back(LogicalType {LogicalTypeId::BIGINT});

	return make_uniq<ConditionCacheInfoBindData>();
}

static unique_ptr<GlobalTableFunctionState> ConditionCacheInfoInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	auto gstate = make_uniq<ConditionCacheInfoState>();
	auto store = ConditionCacheStore::GetOrCreate(context);
	gstate->entries = store->GetAll();
	return std::move(gstate);
}

static void ConditionCacheInfoExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<ConditionCacheInfoState>();

	idx_t count = 0;
	while (gstate.current_idx < gstate.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = gstate.entries[gstate.current_idx];

		idx_t vectors_with_matches = 0;
		for (auto &pair : entry->bitvectors) {
			vectors_with_matches += pair.second.CountSetBits();
		}

		output.data[0].SetValue(count, Value(entry->table_name));
		output.data[1].SetValue(count, Value(entry->filter_key));
		output.data[2].SetValue(count, Value::BIGINT(NumericCast<int64_t>(entry->total_qualifying_rows)));
		output.data[3].SetValue(count, Value::BIGINT(NumericCast<int64_t>(entry->bitvectors.size())));
		output.data[4].SetValue(count, Value::BIGINT(NumericCast<int64_t>(vectors_with_matches)));

		gstate.current_idx++;
		count++;
	}
	output.SetCardinality(count);
}

TableFunction ConditionCacheInfoFunction() {
	TableFunction func("condition_cache_info", {}, ConditionCacheInfoExecute, ConditionCacheInfoBind,
	                   ConditionCacheInfoInit);
	return func;
}

} // namespace duckdb
