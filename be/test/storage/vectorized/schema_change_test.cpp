// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "storage/vectorized/schema_change.h"

#include "column/datum_convert.h"
#include "gtest/gtest.h"
#include "storage/rowset/rowset_factory.h"
#include "storage/storage_engine.h"
#include "storage/vectorized/chunk_helper.h"
#include "storage/vectorized/convert_helper.h"
#include "util/file_utils.h"
#include "util/logging.h"

namespace starrocks::vectorized {

class SchemaChangeTest : public testing::Test {
    void SetUp() override { _sc_procedure = nullptr; }

    void TearDown() override {
        if (_sc_procedure != nullptr) {
            delete _sc_procedure;
        }
    }

    void SetCreateTabletReq(TCreateTabletReq* request, int64_t tablet_id) {
        request->tablet_id = tablet_id;
        request->__set_version(1);
        request->__set_version_hash(0);
        request->tablet_schema.schema_hash = 270068375;
        request->tablet_schema.short_key_column_count = 2;
        request->tablet_schema.keys_type = TKeysType::DUP_KEYS;
        request->tablet_schema.storage_type = TStorageType::COLUMN;
    }

    void AddColumn(TCreateTabletReq* request, std::string column_name, TPrimitiveType::type type, bool is_key) {
        TColumn c;
        c.column_name = column_name;
        c.__set_is_key(is_key);
        c.column_type.type = type;
        request->tablet_schema.columns.push_back(c);
    }

    void CreateSrcTablet(TTabletId tablet_id) {
        StorageEngine* engine = StorageEngine::instance();
        TCreateTabletReq create_tablet_req;
        SetCreateTabletReq(&create_tablet_req, tablet_id);
        AddColumn(&create_tablet_req, "k1", TPrimitiveType::INT, true);
        AddColumn(&create_tablet_req, "k2", TPrimitiveType::INT, true);
        AddColumn(&create_tablet_req, "v1", TPrimitiveType::INT, false);
        AddColumn(&create_tablet_req, "v2", TPrimitiveType::INT, false);
        Status res = engine->create_tablet(create_tablet_req);
        ASSERT_TRUE(res.ok());
        TabletSharedPtr tablet = engine->tablet_manager()->get_tablet(create_tablet_req.tablet_id);
        vectorized::Schema base_schema = ChunkHelper::convert_schema_to_format_v2(tablet->tablet_schema());
        ChunkPtr base_chunk = ChunkHelper::new_chunk(base_schema, config::vector_chunk_size);
        for (size_t i = 0; i < 4; ++i) {
            ColumnPtr& base_col = base_chunk->get_column_by_index(i);
            for (size_t j = 0; j < 4; ++j) {
                Datum datum;
                if (i != 1) {
                    datum.set_int32(i + 1);
                } else {
                    datum.set_int32(4 - j);
                }
                base_col->append_datum(datum);
            }
        }
        RowsetWriterContext writer_context(kDataFormatUnknown, kDataFormatV2);
        writer_context.rowset_id = engine->next_rowset_id();
        writer_context.tablet_uid = tablet->tablet_uid();
        writer_context.tablet_id = tablet->tablet_id();
        writer_context.tablet_schema_hash = tablet->schema_hash();
        writer_context.rowset_path_prefix = tablet->schema_hash_path();
        writer_context.tablet_schema = &(tablet->tablet_schema());
        writer_context.rowset_state = VISIBLE;
        writer_context.version = Version(3, 3);
        writer_context.version_hash = 0;
        std::unique_ptr<RowsetWriter> rowset_writer;
        ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());
        EXPECT_EQ(OLAP_SUCCESS, rowset_writer->add_chunk(*base_chunk));
        EXPECT_EQ(OLAP_SUCCESS, rowset_writer->flush());
        RowsetSharedPtr new_rowset = rowset_writer->build();
        ASSERT_TRUE(new_rowset != nullptr);
        ASSERT_TRUE(tablet->add_rowset(new_rowset, false).ok());
    }

    void SetTabletSchema(const std::string& name, const std::string& type, const std::string& aggregation,
                         uint32_t length, bool is_allow_null, bool is_key, TabletSchema* tablet_schema) {
        TabletSchemaPB tablet_schema_pb;
        ColumnPB* column = tablet_schema_pb.add_column();
        column->set_unique_id(0);
        column->set_name(name);
        column->set_type(type);
        column->set_is_key(is_key);
        column->set_is_nullable(is_allow_null);
        column->set_length(length);
        column->set_aggregation(aggregation);
        tablet_schema->init_from_pb(tablet_schema_pb);
    }

