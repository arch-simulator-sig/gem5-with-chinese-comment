/**
 * Copyright (c) 2018 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Javier Bueno
 */

 #include "mem/cache/prefetch/indirect_memory.hh"

 #include "mem/cache/base.hh"
 #include "mem/cache/prefetch/associative_set_impl.hh"
 #include "params/IndirectMemoryPrefetcher.hh"

IndirectMemoryPrefetcher::IndirectMemoryPrefetcher(
    const IndirectMemoryPrefetcherParams *p) : QueuedPrefetcher(p),
    maxPrefetchDistance(p->max_prefetch_distance),
    shiftValues(p->shift_values), prefetchThreshold(p->prefetch_threshold),
    maxIndirectCounterValue(p->max_indirect_counter_value),
    streamCounterThreshold(p->stream_counter_threshold),
    streamingDistance(p->streaming_distance),
    prefetchTable(p->pt_table_assoc, p->pt_table_entries,
                  p->pt_table_indexing_policy, p->pt_table_replacement_policy),
    ipd(p->ipd_table_assoc, p->ipd_table_entries, p->ipd_table_indexing_policy,
        p->ipd_table_replacement_policy,
        IndirectPatternDetectorEntry(p->addr_array_len, shiftValues.size())),
    ipdEntryTrackingMisses(nullptr),
#if THE_ISA != NULL_ISA
    byteOrder(TheISA::GuestByteOrder)
#else
    byteOrder((ByteOrder) -1)
#endif
{
    /// 初始化父类的预取度
    originDegree_ = streamingDistance;
    throttlingDegree_ = streamingDistance;
    fatal_if(byteOrder == -1, "This prefetcher requires a defined ISA\n");
}

void
IndirectMemoryPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    if (!pfi.hasPC()) {
        return;
    }

    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();

    checkAccessMatchOnActiveEntries(addr);

    // First check if this is a miss, if the prefetcher is tracking misses
    if (ipdEntryTrackingMisses != nullptr && miss) {
        // Check if the entry tracking misses has already set its second index
        if (!ipdEntryTrackingMisses->secondIndexSet) {
            trackMissIndex1(addr);
        } else {
            trackMissIndex2(addr);
        }
    } else {
        // if misses are not being tracked, attempt to detect stream accesses
        PrefetchTableEntry *pt_entry =
            prefetchTable.findEntry(pc, false /* unused */);
        if (pt_entry != nullptr) {
            prefetchTable.accessEntry(pt_entry);

            if (pt_entry->address != addr) {
                // Streaming access found
                pt_entry->streamCounter += 1;
                if (pt_entry->streamCounter >= streamCounterThreshold) {
                    int64_t delta = addr - pt_entry->address;
                    for (unsigned int i = 1; i <= streamingDistance; i += 1) {
                        /// 添加可用的Info信息
                        AddrPriority newPref(addr + delta * i, 0);
                        newPref.info_.setInfo("Delta", abs(delta + 128));
                        newPref.info_.setInfo("Confidence", pt_entry->streamCounter);
                        newPref.info_.setInfo("Depth", i);
                        addresses.push_back(newPref);
                    }
                }
                pt_entry->address = addr;
                pt_entry->secure = is_secure;


                // if this is a read, read the data from the cache and assume
                // it is an index (this is only possible if the data is already
                // in the cache), also, only indexes up to 8 bytes are
                // considered

                if (!miss && !pfi.isWrite() && pfi.getSize() <= 8) {
                    int64_t index = 0;
                    switch(pfi.getSize()) {
                        case sizeof(uint8_t):
                            index = pfi.get<uint8_t>(byteOrder);
                            break;
                        case sizeof(uint16_t):
                            index = pfi.get<uint16_t>(byteOrder);
                            break;
                        case sizeof(uint32_t):
                            index = pfi.get<uint32_t>(byteOrder);
                            break;
                        case sizeof(uint64_t):
                            index = pfi.get<uint64_t>(byteOrder);
                            break;
                        default:
                            panic("Invalid access size\n");
                    }
                    if (!pt_entry->enabled) {
                        // Not enabled (no pattern detected in this stream),
                        // add or update an entry in the pattern detector and
                        // start tracking misses
                        allocateOrUpdateIPDEntry(pt_entry, index);
                    } else {
                        // Enabled entry, update the index
                        pt_entry->index = index;
                        if (!pt_entry->increasedIndirectCounter) {
                            if (pt_entry->indirectCounter > 0) {
                                pt_entry->indirectCounter -= 1;
                            }
                        } else {
                            // Set this to false, to see if the new index
                            // has any match
                            pt_entry->increasedIndirectCounter = false;
                        }

                        // If the counter is high enough, start prefetching
                        if (pt_entry->indirectCounter > prefetchThreshold) {
                            unsigned distance = pt_entry->indirectCounter *
                                maxPrefetchDistance / maxIndirectCounterValue;
                            uint16_t depth = 0;
                            for (int delta = 1; delta < distance; delta += 1) {
                                Addr pf_addr = pt_entry->baseAddr +
                                    (pt_entry->index << pt_entry->shift);
                                /// 添加可用的Info信息
                                AddrPriority newPref(pf_addr, 0);
                                newPref.info_.setInfo("Delta", abs(delta + 128));
                                newPref.info_.setInfo("Confidence", pt_entry->indirectCounter);
                                newPref.info_.setInfo("Depth", depth);
                                addresses.push_back(newPref);
                                depth++;
                            }
                        }
                    }
                }
            }
        } else {
            pt_entry = prefetchTable.findVictim(pc);
            assert(pt_entry != nullptr);
            prefetchTable.insertEntry(pc, false /* unused */, pt_entry);
            pt_entry->address = addr;
            pt_entry->secure = is_secure;
        }
    }
}

