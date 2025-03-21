// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/vectorized/join_hash_map.h"

#include <gtest/gtest.h>

#include "runtime/descriptor_helper.h"
#include "runtime/exec_env.h"

namespace starrocks::vectorized {
class JoinHashMapTest : public ::testing::Test {
protected:
    void SetUp() override {
        config::vector_chunk_size = 4096;
        _object_pool = std::make_shared<ObjectPool>();
        _mem_pool = std::make_shared<MemPool>();
        _runtime_profile = create_runtime_profile();
        _runtime_state = create_runtime_state();
    }
    void TearDown() override {}

private:
    static ColumnPtr create_int32_column(uint32_t row_count, uint32_t start_value);
    static ColumnPtr create_binary_column(uint32_t row_count, uint32_t start_value, MemPool* mem_pool);
    static ColumnPtr create_int32_nullable_column(uint32_t row_count, uint32_t start_value);
    static void check_int32_column(const ColumnPtr& column, uint32_t row_count, uint32_t start_value);
    static void check_binary_column(const ColumnPtr& column, uint32_t row_count, uint32_t start_value);
    static void check_int32_nullable_column(const ColumnPtr& column, uint32_t row_count, uint32_t start_value);
    static ChunkPtr create_int32_probe_chunk(uint32_t count, uint32_t start_value, bool nullable);
    static ChunkPtr create_binary_probe_chunk(uint32_t count, uint32_t start_value, bool nullable, MemPool* mem_pool);
    static ChunkPtr create_int32_build_chunk(uint32_t count, bool nullable);
    static ChunkPtr create_binary_build_chunk(uint32_t count, bool nullable, MemPool* mem_pool);
    static TSlotDescriptor create_slot_descriptor(const std::string& column_name, PrimitiveType column_type,
                                                  int32_t column_pos, bool nullable);
    static void add_tuple_descriptor(TDescriptorTableBuilder* table_desc_builder, PrimitiveType column_type,
                                     bool nullable);
    static std::shared_ptr<RuntimeProfile> create_runtime_profile();
    static std::shared_ptr<RowDescriptor> create_row_desc(const std::shared_ptr<ObjectPool>& object_pool,
                                                          TDescriptorTableBuilder* table_desc_builder, bool nullable);
    static std::shared_ptr<RowDescriptor> create_probe_desc(const std::shared_ptr<ObjectPool>& object_pool,
                                                            TDescriptorTableBuilder* probe_desc_builder, bool nullable);
    static std::shared_ptr<RowDescriptor> create_build_desc(const std::shared_ptr<ObjectPool>& object_pool,
                                                            TDescriptorTableBuilder* build_desc_builder, bool nullable);
    static std::shared_ptr<RuntimeState> create_runtime_state();

    static void check_probe_index(const Buffer<uint32_t>& probe_index, uint32_t step, uint32_t match_count,
                                  uint32_t probe_row_count);
    static void check_build_index(const Buffer<uint32_t>& build_index, uint32_t step, uint32_t match_count,
                                  uint32_t probe_row_count);
    static void check_match_index(const Buffer<uint32_t>& probe_match_index, uint32_t start, int32_t count,
                                  uint32_t match_count);
    static void check_probe_state(const JoinHashTableItems& table_items, const HashTableProbeState& probe_state,
                                  JoinMatchFlag match_flag, uint32_t step, uint32_t match_count,
                                  uint32_t probe_row_count, bool has_null_build_tuple);
    static void check_build_index(const Buffer<uint32_t>& first, const Buffer<uint32_t>& next, uint32_t row_count);
    static void check_build_index(const Buffer<uint8_t>& nulls, const Buffer<uint32_t>& first,
                                  const Buffer<uint32_t>& next, uint32_t row_count);
    static void check_build_slice(const Buffer<Slice>& slices, uint32_t row_count);
    static void check_build_slice(const Buffer<uint8_t>& nulls, const Buffer<Slice>& slices, uint32_t row_count);
    static void check_build_column(const ColumnPtr& build_column, uint32_t row_count);
    static void check_build_column(const Buffer<uint8_t>& nulls, const ColumnPtr& build_column, uint32_t row_count);

    // used for probe from ht
    static void prepare_table_items(JoinHashTableItems* table_items, TJoinOp::type join_type, bool with_other_conjunct,
                                    uint32_t batch_count);

    // used for build func
    void prepare_table_items(JoinHashTableItems* table_items, uint32_t row_count);

    static void prepare_probe_state(HashTableProbeState* probe_state, uint32_t probe_row_count);
    static void prepare_build_data(Buffer<int32_t>* build_data, uint32_t batch_count);
    static void prepare_probe_data(Buffer<int32_t>* probe_data, uint32_t probe_row_count);

    static bool is_check_cur_row_match_count(TJoinOp::type join_type, bool with_other_conjunct);

    // flag: 0(all 0), 1(all 1), 2(half 0), 3(one third 0)
    static Buffer<uint8_t> create_bools(uint32_t count, int32_t flag);
    static ColumnPtr create_tuple_column(const Buffer<uint8_t>& data);
    static ColumnPtr create_column(PrimitiveType PT);
    static ColumnPtr create_column(PrimitiveType PT, uint32_t start, uint32_t count);
    static ColumnPtr create_nullable_column(PrimitiveType PT);
    static ColumnPtr create_nullable_column(PrimitiveType PT, const Buffer<uint8_t>& nulls, uint32_t start,
                                            uint32_t count);