    template <typename T>
    void test_convert_to_varchar(FieldType type, int type_size, T val, const std::string& expect_val) {
        TabletSchema src_tablet_schema;
        SetTabletSchema("SrcColumn", field_type_to_string(type), "REPLACE", type_size, false, false,
                        &src_tablet_schema);

        TabletSchema dst_tablet_schema;
        SetTabletSchema("VarcharColumn", "VARCHAR", "REPLACE", 255, false, false, &dst_tablet_schema);

        Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
        Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

        Datum src_datum;
        src_datum.set<T>(val);
        Datum dst_datum;
        auto converter = vectorized::get_type_converter(type, OLAP_FIELD_TYPE_VARCHAR);
        std::unique_ptr<MemPool> mem_pool(new MemPool());
        Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
        ASSERT_TRUE(st.ok());

        EXPECT_EQ(expect_val, dst_datum.get_slice().to_string());
    }

    template <typename T>
    void test_convert_from_varchar(FieldType type, int type_size, std::string val, T expect_val) {
        TabletSchema src_tablet_schema;
        SetTabletSchema("VarcharColumn", "VARCHAR", "REPLACE", 255, false, false, &src_tablet_schema);

        TabletSchema dst_tablet_schema;
        SetTabletSchema("DstColumn", field_type_to_string(type), "REPLACE", type_size, false, false,
                        &dst_tablet_schema);

        Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
        Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

        Datum src_datum;
        Slice slice;
        slice.data = (char*)val.data();
        slice.size = val.size();
        src_datum.set_slice(slice);
        Datum dst_datum;
        auto converter = vectorized::get_type_converter(OLAP_FIELD_TYPE_VARCHAR, type);
        std::unique_ptr<MemPool> mem_pool(new MemPool());
        Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
        ASSERT_TRUE(st.ok());

        EXPECT_EQ(expect_val, dst_datum.get<T>());
    }

    SchemaChange* _sc_procedure;
};

TEST_F(SchemaChangeTest, convert_tinyint_to_varchar) {
    test_convert_to_varchar<int8_t>(OLAP_FIELD_TYPE_TINYINT, 1, 127, "127");
}

TEST_F(SchemaChangeTest, convert_smallint_to_varchar) {
    test_convert_to_varchar<int16_t>(OLAP_FIELD_TYPE_SMALLINT, 2, 32767, "32767");
}

TEST_F(SchemaChangeTest, convert_int_to_varchar) {
    test_convert_to_varchar<int32_t>(OLAP_FIELD_TYPE_INT, 4, 2147483647, "2147483647");
}

TEST_F(SchemaChangeTest, convert_bigint_to_varchar) {
    test_convert_to_varchar<int64_t>(OLAP_FIELD_TYPE_BIGINT, 8, 9223372036854775807, "9223372036854775807");
}

TEST_F(SchemaChangeTest, convert_largeint_to_varchar) {
    test_convert_to_varchar<int128_t>(OLAP_FIELD_TYPE_LARGEINT, 16, 1701411834604690, "1701411834604690");
}

TEST_F(SchemaChangeTest, convert_float_to_varchar) {
    test_convert_to_varchar<float>(OLAP_FIELD_TYPE_FLOAT, 4, 3.40282e+38, "3.40282e+38");
}

TEST_F(SchemaChangeTest, convert_double_to_varchar) {
    test_convert_to_varchar<double>(OLAP_FIELD_TYPE_DOUBLE, 8, 123.456, "123.456");
}

TEST_F(SchemaChangeTest, convert_varchar_to_tinyint) {
    test_convert_from_varchar<int8_t>(OLAP_FIELD_TYPE_TINYINT, 1, "127", 127);
}

TEST_F(SchemaChangeTest, convert_varchar_to_smallint) {
    test_convert_from_varchar<int16_t>(OLAP_FIELD_TYPE_SMALLINT, 2, "32767", 32767);
}

TEST_F(SchemaChangeTest, convert_varchar_to_int) {
    test_convert_from_varchar<int32_t>(OLAP_FIELD_TYPE_INT, 4, "2147483647", 2147483647);
}

