#include "duckdb/storage/table/version_manager.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

using namespace duckdb;
using namespace std;

index_t VersionManager::GetSelVector(Transaction &transaction, index_t index, sel_t sel_vector[], index_t max_count) {
	// obtain a read lock
	auto read_lock = lock.GetSharedLock();

	auto entry = info.find(index);
	if (entry == info.end()) {
		// no info, use everything
		return max_count;
	} else {
		// get the selection vector from the chunk info
		return entry->second->GetSelVector(transaction, sel_vector, max_count);
	}
}

bool VersionManager::Fetch(Transaction &transaction, index_t row) {
	row -= base_row;
	index_t vector_index = row / STANDARD_VECTOR_SIZE;

	auto entry = info.find(vector_index);
	if (entry == info.end()) {
		// no info, use the row
		return true;
	} else {
		// there is an info: need to figure out if we want to use the row or not
		return entry->second->Fetch(transaction, row - vector_index * STANDARD_VECTOR_SIZE);
	}
}

class VersionDeleteState {
public:
	VersionDeleteState(VersionManager &manager, Transaction &transaction, index_t base_row)
	    : manager(manager), transaction(transaction), current_info(nullptr), current_chunk((index_t)-1), count(0),
	      base_row(base_row) {
	}

	VersionManager &manager;
	Transaction &transaction;
	ChunkInfo *current_info;
	index_t current_chunk;
	row_t rows[STANDARD_VECTOR_SIZE];
	index_t count;
	index_t base_row;
	index_t chunk_row;

public:
	void Delete(row_t row_id);
	void Flush();
};

void VersionManager::Delete(Transaction &transaction, Vector &row_ids) {
	auto ids = (row_t *)row_ids.data;

	VersionDeleteState del_state(*this, transaction, base_row);

	// obtain a write lock
	auto write_lock = lock.GetExclusiveLock();
	VectorOperations::Exec(row_ids, [&](index_t i, index_t k) { del_state.Delete(ids[i] - base_row); });
	del_state.Flush();
}

void VersionDeleteState::Delete(row_t row_id) {
	index_t chunk_idx = row_id / STANDARD_VECTOR_SIZE;
	index_t idx_in_chunk = row_id - chunk_idx * STANDARD_VECTOR_SIZE;

	// check if we are targetting a different chunk than the current chunk
	if (chunk_idx != current_chunk) {
		// if we are, first flush the previous chunk
		Flush();

		// then look up if the chunk already exists
		auto entry = manager.info.find(chunk_idx);
		if (entry == manager.info.end()) {
			// no version info yet: have to create one
			auto new_info = make_unique<ChunkDeleteInfo>(manager, chunk_idx * STANDARD_VECTOR_SIZE);
			current_info = new_info.get();
			manager.info[chunk_idx] = move(new_info);
		} else {
			// version info already exists: alter existing version info
			current_info = entry->second.get();
		}
		current_chunk = chunk_idx;
		chunk_row = chunk_idx * STANDARD_VECTOR_SIZE;
	}

	// now add the row to the set of to-be-deleted rows
	rows[count++] = idx_in_chunk;
}

void VersionDeleteState::Flush() {
	if (count == 0) {
		return;
	}
	// delete in the current info
	current_info->Delete(transaction, rows, count);
	// now push the delete into the undo buffer
	transaction.PushDelete(current_info, rows, count, base_row + chunk_row);
	count = 0;
}

void VersionManager::Append(Transaction &transaction, row_t row_start, index_t count, transaction_t commit_id) {
	index_t chunk_idx = row_start / STANDARD_VECTOR_SIZE;
	index_t idx_in_chunk = row_start - chunk_idx * STANDARD_VECTOR_SIZE;

	// obtain a write lock
	auto write_lock = lock.GetExclusiveLock();
	auto current_info = GetInsertInfo(chunk_idx);
	for (index_t i = 0; i < count; i++) {
		current_info->inserted[idx_in_chunk] = commit_id;
		idx_in_chunk++;
		if (idx_in_chunk == STANDARD_VECTOR_SIZE) {
			chunk_idx++;
			idx_in_chunk = 0;
			current_info = GetInsertInfo(chunk_idx);
		}
	}
	max_row += count;
}

ChunkInsertInfo *VersionManager::GetInsertInfo(index_t chunk_idx) {
	auto entry = info.find(chunk_idx);
	if (entry == info.end()) {
		// no version info yet: have to create one
		auto new_info = make_unique<ChunkInsertInfo>(*this, chunk_idx * STANDARD_VECTOR_SIZE);
		auto result = new_info.get();
		info[chunk_idx] = move(new_info);
		return result;
	} else {
		// version info already exists: check if it is insert or delete info
		auto current_info = entry->second.get();
		if (current_info->type == ChunkInfoType::INSERT_INFO) {
			return (ChunkInsertInfo *)current_info;
		} else {
			assert(current_info->type == ChunkInfoType::DELETE_INFO);
			// delete info, change to insert info
			auto new_info = make_unique<ChunkInsertInfo>((ChunkDeleteInfo &)*current_info);
			auto result = new_info.get();
			info[chunk_idx] = move(new_info);
			return result;
		}
	}
}

void VersionManager::RevertAppend(row_t row_start, row_t row_end) {
	auto write_lock = lock.GetExclusiveLock();

	index_t chunk_start = row_start / STANDARD_VECTOR_SIZE + (row_start % STANDARD_VECTOR_SIZE == 0 ? 0 : 1);
	index_t chunk_end = row_end / STANDARD_VECTOR_SIZE;
	for (; chunk_start <= chunk_end; chunk_start++) {
		info.erase(chunk_start);
	}
}
