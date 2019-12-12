//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Decodes the blocks generated by block_builder.cc.

#include "table/block.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "rocksdb/comparator.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/perf_context_imp.h"

namespace rocksdb {

// Helper routine: decode the next block entry starting at "p",
// storing the number of shared key bytes, non_shared key bytes,
// and the length of the value in "*shared", "*non_shared", and
// "*value_length", respectively.  Will not derefence past "limit".
//
// If any errors are detected, returns nullptr.  Otherwise, returns a
// pointer to the key delta (just past the three decoded values).
static inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared,
                                      uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = reinterpret_cast<const unsigned char*>(p)[0];
  *non_shared = reinterpret_cast<const unsigned char*>(p)[1];
  *value_length = reinterpret_cast<const unsigned char*>(p)[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}

void BlockIter::Next() {
  assert(Valid());
  ParseNextKey();
}

void BlockIter::Prev() {
  assert(Valid());

  // Scan backwards to a restart point before current_
  const uint32_t original = current_;
  while (GetRestartPoint(restart_index_) >= original) {
    if (restart_index_ == 0) {
      // No more entries
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return;
    }
    restart_index_--;
  }

  SeekToRestartPoint(restart_index_);
  do {
    // Loop until end of current entry hits the start of original entry
  } while (ParseNextKey() && NextEntryOffset() < original);
}

void BlockIter::Seek(const Slice& target) {
  PERF_TIMER_GUARD(block_seek_nanos);
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  uint32_t index = 0;
  bool ok = BinarySeek(target, 0, num_restarts_ - 1, &index);

  if (!ok) {
    return;
  }
  SeekToRestartPoint(index);
  // Linear search (within restart block) for first key >= target

  while (true) {
    if (!ParseNextKey() || Compare(key_.GetKey(), target) >= 0) {
      return;
    }
  }
}

void BlockIter::SeekToFirst() {
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  SeekToRestartPoint(0);
  ParseNextKey();
}

void BlockIter::SeekToLast() {
  if (data_ == nullptr) {  // Not init yet
    return;
  }
  SeekToRestartPoint(num_restarts_ - 1);
  while (ParseNextKey() && NextEntryOffset() < restarts_) {
    // Keep skipping
  }
}

void BlockIter::CorruptionError() {
  current_ = restarts_;
  restart_index_ = num_restarts_;
  status_ = Status::Corruption("bad entry in block");
  key_.Clear();
  value_.clear();
}

bool BlockIter::ParseNextKey() {
  current_ = NextEntryOffset();
  const char* p = data_ + current_;
  const char* limit = data_ + restarts_;  // Restarts come right after data
  if (p >= limit) {
    // No more entries to return.  Mark as invalid.
    current_ = restarts_;
    restart_index_ = num_restarts_;
    return false;
  }

  // Decode next entry
  uint32_t shared, non_shared, value_length;
  p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
  if (p == nullptr || key_.Size() < shared) {
    CorruptionError();
    return false;
  } else {
    if (shared == 0) {
      // If this key don't share any bytes with prev key then we don't need
      // to decode it and can use it's address in the block directly.
      key_.SetKey(Slice(p, non_shared), false /* copy */);
    } else {
      // This key share `shared` bytes with prev key, we need to decode it
      key_.TrimAppend(shared, p, non_shared);
    }

    value_ = Slice(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }
}

// Binary search in restart array to find the first restart point
// with a key >= target (TODO: this comment is inaccurate)
bool BlockIter::BinarySeek(const Slice& target, uint32_t left, uint32_t right,
                  uint32_t* index) {
  assert(left <= right);

  while (left < right) {
    uint32_t mid = (left + right + 1) / 2;
    uint32_t region_offset = GetRestartPoint(mid);
    uint32_t shared, non_shared, value_length;
    const char* key_ptr =
        DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                    &non_shared, &value_length);
    if (key_ptr == nullptr || (shared != 0)) {
      CorruptionError();
      return false;
    }
    Slice mid_key(key_ptr, non_shared);
    int cmp = Compare(mid_key, target);
    if (cmp < 0) {
      // Key at "mid" is smaller than "target". Therefore all
      // blocks before "mid" are uninteresting.
      left = mid;
    } else if (cmp > 0) {
      // Key at "mid" is >= "target". Therefore all blocks at or
      // after "mid" are uninteresting.
      right = mid - 1;
    } else {
      left = right = mid;
    }
  }

  *index = left;
  return true;
}

// Compare target key and the block key of the block of `block_index`.
// Return -1 if error.
int BlockIter::CompareBlockKey(uint32_t block_index, const Slice& target) {
  uint32_t region_offset = GetRestartPoint(block_index);
  uint32_t shared, non_shared, value_length;
  const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_,
                                    &shared, &non_shared, &value_length);
  if (key_ptr == nullptr || (shared != 0)) {
    CorruptionError();
    return 1;  // Return target is smaller
  }
  Slice block_key(key_ptr, non_shared);
  return Compare(block_key, target);
}