TEST_F(SchemaChangeTest, convert_varchar_to_bigint) {
    test_convert_from_varchar<int64_t>(OLAP_FIELD_TYPE_BIGINT, 8, "9223372036854775807", 9223372036854775807);
}

TEST_F(SchemaChangeTest, convert_varchar_to_largeint) {
    test_convert_from_varchar<int128_t>(OLAP_FIELD_TYPE_LARGEINT, 16, "1701411834604690", 1701411834604690);
}

TEST_F(SchemaChangeTest, convert_varchar_to_float) {
    test_convert_from_varchar<float>(OLAP_FIELD_TYPE_FLOAT, 4, "3.40282e+38", 3.40282e+38);
}

TEST_F(SchemaChangeTest, convert_varchar_to_double) {
    test_convert_from_varchar<double>(OLAP_FIELD_TYPE_DOUBLE, 8, "123.456", 123.456);
}

TEST_F(SchemaChangeTest, convert_float_to_double) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("SrcColumn", "FLOAT", "REPLACE", 4, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("VarcharColumn", "DOUBLE", "REPLACE", 8, false, false, &dst_tablet_schema);

    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    Datum src_datum;
    src_datum.set_float(1.2345);
    Datum dst_datum;
    auto converter = vectorized::get_type_converter(OLAP_FIELD_TYPE_FLOAT, OLAP_FIELD_TYPE_DOUBLE);
    std::unique_ptr<MemPool> mem_pool(new MemPool());
    Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
    ASSERT_TRUE(st.ok());

    EXPECT_EQ(1.2345, dst_datum.get_double());
}

TEST_F(SchemaChangeTest, convert_datetime_to_date) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("DateTimeColumn", "DATETIME", "REPLACE", 8, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("DateColumn", "DATE", "REPLACE", 3, false, false, &dst_tablet_schema);

    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    std::unique_ptr<MemPool> mem_pool(new MemPool());
    Datum src_datum;
    std::string origin_val = "2021-09-28 16:07:00";

    tm time_tm;
    strptime(origin_val.c_str(), "%Y-%m-%d %H:%M:%S", &time_tm);
    TimestampValue timestamp = TimestampValue::create(2021, 9, 28, 0, 0, 0);
    int64_t value = timestamp.timestamp();
    src_datum.set_int64(value);
    Datum dst_datum;
    auto converter = vectorized::get_type_converter(OLAP_FIELD_TYPE_DATETIME, OLAP_FIELD_TYPE_DATE);

    Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
    ASSERT_TRUE(st.ok());

    int dst_value = (2021 << 9) + (9 << 5) + 28;
    EXPECT_EQ(dst_value, dst_datum.get_uint24());
}

TEST_F(SchemaChangeTest, convert_date_to_datetime) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("DateColumn", "DATE", "REPLACE", 3, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("DateTimeColumn", "DATETIME", "REPLACE", 8, false, false, &dst_tablet_schema);

    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));
    std::unique_ptr<MemPool> mem_pool(new MemPool());
    Datum src_datum;
    std::string origin_val = "2021-09-28";
    tm time_tm;
    strptime(origin_val.c_str(), "%Y-%m-%d", &time_tm);

    DateValue date_v2;
    date_v2.from_date(2021, 9, 28);
    src_datum.set_date(date_v2);
    Datum dst_datum;
    auto converter = vectorized::get_type_converter(OLAP_FIELD_TYPE_DATE, OLAP_FIELD_TYPE_DATETIME);

    Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
    ASSERT_TRUE(st.ok());

    int64_t dst_value = (2021 * 10000L + 9 * 100L + 28) * 1000000L;
    EXPECT_EQ(dst_value, dst_datum.get_int64());
}

TEST_F(SchemaChangeTest, convert_int_to_date_v2) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("IntColumn", "INT", "REPLACE", 4, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("DateColumn", "DATE V2", "REPLACE", 3, false, false, &dst_tablet_schema);

    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    std::unique_ptr<MemPool> mem_pool(new MemPool());
    Datum src_datum;
    std::string origin_val = "2021-09-28";
    tm time_tm;
    strptime(origin_val.c_str(), "%Y-%m-%d", &time_tm);
    src_datum.set_int32(20210928);
    Datum dst_datum;
    auto converter = vectorized::get_type_converter(OLAP_FIELD_TYPE_INT, OLAP_FIELD_TYPE_DATE_V2);

    Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
    ASSERT_TRUE(st.ok());

    EXPECT_EQ("2021-09-28", dst_datum.get_date().to_string());
}

