// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/common/config.h

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

#ifndef STARROCKS_BE_SRC_COMMON_CONFIG_H
#define STARROCKS_BE_SRC_COMMON_CONFIG_H

#include "configbase.h"

namespace starrocks {
namespace config {
// cluster id
CONF_Int32(cluster_id, "-1");
// port on which ImpalaInternalService is exported
CONF_Int32(be_port, "9060");

// port for brpc
CONF_Int32(brpc_port, "8060");

// the number of bthreads for brpc, the default value is set to -1, which means the number of bthreads is #cpu-cores
CONF_Int32(brpc_num_threads, "-1");

// Declare a selection strategy for those servers have many ips.
// Note that there should at most one ip match this list.
// this is a list in semicolon-delimited format, in CIDR notation, e.g. 10.10.10.0/24
// If no ip match this rule, will choose one randomly.
CONF_String(priority_networks, "");

////
//// tcmalloc gc parameter
////
// min memory for TCmalloc, when used memory is smaller than this, do not returned to OS
CONF_mInt64(tc_use_memory_min, "10737418240");
// free memory rate.[0-100]
CONF_mInt64(tc_free_memory_rate, "20");
// tcmalloc gc period, default 60, it should be between [1, 180]
CONF_mInt64(tc_gc_period, "60");

// Bound on the total amount of bytes allocated to thread caches.
// This bound is not strict, so it is possible for the cache to go over this bound
// in certain circumstances. The maximum value of this flag is capped to 1GB.
// This value defaults to 1GB.
// If you suspect your application is not scaling to many threads due to lock contention in TCMalloc,
// you can try increasing this value. This may improve performance, at a cost of extra memory
// use by TCMalloc.
// reference: https://gperftools.github.io/gperftools/tcmalloc.html: TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES
//            https://github.com/gperftools/gperftools/issues/1111
CONF_Int64(tc_max_total_thread_cache_bytes, "1073741824");

// process memory limit specified as number of bytes
// ('<int>[bB]?'), megabytes ('<float>[mM]'), gigabytes ('<float>[gG]'),
// or percentage of the physical memory ('<int>%').
// defaults to bytes if no unit is given"
// must larger than 0. and if larger than physical memory size,
// it will be set to physical memory size.
CONF_String(mem_limit, "80%");

// the port heartbeat service used
CONF_Int32(heartbeat_service_port, "9050");
// the count of heart beat service
CONF_Int32(heartbeat_service_thread_count, "1");
// the count of thread to create table
CONF_Int32(create_tablet_worker_count, "3");
// the count of thread to drop table
CONF_Int32(drop_tablet_worker_count, "3");
// the count of thread to batch load
CONF_Int32(push_worker_count_normal_priority, "3");
// the count of thread to high priority batch load
CONF_Int32(push_worker_count_high_priority, "3");
// the count of thread to publish version
CONF_Int32(publish_version_worker_count, "8");
// the count of thread to clear transaction task
CONF_Int32(clear_transaction_task_worker_count, "1");
// the count of thread to delete
CONF_Int32(delete_worker_count, "3");
// the count of thread to alter table
CONF_Int32(alter_tablet_worker_count, "3");
// the count of thread to clone
CONF_Int32(clone_worker_count, "3");
// the count of thread to clone
CONF_Int32(storage_medium_migrate_count, "1");
// the count of thread to check consistency
CONF_Int32(check_consistency_worker_count, "1");
// the count of thread to upload
CONF_Int32(upload_worker_count, "1");
// the count of thread to download
CONF_Int32(download_worker_count, "1");
// the count of thread to make snapshot
CONF_Int32(make_snapshot_worker_count, "5");
// the count of thread to release snapshot
CONF_Int32(release_snapshot_worker_count, "5");
// the interval time(seconds) for agent report tasks signatrue to FE
CONF_mInt32(report_task_interval_seconds, "10");
// the interval time(seconds) for agent report disk state to FE
CONF_mInt32(report_disk_state_interval_seconds, "60");
// the interval time(seconds) for agent report olap table to FE
CONF_mInt32(report_tablet_interval_seconds, "60");
// the interval time(seconds) for agent report plugin status to FE
// CONF_Int32(report_plugin_interval_seconds, "120");
// the timeout(seconds) for alter table
// CONF_Int32(alter_tablet_timeout_seconds, "86400");
// the timeout(seconds) for make snapshot
// CONF_Int32(make_snapshot_timeout_seconds, "600");
// the timeout(seconds) for release snapshot
// CONF_Int32(release_snapshot_timeout_seconds, "600");
// the max download speed(KB/s)
CONF_mInt32(max_download_speed_kbps, "50000");
// download low speed limit(KB/s)
CONF_mInt32(download_low_speed_limit_kbps, "50");
// download low speed time(seconds)
CONF_mInt32(download_low_speed_time, "300");
// curl verbose mode
// CONF_Int64(curl_verbose_mode, "1");
// seconds to sleep for each time check table status
// CONF_Int32(check_status_sleep_time_seconds, "10");
// sleep time for one second
CONF_Int32(sleep_one_second, "1");
// sleep time for five seconds
CONF_Int32(sleep_five_seconds, "5");

// log dir
CONF_String(sys_log_dir, "${STARROCKS_HOME}/log");
CONF_String(user_function_dir, "${STARROCKS_HOME}/lib/udf");
// INFO, WARNING, ERROR, FATAL
CONF_String(sys_log_level, "INFO");
// TIME-DAY, TIME-HOUR, SIZE-MB-nnn
CONF_String(sys_log_roll_mode, "SIZE-MB-1024");
// log roll num
CONF_Int32(sys_log_roll_num, "10");
// verbose log
CONF_Strings(sys_log_verbose_modules, "");
// verbose log level
CONF_Int32(sys_log_verbose_level, "10");
// log buffer level
CONF_String(log_buffer_level, "");

// Pull load task dir
CONF_String(pull_load_task_dir, "${STARROCKS_HOME}/var/pull_load");

// the maximum number of bytes to display on the debug webserver's log page
CONF_Int64(web_log_bytes, "1048576");
// number of threads available to serve backend execution requests
CONF_Int32(be_service_threads, "64");
// key=value pair of default query options for StarRocks, separated by ','
CONF_String(default_query_options, "");

// If non-zero, StarRocks will output memory usage every log_mem_usage_interval'th fragment completion.
// CONF_Int32(log_mem_usage_interval, "0");

// Controls the number of threads to run work per core.  It's common to pick 2x
// or 3x the number of cores.  This keeps the cores busy without causing excessive
// thrashing.
CONF_Int32(num_threads_per_core, "3");
// if true, compresses tuple data in Serialize
CONF_Bool(compress_rowbatches, "true");
// compress ratio when shuffle row_batches in network, not in storage engine.
// If ratio is less than this value, use uncompressed data instead
CONF_mDouble(rpc_compress_ratio_threshold, "1.1");
// serialize and deserialize each returned row batch
CONF_Bool(serialize_batch, "false");
// interval between profile reports; in seconds
CONF_mInt32(status_report_interval, "5");
// Local directory to copy UDF libraries from HDFS into
CONF_String(local_library_dir, "${UDF_RUNTIME_DIR}");
// number of olap scanner thread pool size
CONF_mInt32(doris_scanner_thread_pool_thread_num, "48");
// number of olap scanner thread pool size
CONF_Int32(doris_scanner_thread_pool_queue_size, "102400");
// number of etl thread pool size
CONF_Int32(etl_thread_pool_size, "8");
// number of etl thread pool size
CONF_Int32(etl_thread_pool_queue_size, "256");
// port on which to run StarRocks test backend
CONF_Int32(port, "20001");
// default thrift client connect timeout(in seconds)
CONF_Int32(thrift_connect_timeout_seconds, "3");
// broker write timeout in seconds
CONF_Int32(broker_write_timeout_seconds, "30");
// default thrift client retry interval (in milliseconds)
CONF_mInt64(thrift_client_retry_interval_ms, "100");
// max row count number for single scan range
CONF_mInt32(doris_scan_range_row_count, "524288");
// size of scanner queue between scanner thread and compute thread
CONF_mInt32(doris_scanner_queue_size, "1024");
// single read execute fragment row size
CONF_mInt32(doris_scanner_row_num, "16384");
// number of max scan keys
CONF_mInt32(doris_max_scan_key_num, "1024");
// the max number of push down values of a single column.
// if exceed, no conditions will be pushed down for that column.
CONF_mInt32(max_pushdown_conditions_per_column, "1024");
// return_row / total_row
CONF_mInt32(doris_max_pushdown_conjuncts_return_rate, "90");
// (Advanced) Maximum size of per-query receive-side buffer
CONF_mInt32(exchg_node_buffer_size_bytes, "10485760");
// insert sort threadhold for sorter
// CONF_Int32(insertion_threadhold, "16");
// the block_size every block allocate for sorter
CONF_Int32(sorter_block_size, "8388608");
// push_write_mbytes_per_sec
CONF_Int32(push_write_mbytes_per_sec, "10");

CONF_mInt64(column_dictionary_key_ratio_threshold, "0");
CONF_mInt64(column_dictionary_key_size_threshold, "0");
// if true, output IR after optimization passes
// CONF_Bool(dump_ir, "false");
// if set, saves the generated IR to the output file.
//CONF_String(module_output, "");
// memory_limitation_per_thread_for_schema_change unit GB
CONF_mInt32(memory_limitation_per_thread_for_schema_change, "2");

// CONF_Int64(max_unpacked_row_block_size, "104857600");

CONF_mInt32(update_cache_expire_sec, "360");
CONF_mInt32(file_descriptor_cache_clean_interval, "3600");
CONF_mInt32(disk_stat_monitor_interval, "5");
CONF_mInt32(unused_rowset_monitor_interval, "30");
CONF_String(storage_root_path, "${STARROCKS_HOME}/storage");
// BE process will exit if the percentage of error disk reach this value.
CONF_mInt32(max_percentage_of_error_disk, "0");
// CONF_Int32(default_num_rows_per_data_block, "1024");
CONF_mInt32(default_num_rows_per_column_file_block, "1024");
CONF_Int32(max_tablet_num_per_shard, "1024");
// pending data policy
CONF_mInt32(pending_data_expire_time_sec, "1800");
// inc_rowset expired interval
CONF_mInt32(inc_rowset_expired_sec, "1800");
// inc_rowset snapshot rs sweep time interval
CONF_mInt32(tablet_rowset_stale_sweep_time_sec, "1800");
// garbage sweep policy
CONF_Int32(max_garbage_sweep_interval, "3600");
CONF_Int32(min_garbage_sweep_interval, "180");
CONF_mInt32(snapshot_expire_time_sec, "172800");
CONF_mInt32(trash_file_expire_time_sec, "259200");
// check row nums for BE/CE and schema change. true is open, false is closed.
CONF_mBool(row_nums_check, "true");
//file descriptors cache, by default, cache 16384 descriptors
CONF_Int32(file_descriptor_cache_capacity, "16384");
// minimum file descriptor number
// modify them upon necessity
CONF_Int32(min_file_descriptor_number, "60000");
CONF_Int64(index_stream_cache_capacity, "10737418240");
// CONF_Int64(max_packed_row_block_size, "20971520");

// Cache for stoage page size
CONF_String(storage_page_cache_limit, "0");
// whether to disable page cache feature in storage
CONF_Bool(disable_storage_page_cache, "true");
// whether to disable column pool
CONF_Bool(disable_column_pool, "false");

CONF_mInt32(base_compaction_check_interval_seconds, "60");
CONF_mInt64(base_compaction_num_cumulative_deltas, "5");
CONF_Int32(base_compaction_num_threads_per_disk, "1");
CONF_mDouble(base_cumulative_delta_ratio, "0.3");
CONF_mInt64(base_compaction_interval_seconds_since_last_operation, "86400");
CONF_mInt32(base_compaction_write_mbytes_per_sec, "5");

// cumulative compaction policy: max delta file's size unit:B
CONF_mInt32(cumulative_compaction_check_interval_seconds, "1");
CONF_mInt64(min_cumulative_compaction_num_singleton_deltas, "5");
CONF_mInt64(max_cumulative_compaction_num_singleton_deltas, "1000");
CONF_Int32(cumulative_compaction_num_threads_per_disk, "1");
CONF_mInt64(cumulative_compaction_budgeted_bytes, "104857600");
// CONF_Int32(cumulative_compaction_write_mbytes_per_sec, "100");
// cumulative compaction skips recently published deltas in order to prevent
// compacting a version that might be queried (in case the query planning phase took some time).
// the following config set the window size
CONF_mInt32(cumulative_compaction_skip_window_seconds, "30");

CONF_mInt32(update_compaction_check_interval_seconds, "60");
CONF_Int32(update_compaction_num_threads_per_disk, "1");
CONF_Int32(update_compaction_per_tablet_min_interval_seconds, "120"); // 2min

// if compaction of a tablet failed, this tablet should not be chosen to
// compaction until this interval passes.
CONF_mInt64(min_compaction_failure_interval_sec, "120"); // 2 min
// Too many compaction tasks may run out of memory.
// This config is to limit the max concurrency of running compaction tasks.
// -1 means no limit, and the max concurrency will be:
//      C = (cumulative_compaction_num_threads_per_disk + base_compaction_num_threads_per_disk) * dir_num
// set it to larger than C will be set to equal to C.
// This config can be set to 0, which means to forbid any compaction, for some special cases.
CONF_Int32(max_compaction_concurrency, "-1");

// Threshold to logging compaction trace, in seconds.
CONF_mInt32(base_compaction_trace_threshold, "120");
CONF_mInt32(cumulative_compaction_trace_threshold, "60");
CONF_mInt32(update_compaction_trace_threshold, "20");

// Max row source mask memory bytes, default is 200M.
// Should be smaller than compaction_mem_limit.
// When the row source mask buffer exceeds this, it will be persisted to a temporary file on the disk.
CONF_Int64(max_row_source_mask_memory_bytes, "209715200");

// Port to start debug webserver on
CONF_Int32(webserver_port, "8040");
// Number of webserver workers
CONF_Int32(webserver_num_workers, "48");
// Period to update rate counters and sampling counters in ms.
CONF_mInt32(periodic_counter_update_period_ms, "500");

// Used for mini Load. mini load data file will be removed after this time.
CONF_Int64(load_data_reserve_hours, "4");
// log error log will be removed after this time
CONF_mInt64(load_error_log_reserve_hours, "48");
CONF_Int32(number_tablet_writer_threads, "16");

// Automatically detect whether a char/varchar column to use dictionary encoding
// If the number of keys in a dictionary is greater than this fraction of the total number of rows
// turn off dictionary dictionary encoding. This only will detect first chunk
// set to 1 means always use dictionary encoding
CONF_Double(dictionary_encoding_ratio, "0.7");
// The minimum chunk size for dictionary encoding speculation
CONF_Int32(dictionary_speculate_min_chunk_size, "10000");

// The maximum amount of data that can be processed by a stream load
CONF_mInt64(streaming_load_max_mb, "10240");
// Some data formats, such as JSON, cannot be streamed.
// Therefore, it is necessary to limit the maximum number of
// such data when using stream load to prevent excessive memory consumption.
CONF_mInt64(streaming_load_max_batch_size_mb, "100");
// the alive time of a TabletsChannel.
// If the channel does not receive any data till this time,
// the channel will be removed.
CONF_Int32(streaming_load_rpc_max_alive_time_sec, "1200");
// the timeout of a rpc to open the tablet writer in remote BE.
// short operation time, can set a short timeout
CONF_Int32(tablet_writer_open_rpc_timeout_sec, "60");
// Deprecated, use query_timeout instread
// the timeout of a rpc to process one batch in tablet writer.
// you may need to increase this timeout if using larger 'streaming_load_max_mb',
// or encounter 'tablet writer write failed' error when loading.
// CONF_Int32(tablet_writer_rpc_timeout_sec, "600");
// OlapTableSink sender's send interval, should be less than the real response time of a tablet writer rpc.
CONF_mInt32(olap_table_sink_send_interval_ms, "10");

// Fragment thread pool
CONF_Int32(fragment_pool_thread_num_min, "64");
CONF_Int32(fragment_pool_thread_num_max, "4096");
CONF_Int32(fragment_pool_queue_size, "2048");

//for cast
// CONF_Bool(cast, "true");

// Spill to disk when query
// Writable scratch directories, splitted by ";"
CONF_String(query_scratch_dirs, "${STARROCKS_HOME}");

// Control the number of disks on the machine.  If 0, this comes from the system settings.
CONF_Int32(num_disks, "0");
// The maximum number of the threads per disk is also the max queue depth per disk.
CONF_Int32(num_threads_per_disk, "0");
// The read size is the size of the reads sent to os.
// There is a trade off of latency and throughout, trying to keep disks busy but
// not introduce seeks.  The literature seems to agree that with 8 MB reads, random
// io and sequential io perform similarly.
CONF_Int32(read_size, "8388608");    // 8 * 1024 * 1024, Read Size (in bytes)
CONF_Int32(min_buffer_size, "1024"); // 1024, The minimum read buffer size (in bytes)

// For each io buffer size, the maximum number of buffers the IoMgr will hold onto
// With 1024B through 8MB buffers, this is up to ~2GB of buffers.
CONF_Int32(max_free_io_buffers, "128");

CONF_Bool(disable_mem_pools, "false");

// Whether to allocate chunk using mmap. If you enable this, you'd better to
// increase vm.max_map_count's value whose default value is 65530.
// you can do it as root via "sysctl -w vm.max_map_count=262144" or
// "echo 262144 > /proc/sys/vm/max_map_count"
// NOTE: When this is set to true, you must set chunk_reserved_bytes_limit
// to a relative large number or the performace is very very bad.
CONF_Bool(use_mmap_allocate_chunk, "false");

// Chunk Allocator's reserved bytes limit,
// Default value is 2GB, increase this variable can improve performance, but will
// acquire more free memory which can not be used by other modules
CONF_Int64(chunk_reserved_bytes_limit, "2147483648");

// The probing algorithm of partitioned hash table.
// Enable quadratic probing hash table
CONF_Bool(enable_quadratic_probing, "false");

// for pprof
CONF_String(pprof_profile_dir, "${STARROCKS_HOME}/log");

// for partition
// CONF_Bool(enable_partitioned_hash_join, "false")
CONF_Bool(enable_partitioned_aggregation, "true");

// to forward compatibility, will be removed later
CONF_mBool(enable_token_check, "true");

// to open/close system metrics
CONF_Bool(enable_system_metrics, "true");

CONF_mBool(enable_prefetch, "true");

// Number of cores StarRocks will used, this will effect only when it's greater than 0.
// Otherwise, StarRocks will use all cores returned from "/proc/cpuinfo".
CONF_Int32(num_cores, "0");

// CONF_Bool(thread_creation_fault_injection, "false");

// Set this to encrypt and perform an integrity
// check on all data spilled to disk during a query
// CONF_Bool(disk_spill_encryption, "false");

// When BE start, If there is a broken disk, BE process will exit by default.
// Otherwise, we will ignore the broken disk,
CONF_Bool(ignore_broken_disk, "false");

// Writable scratch directories
CONF_String(scratch_dirs, "/tmp");

// If false and --scratch_dirs contains multiple directories on the same device,
// then only the first writable directory is used
// CONF_Bool(allow_multiple_scratch_dirs_per_device, "false");

// linux transparent huge page
CONF_Bool(madvise_huge_pages, "false");

// whether use mmap to allocate memory
CONF_Bool(mmap_buffers, "false");

// Sleep time in seconds between memory maintenance iterations
CONF_mInt64(memory_maintenance_sleep_time_s, "10");

// Aligement
CONF_Int32(memory_max_alignment, "16");

// write buffer size before flush
CONF_mInt64(write_buffer_size, "104857600");

// following 2 configs limit the memory consumption of load process on a Backend.
// eg: memory limit to 80% of mem limit config but up to 100GB(default)
// NOTICE(cmy): set these default values very large because we don't want to
// impact the load performace when user upgrading StarRocks.
// user should set these configs properly if necessary.
CONF_Int64(load_process_max_memory_limit_bytes, "107374182400"); // 100GB
CONF_Int32(load_process_max_memory_limit_percent, "30");         // 30%
CONF_Int64(compaction_max_memory_limit, "-1");
CONF_Int32(compaction_max_memory_limit_percent, "100");
CONF_Int64(compaction_memory_limit_per_worker, "2147483648"); // 2GB

// update interval of tablet stat cache
CONF_mInt32(tablet_stat_cache_update_interval_second, "300");

// result buffer cancelled time (unit: second)
CONF_mInt32(result_buffer_cancelled_interval_time, "300");

// the increased frequency of priority for remaining tasks in BlockingPriorityQueue
CONF_mInt32(priority_queue_remaining_tasks_increased_frequency, "512");

// sync tablet_meta when modifing meta
CONF_mBool(sync_tablet_meta, "false");

// default thrift rpc timeout ms
CONF_mInt32(thrift_rpc_timeout_ms, "5000");

// txn commit rpc timeout
CONF_mInt32(txn_commit_rpc_timeout_ms, "10000");

// If set to true, metric calculator will run
CONF_Bool(enable_metric_calculator, "true");

// max consumer num in one data consumer group, for routine load
CONF_mInt32(max_consumer_num_per_group, "3");

// the size of thread pool for routine load task.
// this should be larger than FE config 'max_concurrent_task_num_per_be' (default 5)
CONF_Int32(routine_load_thread_pool_size, "10");

// Is set to true, index loading failure will not causing BE exit,
// and the tablet will be marked as bad, so that FE will try to repair it.
// CONF_Bool(auto_recover_index_loading_failure, "false");

// max external scan cache batch count, means cache max_memory_cache_batch_count * batch_size row
// default is 20, batch_size's defualt value is 1024 means 20 * 1024 rows will be cached
CONF_mInt32(max_memory_sink_batch_count, "20");

// This configuration is used for the context gc thread schedule period
// note: unit is minute, default is 5min
CONF_mInt32(scan_context_gc_interval_min, "5");

// es scroll keep-alive
CONF_String(es_scroll_keepalive, "5m");

// HTTP connection timeout for es
CONF_Int32(es_http_timeout_ms, "5000");

// the max client cache number per each host
// There are variety of client cache in BE, but currently we use the
// same cache size configuration.
// TODO(cmy): use different config to set different client cache if necessary.
CONF_Int32(max_client_cache_size_per_host, "10");

// Dir to save files downloaded by SmallFileMgr
CONF_String(small_file_dir, "${STARROCKS_HOME}/lib/small_file/");
// path gc
CONF_Bool(path_gc_check, "true");
CONF_Int32(path_gc_check_interval_second, "86400");
CONF_mInt32(path_gc_check_step, "1000");
CONF_mInt32(path_gc_check_step_interval_ms, "10");
CONF_mInt32(path_scan_interval_second, "86400");

// The following 2 configs limit the max usage of disk capacity of a data dir.
// If both of these 2 threshold reached, no more data can be writen into that data dir.
// The percent of max used capacity of a data dir
CONF_mInt32(storage_flood_stage_usage_percent, "95"); // 95%
// The min bytes that should be left of a data dir
CONF_mInt64(storage_flood_stage_left_capacity_bytes, "1073741824"); // 1GB
// number of thread for flushing memtable per store
CONF_Int32(flush_thread_num_per_store, "2");

// config for tablet meta checkpoint
CONF_mInt32(tablet_meta_checkpoint_min_new_rowsets_num, "10");
CONF_mInt32(tablet_meta_checkpoint_min_interval_secs, "600");

// Maximum size of a single message body in all protocols
CONF_Int64(brpc_max_body_size, "2147483648");
// Max unwritten bytes in each socket, if the limit is reached, Socket.Write fails with EOVERCROWDED
CONF_Int64(brpc_socket_max_unwritten_bytes, "1073741824");

// max number of txns for every txn_partition_map in txn manager
// this is a self protection to avoid too many txns saving in manager
CONF_mInt64(max_runnings_transactions_per_txn_map, "100");

// tablet_map_lock shard size, the value must be power of two.
// this is a an enhancement for better performance to manage tablet
CONF_Int32(tablet_map_shard_size, "32");

CONF_String(plugin_path, "${STARROCKS_HOME}/plugin");

// txn_map_lock shard size, the value is 2^n, n=0,1,2,3,4
// this is a an enhancement for better performance to manage txn
CONF_Int32(txn_map_shard_size, "128");

// txn_lock shard size, the value is 2^n, n=0,1,2,3,4
// this is a an enhancement for better performance to commit and publish txn
CONF_Int32(txn_shard_size, "1024");

// Whether to continue to start be when load tablet from header failed.
CONF_Bool(ignore_load_tablet_failure, "false");

// Whether to continue to start be when load tablet from header failed.
CONF_Bool(ignore_rowset_stale_unconsistent_delete, "false");

// The chunk size for vector query engine
CONF_Int32(vector_chunk_size, "4096");

// valid range: [0-1000].
// `0` will disable late materialization.
// `1000` will enable late materialization always.
CONF_Int32(late_materialization_ratio, "10");

// valid range: [0-1000].
// `0` will disable late materialization select metric type.
// `1000` will enable late materialization always select metric type.
CONF_Int32(metric_late_materialization_ratio, "1000");

// Max batched bytes for each transmit request
CONF_Int64(max_transmit_batched_bytes, "65536");

CONF_Int16(bitmap_max_filter_items, "30");

// valid range: [0-1000].
CONF_Int16(bitmap_max_filter_ratio, "1");

CONF_Bool(bitmap_filter_enable_not_equal, "false");

// Only 1 and 2 is valid.
// When storage_format_version is 1, use origin storage format for Date, Datetime and Decimal
// type.
// When storage_format_version is 2, DATE_V2, TIMESTAMP and DECIMAL_V2 will be used as
// storage format.
CONF_mInt16(storage_format_version, "2");

// IMPORTANT NOTE: changing this config to 1 must require all BEs to be upgraded to new version,
// which support this config.
// DO NOT change this config unless you known how.
// 0 for BITSHUFFLE_NULL
// 1 for LZ4_NULL
CONF_mInt16(null_encoding, "0");

// do pre-aggregate if effect great than the factor, factor range:[1-100].
CONF_Int16(pre_aggregate_factor, "80");

#ifdef __x86_64__
// enable genearate minidump for crash
CONF_Bool(sys_minidump_enable, "false");

// minidump dir(generated by google_breakpad)
CONF_String(sys_minidump_dir, "${STARROCKS_HOME}");

// max minidump files number could exist
CONF_mInt32(sys_minidump_max_files, "16");

// max minidump file size could exist
CONF_mInt32(sys_minidump_limit, "20480");

// interval(seconds) for cleaning old minidumps
CONF_mInt32(sys_minidump_interval, "600");
#endif

// The maximum number of version per tablet. If the
// number of version exceeds this value, new write
// requests will fail.
CONF_Int16(tablet_max_versions, "1000");

// will remove
CONF_mBool(enable_bitmap_union_disk_format_with_set, "false");

// yield PipelineDriver when maximum number of chunks has been moved
// in current execution round.
CONF_Int64(pipeline_yield_max_chunks_moved, "100");
// yield PipelineDriver when maximum time in nano-seconds has spent
// in current execution round.
CONF_Int64(pipeline_yield_max_time_spent, "100000000");
// the number of scan threads pipeline engine.
CONF_Int64(pipeline_scan_thread_pool_thread_num, "0");
// queue size of scan thread pool for pipeline engine.
CONF_Int64(pipeline_scan_thread_pool_queue_size, "102400");
// the number of exchange threads pipeline engine.
CONF_Int64(pipeline_exchange_thread_pool_thread_num, "0");
// queue size of exchange thread pool for pipeline engine.
CONF_Int64(pipeline_exchange_thread_pool_queue_size, "102400");
// the number of execution threads for pipeline engine.
CONF_Int64(pipeline_exec_thread_pool_thread_num, "0");
// the buffer size of io task
CONF_Int64(pipeline_io_buffer_size, "64");
// bitmap serialize version
CONF_Int16(bitmap_serialize_version, "1");
// schema change vectorized
CONF_Bool(enable_schema_change_vectorized, "true");
// max hdfs file handle
CONF_mInt32(max_hdfs_file_handle, "1000");

} // namespace config

} // namespace starrocks

#endif // STARROCKS_BE_SRC_COMMON_CONFIG_H
