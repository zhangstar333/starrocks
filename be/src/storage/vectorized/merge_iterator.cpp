// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "storage/vectorized/merge_iterator.h"

#include <memory>
#include <queue>
#include <vector>

#include "boost/heap/skew_heap.hpp"
#include "column/chunk.h"
#include "common/config.h"
#include "storage/iterators.h" // StorageReadOptions
#include "storage/vectorized/chunk_helper.h"

namespace starrocks::vectorized {

// Compare the row of index |m| in |lhs|, with the row of index |n| in |rhs|.
inline int compare_chunk(size_t key_columns, const Chunk& lhs, size_t m, const Chunk& rhs, size_t n) {
    for (size_t i = 0; i < key_columns; i++) {
        const ColumnPtr& lc = lhs.get_column_by_index(i);
        const ColumnPtr& rc = rhs.get_column_by_index(i);
        if (int r = lc->compare_at(m, n, *rc, -1); r != 0) {
            return r;
        }
    }
    return 0;
}

static const size_t max_merge_chunk_size = 65536;

// MergingChunk contains a chunk for merge and an index of compared row.
class MergingChunk {
public:
    MergingChunk() {}
    explicit MergingChunk(Chunk* chunk) : _chunk(chunk) {}

    size_t compared_row() const { return _compared_row; }

    void advance(size_t row) { _compared_row += row; }

    size_t remaining_rows() const { return _chunk->num_rows() - _compared_row; }

protected:
    friend class MaskMergeIterator;

    Chunk* _chunk = nullptr;
    // use uint16_t for better heap merge performance
    uint16_t _compared_row = 0;
};

// Compare two chunks by the one specific row of each other.
class ComparableChunk : public MergingChunk {
public:
    explicit ComparableChunk(Chunk* chunk, size_t order, size_t key_columns)
            : MergingChunk(chunk), _order(order), _key_columns(key_columns) {}

    bool operator>(const ComparableChunk& rhs) const {
        DCHECK_EQ(_key_columns, rhs._key_columns);
        int r = compare_chunk(_key_columns, *_chunk, _compared_row, *rhs._chunk, rhs._compared_row);
        return (r > 0) | ((r == 0) & (_order > rhs._order));
    }

    // return true iff all rows in |this| chunk are less than those in |rhs|, i.e, if
    // last row in |this| chunk is less than the first row in |rhs|.
    // assume both |this| and |rhs| are not empty.
    bool less_than_all(const ComparableChunk& rhs) {
        size_t last_row = _chunk->num_rows() - 1;
        int r = compare_chunk(_key_columns, *_chunk, last_row, *rhs._chunk, rhs._compared_row);
        return (r < 0) | ((r == 0) & (_order < rhs._order));
    }

private:
    friend class HeapMergeIterator;

    // used to determinate the order of two rows when their key columns are all equals.
    uint16_t _order;
    uint16_t _key_columns;
};

class MergeIterator : public ChunkIterator {
public:
    explicit MergeIterator(std::vector<ChunkIteratorPtr> children)
            : ChunkIterator(children[0]->schema(), children[0]->chunk_size()),
              _children(std::move(children)),
              _chunk_pool(_children.size()) {
#ifndef NDEBUG
        // ensure that the children's schemas are all the same.
        for (size_t i = 1; i < _children.size(); i++) {
            CHECK_EQ(_schema.num_fields(), _children[i]->schema().num_fields());
            for (size_t j = 0; j < _schema.num_fields(); j++) {
                CHECK_EQ(_schema.field(j)->to_string(), _children[i]->schema().field(j)->to_string());
            }
        }
        // ensure that the key fields are the first |num_key_fields| and sorted by id.
        for (size_t i = 0; i < _schema.num_key_fields(); i++) {
            CHECK(_schema.field(i)->is_key());
        }
        for (size_t i = 0; i + 1 < _schema.num_key_fields(); i++) {
            CHECK_LT(_schema.field(i)->id(), _schema.field(i + 1)->id());
        }
#endif
    }

    ~MergeIterator() override { close(); }

    void close() override;

    size_t merged_rows() const override { return _merged_rows; }

    virtual Status init_encoded_schema(ColumnIdToGlobalDictMap& dict_maps) override {
        ChunkIterator::init_encoded_schema(dict_maps);
        for (int i = 0; i < _children.size(); ++i) {
            RETURN_IF_ERROR(_children[i]->init_encoded_schema(dict_maps));
        }
        return Status::OK();
    }

protected:
    Status init();
    void close_child(size_t child);

    virtual Status fill(size_t child) = 0;