TEST_F(SchemaChangeTest, convert_int_to_date) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("IntColumn", "INT", "REPLACE", 4, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("DateColumn", "DATE", "REPLACE", 3, false, false, &dst_tablet_schema);

    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    std::unique_ptr<MemPool> mem_pool(new MemPool());
    Datum src_datum;
    std::string origin_val = "2021-09-28";
    tm time_tm;
    strptime(origin_val.c_str(), "%Y-%m-%d", &time_tm);
    src_datum.set_int32(20210928);
    Datum dst_datum;
    auto converter = vectorized::get_type_converter(OLAP_FIELD_TYPE_INT, OLAP_FIELD_TYPE_DATE);

    Status st = converter->convert_datum(f.type().get(), src_datum, f2.type().get(), dst_datum, mem_pool.get());
    ASSERT_TRUE(st.ok());

    int dst_value = (time_tm.tm_year + 1900) * 16 * 32 + (time_tm.tm_mon + 1) * 32 + time_tm.tm_mday;
    EXPECT_EQ(dst_value, dst_datum.get_uint24());
}

TEST_F(SchemaChangeTest, convert_int_to_bitmap) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("IntColumn", "INT", "REPLACE", 4, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("BitmapColumn", "OBJECT", "BITMAP_UNION", 8, false, false, &dst_tablet_schema);

    ChunkPtr src_chunk = ChunkHelper::new_chunk(ChunkHelper::convert_schema_to_format_v2(src_tablet_schema), 4096);
    ChunkPtr dst_chunk = ChunkHelper::new_chunk(ChunkHelper::convert_schema_to_format_v2(dst_tablet_schema), 4096);
    ColumnPtr& src_col = src_chunk->get_column_by_index(0);
    ColumnPtr& dst_col = dst_chunk->get_column_by_index(0);
    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    Datum src_datum;
    src_datum.set_int32(2);
    src_col->append_datum(src_datum);

    auto converter = vectorized::get_materialized_converter(OLAP_FIELD_TYPE_INT, OLAP_MATERIALIZE_TYPE_BITMAP);
    Status st = converter->convert_materialized(src_col, dst_col, f.type().get(), src_tablet_schema.column(0));
    ASSERT_TRUE(st.ok());

    Datum dst_datum = dst_col->get(0);
    const BitmapValue* bitmap_val = dst_datum.get_bitmap();
    EXPECT_EQ(bitmap_val->cardinality(), 1);
}

TEST_F(SchemaChangeTest, convert_varchar_to_hll) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("IntColumn", "VARCHAR", "REPLACE", 255, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("HLLColumn", "HLL", "HLL_UNION", 8, false, false, &dst_tablet_schema);

    ChunkPtr src_chunk = ChunkHelper::new_chunk(ChunkHelper::convert_schema_to_format_v2(src_tablet_schema), 4096);
    ChunkPtr dst_chunk = ChunkHelper::new_chunk(ChunkHelper::convert_schema_to_format_v2(dst_tablet_schema), 4096);
    ColumnPtr& src_col = src_chunk->get_column_by_index(0);
    ColumnPtr& dst_col = dst_chunk->get_column_by_index(0);
    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    Datum src_datum;
    std::string str = "test string";
    Slice slice(str.data(), str.size());
    src_datum.set_slice(slice);
    src_col->append_datum(src_datum);

    auto converter = vectorized::get_materialized_converter(OLAP_FIELD_TYPE_VARCHAR, OLAP_MATERIALIZE_TYPE_HLL);
    Status st = converter->convert_materialized(src_col, dst_col, f.type().get(), src_tablet_schema.column(0));
    ASSERT_TRUE(st.ok());

    Datum dst_datum = dst_col->get(0);
    const HyperLogLog* hll = dst_datum.get_hyperloglog();
    EXPECT_EQ(hll->estimate_cardinality(), 1);
}

