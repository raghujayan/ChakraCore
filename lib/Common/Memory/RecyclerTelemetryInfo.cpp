//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include "CommonMemoryPch.h"


#ifdef ENABLE_BASIC_TELEMETRY

#include "Recycler.h"
#include "DataStructures/SList.h"
#include "HeapBucketStats.h"
#include "BucketStatsReporter.h"
#include <psapi.h>

namespace Memory
{
#ifdef DBG
#define AssertOnValidThread(recyclerTelemetryInfo, loc) { AssertMsg(this->IsOnScriptThread(), STRINGIZE("Unexpected thread ID at " ## loc)); }
#else
#define AssertOnValidThread(recyclerTelemetryInfo, loc)
#endif

    RecyclerTelemetryInfo::RecyclerTelemetryInfo(Recycler * recycler, RecyclerTelemetryHostInterface* hostInterface) :
        passCount(0),
        hostInterface(hostInterface),
        gcPassStats(&HeapAllocator::Instance),
        recyclerStartTime(Js::Tick::Now()),
        abortTelemetryCapture(false),
        inPassActiveState(false),
        recycler(recycler)
    {
        mainThreadID = ::GetCurrentThreadId();
    }

    RecyclerTelemetryInfo::~RecyclerTelemetryInfo()
    {
        if (this->hostInterface != nullptr)
        {
            AssertOnValidThread(this, RecyclerTelemetryInfo::~RecyclerTelemetryInfo);
            if (this->gcPassStats.Empty() == false)
            {
                this->hostInterface->TransmitTelemetry(*this);
                this->FreeGCPassStats();
            }
        }
    }

    const GUID& RecyclerTelemetryInfo::GetRecyclerID() const
    {
        return this->recycler->GetRecyclerID();
    }

    bool RecyclerTelemetryInfo::GetIsConcurrentEnabled() const
    {
        return this->recycler->IsConcurrentEnabled();
    }

    bool RecyclerTelemetryInfo::ShouldStartTelemetryCapture() const
    {
        return
            this->hostInterface != nullptr &&
            this->abortTelemetryCapture == false &&
            this->hostInterface->IsTelemetryProviderEnabled();
    }


    void RecyclerTelemetryInfo::FillInSizeData(IdleDecommitPageAllocator* allocator, AllocatorSizes* sizes) const
    {
        sizes->committedBytes = allocator->GetCommittedBytes();
        sizes->reservedBytes = allocator->GetReservedBytes();
        sizes->usedBytes = allocator->GetUsedBytes();
        sizes->numberOfSegments = allocator->GetNumberOfSegments();
    }

    GCPassStatsList::Iterator RecyclerTelemetryInfo::GetGCPassStatsIterator() const
    {
        return this->gcPassStats.GetIterator();
    }

    RecyclerTelemetryGCPassStats* RecyclerTelemetryInfo::GetLastPassStats() const
    {
        RecyclerTelemetryGCPassStats* stats = nullptr;
        if (this->gcPassStats.Empty() == false)
        {
            RecyclerTelemetryGCPassStats& ref = const_cast<RecyclerTelemetryGCPassStats&>(this->gcPassStats.Head());
            stats = &ref;
        }
        return stats;
    }

    void RecyclerTelemetryInfo::StartPass()
    {
        this->inPassActiveState = false;
        if (this->ShouldStartTelemetryCapture())
        {
            Js::Tick start = Js::Tick::Now();
            AssertOnValidThread(this, RecyclerTelemetryInfo::StartPass);

#if DBG
            // validate state of existing GC pass stats structs
            uint16 count = 0;
            if (this->gcPassStats.Empty() == false)
            {
                GCPassStatsList::Iterator it = this->GetGCPassStatsIterator();
                while (it.Next())
                {
                    RecyclerTelemetryGCPassStats& curr = it.Data();
                    AssertMsg(curr.isGCPassActive == false, "unexpected value for isGCPassActive");
                    ++count;
                }
            }
            AssertMsg(count == this->passCount, "RecyclerTelemetryInfo::StartPass() - mismatch between passCount and count.");
#endif


            RecyclerTelemetryGCPassStats* stats = this->gcPassStats.PrependNodeNoThrow(&HeapAllocator::Instance);
            if (stats == nullptr)
            {
                // failed to allocate memory - disable any further telemetry capture for this recycler 
                // and free any existing GC stats we've accumulated
                this->abortTelemetryCapture = true;
                FreeGCPassStats();
                this->hostInterface->TransmitTelemetryError(*this, "Memory Allocation Failed");
            }
            else
            {
                this->inPassActiveState = true;
                passCount++;
                memset(stats, 0, sizeof(RecyclerTelemetryGCPassStats));

                stats->isGCPassActive = true;
                stats->passStartTimeTick = Js::Tick::Now();
                GetSystemTimePreciseAsFileTime(&stats->passStartTimeFileTime);
                if (this->hostInterface != nullptr)
                {
                    LPFILETIME ft = this->hostInterface->GetLastScriptExecutionEndTime();
                    stats->lastScriptExecutionEndTime = *ft;
                }

                stats->processCommittedBytes_start = RecyclerTelemetryInfo::GetProcessCommittedBytes();
                stats->processAllocaterUsedBytes_start = PageAllocator::GetProcessUsedBytes();
                stats->isInScript = this->recycler->GetIsInScript();
                stats->isScriptActive = this->recycler->GetIsScriptActive();

                this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerLeafPageAllocator(), &stats->threadPageAllocator_start);
                this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerPageAllocator(), &stats->recyclerLeafPageAllocator_start);
                this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerLargeBlockPageAllocator(), &stats->recyclerLargeBlockPageAllocator_start);
#ifdef RECYCLER_WRITE_BARRIER_ALLOC_SEPARATE_PAGE
                this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerWithBarrierPageAllocator(), &stats->recyclerWithBarrierPageAllocator_start);