// Binary search in block_ids to find the first block with a key >= target
bool BlockIter::BinaryBlockIndexSeek(const Slice& target, uint32_t* block_ids,
                                     uint32_t left, uint32_t right,
                                     uint32_t* index) {
  assert(left <= right);
  uint32_t left_bound = left;

  while (left <= right) {
    uint32_t mid = (left + right) / 2;

    int cmp = CompareBlockKey(block_ids[mid], target);
    if (!status_.ok()) {
      return false;
    }
    if (cmp < 0) {
      // Key at "target" is larger than "mid". Therefore all
      // blocks before or at "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "target" is <= "mid". Therefore all blocks
      // after "mid" are uninteresting.
      // If there is only one block left, we found it.
      if (left == right) break;
      right = mid;
    }
  }

  if (left == right) {
    // In one of the two following cases:
    // (1) left is the first one of block_ids
    // (2) there is a gap of blocks between block of `left` and `left-1`.
    // we can further distinguish the case of key in the block or key not
    // existing, by comparing the target key and the key of the previous
    // block to the left of the block found.
    if (block_ids[left] > 0 &&
        (left == left_bound || block_ids[left - 1] != block_ids[left] - 1) &&
        CompareBlockKey(block_ids[left] - 1, target) > 0) {
      current_ = restarts_;
      return false;
    }

    *index = block_ids[left];
    return true;
  } else {
    assert(left > right);
    // Mark iterator invalid
    current_ = restarts_;
    return false;
  }
}

uint32_t Block::NumRestarts() const {
  assert(size_ >= 2*sizeof(uint32_t));
  return DecodeFixed32(data_ + size_ - sizeof(uint32_t));
}

Block::Block(BlockContents&& contents)
    : contents_(std::move(contents)),
      data_(contents_.data.data()),
      size_(contents_.data.size()) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // Error marker
  } else {
    restart_offset_ =
        static_cast<uint32_t>(size_) - (1 + NumRestarts()) * sizeof(uint32_t);
    if (restart_offset_ > size_ - sizeof(uint32_t)) {
      // The size is too small for NumRestarts() and therefore
      // restart_offset_ wrapped around.
      size_ = 0;
    }
  }
}

InternalIterator* Block::NewIterator(const Comparator* cmp, BlockIter* iter,
                                     bool total_order_seek) {
  if (size_ < 2*sizeof(uint32_t)) {
    if (iter != nullptr) {
      iter->SetStatus(Status::Corruption("bad block contents"));
      return iter;
    } else {
      return NewErrorInternalIterator(Status::Corruption("bad block contents"));
    }
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) {
    if (iter != nullptr) {
      iter->SetStatus(Status::OK());
      return iter;
    } else {
      return NewEmptyInternalIterator();
    }
  } else {
    if (iter != nullptr) {
      iter->Initialize(cmp, data_, restart_offset_, num_restarts);
    } else {
      iter = new BlockIter(cmp, data_, restart_offset_, num_restarts);
    }
  }

  return iter;
}

size_t Block::ApproximateMemoryUsage() const {
  return usable_size();
}

}  // namespace rocksdb