    std::vector<ChunkIteratorPtr> _children;
    std::vector<ChunkPtr> _chunk_pool;
    size_t _merged_rows = 0;
    bool _inited = false;
};

inline Status MergeIterator::init() {
    DCHECK(_chunk_size > 0);
    DCHECK_EQ(_children.size(), _chunk_pool.size());
    for (size_t i = 0; i < _children.size(); i++) {
        _chunk_pool[i] = ChunkHelper::new_chunk(_schema, _chunk_size);
        RETURN_IF_ERROR(fill(i));
    }
    _inited = true;
    return Status::OK();
}

inline void MergeIterator::close_child(size_t child) {
    if (_chunk_pool[child] == nullptr) {
        return;
    }
    _chunk_pool[child].reset();
    _merged_rows += _children[child]->merged_rows();
    _children[child]->close();
    _children[child].reset();
}

inline void MergeIterator::close() {
    DCHECK_EQ(_children.size(), _chunk_pool.size());
    for (size_t i = 0; i < _children.size(); i++) {
        close_child(i);
    }
    _children.clear();
    _chunk_pool.clear();
}

class HeapMergeIterator final : public MergeIterator {
public:
    explicit HeapMergeIterator(std::vector<ChunkIteratorPtr> children) : MergeIterator(std::move(children)) {}

protected:
    Status do_get_next(Chunk* chunk) override { return do_get_next(chunk, nullptr); }
    Status do_get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) override;
    Status fill(size_t child) override;

private:
    template <typename T, typename Container = std::vector<T>>
    using MinPriorityQueue = std::priority_queue<T, Container, std::greater<T>>;
    using ChunkHeap = MinPriorityQueue<ComparableChunk>;

    ChunkHeap _heap;
};

inline Status HeapMergeIterator::do_get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) {
    if (!_inited) {
        RETURN_IF_ERROR(init());
    }
    size_t rows = 0;
    Status st;

    while (!_heap.empty() && rows < _chunk_size) {
        ComparableChunk min_chunk = _heap.top();
        _heap.pop();
        DCHECK_GT(min_chunk.remaining_rows(), 0);

        size_t offset = min_chunk.compared_row();
        // check whether |min_chunk| has overlapping with others.
        if (offset == 0 && (_heap.empty() || min_chunk.less_than_all(_heap.top()))) {
            if (rows == 0) {
                chunk->swap_chunk(*min_chunk._chunk);
                if (source_masks) {
                    source_masks->insert(source_masks->end(), chunk->num_rows(),
                                         RowSourceMask{min_chunk._order, false});
                }
                return fill(min_chunk._order);
            } else {
                // retrieve |min_chunk| next time to avoid memory copy.
                _heap.push(min_chunk);
                break;
            }
        }

        chunk->append(*min_chunk._chunk, offset, 1);
        min_chunk.advance(1);
        rows += 1;
        if (source_masks) {
            source_masks->emplace_back(RowSourceMask{min_chunk._order, false});
        }
        if (min_chunk.remaining_rows() > 0) {
            _heap.push(min_chunk);
        } else {
            st = fill(min_chunk._order);
            if (!st.ok()) {
                break;
            }
        }
    }
    if (!st.ok()) {
        return st;
    } else if (rows > 0) {
        return Status::OK();
    } else {
        return Status::EndOfFile("End of heap merge iterator");
    }
}

inline Status HeapMergeIterator::fill(size_t child) {
    Chunk* chunk = _chunk_pool[child].get();

    chunk->reset();

    Status st = _children[child]->get_next(chunk);
    if (st.ok()) {
        size_t num_rows = chunk->num_rows();
        DCHECK_GT(num_rows, 0u);
        if (num_rows > max_merge_chunk_size) {
            return Status::InternalError(strings::Substitute(
                    "Merge iterator only supports merging chunks with rows less than $0", max_merge_chunk_size));
        }
        _heap.push(ComparableChunk{chunk, child, _schema.num_key_fields()});
    } else if (st.is_end_of_file()) {
        // ignore Status::EndOfFile.
        close_child(child);
    } else {
        close_child(child);
        return st;
    }
    return Status::OK();
}