TEST_F(SchemaChangeTest, convert_int_to_count) {
    TabletSchema src_tablet_schema;
    SetTabletSchema("IntColumn", "INT", "REPLACE", 4, false, false, &src_tablet_schema);

    TabletSchema dst_tablet_schema;
    SetTabletSchema("CountColumn", "BIGINT", "SUM", 8, false, false, &dst_tablet_schema);

    ChunkPtr src_chunk = ChunkHelper::new_chunk(ChunkHelper::convert_schema_to_format_v2(src_tablet_schema), 4096);
    ChunkPtr dst_chunk = ChunkHelper::new_chunk(ChunkHelper::convert_schema_to_format_v2(dst_tablet_schema), 4096);
    ColumnPtr& src_col = src_chunk->get_column_by_index(0);
    ColumnPtr& dst_col = dst_chunk->get_column_by_index(0);
    Field f = ChunkHelper::convert_field_to_format_v2(0, src_tablet_schema.column(0));
    Field f2 = ChunkHelper::convert_field_to_format_v2(0, dst_tablet_schema.column(0));

    Datum src_datum;
    src_datum.set_int32(2);
    src_col->append_datum(src_datum);

    auto converter = vectorized::get_materialized_converter(OLAP_FIELD_TYPE_INT, OLAP_MATERIALIZE_TYPE_COUNT);
    Status st = converter->convert_materialized(src_col, dst_col, f.type().get(), src_tablet_schema.column(0));
    ASSERT_TRUE(st.ok());

    Datum dst_datum = dst_col->get(0);
    EXPECT_EQ(dst_datum.get_int64(), 1);
}

TEST_F(SchemaChangeTest, schema_change_directly) {
    CreateSrcTablet(1001);
    StorageEngine* engine = StorageEngine::instance();
    TCreateTabletReq create_tablet_req;
    SetCreateTabletReq(&create_tablet_req, 1002);
    AddColumn(&create_tablet_req, "k1", TPrimitiveType::INT, true);
    AddColumn(&create_tablet_req, "k2", TPrimitiveType::INT, true);
    AddColumn(&create_tablet_req, "v1", TPrimitiveType::BIGINT, false);
    AddColumn(&create_tablet_req, "v2", TPrimitiveType::VARCHAR, false);
    Status res = engine->create_tablet(create_tablet_req);
    ASSERT_TRUE(res.ok()) << res.to_string();
    TabletSharedPtr new_tablet = engine->tablet_manager()->get_tablet(create_tablet_req.tablet_id);
    TabletSharedPtr base_tablet = engine->tablet_manager()->get_tablet(1001);

    ChunkChanger chunk_changer(new_tablet->tablet_schema());
    for (size_t i = 0; i < 4; ++i) {
        ColumnMapping* column_mapping = chunk_changer.get_mutable_column_mapping(i);
        column_mapping->ref_column = i;
    }

    _sc_procedure = new (std::nothrow) SchemaChangeDirectly(chunk_changer);
    Version version(3, 3);
    RowsetSharedPtr rowset = base_tablet->get_rowset_by_version(version);
    ASSERT_TRUE(rowset != nullptr);

    TabletReaderParams read_params;
    read_params.reader_type = ReaderType::READER_ALTER_TABLE;
    read_params.skip_aggregation = false;
    read_params.chunk_size = config::vector_chunk_size;
    vectorized::Schema base_schema = ChunkHelper::convert_schema_to_format_v2(base_tablet->tablet_schema());
    vectorized::TabletReader* tablet_rowset_reader = new TabletReader(base_tablet, rowset->version(), base_schema);
    ASSERT_TRUE(tablet_rowset_reader != nullptr);
    ASSERT_TRUE(tablet_rowset_reader->prepare().ok());
    ASSERT_TRUE(tablet_rowset_reader->open(read_params).ok());

    RowsetWriterContext writer_context(kDataFormatUnknown, kDataFormatV2);
    writer_context.rowset_id = engine->next_rowset_id();
    writer_context.tablet_uid = new_tablet->tablet_uid();
    writer_context.tablet_id = new_tablet->tablet_id();
    writer_context.tablet_schema_hash = new_tablet->schema_hash();
    writer_context.rowset_path_prefix = new_tablet->schema_hash_path();
    writer_context.tablet_schema = &(new_tablet->tablet_schema());
    writer_context.rowset_state = VISIBLE;
    writer_context.version = Version(3, 3);
    writer_context.version_hash = 0;
    std::unique_ptr<RowsetWriter> rowset_writer;
    ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());

    ASSERT_TRUE(_sc_procedure->process(tablet_rowset_reader, rowset_writer.get(), new_tablet, base_tablet, rowset));
    delete tablet_rowset_reader;
    (void)StorageEngine::instance()->tablet_manager()->drop_tablet(1001);
    (void)StorageEngine::instance()->tablet_manager()->drop_tablet(1002);
}