    std::shared_ptr<ObjectPool> _object_pool = nullptr;
    std::shared_ptr<MemPool> _mem_pool = nullptr;
    std::shared_ptr<RuntimeProfile> _runtime_profile = nullptr;
    std::shared_ptr<RuntimeState> _runtime_state = nullptr;
};

ColumnPtr JoinHashMapTest::create_tuple_column(const Buffer<uint8_t>& data) {
    ColumnPtr column = BooleanColumn::create();
    for (auto v : data) {
        column->append_datum(v);
    }
    return column;
}

Buffer<uint8_t> JoinHashMapTest::create_bools(uint32_t count, int32_t flag) {
    Buffer<uint8_t> nulls(count);

    if (flag == 0) {
        // all 0
        for (uint32_t i = 0; i < count; i++) {
            nulls[i] = 0;
        }
        return nulls;
    }

    if (flag == 1) {
        // all 0
        for (uint32_t i = 0; i < count; i++) {
            nulls[i] = 1;
        }
        return nulls;
    }

    if (flag == 2) {
        // half 0
        for (uint32_t i = 0; i < count; i++) {
            nulls[i] = static_cast<uint8_t>(i % 2 == 0);
        }
        return nulls;
    }

    if (flag == 3) {
        // one third 0
        for (uint32_t i = 0; i < count; i++) {
            nulls[i] = static_cast<uint8_t>(i % 3 == 0);
        }
    }

    if (flag == 4) {
        for (uint32_t i = 0; i < count; i++) {
            nulls[i] = static_cast<uint8_t>((i % 3 == 0) || (i % 2 == 0));
        }
    }

    return nulls;
}

ColumnPtr JoinHashMapTest::create_column(PrimitiveType PT) {
    if (PT == PrimitiveType::TYPE_INT) {
        return FixedLengthColumn<int32_t>::create();
    }

    if (PT == PrimitiveType::TYPE_VARCHAR) {
        return BinaryColumn::create();
    }

    return nullptr;
}

ColumnPtr JoinHashMapTest::create_column(PrimitiveType PT, uint32_t start, uint32_t count) {
    if (PT == PrimitiveType::TYPE_INT) {
        auto column = FixedLengthColumn<int32_t>::create();
        for (auto i = 0; i < count; i++) {
            column->append_datum(Datum(static_cast<int32_t>(start + i)));
        }
        return column;
    }

    if (PT == PrimitiveType::TYPE_VARCHAR) {
        auto column = BinaryColumn::create();
        for (auto i = 0; i < count; i++) {
            column->append_string(std::to_string(start + i));
        }
        return column;
    }

    return nullptr;
}

ColumnPtr JoinHashMapTest::create_nullable_column(PrimitiveType PT) {
    auto null_column = FixedLengthColumn<uint8_t>::create();

    if (PT == PrimitiveType::TYPE_INT) {
        auto data_column = FixedLengthColumn<int32_t>::create();
        return NullableColumn::create(data_column, null_column);
    }

    if (PT == PrimitiveType::TYPE_VARCHAR) {
        auto data_column = BinaryColumn::create();
        return NullableColumn::create(data_column, null_column);
    }

    return nullptr;
}

ColumnPtr JoinHashMapTest::create_nullable_column(PrimitiveType PT, const Buffer<uint8_t>& nulls, uint32_t start,
                                                  uint32_t count) {
    auto null_column = FixedLengthColumn<uint8_t>::create();

    if (PT == PrimitiveType::TYPE_INT) {
        auto data_column = FixedLengthColumn<int32_t>::create();
        for (auto i = 0; i < count; i++) {
            null_column->append_datum(Datum(static_cast<uint8_t>(nulls[i])));
            if (nulls[i] == 0) {
                data_column->append_datum(Datum(static_cast<int32_t>(start + i)));
            } else {
                data_column->append_default();
            }
        }
        return NullableColumn::create(data_column, null_column);
    }

    if (PT == PrimitiveType::TYPE_VARCHAR) {
        auto data_column = BinaryColumn::create();
        for (auto i = 0; i < count; i++) {
            null_column->append_datum(Datum(static_cast<uint8_t>(nulls[i])));
            if (nulls[i] == 0) {
                data_column->append_string(std::to_string(start + i));
            } else {
                data_column->append_default();
            }
        }
        return NullableColumn::create(data_column, null_column);
    }

    return nullptr;
}

ColumnPtr JoinHashMapTest::create_int32_column(uint32_t row_count, uint32_t start_value) {
    const auto& int_type_desc = TypeDescriptor(PrimitiveType::TYPE_INT);
    ColumnPtr column = ColumnHelper::create_column(int_type_desc, false);
    for (int32_t i = 0; i < row_count; i++) {
        column->append_datum(Datum(static_cast<int32_t>(start_value) + i));
    }
    return column;
}

ColumnPtr JoinHashMapTest::create_binary_column(uint32_t row_count, uint32_t start_value, MemPool* mem_pool) {
    const auto& varchar_type_desc = TypeDescriptor::create_varchar_type(TypeDescriptor::MAX_VARCHAR_LENGTH);
    ColumnPtr column = ColumnHelper::create_column(varchar_type_desc, false);
    auto* binary_column = ColumnHelper::as_raw_column<BinaryColumn>(column);
    for (int32_t i = 0; i < row_count; i++) {
        std::string str = std::to_string(start_value + i);
        Slice slice;
        slice.data = reinterpret_cast<char*>(mem_pool->allocate(str.size()));
        slice.size = str.size();
        memcpy(slice.data, str.data(), str.size());
        binary_column->append(slice);
    }
    return column;
}

void JoinHashMapTest::check_probe_index(const Buffer<uint32_t>& probe_index, uint32_t step, uint32_t match_count,
                                        uint32_t probe_row_count) {
    uint32_t check_count;
    if (config::vector_chunk_size * (step + 1) <= probe_row_count * match_count) {
        check_count = config::vector_chunk_size;
    } else {
        check_count = probe_row_count * match_count - config::vector_chunk_size * step;
    }

    uint32_t start = config::vector_chunk_size * step;
    for (auto i = 0; i < check_count; i++) {
        ASSERT_EQ(probe_index[i], (start + i) / match_count);
    }
}

void JoinHashMapTest::check_build_slice(const Buffer<Slice>& slices, uint32_t row_count) {
    ASSERT_EQ(slices.size(), row_count + 1);
    ASSERT_EQ(slices[0], Slice());

    for (size_t i = 0; i < row_count; i++) {
        Buffer<uint8_t> buffer(1024);
        uint32_t offset = 0;

        // serialize int
        int32_t index = i;
        memcpy(buffer.data() + offset, &index, sizeof(int32_t));
        offset += sizeof(int32_t);

        // serialize varchar
        std::string str = std::to_string(index);
        uint32_t len = str.length();
        memcpy(buffer.data() + offset, &len, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        memcpy(buffer.data() + offset, str.data(), len);
        offset += len;

        // check
        ASSERT_EQ(slices[index + 1], Slice(buffer.data(), offset));
    }
}

void JoinHashMapTest::check_build_slice(const Buffer<uint8_t>& nulls, const Buffer<Slice>& slices, uint32_t row_count) {
    ASSERT_EQ(slices.size(), row_count + 1);
    ASSERT_EQ(slices[0], Slice());

    for (size_t i = 0; i < row_count; i++) {
        Buffer<uint8_t> buffer(1024);
        uint32_t offset = 0;

        if (nulls[i] == 0) {
            // serialize int
            int32_t index = i;
            memcpy(buffer.data() + offset, &index, sizeof(int32_t));
            offset += sizeof(int32_t);

            // serialize varchar
            std::string str = std::to_string(index);
            uint32_t len = str.length();
            memcpy(buffer.data() + offset, &len, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            memcpy(buffer.data() + offset, str.data(), len);
            offset += len;

            // check
            ASSERT_EQ(slices[index + 1], Slice(buffer.data(), offset));
        }
    }
}

void JoinHashMapTest::check_build_column(const Buffer<uint8_t>& nulls, const ColumnPtr& build_column,
                                         uint32_t row_count) {
    auto* column = ColumnHelper::as_raw_column<Int64Column>(build_column);
    const auto& data = column->get_data();

    ASSERT_EQ(column->size(), row_count + 1);
    ASSERT_EQ(data[0], 0);

    for (size_t i = 0; i < row_count; i++) {
        if (nulls[i] == 0) {
            int32_t index = i;
            int64_t check_value = 0;
            uint8_t* ptr = reinterpret_cast<uint8_t*>(&check_value);
            memcpy(ptr, &index, 4);
            memcpy(ptr + 4, &index, 4);
            ASSERT_EQ(check_value, data[i + 1]);
        }
    }
}

void JoinHashMapTest::check_build_column(const ColumnPtr& build_column, uint32_t row_count) {
    auto* column = ColumnHelper::as_raw_column<Int64Column>(build_column);
    const auto& data = column->get_data();

    ASSERT_EQ(column->size(), row_count + 1);
    ASSERT_EQ(data[0], 0);

    for (size_t i = 0; i < row_count; i++) {
        int32_t index = i;
        int64_t check_value = 0;
        uint8_t* ptr = reinterpret_cast<uint8_t*>(&check_value);
        memcpy(ptr, &index, 4);
        memcpy(ptr + 4, &index, 4);
        ASSERT_EQ(check_value, data[i + 1]);
    }
}

void JoinHashMapTest::check_build_index(const Buffer<uint32_t>& first, const Buffer<uint32_t>& next,
                                        uint32_t row_count) {
    ASSERT_EQ(first.size(), JoinHashMapHelper::calc_bucket_size(row_count));
    ASSERT_EQ(next.size(), row_count + 1);
    ASSERT_EQ(next[0], 0);

    Buffer<uint32_t> check_index(row_count + 1, 0);

    for (unsigned int item : first) {
        if (item == 0) {
            continue;
        }
        auto index = item;
        while (index != 0) {
            check_index[index]++;
            index = next[index];
        }
    }

    ASSERT_EQ(check_index[0], 0);
    for (auto i = 1; i < check_index.size(); i++) {
        ASSERT_EQ(check_index[i], 1);
    }
}

void JoinHashMapTest::check_build_index(const Buffer<uint8_t>& nulls, const Buffer<uint32_t>& first,
                                        const Buffer<uint32_t>& next, uint32_t row_count) {
    ASSERT_EQ(first.size(), JoinHashMapHelper::calc_bucket_size(row_count));
    ASSERT_EQ(next.size(), row_count + 1);
    ASSERT_EQ(next[0], 0);

    Buffer<uint32_t> check_index(row_count + 1, 0);

    for (unsigned int item : first) {
        if (item == 0) {
            continue;
        }
        auto index = item;
        while (index != 0) {
            check_index[index]++;
            index = next[index];
        }
    }

    ASSERT_EQ(check_index[0], 0);
    for (auto i = 0; i < row_count; i++) {
        if (nulls[i] == 1) {
            ASSERT_EQ(check_index[i + 1], 0);
        } else {
            ASSERT_EQ(check_index[i + 1], 1);
        }
    }
}

void JoinHashMapTest::check_build_index(const Buffer<uint32_t>& build_index, uint32_t step, uint32_t match_count,
                                        uint32_t probe_row_count) {
    uint32_t check_count = 0;
    if (config::vector_chunk_size * (step + 1) <= probe_row_count * match_count) {
        check_count = config::vector_chunk_size;
    } else {
        check_count = probe_row_count * match_count - config::vector_chunk_size * step;
    }

    uint32_t start = config::vector_chunk_size * step;
    for (auto i = 0; i < check_count; i++) {
        uint32_t quo = (start + i) / match_count;
        uint32_t rem = (start + i) % match_count;
        ASSERT_EQ(build_index[i], 1 + quo + config::vector_chunk_size * (match_count - 1 - rem));
    }
}

void JoinHashMapTest::check_match_index(const Buffer<uint32_t>& probe_match_index, uint32_t start, int32_t count,
                                        uint32_t match_count) {
    for (uint32_t i = 0; i < count / match_count; i++) {
        ASSERT_EQ(probe_match_index[i], match_count);
    }
    ASSERT_EQ(probe_match_index[count / match_count], count % match_count + 1);
}

void JoinHashMapTest::check_probe_state(const JoinHashTableItems& table_items, const HashTableProbeState& probe_state,
                                        JoinMatchFlag match_flag, uint32_t step, uint32_t match_count,
                                        uint32_t probe_row_count, bool has_null_build_tuple) {
    ASSERT_EQ(probe_state.match_flag, match_flag);
    ASSERT_EQ(probe_state.has_remain, (step + 1) * config::vector_chunk_size < probe_row_count * match_count);
    ASSERT_EQ(probe_state.has_null_build_tuple, has_null_build_tuple);
    if (probe_row_count * match_count > (step + 1) * config::vector_chunk_size) {
        ASSERT_EQ(probe_state.count, config::vector_chunk_size);
        if (is_check_cur_row_match_count(table_items.join_type, table_items.with_other_conjunct)) {
            ASSERT_EQ(probe_state.cur_row_match_count, ((step + 1) * config::vector_chunk_size + 1) % match_count);
        } else {
            ASSERT_EQ(probe_state.cur_row_match_count, 0);
        }
        ASSERT_EQ(probe_state.cur_probe_index, (step + 1) * config::vector_chunk_size / match_count);
    } else {
        ASSERT_EQ(probe_state.count, probe_row_count * match_count - step * config::vector_chunk_size);
        ASSERT_EQ(probe_state.cur_row_match_count, 0);
        ASSERT_EQ(probe_state.cur_probe_index, 0);
    }
    check_probe_index(probe_state.probe_index, step, match_count, probe_row_count);
    check_build_index(probe_state.build_index, step, match_count, probe_row_count);
}

void JoinHashMapTest::prepare_build_data(Buffer<int32_t>* build_data, uint32_t batch_count) {
    build_data->resize(1 + batch_count * config::vector_chunk_size, 0);
    for (size_t i = 0; i < config::vector_chunk_size; i++) {
        for (size_t j = 0; j < batch_count; j++) {
            (*build_data)[1 + j * config::vector_chunk_size + i] = i;
        }
    }
}

void JoinHashMapTest::prepare_probe_data(Buffer<int32_t>* probe_data, uint32_t probe_row_count) {
    probe_data->resize(probe_row_count);
    for (size_t i = 0; i < probe_row_count; i++) {
        (*probe_data)[i] = i;
    }
}

bool JoinHashMapTest::is_check_cur_row_match_count(TJoinOp::type join_type, bool with_other_conjunct) {
    return join_type == TJoinOp::LEFT_OUTER_JOIN && with_other_conjunct;
}

void JoinHashMapTest::prepare_table_items(JoinHashTableItems* table_items, TJoinOp::type join_type,
                                          bool with_other_conjunct, uint32_t batch_count) {
    table_items->join_type = join_type;
    table_items->with_other_conjunct = with_other_conjunct;
    table_items->join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items->next.resize(1 + batch_count * config::vector_chunk_size, 0);
    for (size_t i = 0; i < config::vector_chunk_size; i++) {
        (table_items->next)[1 + i] = 0;
        for (size_t j = 1; j < batch_count; j++) {
            (table_items->next)[1 + j * config::vector_chunk_size + i] = 1 + (j - 1) * config::vector_chunk_size + i;
        }
    }
}

void JoinHashMapTest::prepare_table_items(JoinHashTableItems* table_items, uint32_t row_count) {
    table_items->bucket_size = JoinHashMapHelper::calc_bucket_size(row_count);
    table_items->first.resize(table_items->bucket_size);
    table_items->row_count = row_count;
    table_items->next.resize(row_count + 1);
    table_items->build_pool = std::make_unique<MemPool>();
    table_items->probe_pool = std::make_unique<MemPool>();
    table_items->search_ht_timer = ADD_TIMER(_runtime_profile, "SearchHashTableTimer");
    table_items->output_build_column_timer = ADD_TIMER(_runtime_profile, "OutputBuildColumnTimer");
    table_items->output_probe_column_timer = ADD_TIMER(_runtime_profile, "OutputProbeColumnTimer");
    table_items->output_tuple_column_timer = ADD_TIMER(_runtime_profile, "OutputTupleColumnTimer");
}

void JoinHashMapTest::prepare_probe_state(HashTableProbeState* probe_state, uint32_t probe_row_count) {
    probe_state->probe_row_count = probe_row_count;
    probe_state->cur_probe_index = 0;
    JoinHashMapHelper::prepare_map_index(probe_state);

    for (size_t i = 0; i < probe_row_count; i++) {
        probe_state->next[i] = 1 + 2 * config::vector_chunk_size + i;
    }
}

void JoinHashMapTest::check_int32_column(const ColumnPtr& column, uint32_t row_count, uint32_t start_value) {
    auto* int_32_column = ColumnHelper::as_raw_column<Int32Column>(column);
    auto& data = int_32_column->get_data();

    for (uint32_t i = 0; i < row_count; i++) {
        ASSERT_EQ(data[i], start_value + i);
    }
}

ColumnPtr JoinHashMapTest::create_int32_nullable_column(uint32_t row_count, uint32_t start_value) {
    const auto& int_type_desc = TypeDescriptor(PrimitiveType::TYPE_INT);
    ColumnPtr data_column = ColumnHelper::create_column(int_type_desc, false);
    NullColumnPtr null_column = NullColumn::create();
    for (int32_t i = 0; i < row_count; i++) {
        if ((start_value + i) % 2 == 0) {
            data_column->append_datum(static_cast<int32_t>(start_value + i));
            null_column->append(0);
        } else {
            data_column->append_default();
            null_column->append(1);
        }
    }
    return NullableColumn::create(data_column, null_column);
}

void JoinHashMapTest::check_binary_column(const ColumnPtr& column, uint32_t row_count, uint32_t start_value) {
    auto* binary_column = ColumnHelper::as_raw_column<BinaryColumn>(column);
    auto& data = binary_column->get_data();

    for (uint32_t i = 0; i < row_count; i++) {
        std::string str = std::to_string(start_value + i);
        Slice check_slice;
        check_slice.data = str.data();
        check_slice.size = str.size();
        ASSERT_TRUE(JoinKeyEqual<Slice>()(check_slice, data[i]));
    }
}

void JoinHashMapTest::check_int32_nullable_column(const ColumnPtr& column, uint32_t row_count, uint32_t start_value) {
    auto* nullable_column = ColumnHelper::as_raw_column<NullableColumn>(column);
    auto& data_column = nullable_column->data_column();
    auto& data = ColumnHelper::as_raw_column<Int32Column>(data_column)->get_data();
    const auto& null_column = nullable_column->null_column();
    auto& null_data = null_column->get_data();

    uint32_t index = 0;
    for (uint32_t i = 0; i < row_count; i++) {
        if ((start_value + i) % 2 == 0) {
            ASSERT_EQ(data[index], start_value + i);
            ASSERT_EQ(null_data[index], 0);
            index++;
        }
    }
    ASSERT_EQ(index, data.size());
    ASSERT_EQ(index, null_data.size());
}

ChunkPtr JoinHashMapTest::create_int32_probe_chunk(uint32_t count, uint32_t start_value, bool nullable) {
    auto chunk = std::make_shared<Chunk>();
    if (!nullable) {
        chunk->append_column(create_int32_column(count, start_value), 0);
        chunk->append_column(create_int32_column(count, start_value + 10), 1);
        chunk->append_column(create_int32_column(count, start_value + 20), 2);
    } else {
        chunk->append_column(create_int32_nullable_column(count, start_value), 0);
        chunk->append_column(create_int32_nullable_column(count, start_value + 10), 1);
        chunk->append_column(create_int32_nullable_column(count, start_value + 20), 2);
    }
    return chunk;
}

ChunkPtr JoinHashMapTest::create_binary_probe_chunk(uint32_t count, uint32_t start_value, bool nullable,
                                                    MemPool* mem_pool) {
    auto chunk = std::make_shared<Chunk>();
    if (!nullable) {
        chunk->append_column(create_binary_column(count, start_value, mem_pool), 0);
        chunk->append_column(create_binary_column(count, start_value + 10, mem_pool), 1);
        chunk->append_column(create_binary_column(count, start_value + 20, mem_pool), 2);
    } else {
        //TODO:
    }
    return chunk;
}

ChunkPtr JoinHashMapTest::create_int32_build_chunk(uint32_t count, bool nullable) {
    auto chunk = std::make_shared<Chunk>();
    if (!nullable) {
        chunk->append_column(create_int32_column(count, 0), 3);
        chunk->append_column(create_int32_column(count, 10), 4);
        chunk->append_column(create_int32_column(count, 20), 5);
    } else {
        chunk->append_column(create_int32_nullable_column(count, 0), 3);
        chunk->append_column(create_int32_nullable_column(count, 10), 4);
        chunk->append_column(create_int32_nullable_column(count, 20), 5);
    }
    return chunk;
}

ChunkPtr JoinHashMapTest::create_binary_build_chunk(uint32_t count, bool nullable, MemPool* mem_pool) {
    auto chunk = std::make_shared<Chunk>();
    if (!nullable) {
        chunk->append_column(create_binary_column(count, 0, mem_pool), 3);
        chunk->append_column(create_binary_column(count, 10, mem_pool), 4);
        chunk->append_column(create_binary_column(count, 20, mem_pool), 5);
    } else {
        //TODO: implement
    }
    return chunk;
}

TSlotDescriptor JoinHashMapTest::create_slot_descriptor(const std::string& column_name, PrimitiveType column_type,
                                                        int32_t column_pos, bool nullable) {
    TSlotDescriptorBuilder slot_desc_builder;
    if (column_type == PrimitiveType::TYPE_VARCHAR) {
        return slot_desc_builder.string_type(255)
                .column_name(column_name)
                .column_pos(column_pos)
                .nullable(nullable)
                .build();
    }
    return slot_desc_builder.type(column_type)
            .column_name(column_name)
            .column_pos(column_pos)
            .nullable(nullable)
            .build();
}

void JoinHashMapTest::add_tuple_descriptor(TDescriptorTableBuilder* table_desc_builder, PrimitiveType column_type,
                                           bool nullable) {
    TTupleDescriptorBuilder tuple_desc_builder;

    auto slot_desc_0 = create_slot_descriptor("c0", column_type, 0, nullable);
    auto slot_desc_1 = create_slot_descriptor("c1", column_type, 1, nullable);
    auto slot_desc_2 = create_slot_descriptor("c2", column_type, 2, nullable);

    tuple_desc_builder.add_slot(slot_desc_0);
    tuple_desc_builder.add_slot(slot_desc_1);
    tuple_desc_builder.add_slot(slot_desc_2);

    tuple_desc_builder.build(table_desc_builder);
}

std::shared_ptr<RuntimeProfile> JoinHashMapTest::create_runtime_profile() {
    auto profile = std::make_shared<RuntimeProfile>("test");
    profile->set_metadata(1);
    return profile;
}

std::shared_ptr<RowDescriptor> JoinHashMapTest::create_row_desc(const std::shared_ptr<ObjectPool>& object_pool,
                                                                TDescriptorTableBuilder* table_desc_builder,
                                                                bool nullable) {
    std::vector<TTupleId> row_tuples = std::vector<TTupleId>{0, 1};
    std::vector<bool> nullable_tuples = std::vector<bool>{nullable, nullable};
    DescriptorTbl* tbl = nullptr;
    DescriptorTbl::create(object_pool.get(), table_desc_builder->desc_tbl(), &tbl);

    return std::make_shared<RowDescriptor>(*tbl, row_tuples, nullable_tuples);
}

std::shared_ptr<RowDescriptor> JoinHashMapTest::create_probe_desc(const std::shared_ptr<ObjectPool>& object_pool,
                                                                  TDescriptorTableBuilder* probe_desc_builder,
                                                                  bool nullable) {
    std::vector<TTupleId> row_tuples = std::vector<TTupleId>{0};
    std::vector<bool> nullable_tuples = std::vector<bool>{nullable};
    DescriptorTbl* tbl = nullptr;
    DescriptorTbl::create(object_pool.get(), probe_desc_builder->desc_tbl(), &tbl);

    return std::make_shared<RowDescriptor>(*tbl, row_tuples, nullable_tuples);
}

std::shared_ptr<RowDescriptor> JoinHashMapTest::create_build_desc(const std::shared_ptr<ObjectPool>& object_pool,
                                                                  TDescriptorTableBuilder* build_desc_builder,
                                                                  bool nullable) {
    std::vector<TTupleId> row_tuples = std::vector<TTupleId>{1};
    std::vector<bool> nullable_tuples = std::vector<bool>{nullable};
    DescriptorTbl* tbl = nullptr;
    DescriptorTbl::create(object_pool.get(), build_desc_builder->desc_tbl(), &tbl);

    return std::make_shared<RowDescriptor>(*tbl, row_tuples, nullable_tuples);
}

std::shared_ptr<RuntimeState> JoinHashMapTest::create_runtime_state() {
    TUniqueId fragment_id;
    TQueryOptions query_options;
    TQueryGlobals query_globals;
    auto runtime_state = std::make_shared<RuntimeState>(fragment_id, query_options, query_globals, nullptr);
    runtime_state->init_instance_mem_tracker();
    return runtime_state;
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, JoinKeyHash) {
    auto v1 = JoinKeyHash<int64_t>()(1);
    auto v2 = JoinKeyHash<int32_t>()(1);
    auto v3 = JoinKeyHash<Slice>()(Slice{"abcd", 4});

    ASSERT_EQ(v1, 2592448939l);
    ASSERT_EQ(v2, 98743132903886l);
    ASSERT_EQ(v3, 2777932099l);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, JoinKeyEqual) {
    ASSERT_TRUE(JoinKeyEqual<int32_t>()(1, 1));
    ASSERT_FALSE(JoinKeyEqual<int32_t>()(1, 2));
    ASSERT_TRUE(JoinKeyEqual<Slice>()(Slice{"abcd", 4}, Slice{"abcd", 4}));
    ASSERT_FALSE(JoinKeyEqual<Slice>()(Slice{"abcd", 4}, Slice{"efgh", 4}));
    ASSERT_FALSE(JoinKeyEqual<Slice>()(Slice{"abcd", 4}, Slice{"abc", 3}));
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, CalcBucketNum) {
    uint32_t bucket_num = JoinHashMapHelper::calc_bucket_num(1, 4);
    ASSERT_EQ(2, bucket_num);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, CalcBucketNums) {
    Buffer<int32_t> data{1, 2, 3, 4};
    Buffer<uint32_t> buckets{0, 0, 0, 0};
    Buffer<uint32_t> check_buckets{2, 2, 3, 1};

    JoinHashMapHelper::calc_bucket_nums<int32_t>(data, 4, &buckets, 0, 4);
    for (size_t i = 0; i < buckets.size(); i++) {
        ASSERT_EQ(buckets[i], check_buckets[i]);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, PrepareMapIndex) {
    HashTableProbeState probe_state;
    JoinHashMapHelper::prepare_map_index(&probe_state);

    ASSERT_EQ(probe_state.build_index.size(), config::vector_chunk_size + 8);
    ASSERT_EQ(probe_state.probe_index.size(), config::vector_chunk_size + 8);
    ASSERT_EQ(probe_state.next.size(), config::vector_chunk_size);
    ASSERT_EQ(probe_state.probe_match_index.size(), config::vector_chunk_size);
    ASSERT_EQ(probe_state.probe_match_filter.size(), config::vector_chunk_size);
    ASSERT_EQ(probe_state.buckets.size(), config::vector_chunk_size);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, GetHashKey) {
    auto c1 = JoinHashMapTest::create_int32_column(2, 0);
    auto c2 = JoinHashMapTest::create_int32_column(2, 2);
    Columns columns{c1, c2};
    Buffer<uint8_t> buffer(1024);

    auto slice = JoinHashMapHelper::get_hash_key(columns, 0, buffer.data());
    ASSERT_EQ(slice.size, 8);
    const auto* ptr = reinterpret_cast<const int32_t*>(slice.data);
    ASSERT_EQ(ptr[0], 0);
    ASSERT_EQ(ptr[1], 2);
    ASSERT_EQ(ptr[2], 0);
    ASSERT_EQ(ptr[3], 0);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, CompileFixedSizeKeyColumn) {
    auto type = TypeDescriptor::from_primtive_type(PrimitiveType::TYPE_BIGINT);
    auto data_column = ColumnHelper::create_column(type, false);
    data_column->resize(2);

    auto c1 = JoinHashMapTest::create_int32_column(2, 0);
    auto c2 = JoinHashMapTest::create_int32_column(2, 2);
    Columns columns{c1, c2};

    JoinHashMapHelper::serialize_fixed_size_key_column<PrimitiveType::TYPE_BIGINT>(columns, data_column.get(), 0, 2);

    auto* c3 = ColumnHelper::as_raw_column<Int64Column>(data_column);
    ASSERT_EQ(c3->get_data()[0], 8589934592l);
    ASSERT_EQ(c3->get_data()[1], 12884901889l);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeNullOutput) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    table_items.probe_column_count = 3;

    auto object_pool = std::make_shared<ObjectPool>();
    TDescriptorTableBuilder row_desc_builder;
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);
    auto row_desc = create_row_desc(object_pool, &row_desc_builder, false);
    table_items.probe_slots = row_desc->tuple_descriptors()[0]->slots();

    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);

    auto chunk = std::make_shared<Chunk>();
    auto status = join_hash_map->_probe_null_output(&chunk, 2);
    ASSERT_TRUE(status.ok());

    ASSERT_EQ(chunk->num_columns(), 3);

    for (size_t i = 0; i < chunk->num_columns(); i++) {
        auto null_column = ColumnHelper::as_raw_column<NullableColumn>(chunk->columns()[i])->null_column();
        for (size_t j = 0; j < 2; j++) {
            ASSERT_EQ(null_column->get_data()[j], 1);
        }
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, BuildDefaultOutput) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    table_items.build_column_count = 3;

    auto object_pool = std::make_shared<ObjectPool>();
    TDescriptorTableBuilder row_desc_builder;
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);
    auto row_desc = create_row_desc(object_pool, &row_desc_builder, false);
    table_items.build_slots = row_desc->tuple_descriptors()[0]->slots();

