#pragma once

#include "duckdb.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

// DuckDB storage/execution constants
static constexpr idx_t CONDITION_CACHE_ROW_GROUP_SIZE = 122880;
static constexpr idx_t CONDITION_CACHE_VECTOR_SIZE = 2048;
static constexpr idx_t VECTORS_PER_ROW_GROUP = 60; // 122880 / 2048

// Per row-group bitvector: bit[i] = 1 means vector i has at least one qualifying row
// 60 bits fits in one uint64_t
struct RowGroupBitvector {
	uint64_t mask = 0;

	bool VectorHasRows(idx_t vector_index) const {
		D_ASSERT(vector_index < VECTORS_PER_ROW_GROUP);
		return (mask >> vector_index) & 1ULL;
	}

	void SetVector(idx_t vector_index) {
		D_ASSERT(vector_index < VECTORS_PER_ROW_GROUP);
		mask |= (1ULL << vector_index);
	}

	bool IsEmpty() const {
		return mask == 0;
	}

	idx_t CountSetBits() const {
		// Count number of vectors with qualifying rows
		uint64_t v = mask;
		idx_t count = 0;
		while (v) {
			count += v & 1ULL;
			v >>= 1;
		}
		return count;
	}
};

// A single cache entry: the bitvectors for one (table, predicate) combination
struct ConditionCacheEntry {
	string table_name;
	string filter_key;
	unordered_map<idx_t, RowGroupBitvector> bitvectors; // rg_idx -> bitvector
	idx_t total_qualifying_rows = 0;
};

// Thread-local state for capturing filter key during condition_cache_build
struct BuildCaptureState {
	bool active = false;
	string target_table;
	string captured_filter_key;
};

// Global thread-local build capture state
extern thread_local BuildCaptureState tl_build_capture;

// Stored in DuckDB's per-database ObjectCache, non-evictable
class ConditionCacheStore : public ObjectCacheEntry {
public:
	static constexpr const char *CACHE_KEY = "condition_cache_store";

	static string ObjectType() {
		return "condition_cache_store";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	// Lookup by filter key
	shared_ptr<ConditionCacheEntry> Lookup(const string &filter_key);

	// Lookup all entries for a table
	vector<shared_ptr<ConditionCacheEntry>> LookupByTable(const string &table_name);

	// Insert an entry
	void Insert(shared_ptr<ConditionCacheEntry> entry);

	// Clear all entries
	void Clear();

	// Get all entries (for condition_cache_info)
	vector<shared_ptr<ConditionCacheEntry>> GetAll();

	// Get or create the store from a client context
	static shared_ptr<ConditionCacheStore> GetOrCreate(ClientContext &context);

private:
	mutex cache_lock;
	// filter_key -> entry
	unordered_map<string, shared_ptr<ConditionCacheEntry>> entries;
};

} // namespace duckdb
