// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/analysis/DescriptorTable.java

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

package com.starrocks.analysis;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.google.common.collect.Sets;
import com.starrocks.catalog.PartitionKey;
import com.starrocks.catalog.Table;
import com.starrocks.common.IdGenerator;
import com.starrocks.thrift.TDescriptorTable;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;

/**
 * Repository for tuple (and slot) descriptors.
 * Descriptors should only be created through this class, which assigns
 * them unique ids..
 */
public class DescriptorTable {
    private static final Logger LOG = LogManager.getLogger(DescriptorTable.class);

    private final HashMap<TupleId, TupleDescriptor> tupleDescs = new HashMap<TupleId, TupleDescriptor>();
    // List of referenced tables with no associated TupleDescriptor to ship to the BE.
    // For example, the output table of an insert query.
    private final List<Table> referencedTables = new ArrayList<Table>();
    // For each table, the set of partitions that are referenced by at least one scan range.
    private final Map<Table, List<ReferencedPartitionInfo>> referencedPartitionsPerTable = Maps.newHashMap();
    private final IdGenerator<TupleId> tupleIdGenerator_ = TupleId.createGenerator();
    private final IdGenerator<SlotId> slotIdGenerator_ = SlotId.createGenerator();
    private final HashMap<SlotId, SlotDescriptor> slotDescs = Maps.newHashMap();

    public DescriptorTable() {
    }

    public TupleDescriptor createTupleDescriptor() {
        TupleDescriptor d = new TupleDescriptor(tupleIdGenerator_.getNextId());
        tupleDescs.put(d.getId(), d);
        return d;
    }

    public TupleDescriptor createTupleDescriptor(String debugName) {
        TupleDescriptor d = new TupleDescriptor(tupleIdGenerator_.getNextId(), debugName);
        tupleDescs.put(d.getId(), d);
        return d;
    }

    public SlotDescriptor addSlotDescriptor(TupleDescriptor d) {
        SlotDescriptor result = new SlotDescriptor(slotIdGenerator_.getNextId(), d);
        d.addSlot(result);
        slotDescs.put(result.getId(), result);
        return result;
    }

    public SlotDescriptor addSlotDescriptor(TupleDescriptor d, SlotId slotId) {
        SlotDescriptor result = new SlotDescriptor(slotId, d);
        if (null == d.getSlot(slotId.asInt())) {
            d.addSlot(result);
        }
        slotDescs.put(result.getId(), result);
        return result;
    }

    /**
     * Create copy of src with new id. The returned descriptor has its mem layout
     * computed.
     */
    public TupleDescriptor copyTupleDescriptor(TupleId srcId, String debugName) {
        TupleDescriptor d = new TupleDescriptor(tupleIdGenerator_.getNextId(), debugName);
        tupleDescs.put(d.getId(), d);
        // create copies of slots
        TupleDescriptor src = tupleDescs.get(srcId);
        for (SlotDescriptor slot : src.getSlots()) {
            copySlotDescriptor(d, slot);
        }
        d.computeMemLayout();
        return d;
    }

    /**
     * Append copy of src to dest.
     */
    public SlotDescriptor copySlotDescriptor(TupleDescriptor dest, SlotDescriptor src) {
        SlotDescriptor result = new SlotDescriptor(slotIdGenerator_.getNextId(), dest, src);
        dest.addSlot(result);
        slotDescs.put(result.getId(), result);
        return result;
    }

    public TupleDescriptor getTupleDesc(TupleId id) {
        return tupleDescs.get(id);
    }

    public SlotDescriptor getSlotDesc(SlotId id) {
        return slotDescs.get(id);
    }

    public Collection<TupleDescriptor> getTupleDescs() {
        return tupleDescs.values();
    }

    public void addReferencedTable(Table table) {
        referencedTables.add(table);
    }

    public void addReferencedPartitions(Table table, ReferencedPartitionInfo info) {
        List<ReferencedPartitionInfo> partitions = referencedPartitionsPerTable.get(table);
        if (partitions == null) {
            partitions = Lists.newArrayList();
            referencedPartitionsPerTable.put(table, partitions);
        }
        partitions.add(info);
    }

    /**
     * Marks all slots in list as materialized.
     */
    public void markSlotsMaterialized(List<SlotId> ids) {
        for (SlotId id : ids) {
            getSlotDesc(id).setIsMaterialized(true);
        }
    }

    // Computes physical layout parameters of all descriptors.
    // Call this only after the last descriptor was added.
    public void computeMemLayout() {
        for (TupleDescriptor d : tupleDescs.values()) {
            d.computeMemLayout();
        }
    }

    public TDescriptorTable toThrift() {
        TDescriptorTable result = new TDescriptorTable();
        HashSet<Table> referencedTbls = Sets.newHashSet();
        for (TupleDescriptor tupleD : tupleDescs.values()) {
            // inline view of a non-constant select has a non-materialized tuple descriptor
            // in the descriptor table just for type checking, which we need to skip
            if (tupleD.getIsMaterialized()) {
                result.addToTupleDescriptors(tupleD.toThrift());
                // an inline view of a constant select has a materialized tuple
                // but its table has no id
                if (tupleD.getTable() != null
                        && tupleD.getTable().getId() >= 0) {
                    referencedTbls.add(tupleD.getTable());
                }
                for (SlotDescriptor slotD : tupleD.getMaterializedSlots()) {
                    result.addToSlotDescriptors(slotD.toThrift());
                }
            }
        }

        referencedTbls.addAll(referencedTables);

        for (Table tbl : referencedTbls) {
            result.addToTableDescriptors(
                    tbl.toThrift(referencedPartitionsPerTable.getOrDefault(tbl, Lists.newArrayList())));
        }
        return result;
    }

    public String debugString() {
        StringBuilder out = new StringBuilder();
        out.append("tuples:\n");
        for (TupleDescriptor desc : tupleDescs.values()) {
            out.append(desc + "\n");
        }
        out.append("\n ");
        out.append("slotDesc size: " + slotDescs.size() + "\n");
        for (SlotDescriptor desc : slotDescs.values()) {
            out.append(desc.debugString());
            out.append("\n");
        }
        out.append("\n ");
        return out.toString();
    }

    public String getExplainString() {
        StringBuilder out = new StringBuilder();
        out.append("\nTuples:\n");
        for (TupleDescriptor desc : tupleDescs.values()) {
            out.append(desc.getExplainString() + "\n");
        }
        return out.toString();
    }

    public static class ReferencedPartitionInfo {
        private long id;
        private PartitionKey key;

        public ReferencedPartitionInfo(long id, PartitionKey key) {
            this.id = id;
            this.key = key;
        }

        public long getId() {
            return id;
        }

        public PartitionKey getKey() {
            return key;
        }
    }
}