    auto chunk = std::make_shared<Chunk>();
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    auto status = join_hash_map->_build_default_output(&chunk, 2);
    ASSERT_TRUE(status.ok());

    ASSERT_EQ(chunk->num_columns(), 3);

    for (size_t i = 0; i < chunk->num_columns(); i++) {
        auto null_column = ColumnHelper::as_raw_column<NullableColumn>(chunk->columns()[i])->null_column();
        for (size_t j = 0; j < 2; j++) {
            ASSERT_EQ(null_column->get_data()[j], 1);
        }
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, JoinBuildProbeFunc) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    auto type = TypeDescriptor::from_primtive_type(PrimitiveType::TYPE_INT);
    auto build_column = ColumnHelper::create_column(type, false);
    build_column->append_default();
    build_column->append(*JoinHashMapTest::create_int32_column(10, 0), 0, 10);
    auto probe_column = JoinHashMapTest::create_int32_column(10, 0);
    table_items.first.resize(16, 0);
    table_items.key_columns.emplace_back(build_column);
    table_items.bucket_size = 16;
    table_items.row_count = 10;
    table_items.next.resize(11);
    probe_state.probe_row_count = 10;
    probe_state.buckets.resize(config::vector_chunk_size);
    probe_state.next.resize(config::vector_chunk_size, 0);
    Columns probe_columns{probe_column};
    probe_state.key_columns = &probe_columns;

