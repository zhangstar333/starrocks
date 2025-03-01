// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/export_sink.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/export_sink.h"

#include <memory>
#include <sstream>

#include "column/column.h"
#include "env/env_broker.h"
#include "exec/broker_writer.h"
#include "exec/plain_text_builder.h"
#include "exprs/expr.h"
#include "gutil/strings/substitute.h"
#include "runtime/exec_env.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"
#include "runtime/tuple_row.h"
#include "util/runtime_profile.h"
#include "util/time.h"
#include "util/types.h"

namespace starrocks {

ExportSink::ExportSink(ObjectPool* pool, const RowDescriptor& row_desc, const std::vector<TExpr>& t_exprs)
        : _state(nullptr),
          _pool(pool),
          _row_desc(row_desc),
          _t_output_expr(t_exprs),
          _profile(nullptr),
          _bytes_written_counter(nullptr),
          _rows_written_counter(nullptr),
          _write_timer(nullptr) {}

Status ExportSink::init(const TDataSink& t_sink) {
    RETURN_IF_ERROR(DataSink::init(t_sink));
    _t_export_sink = t_sink.export_sink;

    // From the thrift expressions create the real exprs.
    RETURN_IF_ERROR(Expr::create_expr_trees(_pool, _t_output_expr, &_output_expr_ctxs));
    return Status::OK();
}

Status ExportSink::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(DataSink::prepare(state));

    _state = state;

    std::stringstream title;
    title << "ExportSink (frag_id=" << state->fragment_instance_id() << ")";
    // create profile
    _profile = state->obj_pool()->add(new RuntimeProfile(title.str()));
    SCOPED_TIMER(_profile->total_time_counter());

    // Prepare the exprs to run.
    RETURN_IF_ERROR(Expr::prepare(_output_expr_ctxs, state, _row_desc));

    // TODO(lingbin): add some Counter
    _bytes_written_counter = ADD_COUNTER(profile(), "BytesExported", TUnit::BYTES);
    _rows_written_counter = ADD_COUNTER(profile(), "RowsExported", TUnit::UNIT);
    _write_timer = ADD_TIMER(profile(), "WriteTime");

    return Status::OK();
}

Status ExportSink::open(RuntimeState* state) {
    // Prepare the exprs to run.
    RETURN_IF_ERROR(Expr::open(_output_expr_ctxs, state));
    // open broker
    int query_timeout = state->query_options().query_timeout;
    int timeout_ms = query_timeout > 3600 ? 3600000 : query_timeout * 1000;
    RETURN_IF_ERROR(open_file_writer(timeout_ms));
    return Status::OK();
}

Status ExportSink::close(RuntimeState* state, Status exec_status) {
    Expr::close(_output_expr_ctxs, state);
    if (_file_builder != nullptr) {
        Status st = _file_builder->finish();
        _file_builder.reset();
        return st;
    }
    return Status::OK();
}

Status ExportSink::open_file_writer(int timeout_ms) {
    std::unique_ptr<WritableFile> output_file;
    std::string file_name;
    RETURN_IF_ERROR(gen_file_name(&file_name));
    std::string file_path = _t_export_sink.export_path + "/" + file_name;
    WritableFileOptions options{.sync_on_close = false, .mode = Env::MUST_CREATE};

    const auto& file_type = _t_export_sink.file_type;
    switch (file_type) {
    case TFileType::FILE_LOCAL:
        RETURN_IF_ERROR(Env::Default()->new_writable_file(options, file_path, &output_file));
        break;
    case TFileType::FILE_BROKER: {
        const TNetworkAddress& broker_addr = _t_export_sink.broker_addresses[0];
        EnvBroker env_broker(broker_addr, _t_export_sink.properties, timeout_ms);
        RETURN_IF_ERROR(env_broker.new_writable_file(options, file_path, &output_file));
        break;
    }
    case TFileType::FILE_STREAM:
        return Status::NotSupported(strings::Substitute("Unsupported file type $0", file_type));
    }

    _file_builder = std::make_unique<PlainTextBuilder>(
            PlainTextBuilderOptions{.column_terminated_by = _t_export_sink.column_separator,
                                    .line_terminated_by = _t_export_sink.row_delimiter},
            std::move(output_file), _output_expr_ctxs);

    _state->add_export_output_file(file_path);
    return Status::OK();
}

// TODO(lingbin): add some other info to file name, like partition
Status ExportSink::gen_file_name(std::string* file_name) {
    if (!_t_export_sink.__isset.file_name_prefix) {
        return Status::InternalError("file name prefix is not set");
    }

    std::stringstream file_name_ss;
    // now file-number is 0.
    // <file-name-prefix>_<file-number>.csv.<timestamp>
    file_name_ss << _t_export_sink.file_name_prefix << "0.csv." << UnixMillis();
    *file_name = file_name_ss.str();
    return Status::OK();
}

Status ExportSink::send_chunk(RuntimeState*, vectorized::Chunk* chunk) {
    return _file_builder->add_chunk(chunk);
}

} // namespace starrocks