TEST_F(SchemaChangeTest, schema_change_with_sorting) {
    CreateSrcTablet(1003);
    StorageEngine* engine = StorageEngine::instance();
    TCreateTabletReq create_tablet_req;
    SetCreateTabletReq(&create_tablet_req, 1004);
    AddColumn(&create_tablet_req, "k1", TPrimitiveType::INT, true);
    AddColumn(&create_tablet_req, "k2", TPrimitiveType::INT, true);
    AddColumn(&create_tablet_req, "v1", TPrimitiveType::BIGINT, false);
    AddColumn(&create_tablet_req, "v2", TPrimitiveType::VARCHAR, false);
    Status res = engine->create_tablet(create_tablet_req);
    ASSERT_TRUE(res.ok()) << res.to_string();
    TabletSharedPtr new_tablet = engine->tablet_manager()->get_tablet(create_tablet_req.tablet_id,
                                                                      create_tablet_req.tablet_schema.schema_hash);
    TabletSharedPtr base_tablet = engine->tablet_manager()->get_tablet(1003);

    ChunkChanger chunk_changer(new_tablet->tablet_schema());
    ColumnMapping* column_mapping = chunk_changer.get_mutable_column_mapping(0);
    column_mapping->ref_column = 1;
    column_mapping = chunk_changer.get_mutable_column_mapping(1);
    column_mapping->ref_column = 0;
    column_mapping = chunk_changer.get_mutable_column_mapping(2);
    column_mapping->ref_column = 2;
    column_mapping = chunk_changer.get_mutable_column_mapping(3);
    column_mapping->ref_column = 3;

    _sc_procedure = new (std::nothrow) SchemaChangeWithSorting(
            chunk_changer, config::memory_limitation_per_thread_for_schema_change * 1024 * 1024 * 1024);
    Version version(3, 3);
    RowsetSharedPtr rowset = base_tablet->get_rowset_by_version(version);
    ASSERT_TRUE(rowset != nullptr);

    TabletReaderParams read_params;
    read_params.reader_type = ReaderType::READER_ALTER_TABLE;
    read_params.skip_aggregation = false;
    read_params.chunk_size = config::vector_chunk_size;
    vectorized::Schema base_schema = ChunkHelper::convert_schema_to_format_v2(base_tablet->tablet_schema());
    vectorized::TabletReader* tablet_rowset_reader = new TabletReader(base_tablet, rowset->version(), base_schema);
    ASSERT_TRUE(tablet_rowset_reader != nullptr);
    ASSERT_TRUE(tablet_rowset_reader->prepare().ok());
    ASSERT_TRUE(tablet_rowset_reader->open(read_params).ok());

    RowsetWriterContext writer_context(kDataFormatUnknown, kDataFormatV2);
    writer_context.rowset_id = engine->next_rowset_id();
    writer_context.tablet_uid = new_tablet->tablet_uid();
    writer_context.tablet_id = new_tablet->tablet_id();
    writer_context.tablet_schema_hash = new_tablet->schema_hash();
    writer_context.rowset_path_prefix = new_tablet->schema_hash_path();
    writer_context.tablet_schema = &(new_tablet->tablet_schema());
    writer_context.rowset_state = VISIBLE;
    writer_context.version = Version(3, 3);
    writer_context.version_hash = 0;
    std::unique_ptr<RowsetWriter> rowset_writer;
    ASSERT_TRUE(RowsetFactory::create_rowset_writer(writer_context, &rowset_writer).ok());

    ASSERT_TRUE(_sc_procedure->process(tablet_rowset_reader, rowset_writer.get(), new_tablet, base_tablet, rowset));
    delete tablet_rowset_reader;
    (void)StorageEngine::instance()->tablet_manager()->drop_tablet(1003);
    (void)StorageEngine::instance()->tablet_manager()->drop_tablet(1004);
}

} // namespace starrocks::vectorized
