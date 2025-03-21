// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/http/action/checksum_action.cpp

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

#include "http/action/checksum_action.h"

#include <sstream>
#include <string>

#include "boost/lexical_cast.hpp"
#include "common/logging.h"
#include "http/http_channel.h"
#include "http/http_headers.h"
#include "http/http_request.h"
#include "http/http_response.h"
#include "http/http_status.h"
#include "runtime/exec_env.h"
#include "storage/olap_define.h"
#include "storage/storage_engine.h"
#include "storage/task/engine_checksum_task.h"

namespace starrocks {

const std::string TABLET_ID = "tablet_id";
// do not use name "VERSION",
// or will be conflict with "VERSION" in thrift/config.h
const std::string TABLET_VERSION = "version";
const std::string VERSION_HASH = "version_hash";
const std::string SCHEMA_HASH = "schema_hash";

ChecksumAction::ChecksumAction(ExecEnv* exec_env) : _exec_env(exec_env) {}

void ChecksumAction::handle(HttpRequest* req) {
    LOG(INFO) << "accept one request " << req->debug_string();

    // Get tablet id
    const std::string& tablet_id_str = req->param(TABLET_ID);
    if (tablet_id_str.empty()) {
        std::string error_msg = std::string("parameter " + TABLET_ID + " not specified in url.");

        HttpChannel::send_reply(req, HttpStatus::BAD_REQUEST, error_msg);
        return;
    }

    // Get version
    const std::string& version_str = req->param(TABLET_VERSION);
    if (version_str.empty()) {
        std::string error_msg = std::string("parameter " + TABLET_VERSION + " not specified in url.");
        HttpChannel::send_reply(req, HttpStatus::BAD_REQUEST, error_msg);
        return;
    }

    // Get version hash
    const std::string& version_hash_str = req->param(VERSION_HASH);
    if (version_hash_str.empty()) {
        std::string error_msg = std::string("parameter " + VERSION_HASH + " not specified in url.");
        HttpChannel::send_reply(req, HttpStatus::BAD_REQUEST, error_msg);
        return;
    }

    // Get schema hash
    const std::string& schema_hash_str = req->param(SCHEMA_HASH);
    if (schema_hash_str.empty()) {
        std::string error_msg = std::string("parameter " + SCHEMA_HASH + " not specified in url.");
        HttpChannel::send_reply(req, HttpStatus::BAD_REQUEST, error_msg);
        return;
    }

    // valid str format
    int64_t tablet_id;
    int64_t version;
    int64_t version_hash;
    int32_t schema_hash;
    try {
        tablet_id = boost::lexical_cast<int64_t>(tablet_id_str);
        version = boost::lexical_cast<int64_t>(version_str);
        version_hash = boost::lexical_cast<int64_t>(version_hash_str);
        schema_hash = boost::lexical_cast<int64_t>(schema_hash_str);
    } catch (boost::bad_lexical_cast& e) {
        std::string error_msg = std::string("param format is invalid: ") + std::string(e.what());
        HttpChannel::send_reply(req, HttpStatus::BAD_REQUEST, error_msg);
        return;
    }

    VLOG_ROW << "get checksum tablet info: " << tablet_id << "-" << version << "-" << version_hash << "-"
             << schema_hash;

    int64_t checksum = do_checksum(tablet_id, version, version_hash, schema_hash, req);
    if (checksum == -1L) {
        std::string error_msg = std::string("checksum failed");
        HttpChannel::send_reply(req, HttpStatus::INTERNAL_SERVER_ERROR, error_msg);
        return;
    } else {
        std::stringstream result;
        result << checksum;
        std::string result_str = result.str();
        HttpChannel::send_reply(req, result_str);
    }

    LOG(INFO) << "deal with checksum request finished! tablet id: " << tablet_id;
}

int64_t ChecksumAction::do_checksum(int64_t tablet_id, int64_t version, int64_t version_hash, int32_t schema_hash,
                                    HttpRequest* req) {
    OLAPStatus res = OLAP_SUCCESS;
    uint32_t checksum;
    EngineChecksumTask engine_task(ExecEnv::GetInstance()->consistency_mem_tracker(), tablet_id, schema_hash, version,
                                   version_hash, &checksum);
    res = engine_task.execute();
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "checksum failed. status: " << res << ", signature: " << tablet_id;
        return -1L;
    } else {
        LOG(INFO) << "checksum success. status: " << res << ", signature: " << tablet_id << ". checksum: " << checksum;
    }

    return static_cast<int64_t>(checksum);
}

} // end namespace starrocks
