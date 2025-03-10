// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include <memory>
#include <vector>

#pragma once

namespace starrocks {

class DecimalV2Value;
class HyperLogLog;
class BitmapValue;
class PercentileValue;

namespace vectorized {

class DateValue;
class TimestampValue;

typedef __int128 int128_t;

class Chunk;
class Field;
class Column;
class Schema;
struct RuntimeChunkMeta;

// We may change the Buffer implementation in the future.
template <typename T>
using Buffer = std::vector<T>;

class ArrayColumn;
class BinaryColumn;

template <typename T>
class FixedLengthColumn;

template <typename T>
class DecimalV3Column;

using ColumnPtr = std::shared_ptr<Column>;
using MutableColumnPtr = std::unique_ptr<Column>;
using Columns = std::vector<ColumnPtr>;
using MutableColumns = std::vector<MutableColumnPtr>;

using Int8Column = FixedLengthColumn<int8_t>;
using UInt8Column = FixedLengthColumn<uint8_t>;
using BooleanColumn = UInt8Column;
using Int16Column = FixedLengthColumn<int16_t>;
using UInt16Column = FixedLengthColumn<uint16_t>;
using Int32Column = FixedLengthColumn<int32_t>;
using UInt32Column = FixedLengthColumn<uint32_t>;
using Int64Column = FixedLengthColumn<int64_t>;
using UInt64Column = FixedLengthColumn<uint64_t>;
using Int128Column = FixedLengthColumn<int128_t>;
using DoubleColumn = FixedLengthColumn<double>;
using FloatColumn = FixedLengthColumn<float>;
using DateColumn = FixedLengthColumn<DateValue>;
using DecimalColumn = FixedLengthColumn<DecimalV2Value>;
using TimestampColumn = FixedLengthColumn<TimestampValue>;
using Decimal32Column = DecimalV3Column<int32_t>;
using Decimal64Column = DecimalV3Column<int64_t>;
using Decimal128Column = DecimalV3Column<int128_t>;

template <typename T>
constexpr bool is_decimal_column = false;
template <typename T>
constexpr bool is_decimal_column<DecimalV3Column<T>> = true;
template <typename ColumnType>
using DecimalColumnType = std::enable_if_t<is_decimal_column<ColumnType>, ColumnType>;

template <typename T>
class ObjectColumn;

using HyperLogLogColumn = ObjectColumn<HyperLogLog>;
using BitmapColumn = ObjectColumn<BitmapValue>;
using PercentileColumn = ObjectColumn<PercentileValue>;

using ChunkPtr = std::shared_ptr<Chunk>;
using ChunkUniquePtr = std::unique_ptr<Chunk>;

using SchemaPtr = std::shared_ptr<Schema>;

using Fields = std::vector<std::shared_ptr<Field>>;
using FieldPtr = std::shared_ptr<Field>;

using Filter = Buffer<uint8_t>;
using FilterPtr = std::shared_ptr<Filter>;

} // namespace vectorized
} // namespace starrocks