    auto status = JoinBuildFunc<PrimitiveType::TYPE_INT>::prepare(nullptr, &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    JoinBuildFunc<PrimitiveType::TYPE_INT>::construct_hash_table(&table_items, &probe_state);
    JoinProbeFunc<PrimitiveType::TYPE_INT>::prepare(&table_items, &probe_state);
    JoinProbeFunc<PrimitiveType::TYPE_INT>::lookup_init(table_items, &probe_state);

    for (size_t i = 0; i < 10; i++) {
        size_t found_count = 0;
        size_t probe_index = probe_state.next[i];
        auto data = ColumnHelper::as_raw_column<Int32Column>(table_items.key_columns[0])->get_data();
        while (probe_index != 0) {
            if (JoinKeyEqual<int32_t>()(i, data[probe_index])) {
                found_count++;
            }
            probe_index = table_items.next[probe_index];
        }
        ASSERT_EQ(found_count, 1);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, JoinBuildProbeFuncNullable) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    auto type = TypeDescriptor::from_primtive_type(PrimitiveType::TYPE_INT);
    auto build_column = ColumnHelper::create_column(type, true);
    build_column->append_default();
    build_column->append(*JoinHashMapTest::create_int32_nullable_column(10, 0), 0, 10);
    auto probe_column = JoinHashMapTest::create_int32_nullable_column(10, 0);
    table_items.first.resize(16, 0);
    table_items.key_columns.emplace_back(build_column);
    table_items.bucket_size = 16;
    table_items.row_count = 10;
    table_items.next.resize(11);
    probe_state.probe_row_count = 10;
    probe_state.buckets.resize(config::vector_chunk_size);
    probe_state.next.resize(config::vector_chunk_size, 0);
    Columns probe_columns{probe_column};
    probe_state.key_columns = &probe_columns;

    auto status = JoinBuildFunc<TYPE_INT>::prepare(nullptr, &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    JoinBuildFunc<TYPE_INT>::construct_hash_table(&table_items, &probe_state);
    JoinProbeFunc<TYPE_INT>::prepare(&table_items, &probe_state);
    JoinProbeFunc<TYPE_INT>::lookup_init(table_items, &probe_state);

    for (size_t i = 0; i < 10; i++) {
        size_t found_count = 0;
        size_t probe_index = probe_state.next[i];
        auto data_column = ColumnHelper::as_raw_column<NullableColumn>(table_items.key_columns[0])->data_column();
        auto data = ColumnHelper::as_raw_column<Int32Column>(data_column)->get_data();
        while (probe_index != 0) {
            if (JoinKeyEqual<int32_t>()(i, data[probe_index])) {
                found_count++;
            }
            probe_index = table_items.next[probe_index];
        }
        if (i % 2 == 1) {
            ASSERT_EQ(found_count, 0);
        } else {
            ASSERT_EQ(found_count, 1);
        }
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, FixedSizeJoinBuildProbeFunc) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    auto type = TypeDescriptor::from_primtive_type(TYPE_INT);

    auto build_column1 = ColumnHelper::create_column(type, false);
    build_column1->append_default();
    build_column1->append(*JoinHashMapTest::create_int32_column(10, 0), 0, 10);

    auto build_column2 = ColumnHelper::create_column(type, false);
    build_column2->append_default();
    build_column2->append(*JoinHashMapTest::create_int32_column(10, 100), 0, 10);

    auto probe_column1 = JoinHashMapTest::create_int32_column(10, 0);
    auto probe_column2 = JoinHashMapTest::create_int32_column(10, 100);

    table_items.first.resize(16, 0);
    table_items.key_columns.emplace_back(build_column1);
    table_items.key_columns.emplace_back(build_column2);
    table_items.bucket_size = 16;
    table_items.row_count = 10;
    table_items.next.resize(11);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    probe_state.probe_row_count = 10;
    probe_state.buckets.resize(config::vector_chunk_size);
    probe_state.next.resize(config::vector_chunk_size, 0);
    Columns probe_columns{probe_column1, probe_column2};
    probe_state.key_columns = &probe_columns;

    auto status = FixedSizeJoinBuildFunc<TYPE_BIGINT>::prepare(runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinBuildFunc<TYPE_BIGINT>::construct_hash_table(&table_items, &probe_state);

    status = FixedSizeJoinProbeFunc<TYPE_BIGINT>::prepare(&table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinProbeFunc<TYPE_BIGINT>::lookup_init(table_items, &probe_state);

    for (size_t i = 0; i < 10; i++) {
        size_t found_count = 0;
        size_t probe_index = probe_state.next[i];
        auto* data_column = ColumnHelper::as_raw_column<Int64Column>(table_items.build_key_column);
        auto data = data_column->get_data();
        while (probe_index != 0) {
            if (JoinKeyEqual<int64_t>()((100 + i) * (1ul << 32u) + i, data[probe_index])) {
                found_count++;
            }
            probe_index = table_items.next[probe_index];
        }
        ASSERT_EQ(found_count, 1);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, FixedSizeJoinBuildProbeFuncNullable) {
    auto runtime_state = create_runtime_state();
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    runtime_state->init_instance_mem_tracker();

    auto type = TypeDescriptor::from_primtive_type(PrimitiveType::TYPE_INT);

    auto build_column1 = ColumnHelper::create_column(type, true);
    build_column1->append_default();
    build_column1->append(*JoinHashMapTest::create_int32_nullable_column(10, 0), 0, 10);

    auto build_column2 = ColumnHelper::create_column(type, true);
    build_column2->append_default();
    build_column2->append(*JoinHashMapTest::create_int32_nullable_column(10, 100), 0, 10);

    auto probe_column1 = JoinHashMapTest::create_int32_nullable_column(10, 0);
    auto probe_column2 = JoinHashMapTest::create_int32_nullable_column(10, 100);

    table_items.first.resize(16, 0);
    table_items.key_columns.emplace_back(build_column1);
    table_items.key_columns.emplace_back(build_column2);
    table_items.bucket_size = 16;
    table_items.row_count = 10;
    table_items.next.resize(11);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    probe_state.probe_row_count = 10;
    probe_state.buckets.resize(config::vector_chunk_size);
    probe_state.next.resize(config::vector_chunk_size, 0);
    Columns probe_columns{probe_column1, probe_column2};
    probe_state.key_columns = &probe_columns;

    auto status = FixedSizeJoinBuildFunc<TYPE_BIGINT>::prepare(runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinBuildFunc<TYPE_BIGINT>::construct_hash_table(&table_items, &probe_state);

    status = FixedSizeJoinProbeFunc<TYPE_BIGINT>::prepare(&table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinProbeFunc<TYPE_BIGINT>::lookup_init(table_items, &probe_state);

    for (size_t i = 0; i < 10; i++) {
        size_t found_count = 0;
        size_t probe_index = probe_state.next[i];
        auto* data_column = ColumnHelper::as_raw_column<Int64Column>(table_items.build_key_column);
        auto data = data_column->get_data();
        while (probe_index != 0) {
            if (JoinKeyEqual<int64_t>()((100 + i) * (1ul << 32ul) + i, data[probe_index])) {
                found_count++;
            }
            probe_index = table_items.next[probe_index];
        }
        if (i % 2 == 0) {
            ASSERT_EQ(found_count, 1);
        } else {
            ASSERT_EQ(found_count, 0);
        }
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, SerializedJoinBuildProbeFunc) {
    auto runtime_state = create_runtime_state();
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    runtime_state->init_instance_mem_tracker();

    auto type = TypeDescriptor::from_primtive_type(PrimitiveType::TYPE_INT);

    auto build_column1 = ColumnHelper::create_column(type, true);
    build_column1->append_default();
    build_column1->append(*JoinHashMapTest::create_int32_column(10, 0), 0, 10);

    auto build_column2 = ColumnHelper::create_column(type, true);
    build_column2->append_default();
    build_column2->append(*JoinHashMapTest::create_int32_column(10, 100), 0, 10);

    auto probe_column1 = JoinHashMapTest::create_int32_column(10, 0);
    auto probe_column2 = JoinHashMapTest::create_int32_column(10, 100);

    table_items.first.resize(16, 0);
    table_items.key_columns.emplace_back(build_column1);
    table_items.key_columns.emplace_back(build_column2);
    table_items.bucket_size = 16;
    table_items.row_count = 10;
    table_items.next.resize(11);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items.build_pool = std::make_unique<MemPool>();
    table_items.probe_pool = std::make_unique<MemPool>();
    probe_state.probe_row_count = 10;
    probe_state.buckets.resize(config::vector_chunk_size);
    probe_state.next.resize(config::vector_chunk_size, 0);
    Columns probe_columns{probe_column1, probe_column2};
    probe_state.key_columns = &probe_columns;
    Buffer<uint8_t> buffer(1024);

    auto status = SerializedJoinBuildFunc::prepare(runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());

    SerializedJoinBuildFunc::construct_hash_table(&table_items, &probe_state);
    SerializedJoinProbeFunc::prepare(&table_items, &probe_state);
    SerializedJoinProbeFunc::lookup_init(table_items, &probe_state);

    for (size_t i = 0; i < 10; i++) {
        size_t found_count = 0;
        size_t probe_index = probe_state.next[i];
        auto data = table_items.build_slice;
        while (probe_index != 0) {
            if (JoinKeyEqual<Slice>()(JoinHashMapHelper::get_hash_key(*probe_state.key_columns, i, buffer.data()),
                                      data[probe_index])) {
                found_count++;
            }
            probe_index = table_items.next[probe_index];
        }
        ASSERT_EQ(found_count, 1);
    }
    table_items.build_pool.reset();
    table_items.probe_pool.reset();
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, SerializedJoinBuildProbeFuncNullable) {
    auto runtime_state = create_runtime_state();
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    runtime_state->init_instance_mem_tracker();

    auto type = TypeDescriptor::from_primtive_type(PrimitiveType::TYPE_INT);

    auto build_column1 = ColumnHelper::create_column(type, true);
    build_column1->append_default();
    build_column1->append(*JoinHashMapTest::create_int32_nullable_column(10, 0), 0, 10);

    auto build_column2 = ColumnHelper::create_column(type, true);
    build_column2->append_default();
    build_column2->append(*JoinHashMapTest::create_int32_nullable_column(10, 100), 0, 10);

    auto probe_column1 = JoinHashMapTest::create_int32_nullable_column(10, 0);
    auto probe_column2 = JoinHashMapTest::create_int32_nullable_column(10, 100);

    table_items.first.resize(16, 0);
    table_items.key_columns.emplace_back(build_column1);
    table_items.key_columns.emplace_back(build_column2);
    table_items.bucket_size = 16;
    table_items.row_count = 10;
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    table_items.next.resize(11);
    table_items.build_pool = std::make_unique<MemPool>();
    table_items.probe_pool = std::make_unique<MemPool>();
    probe_state.probe_row_count = 10;
    probe_state.buckets.resize(config::vector_chunk_size);
    probe_state.next.resize(config::vector_chunk_size, 0);
    Columns probe_columns{probe_column1, probe_column2};
    probe_state.key_columns = &probe_columns;
    Buffer<uint8_t> buffer(1024);

    auto status = SerializedJoinBuildFunc::prepare(runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());

    SerializedJoinBuildFunc::construct_hash_table(&table_items, &probe_state);
    SerializedJoinProbeFunc::prepare(&table_items, &probe_state);
    SerializedJoinProbeFunc::lookup_init(table_items, &probe_state);

    Columns probe_data_columns;
    probe_data_columns.emplace_back(
            ColumnHelper::as_raw_column<NullableColumn>((*probe_state.key_columns)[0])->data_column());
    probe_data_columns.emplace_back(
            ColumnHelper::as_raw_column<NullableColumn>((*probe_state.key_columns)[1])->data_column());

    for (size_t i = 0; i < 10; i++) {
        size_t found_count = 0;
        size_t probe_index = probe_state.next[i];
        auto data = table_items.build_slice;
        while (probe_index != 0) {
            auto probe_slice = JoinHashMapHelper::get_hash_key(probe_data_columns, i, buffer.data());
            if (JoinKeyEqual<Slice>()(probe_slice, data[probe_index])) {
                found_count++;
            }
            probe_index = table_items.next[probe_index];
        }
        if (i % 2 == 0) {
            ASSERT_EQ(found_count, 1);
        } else {
            ASSERT_EQ(found_count, 0);
        }
    }
    table_items.build_pool.reset();
    table_items.probe_pool.reset();
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtFirstOneToOneAllMatch) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    probe_state.probe_row_count = 4096;
    probe_state.cur_probe_index = 0;
    probe_state.next.resize(config::vector_chunk_size);
    table_items.next.resize(8193);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    JoinHashMapHelper::prepare_map_index(&probe_state);
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    Buffer<int32_t> build_data(8193);
    Buffer<int32_t> probe_data(4096);

    table_items.next[0] = 0;
    for (size_t i = 0; i < 4096; i++) {
        build_data[1 + i] = i;
        build_data[1 + 4096 + i] = i;
        table_items.next[1 + i] = 0;
        table_items.next[1 + 4096 + i] = 1 + i;
    }

    for (size_t i = 0; i < 4096; i++) {
        probe_data[i] = i;
        probe_state.next[i] = 1 + i;
    }

    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht<true>(build_data, probe_data);

    ASSERT_EQ(probe_state.match_flag, JoinMatchFlag::ALL_MATCH_ONE);
    ASSERT_FALSE(probe_state.has_remain);
    ASSERT_EQ(probe_state.cur_probe_index, 0);
    ASSERT_EQ(probe_state.count, 4096);
    ASSERT_EQ(probe_state.cur_row_match_count, 0);
    for (uint32_t i = 0; i < 4096; i++) {
        ASSERT_EQ(probe_state.probe_index[i], i);
        ASSERT_EQ(probe_state.build_index[i], i + 1);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtFirstOneToOneMostMatch) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    probe_state.probe_row_count = 4096;
    probe_state.cur_probe_index = 0;
    probe_state.next.resize(config::vector_chunk_size);
    table_items.next.resize(8193);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    JoinHashMapHelper::prepare_map_index(&probe_state);
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    Buffer<int32_t> build_data(8193);
    Buffer<int32_t> probe_data(4096);

    table_items.next[0] = 0;
    for (size_t i = 0; i < 4096; i++) {
        if (i % 4 == 0) {
            build_data[1 + i] = 100000;
        } else {
            build_data[1 + i] = i;
        }
        build_data[4096 + 1 + i] = i;
        table_items.next[1 + i] = 0;
        table_items.next[4096 + 1 + i] = 1 + i;
    }

    for (size_t i = 0; i < 4096; i++) {
        probe_data[i] = i;
        probe_state.next[i] = 1 + i;
    }

    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht<true>(build_data, probe_data);

    ASSERT_EQ(probe_state.match_flag, JoinMatchFlag::MOST_MATCH_ONE);
    ASSERT_FALSE(probe_state.has_remain);
    ASSERT_EQ(probe_state.cur_probe_index, 0);
    ASSERT_EQ(probe_state.count, 3072);
    ASSERT_EQ(probe_state.cur_row_match_count, 0);
    size_t cur_index = 0;
    for (uint32_t i = 0; i < 4096; i++) {
        if (i % 4 == 0) {
            continue;
        }
        ASSERT_EQ(probe_state.probe_index[cur_index], i);
        ASSERT_EQ(probe_state.build_index[cur_index], i + 1);
        cur_index++;
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtFirstOneToMany) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    probe_state.probe_row_count = 3000;
    probe_state.cur_probe_index = 0;
    probe_state.next.resize(config::vector_chunk_size);
    table_items.next.resize(8193);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    JoinHashMapHelper::prepare_map_index(&probe_state);
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    Buffer<int32_t> build_data(8193);
    Buffer<int32_t> probe_data(3000);

    table_items.next[0] = 0;
    for (size_t i = 0; i < 4096; i++) {
        build_data[1 + i] = i;
        build_data[4096 + 1 + i] = i;
        table_items.next[1 + i] = 0;
        table_items.next[4096 + 1 + i] = 1 + i;
    }

    for (size_t i = 0; i < 3000; i++) {
        probe_data[i] = i;
        probe_state.next[i] = 4096 + 1 + i;
    }

    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht<true>(build_data, probe_data);
    ASSERT_EQ(probe_state.match_flag, JoinMatchFlag::NORMAL);
    ASSERT_TRUE(probe_state.has_remain);
    ASSERT_EQ(probe_state.cur_probe_index, 2048);
    ASSERT_EQ(probe_state.count, 4096);
    ASSERT_EQ(probe_state.cur_row_match_count, 1);
    for (uint32_t i = 0; i < 2048; i += 1) {
        ASSERT_EQ(probe_state.probe_index[2 * i], i);
        ASSERT_EQ(probe_state.build_index[2 * i], i + 1 + 4096);

        ASSERT_EQ(probe_state.probe_index[2 * i + 1], i);
        ASSERT_EQ(probe_state.build_index[2 * i + 1], i + 1);
    }

    join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht<false>(build_data, probe_data);
    ASSERT_EQ(probe_state.match_flag, JoinMatchFlag::NORMAL);
    ASSERT_FALSE(probe_state.has_remain);
    ASSERT_EQ(probe_state.cur_probe_index, 0);
    ASSERT_EQ(probe_state.count, 1904);
    ASSERT_EQ(probe_state.cur_row_match_count, 0);
    for (uint32_t i = 0; i < 952; i += 1) {
        ASSERT_EQ(probe_state.probe_index[2 * i], i + 2048);
        ASSERT_EQ(probe_state.build_index[2 * i], i + 1 + 4096 + 2048);

        ASSERT_EQ(probe_state.probe_index[2 * i + 1], i + 2048);
        ASSERT_EQ(probe_state.build_index[2 * i + 1], i + 1 + 2048);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtForLeftJoinFoundEmpty) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    probe_state.probe_row_count = 3000;
    probe_state.cur_probe_index = 0;
    probe_state.next.resize(config::vector_chunk_size);
    table_items.next.resize(8193);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    JoinHashMapHelper::prepare_map_index(&probe_state);
    auto runtime_state = create_runtime_state();
    runtime_state->init_instance_mem_tracker();

    Buffer<int32_t> build_data(8193);
    Buffer<int32_t> probe_data(3000);

    table_items.next[0] = 0;
    for (size_t i = 0; i < 4096; i++) {
        build_data[1 + i] = i;
        build_data[1 + 4096 + i] = i;
        table_items.next[1 + i] = 0;
        table_items.next[1 + 4096 + i] = 1 + i;
    }
    for (size_t i = 2; i < 4096; i++) {
        table_items.next[i] = 1;
    }

    for (size_t i = 0; i < 3000; i++) {
        probe_data[i] = i;
        probe_state.next[i] = 4096 + 1 + i;
    }

    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_left_outer_join<true>(build_data, probe_data);
    ASSERT_EQ(probe_state.match_flag, JoinMatchFlag::NORMAL);
    ASSERT_TRUE(probe_state.has_remain);
    ASSERT_EQ(probe_state.cur_probe_index, 2048);
    ASSERT_EQ(probe_state.count, 4096);
    ASSERT_EQ(probe_state.cur_row_match_count, 1);
    ASSERT_FALSE(probe_state.has_null_build_tuple);
    for (uint32_t i = 0; i < 2048; i += 1) {
        ASSERT_EQ(probe_state.probe_index[2 * i], i);
        ASSERT_EQ(probe_state.build_index[2 * i], i + 1 + 4096);

        ASSERT_EQ(probe_state.probe_index[2 * i + 1], i);
        ASSERT_EQ(probe_state.build_index[2 * i + 1], i + 1);
    }

    join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_left_outer_join<false>(build_data, probe_data);
    ASSERT_EQ(probe_state.match_flag, JoinMatchFlag::NORMAL);
    ASSERT_FALSE(probe_state.has_remain);
    ASSERT_EQ(probe_state.cur_probe_index, 0);
    ASSERT_EQ(probe_state.count, 1904);
    ASSERT_EQ(probe_state.cur_row_match_count, 0);
    ASSERT_FALSE(probe_state.has_null_build_tuple);
    for (uint32_t i = 0; i < 952; i += 1) {
        ASSERT_EQ(probe_state.probe_index[2 * i], i + 2048);
        ASSERT_EQ(probe_state.build_index[2 * i], i + 1 + 4096 + 2048);

        ASSERT_EQ(probe_state.probe_index[2 * i + 1], i + 2048);
        ASSERT_EQ(probe_state.build_index[2 * i + 1], i + 1 + 2048);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtForLeftJoinNextEmpty) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    Buffer<int32_t> build_data;
    Buffer<int32_t> probe_data;

    uint32_t match_count = 3;
    uint32_t probe_row_count = 3000;

    this->prepare_table_items(&table_items, TJoinOp::LEFT_OUTER_JOIN, true, match_count);
    this->prepare_build_data(&build_data, match_count);
    this->prepare_probe_state(&probe_state, probe_row_count);
    this->prepare_probe_data(&probe_data, probe_row_count);

    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_left_outer_join_with_other_conjunct<true>(build_data, probe_data);

    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 0, match_count, probe_row_count, false);
    this->check_match_index(probe_state.probe_match_index, 0, config::vector_chunk_size, match_count);
}

// Test case for right semi join with other conjunct.
// - One probe row match three build row.
// - All match.
// - The build rows for one probe row, exist in different chunk
// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtForRightSemiJoinWithOtherConjunct) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    Buffer<int32_t> build_data;
    Buffer<int32_t> probe_data;

    uint32_t match_count = 3;
    uint32_t probe_row_count = 2000;

    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    this->prepare_table_items(&table_items, TJoinOp::RIGHT_SEMI_JOIN, true, match_count);
    this->prepare_build_data(&build_data, match_count);
    this->prepare_probe_state(&probe_state, probe_row_count);
    this->prepare_probe_data(&probe_data, probe_row_count);

    // first probe
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_right_semi_join_with_other_conjunct<true>(build_data, probe_data);
    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 0, match_count, probe_row_count, false);

    // second probe
    join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_right_semi_join_with_other_conjunct<false>(build_data, probe_data);
    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 1, match_count, probe_row_count, false);
}

// Test case for right outer join with other conjunct.
// - One probe row match three build row.
// - All match.
// - The build rows for one probe row, exist in different chunk
// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtForRightOuterJoinWithOtherConjunct) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    Buffer<int32_t> build_data;
    Buffer<int32_t> probe_data;

    uint32_t match_count = 3;
    uint32_t probe_row_count = 2000;

    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    this->prepare_table_items(&table_items, TJoinOp::RIGHT_OUTER_JOIN, true, match_count);
    this->prepare_build_data(&build_data, match_count);
    this->prepare_probe_state(&probe_state, probe_row_count);
    this->prepare_probe_data(&probe_data, probe_row_count);

    // first probe
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_right_outer_join_with_other_conjunct<true>(build_data, probe_data);
    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 0, match_count, probe_row_count, false);

    // second probe
    join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_right_outer_join_with_other_conjunct<false>(build_data, probe_data);
    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 1, match_count, probe_row_count, false);
}

