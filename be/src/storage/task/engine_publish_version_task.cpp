// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/task/engine_publish_version_task.cpp

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

#include "storage/task/engine_publish_version_task.h"

#include <map>

#include "storage/data_dir.h"
#include "storage/rowset/rowset_meta_manager.h"
#include "storage/tablet_manager.h"
#include "storage/update_manager.h"

namespace starrocks {

using std::map;

EnginePublishVersionTask::EnginePublishVersionTask(TPublishVersionRequest& publish_version_req,
                                                   std::vector<TTabletId>* error_tablet_ids)
        : _publish_version_req(publish_version_req), _error_tablet_ids(error_tablet_ids) {}

OLAPStatus EnginePublishVersionTask::finish() {
    OLAPStatus res = OLAP_SUCCESS;
    int64_t transaction_id = _publish_version_req.transaction_id;

    // each partition
    for (auto& par_ver_info : _publish_version_req.partition_version_infos) {
        int64_t partition_id = par_ver_info.partition_id;
        // get all partition related tablets and check whether the tablet have the related version
        std::set<TabletInfo> partition_related_tablet_infos;
        StorageEngine::instance()->tablet_manager()->get_partition_related_tablets(partition_id,
                                                                                   &partition_related_tablet_infos);
        if (_publish_version_req.strict_mode && partition_related_tablet_infos.empty()) {
            VLOG(1) << "could not find related tablet for partition " << partition_id << ", skip publish version";
            continue;
        }

        map<TabletInfo, RowsetSharedPtr> tablet_related_rs;
        StorageEngine::instance()->txn_manager()->get_txn_related_tablets(transaction_id, partition_id,
                                                                          &tablet_related_rs);

        Version version(par_ver_info.version, par_ver_info.version);
        VersionHash version_hash = par_ver_info.version_hash;

        // each tablet
        for (auto& tablet_rs : tablet_related_rs) {
            OLAPStatus publish_status = OLAP_SUCCESS;
            const TabletInfo& tablet_info = tablet_rs.first;
            const RowsetSharedPtr& rowset = tablet_rs.second;
            VLOG(1) << "begin to publish version on tablet. "
                    << "tablet_id=" << tablet_info.tablet_id << ", schema_hash=" << tablet_info.schema_hash
                    << ", version=" << version.first << ", version_hash=" << version_hash
                    << ", transaction_id=" << transaction_id;
            // if rowset is null, it means this be received write task, but failed during write
            // and receive fe's publish version task
            // this be must return as an error tablet
            if (rowset == nullptr) {
                LOG(WARNING) << "could not find related rowset for tablet " << tablet_info.tablet_id << " txn id "
                             << transaction_id;
                _error_tablet_ids->push_back(tablet_info.tablet_id);
                res = OLAP_ERR_PUSH_ROWSET_NOT_FOUND;
                continue;
            }
            TabletSharedPtr tablet = StorageEngine::instance()->tablet_manager()->get_tablet(tablet_info.tablet_id,
                                                                                             tablet_info.tablet_uid);
            if (tablet == nullptr) {
                LOG(WARNING) << "can't get tablet when publish version. tablet_id=" << tablet_info.tablet_id
                             << " schema_hash=" << tablet_info.schema_hash;
                _error_tablet_ids->push_back(tablet_info.tablet_id);
                res = OLAP_ERR_PUSH_TABLE_NOT_EXIST;
                continue;
            }

            if (tablet->keys_type() == KeysType::PRIMARY_KEYS) {
                VLOG(1) << "UpdateManager::on_rowset_published tablet:" << tablet->tablet_id()
                        << " rowset: " << rowset->rowset_id().to_string() << " version: " << version.second;
                publish_status = StorageEngine::instance()->txn_manager()->publish_txn2(transaction_id, partition_id,
                                                                                        tablet, version.second);
            } else {
                publish_status = StorageEngine::instance()->txn_manager()->publish_txn(
                        partition_id, tablet, transaction_id, version, version_hash);
            }
            if (publish_status != OLAP_SUCCESS) {
                LOG(WARNING) << "failed to publish version. rowset_id=" << rowset->rowset_id()
                             << ", tablet_id=" << tablet_info.tablet_id << ", txn_id=" << transaction_id;
                _error_tablet_ids->push_back(tablet_info.tablet_id);
                res = publish_status;
                continue;
            }

            if (tablet->keys_type() != KeysType::PRIMARY_KEYS) {
                // add visible rowset to tablet
                auto st = tablet->add_inc_rowset(rowset);
                publish_status =
                        st.ok() ? OLAP_SUCCESS
                                : (st.is_already_exist() ? OLAP_ERR_PUSH_VERSION_ALREADY_EXIST : OLAP_ERR_OTHER_ERROR);
                if (publish_status != OLAP_SUCCESS && publish_status != OLAP_ERR_PUSH_VERSION_ALREADY_EXIST) {
                    LOG(WARNING) << "fail to add visible rowset to tablet. rowset_id=" << rowset->rowset_id()
                                 << ", tablet_id=" << tablet_info.tablet_id << ", txn_id=" << transaction_id
                                 << ", res=" << publish_status;
                    _error_tablet_ids->push_back(tablet_info.tablet_id);
                    res = publish_status;
                    continue;
                }
            }
            partition_related_tablet_infos.erase(tablet_info);
            VLOG(1) << "publish version successfully on tablet. tablet=" << tablet->full_name()
                    << ", transaction_id=" << transaction_id << ", version=" << version.first
                    << ", res=" << publish_status;
        }

        // check if the related tablet remained all have the version
        for (auto& tablet_info : partition_related_tablet_infos) {
            // has to use strict mode to check if check all tablets
            if (!_publish_version_req.strict_mode) {
                break;
            }
            TabletSharedPtr tablet = StorageEngine::instance()->tablet_manager()->get_tablet(tablet_info.tablet_id);
            if (tablet == nullptr) {
                _error_tablet_ids->push_back(tablet_info.tablet_id);
            } else {
                // check if the version exist, if not exist, then set publish failed
                if (!tablet->check_version_exist(version)) {
                    _error_tablet_ids->push_back(tablet_info.tablet_id);
                    // TODO(zc)
                    // generate a pull rowset meta task to pull rowset from remote meta store and storage
                    // pull rowset meta using tablet_id + txn_id
                    // it depends on the tablet type to download file or only meta
                }
            }
        }
    }

    VLOG(1) << "finish to publish version on transaction."
            << "transaction_id=" << transaction_id << ", error_tablet_size=" << _error_tablet_ids->size();
    return res;
}

} // namespace starrocks