ChunkIteratorPtr new_heap_merge_iterator(const std::vector<ChunkIteratorPtr>& children) {
    DCHECK(!children.empty());
    if (children.size() == 1) {
        return children[0];
    }

    // The `ComparableChunk` is using `uint16_t` to save the chunk order, if the size of
    // children is greater than UINT16_MAX, the value of order will overflow.
    const static size_t kMaxChildrenSize = std::numeric_limits<uint16_t>::max();

    if (children.size() <= kMaxChildrenSize) {
        return std::make_shared<HeapMergeIterator>(children);
    }
    std::vector<ChunkIteratorPtr> sub_merge_iterators;
    sub_merge_iterators.reserve((children.size() + kMaxChildrenSize - 1) / kMaxChildrenSize);
    for (size_t i = 0; i < children.size(); i += kMaxChildrenSize) {
        size_t j = std::min(i + kMaxChildrenSize, children.size());
        std::vector<ChunkIteratorPtr> v(children.begin() + i, children.begin() + j);
        sub_merge_iterators.emplace_back(new_heap_merge_iterator(v));
    }
    return new_heap_merge_iterator(sub_merge_iterators);
}

// Merge iterator based on source masks.
// The order of rows is determinate by mask sequence.
class MaskMergeIterator final : public MergeIterator {
public:
    explicit MaskMergeIterator(std::vector<ChunkIteratorPtr> children, RowSourceMaskBuffer* mask_buffer)
            : MergeIterator(std::move(children)), _chunks(_children.size()), _mask_buffer(mask_buffer) {
        DCHECK(_mask_buffer);
    }

protected:
    Status do_get_next(Chunk* chunk) override { return Status::NotSupported("get chunk not supported"); }
    Status do_get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) override;
    Status fill(size_t child) override;

private:
    std::vector<MergingChunk> _chunks;
    RowSourceMaskBuffer* _mask_buffer = nullptr;
};

inline Status MaskMergeIterator::do_get_next(Chunk* chunk, std::vector<RowSourceMask>* source_masks) {
    if (!_inited) {
        RETURN_IF_ERROR(init());
    }
    size_t rows = 0;
    Status st;

    auto st_or = _mask_buffer->has_remaining();
    if (!st_or.ok()) {
        return st_or.status();
    }
    while (st_or.value() && rows < _chunk_size) {
        RowSourceMask mask = _mask_buffer->current();
        uint16_t child = mask.get_source_num();
        auto& min_chunk = _chunks[child];
        DCHECK_GT(min_chunk.remaining_rows(), 0);

        size_t offset = min_chunk.compared_row();
        // check whether |min_chunk| has overlapping with others.
        size_t min_chunk_num_rows = min_chunk._chunk->num_rows();
        if (offset == 0 && _mask_buffer->has_same_source(child, min_chunk_num_rows)) {
            if (rows == 0) {
                chunk->swap_chunk(*min_chunk._chunk);
                for (int i = 0; i < min_chunk_num_rows; ++i) {
                    source_masks->emplace_back(_mask_buffer->current());
                    _mask_buffer->advance();
                }
                return fill(child);
            } else {
                // retrieve |min_chunk| next time to avoid memory copy.
                break;
            }
        }

        chunk->append(*min_chunk._chunk, offset, 1);
        min_chunk.advance(1);
        rows += 1;
        source_masks->emplace_back(mask);
        _mask_buffer->advance();
        if (min_chunk.remaining_rows() == 0) {
            st = fill(child);
            if (!st.ok()) {
                break;
            }
        }

        st_or = _mask_buffer->has_remaining();
        if (!st_or.ok()) {
            return st_or.status();
        }
    }
    if (!st.ok()) {
        return st;
    } else if (rows > 0) {
        return Status::OK();
    } else {
        for (auto& chunk : _chunk_pool) {
            DCHECK(chunk == nullptr);
        }
        return Status::EndOfFile("End of mask merge iterator");
    }
}

inline Status MaskMergeIterator::fill(size_t child) {
    Chunk* chunk = _chunk_pool[child].get();

    chunk->reset();

    Status st = _children[child]->get_next(chunk);
    if (st.ok()) {
        size_t num_rows = chunk->num_rows();
        DCHECK_GT(num_rows, 0u);
        if (num_rows > max_merge_chunk_size) {
            return Status::InternalError(strings::Substitute(
                    "Merge iterator only supports merging chunks with rows less than $0", max_merge_chunk_size));
        }
        _chunks[child] = MergingChunk(chunk);
    } else if (st.is_end_of_file()) {
        // ignore Status::EndOfFile.
        close_child(child);
    } else {
        close_child(child);
        return st;
    }
    return Status::OK();
}

ChunkIteratorPtr new_mask_merge_iterator(const std::vector<ChunkIteratorPtr>& children,
                                         RowSourceMaskBuffer* mask_buffer) {
    DCHECK(children.size() > 1 && children.size() <= RowSourceMask::MAX_SOURCES);
    return std::make_shared<MaskMergeIterator>(std::move(children), mask_buffer);
}

} // namespace starrocks::vectorized