// Test case for right anti join with other conjunct.
// - One probe row match three build row.
// - All match.
// - The build rows for one probe row, exist in different chunk
// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, ProbeFromHtForRightAntiJoinWithOtherConjunct) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    Buffer<int32_t> build_data;
    Buffer<int32_t> probe_data;

    uint32_t match_count = 3;
    uint32_t probe_row_count = 2000;

    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    this->prepare_table_items(&table_items, TJoinOp::RIGHT_ANTI_JOIN, true, match_count);
    this->prepare_build_data(&build_data, match_count);
    this->prepare_probe_state(&probe_state, probe_row_count);
    this->prepare_probe_data(&probe_data, probe_row_count);

    // first probe
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_right_anti_join_with_other_conjunct<true>(build_data, probe_data);
    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 0, match_count, probe_row_count, false);

    // second probe
    join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_probe_from_ht_for_right_anti_join_with_other_conjunct<false>(build_data, probe_data);
    this->check_probe_state(table_items, probe_state, JoinMatchFlag::NORMAL, 1, match_count, probe_row_count, false);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, OneKeyJoinHashTable) {
    auto runtime_profile = create_runtime_profile();
    auto runtime_state = create_runtime_state();
    std::shared_ptr<ObjectPool> object_pool = std::make_shared<ObjectPool>();
    config::vector_chunk_size = 4096;

    TDescriptorTableBuilder row_desc_builder;
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);

    std::shared_ptr<RowDescriptor> row_desc = create_row_desc(object_pool, &row_desc_builder, false);
    std::shared_ptr<RowDescriptor> probe_row_desc = create_probe_desc(object_pool, &row_desc_builder, false);
    std::shared_ptr<RowDescriptor> build_row_desc = create_build_desc(object_pool, &row_desc_builder, false);

    HashTableParam param;
    param.with_other_conjunct = false;
    param.join_type = TJoinOp::INNER_JOIN;
    param.row_desc = row_desc.get();
    param.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    param.probe_row_desc = probe_row_desc.get();
    param.build_row_desc = build_row_desc.get();
    param.search_ht_timer = ADD_TIMER(runtime_profile, "SearchHashTableTimer");
    param.output_build_column_timer = ADD_TIMER(runtime_profile, "OutputBuildColumnTimer");
    param.output_probe_column_timer = ADD_TIMER(runtime_profile, "OutputProbeColumnTimer");
    param.output_tuple_column_timer = ADD_TIMER(runtime_profile, "OutputTupleColumnTimer");

    JoinHashTable hash_table;
    hash_table.create(param);

    auto build_chunk = create_int32_build_chunk(10, false);
    auto probe_chunk = create_int32_probe_chunk(5, 1, false);
    Columns probe_key_columns;
    probe_key_columns.emplace_back(probe_chunk->columns()[0]);

    ASSERT_TRUE(hash_table.append_chunk(runtime_state.get(), build_chunk).ok());
    hash_table.get_key_columns().emplace_back(hash_table.get_build_chunk()->columns()[0]);
    ASSERT_TRUE(hash_table.build(runtime_state.get()).ok());

    ChunkPtr result_chunk = std::make_shared<Chunk>();
    bool eos = false;

    ASSERT_TRUE(hash_table.probe(probe_key_columns, &probe_chunk, &result_chunk, &eos).ok());

    ASSERT_EQ(result_chunk->num_columns(), 6);

    ColumnPtr column1 = result_chunk->get_column_by_slot_id(0);
    check_int32_column(column1, 5, 1);
    ColumnPtr column2 = result_chunk->get_column_by_slot_id(1);
    check_int32_column(column2, 5, 11);
    ColumnPtr column3 = result_chunk->get_column_by_slot_id(2);
    check_int32_column(column3, 5, 21);
    ColumnPtr column4 = result_chunk->get_column_by_slot_id(3);
    check_int32_column(column4, 5, 1);
    ColumnPtr column5 = result_chunk->get_column_by_slot_id(4);
    check_int32_column(column5, 5, 11);
    ColumnPtr column6 = result_chunk->get_column_by_slot_id(5);
    check_int32_column(column6, 5, 21);

    hash_table.close();
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, OneNullableKeyJoinHashTable) {
    auto runtime_profile = create_runtime_profile();
    auto runtime_state = create_runtime_state();
    std::shared_ptr<ObjectPool> object_pool = std::make_shared<ObjectPool>();
    config::vector_chunk_size = 4096;

    TDescriptorTableBuilder row_desc_builder;
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, true);
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, true);

    std::shared_ptr<RowDescriptor> row_desc = create_row_desc(object_pool, &row_desc_builder, true);
    std::shared_ptr<RowDescriptor> probe_row_desc = create_probe_desc(object_pool, &row_desc_builder, true);
    std::shared_ptr<RowDescriptor> build_row_desc = create_build_desc(object_pool, &row_desc_builder, true);

    HashTableParam param;
    param.with_other_conjunct = false;
    param.join_type = TJoinOp::INNER_JOIN;
    param.row_desc = row_desc.get();
    param.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    param.probe_row_desc = probe_row_desc.get();
    param.build_row_desc = build_row_desc.get();
    param.search_ht_timer = ADD_TIMER(runtime_profile, "SearchHashTableTimer");
    param.output_build_column_timer = ADD_TIMER(runtime_profile, "OutputBuildColumnTimer");
    param.output_probe_column_timer = ADD_TIMER(runtime_profile, "OutputProbeColumnTimer");
    param.output_tuple_column_timer = ADD_TIMER(runtime_profile, "OutputTupleColumnTimer");

    JoinHashTable hash_table;
    hash_table.create(param);

    auto build_chunk = create_int32_build_chunk(10, true);
    auto probe_chunk = create_int32_probe_chunk(5, 1, true);
    Columns probe_key_columns;
    probe_key_columns.emplace_back(probe_chunk->columns()[0]);

    ASSERT_TRUE(hash_table.append_chunk(runtime_state.get(), build_chunk).ok());
    hash_table.get_key_columns().emplace_back(hash_table.get_build_chunk()->columns()[0]);
    ASSERT_TRUE(hash_table.build(runtime_state.get()).ok());

    ChunkPtr result_chunk = std::make_shared<Chunk>();
    bool eos = false;

    ASSERT_TRUE(hash_table.probe(probe_key_columns, &probe_chunk, &result_chunk, &eos).ok());

    ASSERT_EQ(result_chunk->num_columns(), 6);

    ColumnPtr column1 = result_chunk->get_column_by_slot_id(0);
    check_int32_nullable_column(column1, 5, 1);
    ColumnPtr column2 = result_chunk->get_column_by_slot_id(1);
    check_int32_nullable_column(column2, 5, 11);
    ColumnPtr column3 = result_chunk->get_column_by_slot_id(2);
    check_int32_nullable_column(column3, 5, 21);
    ColumnPtr column4 = result_chunk->get_column_by_slot_id(3);
    check_int32_nullable_column(column4, 5, 1);
    ColumnPtr column5 = result_chunk->get_column_by_slot_id(4);
    check_int32_nullable_column(column5, 5, 11);
    ColumnPtr column6 = result_chunk->get_column_by_slot_id(5);
    check_int32_nullable_column(column6, 5, 21);

    hash_table.close();
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, FixedSizeJoinHashTable) {
    auto runtime_profile = create_runtime_profile();
    auto runtime_state = create_runtime_state();
    std::shared_ptr<ObjectPool> object_pool = std::make_shared<ObjectPool>();
    std::shared_ptr<MemPool> mem_pool = std::make_shared<MemPool>();
    config::vector_chunk_size = 4096;

    TDescriptorTableBuilder row_desc_builder;
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_INT, false);

    std::shared_ptr<RowDescriptor> row_desc = create_row_desc(object_pool, &row_desc_builder, false);
    std::shared_ptr<RowDescriptor> probe_row_desc = create_probe_desc(object_pool, &row_desc_builder, false);
    std::shared_ptr<RowDescriptor> build_row_desc = create_build_desc(object_pool, &row_desc_builder, false);

    HashTableParam param;
    param.with_other_conjunct = false;
    param.join_type = TJoinOp::INNER_JOIN;
    param.row_desc = row_desc.get();
    param.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    param.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});
    param.probe_row_desc = probe_row_desc.get();
    param.build_row_desc = build_row_desc.get();
    param.search_ht_timer = ADD_TIMER(runtime_profile, "SearchHashTableTimer");
    param.output_build_column_timer = ADD_TIMER(runtime_profile, "OutputBuildColumnTimer");
    param.output_probe_column_timer = ADD_TIMER(runtime_profile, "OutputProbeColumnTimer");
    param.output_tuple_column_timer = ADD_TIMER(runtime_profile, "OutputTupleColumnTimer");

    JoinHashTable hash_table;
    hash_table.create(param);

    auto build_chunk = create_int32_build_chunk(10, false);
    auto probe_chunk = create_int32_probe_chunk(5, 1, false);
    Columns probe_key_columns;
    probe_key_columns.emplace_back(probe_chunk->columns()[0]);
    probe_key_columns.emplace_back(probe_chunk->columns()[1]);

    ASSERT_TRUE(hash_table.append_chunk(runtime_state.get(), build_chunk).ok());
    hash_table.get_key_columns().emplace_back(hash_table.get_build_chunk()->columns()[0]);
    hash_table.get_key_columns().emplace_back(hash_table.get_build_chunk()->columns()[1]);
    ASSERT_TRUE(hash_table.build(runtime_state.get()).ok());

    ChunkPtr result_chunk = std::make_shared<Chunk>();
    bool eos = false;

    ASSERT_TRUE(hash_table.probe(probe_key_columns, &probe_chunk, &result_chunk, &eos).ok());

    ASSERT_EQ(result_chunk->num_columns(), 6);

    ColumnPtr column1 = result_chunk->get_column_by_slot_id(0);
    check_int32_column(column1, 5, 1);
    ColumnPtr column2 = result_chunk->get_column_by_slot_id(1);
    check_int32_column(column2, 5, 11);
    ColumnPtr column3 = result_chunk->get_column_by_slot_id(2);
    check_int32_column(column3, 5, 21);
    ColumnPtr column4 = result_chunk->get_column_by_slot_id(3);
    check_int32_column(column4, 5, 1);
    ColumnPtr column5 = result_chunk->get_column_by_slot_id(4);
    check_int32_column(column5, 5, 11);
    ColumnPtr column6 = result_chunk->get_column_by_slot_id(5);
    check_int32_column(column6, 5, 21);

    hash_table.close();
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, SerializeJoinHashTable) {
    auto runtime_profile = create_runtime_profile();
    auto runtime_state = create_runtime_state();

    TDescriptorTableBuilder row_desc_builder;
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_VARCHAR, false);
    add_tuple_descriptor(&row_desc_builder, PrimitiveType::TYPE_VARCHAR, false);

    std::shared_ptr<RowDescriptor> row_desc = create_row_desc(_object_pool, &row_desc_builder, false);
    std::shared_ptr<RowDescriptor> probe_row_desc = create_probe_desc(_object_pool, &row_desc_builder, false);
    std::shared_ptr<RowDescriptor> build_row_desc = create_build_desc(_object_pool, &row_desc_builder, false);

    HashTableParam param;
    param.with_other_conjunct = false;
    param.join_type = TJoinOp::INNER_JOIN;
    param.row_desc = row_desc.get();
    param.join_keys.emplace_back(JoinKeyDesc{TYPE_VARCHAR, false});
    param.join_keys.emplace_back(JoinKeyDesc{TYPE_VARCHAR, false});
    param.probe_row_desc = probe_row_desc.get();
    param.build_row_desc = build_row_desc.get();
    param.search_ht_timer = ADD_TIMER(runtime_profile, "SearchHashTableTimer");
    param.output_build_column_timer = ADD_TIMER(runtime_profile, "OutputBuildColumnTimer");
    param.output_probe_column_timer = ADD_TIMER(runtime_profile, "OutputProbeColumnTimer");
    param.output_tuple_column_timer = ADD_TIMER(runtime_profile, "OutputTupleColumnTimer");

    JoinHashTable hash_table;
    hash_table.create(param);

    auto build_chunk = create_binary_build_chunk(10, false, _mem_pool.get());
    auto probe_chunk = create_binary_probe_chunk(5, 1, false, _mem_pool.get());
    Columns probe_key_columns;
    probe_key_columns.emplace_back(probe_chunk->columns()[0]);
    probe_key_columns.emplace_back(probe_chunk->columns()[1]);

    ASSERT_TRUE(hash_table.append_chunk(runtime_state.get(), build_chunk).ok());
    hash_table.get_key_columns().emplace_back(hash_table.get_build_chunk()->columns()[0]);
    hash_table.get_key_columns().emplace_back(hash_table.get_build_chunk()->columns()[1]);
    ASSERT_TRUE(hash_table.build(runtime_state.get()).ok());

    ChunkPtr result_chunk = std::make_shared<Chunk>();
    bool eos = false;

    ASSERT_TRUE(hash_table.probe(probe_key_columns, &probe_chunk, &result_chunk, &eos).ok());

    ASSERT_EQ(result_chunk->num_columns(), 6);

    ColumnPtr column1 = result_chunk->get_column_by_slot_id(0);
    check_binary_column(column1, 5, 1);
    ColumnPtr column2 = result_chunk->get_column_by_slot_id(1);
    check_binary_column(column2, 5, 11);
    ColumnPtr column3 = result_chunk->get_column_by_slot_id(2);
    check_binary_column(column3, 5, 21);
    ColumnPtr column4 = result_chunk->get_column_by_slot_id(3);
    check_binary_column(column4, 5, 1);
    ColumnPtr column5 = result_chunk->get_column_by_slot_id(4);
    check_binary_column(column5, 5, 11);
    ColumnPtr column6 = result_chunk->get_column_by_slot_id(5);
    check_binary_column(column6, 5, 21);

    hash_table.close();
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, FixedSizeJoinBuildFuncForNotNullableColumn) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    uint32_t build_row_count = 9000;
    uint32_t probe_row_count = 10;

    prepare_table_items(&table_items, build_row_count);
    prepare_probe_state(&probe_state, probe_row_count);

    // Add int column
    auto column_1 = create_column(TYPE_INT);
    column_1->append_default();
    column_1->append(*create_column(TYPE_INT, 0, build_row_count));
    table_items.key_columns.emplace_back(column_1);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Add int column
    auto column_2 = create_column(TYPE_INT);
    column_2->append_default();
    column_2->append(*create_column(TYPE_INT, 0, build_row_count), 0, build_row_count);
    table_items.key_columns.emplace_back(column_2);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Construct Hash Table
    Status status = FixedSizeJoinBuildFunc<TYPE_BIGINT>::prepare(_runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinBuildFunc<TYPE_BIGINT>::construct_hash_table(&table_items, &probe_state);

    // Check
    check_build_index(table_items.first, table_items.next, build_row_count);
    check_build_column(table_items.build_key_column, build_row_count);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, FixedSizeJoinBuildFuncForNullableColumn) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    uint32_t build_row_count = 9000;
    uint32_t probe_row_count = 10;

    prepare_table_items(&table_items, build_row_count);
    prepare_probe_state(&probe_state, probe_row_count);

    // Add int column
    auto nulls_1 = create_bools(build_row_count, 0);
    auto column_1 = create_nullable_column(TYPE_INT);
    column_1->append_datum(0);
    column_1->append(*create_nullable_column(TYPE_INT, nulls_1, 0, build_row_count));
    table_items.key_columns.emplace_back(column_1);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Add int column
    auto nulls_2 = create_bools(build_row_count, 0);
    auto column_2 = create_nullable_column(TYPE_INT);
    column_2->append_datum(0);
    column_2->append(*create_nullable_column(TYPE_INT, nulls_2, 0, build_row_count), 0, build_row_count);
    table_items.key_columns.emplace_back(column_2);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Construct Hash Table
    Status status = FixedSizeJoinBuildFunc<TYPE_BIGINT>::prepare(_runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinBuildFunc<TYPE_BIGINT>::construct_hash_table(&table_items, &probe_state);

    // Check
    check_build_index(table_items.first, table_items.next, build_row_count);
    check_build_column(table_items.build_key_column, build_row_count);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, FixedSizeJoinBuildFuncForPartialNullableColumn) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    uint32_t build_row_count = 9000;
    uint32_t probe_row_count = 10;

    prepare_table_items(&table_items, build_row_count);
    prepare_probe_state(&probe_state, probe_row_count);

    // Add int column
    auto nulls_1 = create_bools(build_row_count, 3);
    auto column_1 = create_nullable_column(TYPE_INT);
    column_1->append_datum(0);
    column_1->append(*create_nullable_column(TYPE_INT, nulls_1, 0, build_row_count));
    table_items.key_columns.emplace_back(column_1);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Add int column
    auto nulls_2 = create_bools(build_row_count, 2);
    auto column_2 = create_nullable_column(TYPE_INT);
    column_2->append_datum(0);
    column_2->append(*create_nullable_column(TYPE_INT, nulls_2, 0, build_row_count), 0, build_row_count);
    table_items.key_columns.emplace_back(column_2);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Construct Hash Table
    Status status = FixedSizeJoinBuildFunc<TYPE_BIGINT>::prepare(_runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    FixedSizeJoinBuildFunc<TYPE_BIGINT>::construct_hash_table(&table_items, &probe_state);

    // Check
    auto nulls = create_bools(build_row_count, 4);
    check_build_index(nulls, table_items.first, table_items.next, build_row_count);
    check_build_column(nulls, table_items.build_key_column, build_row_count);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, SerializedJoinBuildFuncForNotNullableColumn) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    uint32_t build_row_count = 9000;
    uint32_t probe_row_count = 10;

    prepare_table_items(&table_items, build_row_count);
    prepare_probe_state(&probe_state, probe_row_count);

    // Add int column
    auto column_1 = create_column(TYPE_INT);
    column_1->append_default();
    column_1->append(*create_column(TYPE_INT, 0, build_row_count));
    table_items.key_columns.emplace_back(column_1);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Add binary column
    auto column_2 = create_column(TYPE_VARCHAR);
    column_2->append_default();
    column_2->append(*create_column(TYPE_VARCHAR, 0, build_row_count), 0, build_row_count);
    table_items.key_columns.emplace_back(column_2);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_VARCHAR, false});

    // Construct Hash Table
    Status status = SerializedJoinBuildFunc::prepare(_runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    SerializedJoinBuildFunc::construct_hash_table(&table_items, &probe_state);

    // Check
    check_build_index(table_items.first, table_items.next, build_row_count);
    check_build_slice(table_items.build_slice, build_row_count);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, SerializedJoinBuildFuncForNullableColumn) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    uint32_t build_row_count = 9000;
    uint32_t probe_row_count = 10;

    prepare_table_items(&table_items, build_row_count);
    prepare_probe_state(&probe_state, probe_row_count);

    // Add int column
    auto nulls_1 = create_bools(build_row_count, 0);
    auto column_1 = create_nullable_column(TYPE_INT);
    column_1->append_datum(0);
    column_1->append(*create_nullable_column(TYPE_INT, nulls_1, 0, build_row_count));
    table_items.key_columns.emplace_back(column_1);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Add binary column
    auto nulls_2 = create_bools(build_row_count, 0);
    auto column_2 = create_nullable_column(TYPE_VARCHAR);
    column_2->append_datum(Slice());
    column_2->append(*create_nullable_column(TYPE_VARCHAR, nulls_2, 0, build_row_count), 0, build_row_count);
    table_items.key_columns.emplace_back(column_2);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_VARCHAR, false});

    // Construct Hash Table
    Status status = SerializedJoinBuildFunc::prepare(_runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    SerializedJoinBuildFunc::construct_hash_table(&table_items, &probe_state);

    // Check
    check_build_index(table_items.first, table_items.next, build_row_count);
    check_build_slice(table_items.build_slice, build_row_count);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, SerializedJoinBuildFuncForPartialNullColumn) {
    JoinHashTableItems table_items;
    HashTableProbeState probe_state;
    uint32_t build_row_count = 9000;
    uint32_t probe_row_count = 10;

    prepare_table_items(&table_items, build_row_count);
    prepare_probe_state(&probe_state, probe_row_count);

    // Add int column
    auto nulls_1 = create_bools(build_row_count, 3);
    auto column_1 = create_nullable_column(TYPE_INT);
    column_1->append_datum(0);
    column_1->append(*create_nullable_column(TYPE_INT, nulls_1, 0, build_row_count));
    table_items.key_columns.emplace_back(column_1);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_INT, false});

    // Add binary column
    auto nulls_2 = create_bools(build_row_count, 2);
    auto column_2 = create_nullable_column(TYPE_VARCHAR);
    column_2->append_datum(Slice());
    column_2->append(*create_nullable_column(TYPE_VARCHAR, nulls_2, 0, build_row_count), 0, build_row_count);
    table_items.key_columns.emplace_back(column_2);
    table_items.join_keys.emplace_back(JoinKeyDesc{TYPE_VARCHAR, false});

    // Construct Hash Table
    Status status = SerializedJoinBuildFunc::prepare(_runtime_state.get(), &table_items, &probe_state);
    ASSERT_TRUE(status.ok());
    SerializedJoinBuildFunc::construct_hash_table(&table_items, &probe_state);

    // Check
    auto nulls = create_bools(build_row_count, 4);
    check_build_index(nulls, table_items.first, table_items.next, build_row_count);
    check_build_slice(nulls, table_items.build_slice, build_row_count);
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, BuildTupleOutputForTupleNotExist1) {
    uint32_t build_row_count = 10;
    uint32_t probe_row_count = 5;
    auto uint8_array_1 = create_bools(build_row_count, 2);
    auto uint8_array_2 = create_bools(build_row_count, 3);

    ColumnPtr build_tuple_column_1 = create_tuple_column(uint8_array_1);
    ColumnPtr build_tuple_column_2 = create_tuple_column(uint8_array_2);

    JoinHashTableItems table_items;
    table_items.build_chunk = std::make_shared<Chunk>();
    table_items.build_chunk->append_tuple_column(build_tuple_column_1, 0);
    table_items.build_chunk->append_tuple_column(build_tuple_column_2, 1);
    table_items.output_build_tuple_ids = {0, 1};

    HashTableProbeState probe_state;
    probe_state.has_null_build_tuple = false;
    probe_state.count = probe_row_count;
    for (uint32_t i = 0; i < probe_row_count; i++) {
        probe_state.build_index.emplace_back(i + 1);
    }

    ChunkPtr probe_chunk = std::make_shared<Chunk>();
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_build_tuple_output(&probe_chunk);

    // check
    ASSERT_EQ(probe_chunk->num_columns(), 2);
    ColumnPtr probe_tuple_column_1 = probe_chunk->get_tuple_column_by_id(0);
    ColumnPtr probe_tuple_column_2 = probe_chunk->get_tuple_column_by_id(1);

    for (uint32_t i = 0; i < probe_row_count; i++) {
        ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), (i + 1) % 2 == 0);
    }
    for (uint32_t i = 0; i < probe_row_count; i++) {
        ASSERT_EQ(probe_tuple_column_2->get(i).get_int8(), (i + 1) % 3 == 0);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, BuildTupleOutputForTupleNotExist2) {
    // prepare data
    uint32_t build_row_count = 10;
    uint32_t probe_row_count = 5;
    auto uint8_array_1 = create_bools(build_row_count, 2);
    auto uint8_array_2 = create_bools(build_row_count, 3);

    ColumnPtr build_tuple_column_1 = create_tuple_column(uint8_array_1);
    ColumnPtr build_tuple_column_2 = create_tuple_column(uint8_array_2);

    JoinHashTableItems table_items;
    table_items.build_chunk = std::make_shared<Chunk>();
    table_items.build_chunk->append_tuple_column(build_tuple_column_1, 0);
    table_items.build_chunk->append_tuple_column(build_tuple_column_2, 1);
    table_items.output_build_tuple_ids = {0, 1};

    HashTableProbeState probe_state;
    probe_state.has_null_build_tuple = true;
    probe_state.count = probe_row_count;
    for (uint32_t i = 0; i < probe_row_count; i++) {
        if (i == 1 || i == 2) {
            probe_state.build_index.emplace_back(0);
        } else {
            probe_state.build_index.emplace_back(i + 1);
        }
    }

    // exec
    ChunkPtr probe_chunk = std::make_shared<Chunk>();
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_build_tuple_output(&probe_chunk);

    // check
    ASSERT_EQ(probe_chunk->num_columns(), 2);
    ColumnPtr probe_tuple_column_1 = probe_chunk->get_tuple_column_by_id(0);
    ColumnPtr probe_tuple_column_2 = probe_chunk->get_tuple_column_by_id(1);

    for (uint32_t i = 0; i < probe_row_count; i++) {
        if (i == 1 || i == 2) {
            ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), 0);
        } else {
            ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), (i + 1) % 2 == 0);
        }
    }
    for (uint32_t i = 0; i < probe_row_count; i++) {
        if (i == 1 || i == 2) {
            ASSERT_EQ(probe_tuple_column_2->get(i).get_int8(), 0);
        } else {
            ASSERT_EQ(probe_tuple_column_2->get(i).get_int8(), (i + 1) % 3 == 0);
        }
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, BuildTupleOutputForTupleExist1) {
    // prepare data
    uint32_t probe_row_count = 5;

    JoinHashTableItems table_items;
    table_items.build_chunk = std::make_shared<Chunk>();
    table_items.output_build_tuple_ids = {0, 1};
    table_items.right_to_nullable = true;

    HashTableProbeState probe_state;
    probe_state.has_null_build_tuple = true;
    probe_state.count = probe_row_count;
    for (uint32_t i = 0; i < probe_row_count; i++) {
        probe_state.build_index.emplace_back(i + 1);
    }

    // exec
    ChunkPtr probe_chunk = std::make_shared<Chunk>();
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_build_tuple_output(&probe_chunk);

    // check
    ASSERT_EQ(probe_chunk->num_columns(), 2);
    ColumnPtr probe_tuple_column_1 = probe_chunk->get_tuple_column_by_id(0);
    ColumnPtr probe_tuple_column_2 = probe_chunk->get_tuple_column_by_id(1);

    for (uint32_t i = 0; i < probe_row_count; i++) {
        ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), 1);
    }
    for (uint32_t i = 0; i < probe_row_count; i++) {
        ASSERT_EQ(probe_tuple_column_2->get(i).get_int8(), 1);
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, BuildTupleOutputForTupleExist2) {
    // prepare data
    uint32_t probe_row_count = 5;

    JoinHashTableItems table_items;
    table_items.build_chunk = std::make_shared<Chunk>();
    table_items.output_build_tuple_ids = {0, 1};
    table_items.right_to_nullable = true;

    HashTableProbeState probe_state;
    probe_state.has_null_build_tuple = true;
    probe_state.count = probe_row_count;
    for (uint32_t i = 0; i < probe_row_count; i++) {
        if (i == 1 || i == 2) {
            probe_state.build_index.emplace_back(0);
        } else {
            probe_state.build_index.emplace_back(1);
        }
    }

    // exec
    ChunkPtr probe_chunk = std::make_shared<Chunk>();
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_build_tuple_output(&probe_chunk);

    // check
    ASSERT_EQ(probe_chunk->num_columns(), 2);
    ColumnPtr probe_tuple_column_1 = probe_chunk->get_tuple_column_by_id(0);
    ColumnPtr probe_tuple_column_2 = probe_chunk->get_tuple_column_by_id(1);

    for (uint32_t i = 0; i < probe_row_count; i++) {
        if (i == 1 || i == 2) {
            ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), 0);
        } else {
            ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), 1);
        }
    }
    for (uint32_t i = 0; i < probe_row_count; i++) {
        if (i == 1 || i == 2) {
            ASSERT_EQ(probe_tuple_column_1->get(i).get_int8(), 0);
        } else {
            ASSERT_EQ(probe_tuple_column_2->get(i).get_int8(), 1);
        }
    }
}

// NOLINTNEXTLINE
TEST_F(JoinHashMapTest, BuildTupleOutputForTupleExist3) {
    // prepare data
    uint32_t probe_row_count = 5;

    JoinHashTableItems table_items;
    table_items.build_chunk = std::make_shared<Chunk>();
    table_items.output_build_tuple_ids = {0, 1};
    table_items.right_to_nullable = false;

    HashTableProbeState probe_state;
    probe_state.has_null_build_tuple = true;
    probe_state.count = probe_row_count;
    for (uint32_t i = 0; i < probe_row_count; i++) {
        probe_state.build_index.emplace_back(i + 1);
    }

    // exec
    ChunkPtr probe_chunk = std::make_shared<Chunk>();
    auto join_hash_map = std::make_unique<JoinHashMapForOneKey(TYPE_INT)>(&table_items, &probe_state);
    join_hash_map->_build_tuple_output(&probe_chunk);

    // check
    ASSERT_EQ(probe_chunk->num_columns(), 0);
}

} // namespace starrocks::vectorized
