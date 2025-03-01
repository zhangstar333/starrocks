// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include <memory>

#include "column/column.h"
#include "runtime/global_dicts.h"
#include "storage/rowset/segment_v2/column_iterator.h"

namespace starrocks::segment_v2 {
// DictCodeColumnIterator is a wrapper/proxy on another column iterator that will
// transform the invoking of `next_batch(size_t*, Column*)` to the invoking of
// `next_dict_codes(size_t*, Column*)`.
class DictCodeColumnIterator final : public ColumnIterator {
public:
    using Column = starrocks::vectorized::Column;
    using ColumnPredicate = starrocks::vectorized::ColumnPredicate;
    // does not take the ownership of |iter|.
    DictCodeColumnIterator(ColumnId cid, ColumnIterator* iter) : _cid(cid), _col_iter(iter) {}

    ~DictCodeColumnIterator() override = default;

    ColumnId column_id() const { return _cid; }

    ColumnIterator* column_iterator() const { return _col_iter; }

    Status next_batch(size_t* n, Column* dst) override { return _col_iter->next_dict_codes(n, dst); }

    Status fetch_values_by_rowid(const rowid_t* rowids, size_t size, vectorized::Column* values) override {
        return _col_iter->fetch_values_by_rowid(rowids, size, values);
    }

    Status seek_to_first() override { return _col_iter->seek_to_first(); }

    Status seek_to_ordinal(ordinal_t ord) override { return _col_iter->seek_to_ordinal(ord); }

    Status next_batch(size_t* n, ColumnBlockView* dst, bool* has_null) override {
        return _col_iter->next_batch(n, dst, has_null);
    }

    ordinal_t get_current_ordinal() const override { return _col_iter->get_current_ordinal(); }

    bool all_page_dict_encoded() const override { return _col_iter->all_page_dict_encoded(); }

    int dict_lookup(const Slice& word) override { return _col_iter->dict_lookup(word); }

    Status next_dict_codes(size_t* n, vectorized::Column* dst) override { return _col_iter->next_dict_codes(n, dst); }

    Status decode_dict_codes(const int32_t* codes, size_t size, vectorized::Column* words) override {
        return _col_iter->decode_dict_codes(codes, size, words);
    }

    Status get_row_ranges_by_zone_map(const std::vector<const vectorized::ColumnPredicate*>& predicates,
                                      const ColumnPredicate* del_predicate,
                                      vectorized::SparseRange* row_ranges) override {
        return _col_iter->get_row_ranges_by_zone_map(predicates, del_predicate, row_ranges);
    }

private:
    ColumnId _cid;
    ColumnIterator* _col_iter;
};

// GlobalDictCodeColumnIterator is similar to DictCodeColumnIterator
// used in global dict optimize
class GlobalDictCodeColumnIterator final : public ColumnIterator {
public:
    using Column = starrocks::vectorized::Column;
    using ColumnPredicate = starrocks::vectorized::ColumnPredicate;
    using GlobalDictMap = starrocks::vectorized::GlobalDictMap;
    using LowCardDictColumn = starrocks::vectorized::LowCardDictColumn;

    GlobalDictCodeColumnIterator(ColumnId cid, ColumnIterator* iter, GlobalDictMap* gdict)
            : _cid(cid), _col_iter(iter), _global_dict(gdict) {}

    ~GlobalDictCodeColumnIterator() = default;

    ColumnId column_id() const { return _cid; }

    ColumnIterator* column_iterator() const { return _col_iter; }

    Status next_batch(size_t* n, Column* dst) override { return _col_iter->next_dict_codes(n, dst); }

    Status fetch_values_by_rowid(const rowid_t* rowids, size_t size, vectorized::Column* values) override {
        if (_local_dict_code_col == nullptr) {
            _init_local_dict_col();
        }
        _local_dict_code_col->reset_column();
        RETURN_IF_ERROR(_col_iter->fetch_dict_codes_by_rowid(rowids, size, _local_dict_code_col.get()));
        const auto& container = _get_local_dict_col_container(_local_dict_code_col.get());
        RETURN_IF_ERROR(decode_dict_codes(container.data(), container.size(), values));
        return Status::OK();
    }

    Status seek_to_first() override { return _col_iter->seek_to_first(); }

    Status seek_to_ordinal(ordinal_t ord) override { return _col_iter->seek_to_ordinal(ord); }

    Status next_batch(size_t* n, ColumnBlockView* dst, bool* has_null) override {
        return Status::InternalError("scalar next_batch() should never be called");
    }

    ordinal_t get_current_ordinal() const override { return _col_iter->get_current_ordinal(); }

    bool all_page_dict_encoded() const override { return _col_iter->all_page_dict_encoded(); }

    // used for rewrite predicate
    // we need return local dict code
    int dict_lookup(const Slice& word) override { return _col_iter->dict_lookup(word); }

    Status next_dict_codes(size_t* n, vectorized::Column* dst) override {
        return Status::NotSupported("GlobalDictCodeColumnIterator does not support next_dict_codes");
    }

    Status decode_dict_codes(const int32_t* codes, size_t size, vectorized::Column* words) override {
        RETURN_IF_ERROR(_build_to_global_dict());
        LowCardDictColumn::Container* container = nullptr;
        bool output_nullable = words->is_nullable();

        if (output_nullable) {
            vectorized::ColumnPtr& data_column = down_cast<vectorized::NullableColumn*>(words)->data_column();
            container = &down_cast<LowCardDictColumn*>(data_column.get())->get_data();
        } else {
            container = &down_cast<LowCardDictColumn*>(words)->get_data();
        }

        auto& res_data = *container;
        res_data.resize(size);
        for (size_t i = 0; i < size; ++i) {
#ifndef NDEBUG
            DCHECK(codes[i] <= vectorized::DICT_DECODE_MAX_SIZE);
            if (codes[i] < 0) {
                DCHECK(output_nullable);
            }
#endif
            res_data[i] = _local_to_global[codes[i]];
        }

        if (output_nullable) {
            auto& null_data = down_cast<vectorized::NullableColumn*>(words)->null_column_data();
            null_data.resize(size);
            for (int i = 0; i < size; ++i) {
                null_data[i] = (res_data[i] == 0);
            }
            down_cast<vectorized::NullableColumn*>(words)->update_has_null();
        }

        return Status::OK();
    }

    Status get_row_ranges_by_zone_map(const std::vector<const vectorized::ColumnPredicate*>& predicates,
                                      const ColumnPredicate* del_predicate,
                                      vectorized::SparseRange* row_ranges) override {
        return _col_iter->get_row_ranges_by_zone_map(predicates, del_predicate, row_ranges);
    }

private:
    Status _build_to_global_dict();
    void _init_local_dict_col();
    const LowCardDictColumn::Container& _get_local_dict_col_container(Column* column);

    ColumnId _cid;
    ColumnIterator* _col_iter;

    std::vector<int16_t> _local_to_global_holder;

    // _local_to_global[-1] is accessable
    int16_t* _local_to_global;

    // global dict
    GlobalDictMap* _global_dict;
    vectorized::ColumnPtr _local_dict_code_col;
};

} // namespace starrocks::segment_v2