#endif
                stats->startPassProcessingElapsedTime = Js::Tick::Now() - start;
            }
        }
    }

    void RecyclerTelemetryInfo::EndPass()
    {
        if (!this->inPassActiveState)
        {
            return;
        }
        this->inPassActiveState = false;

        Js::Tick start = Js::Tick::Now();

        AssertOnValidThread(this, RecyclerTelemetryInfo::EndPass);
        RecyclerTelemetryGCPassStats* lastPassStats = this->GetLastPassStats();

        lastPassStats->isGCPassActive = false;
        lastPassStats->passEndTimeTick = Js::Tick::Now();

        lastPassStats->processCommittedBytes_end = RecyclerTelemetryInfo::GetProcessCommittedBytes();
        lastPassStats->processAllocaterUsedBytes_end = PageAllocator::GetProcessUsedBytes();

        this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerLeafPageAllocator(), &lastPassStats->threadPageAllocator_end);
        this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerPageAllocator(), &lastPassStats->recyclerLeafPageAllocator_end);
        this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerLargeBlockPageAllocator(), &lastPassStats->recyclerLargeBlockPageAllocator_end);
#ifdef RECYCLER_WRITE_BARRIER_ALLOC_SEPARATE_PAGE
        this->FillInSizeData(this->recycler->GetHeapInfo()->GetRecyclerWithBarrierPageAllocator(), &lastPassStats->recyclerWithBarrierPageAllocator_end);
#endif

        // get bucket stats
        Js::Tick bucketStatsStart = Js::Tick::Now();
        BucketStatsReporter bucketReporter(this->recycler);
        this->recycler->GetHeapInfo()->GetBucketStats(bucketReporter);
        memcpy(&lastPassStats->bucketStats, bucketReporter.GetTotalStats(), sizeof(HeapBucketStats));
        lastPassStats->computeBucketStatsElapsedTime = Js::Tick::Now() - bucketStatsStart;

        lastPassStats->endPassProcessingElapsedTime = Js::Tick::Now() - start;

        if (ShouldTransmit() && this->hostInterface != nullptr)
        {
            if (this->hostInterface->TransmitTelemetry(*this))
            {
                this->lastTransmitTime = lastPassStats->passEndTimeTick;
                Reset();
            }
        }
    }

    void RecyclerTelemetryInfo::Reset()
    {
        FreeGCPassStats();
        memset(&this->threadPageAllocator_decommitStats, 0, sizeof(AllocatorDecommitStats));
        memset(&this->recyclerLeafPageAllocator_decommitStats, 0, sizeof(AllocatorDecommitStats));
        memset(&this->recyclerLargeBlockPageAllocator_decommitStats, 0, sizeof(AllocatorDecommitStats));
        memset(&this->threadPageAllocator_decommitStats, 0, sizeof(AllocatorDecommitStats));
    }

    void RecyclerTelemetryInfo::FreeGCPassStats()
    {
        if (this->gcPassStats.Empty() == false)
        {
            AssertMsg(this->passCount > 0, "unexpected value for passCount");
            AssertOnValidThread(this, RecyclerTelemetryInfo::FreeGCPassStats);
            this->gcPassStats.Clear();
            this->passCount = 0;
        }
    }

    bool RecyclerTelemetryInfo::ShouldTransmit() const
    {
        // for now, try to transmit telemetry when we have >= 16
        return (this->hostInterface != nullptr &&  this->passCount >= 16);
    }

    void RecyclerTelemetryInfo::IncrementUserThreadBlockedCount(Js::TickDelta waitTime, RecyclerWaitReason caller)
    {
        RecyclerTelemetryGCPassStats* lastPassStats = this->GetLastPassStats();
#ifdef DBG
        if (this->inPassActiveState)
        {
            AssertMsg(lastPassStats != nullptr && lastPassStats->isGCPassActive == true, "unexpected Value in  RecyclerTelemetryInfo::IncrementUserThreadBlockedCount");
        }
#endif

        if (this->inPassActiveState && lastPassStats != nullptr)
        {
            AssertOnValidThread(this, RecyclerTelemetryInfo::IncrementUserThreadBlockedCount);
            lastPassStats->uiThreadBlockedTimes[caller] += waitTime;
        }
    }

    bool RecyclerTelemetryInfo::IsOnScriptThread() const
    {
        bool isValid = false;
        if (this->hostInterface != nullptr)
        {
            if (this->hostInterface->IsThreadBound())
            {
                isValid = ::GetCurrentThreadId() == this->hostInterface->GetCurrentScriptThreadID();
            }
            else
            {
                isValid = ::GetCurrentThreadId() == this->mainThreadID;
            }
        }
        return isValid;
    }

    size_t RecyclerTelemetryInfo::GetProcessCommittedBytes()
    {
        HANDLE process = GetCurrentProcess();
        PROCESS_MEMORY_COUNTERS memCounters = { 0 };
        size_t committedBytes = 0;
        if (GetProcessMemoryInfo(process, &memCounters, sizeof(PROCESS_MEMORY_COUNTERS)))
        {
            committedBytes = memCounters.PagefileUsage;
        }
        return committedBytes;
    }

}
#endif