void
IndirectMemoryPrefetcher::allocateOrUpdateIPDEntry(
    const PrefetchTableEntry *pt_entry, int64_t index)
{
    // The address of the pt_entry is used to index the IPD
    Addr ipd_entry_addr = (Addr) pt_entry;
    IndirectPatternDetectorEntry *ipd_entry = ipd.findEntry(ipd_entry_addr,
                                                            false/* unused */);
    if (ipd_entry != nullptr) {
        ipd.accessEntry(ipd_entry);
        if (!ipd_entry->secondIndexSet) {
            // Second time we see an index, fill idx2
            ipd_entry->idx2 = index;
            ipd_entry->secondIndexSet = true;
            ipdEntryTrackingMisses = ipd_entry;
        } else {
            // Third access! no pattern has been found so far,
            // release the IPD entry
            ipd_entry->reset();
            ipdEntryTrackingMisses = nullptr;
        }
    } else {
        ipd_entry = ipd.findVictim(ipd_entry_addr);
        assert(ipd_entry != nullptr);
        ipd.insertEntry(ipd_entry_addr, false /* unused */, ipd_entry);
        ipd_entry->idx1 = index;
        ipdEntryTrackingMisses = ipd_entry;
    }
}

void
IndirectMemoryPrefetcher::trackMissIndex1(Addr miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // If the second index is not set, we are just filling the baseAddr
    // vector
    assert(entry->numMisses < entry->baseAddr.size());
    std::vector<Addr> &ba_array = entry->baseAddr[entry->numMisses];
    int idx = 0;
    for (int shift : shiftValues) {
        ba_array[idx] = miss_addr - (entry->idx1 << shift);
        idx += 1;
    }
    entry->numMisses += 1;
    if (entry->numMisses == entry->baseAddr.size()) {
        // stop tracking misses once we have tracked enough
        ipdEntryTrackingMisses = nullptr;
    }
}
void
IndirectMemoryPrefetcher::trackMissIndex2(Addr miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // Second index is filled, compare the addresses generated during
    // the previous misses (using idx1) against newly generated values
    // using idx2, if a match is found, fill the additional fields
    // of the PT entry
    for (int midx = 0; midx < entry->numMisses; midx += 1)
    {
        std::vector<Addr> &ba_array = entry->baseAddr[midx];
        int idx = 0;
        for (int shift : shiftValues) {
            if (ba_array[idx] == (miss_addr - (entry->idx2 << shift))) {
                // Match found!
                // Fill the corresponding pt_entry
                PrefetchTableEntry *pt_entry =
                    (PrefetchTableEntry *) entry->getTag();
                pt_entry->baseAddr = ba_array[idx];
                pt_entry->shift = shift;
                pt_entry->enabled = true;
                pt_entry->indirectCounter = 0;
                // Release the current IPD Entry
                entry->reset();
                // Do not track more misses
                ipdEntryTrackingMisses = nullptr;
                return;
            }
            idx += 1;
        }
    }
}

void
IndirectMemoryPrefetcher::checkAccessMatchOnActiveEntries(Addr addr)
{
    for (auto &pt_entry : prefetchTable) {
        if (pt_entry.enabled) {
            if (addr == pt_entry.baseAddr +
                       (pt_entry.index << pt_entry.shift)) {
                if (pt_entry.indirectCounter < maxIndirectCounterValue) {
                    pt_entry.indirectCounter += 1;
                    pt_entry.increasedIndirectCounter = true;
                }
            }
        }
    }
}

IndirectMemoryPrefetcher*
IndirectMemoryPrefetcherParams::create()
{
    return new IndirectMemoryPrefetcher(this);
}
