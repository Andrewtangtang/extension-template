#define DUCKDB_EXTENSION_MAIN

#include "condition_cache_extension.hpp"
#include "condition_cache_optimizer.hpp"
#include "condition_cache_state.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

// Forward declarations for table functions (defined in condition_cache_functions.cpp)
TableFunction ConditionCacheBuildFunction();
TableFunction ConditionCacheClearFunction();
TableFunction ConditionCacheInfoFunction();

static void LoadInternal(ExtensionLoader &loader) {
	// Register table functions
	loader.RegisterFunction(ConditionCacheBuildFunction());
	loader.RegisterFunction(ConditionCacheClearFunction());
	loader.RegisterFunction(ConditionCacheInfoFunction());

	// Register optimizer extension
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	config.optimizer_extensions.push_back(ConditionCacheOptimizer());
}

void ConditionCacheExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ConditionCacheExtension::Name() {
	return "condition_cache";
}

std::string ConditionCacheExtension::Version() const {
#ifdef EXT_VERSION_CONDITION_CACHE
	return EXT_VERSION_CONDITION_CACHE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(condition_cache, loader) {
	duckdb::LoadInternal(loader);
}
}
