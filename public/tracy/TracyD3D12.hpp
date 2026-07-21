#ifndef __TRACYD3D12_HPP__
#define __TRACYD3D12_HPP__

#ifndef TRACY_ENABLE

#define TracyD3D12Context(device, queue) nullptr
#define TracyD3D12Destroy(ctx)
#define TracyD3D12ContextName(ctx, name, size)

#define TracyD3D12NewFrame(ctx)

#define TracyD3D12Zone(ctx, cmdList, name)
#define TracyD3D12ZoneC(ctx, cmdList, name, color)
#define TracyD3D12NamedZone(ctx, varname, cmdList, name, active)
#define TracyD3D12NamedZoneC(ctx, varname, cmdList, name, color, active)
#define TracyD3D12ZoneTransient(ctx, varname, cmdList, name, active)

#define TracyD3D12ZoneS(ctx, cmdList, name, depth)
#define TracyD3D12ZoneCS(ctx, cmdList, name, color, depth)
#define TracyD3D12NamedZoneS(ctx, varname, cmdList, name, depth, active)
#define TracyD3D12NamedZoneCS(ctx, varname, cmdList, name, color, depth, active)
#define TracyD3D12ZoneTransientS(ctx, varname, cmdList, name, depth, active)

#define TracyD3D12Collect(ctx)

namespace tracy
{
    class D3D12ZoneScope {};
}

using TracyD3D12Ctx = void*;

#else

#include "Tracy.hpp"
#include "../client/TracyProfiler.hpp"
#include "../client/TracyCallstack.hpp"

#include <cstdlib>
#include <cassert>
#include <chrono>
#include <d3d12.h>
#include <dxgi.h>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#define TracyD3D12Panic(msg, ...) do { assert(false && "TracyD3D12: " msg); TracyMessageLC("TracyD3D12: " msg, tracy::Color::Red4); __VA_ARGS__; } while(false);
#define TracyD3D12Error(msg) do { TracyMessageLC("TracyD3D12: " msg, tracy::Color::Red4); } while(false);

namespace tracy
{

    struct D3D12QueryPayload
    {
        uint32_t m_queryIdStart = 0;
        uint32_t m_queryCount = 0;
        uint64_t m_fenceValue = 0;
        uint64_t m_connectionId = 0;
    };

    // Command queue context.
    class D3D12QueueCtx
    {
        friend class D3D12ZoneScope;

        ID3D12Device* m_device = nullptr;
        ID3D12CommandQueue* m_queue = nullptr;
        uint8_t m_contextId = 255;  // TODO: apparently, 255 means "invalid id"; is this documented somewhere?
        ID3D12QueryHeap* m_queryHeap = nullptr;
        ID3D12Resource* m_readbackBuffer = nullptr;

        // In-progress payload.
        uint32_t m_queryLimit = 0;
        std::atomic<uint32_t> m_queryCounter = 0;
        uint32_t m_previousQueryCounter = 0;
        uint32_t m_allocatedQueries = 0;
        std::mutex m_queryLock;

        uint64_t m_activePayload = 0;
        ID3D12Fence* m_payloadFence = nullptr;
        std::queue<D3D12QueryPayload> m_payloadQueue;
        std::vector<uint64_t> m_queryConnection;

        UINT64 m_prevCalibrationTicksCPU = 0;
        bool m_valid = false;
        bool m_submissionFailed = false;

        static tracy_force_inline uint64_t CurrentConnectionId()
        {
#ifdef TRACY_ON_DEMAND
            return GetProfiler().ConnectionId();
#else
            return 1;
#endif
        }

        static tracy_force_inline bool IsConnectionActive(uint64_t connectionId)
        {
#ifdef TRACY_ON_DEMAND
            return GetProfiler().IsConnected() && GetProfiler().ConnectionId() == connectionId;
#else
            (void)connectionId;
            return true;
#endif
        }

        static bool AllocateContextId(uint8_t& id)
        {
            auto& counter = GetGpuCtxCounter();
            auto current = counter.load(std::memory_order_relaxed);
            while (current < 255)
            {
                const auto next = static_cast<uint8_t>(current + 1);
                if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed))
                {
                    id = current;
                    return true;
                }
            }
            return false;
        }

        void ReleaseResources()
        {
            if (m_payloadFence)
            {
                m_payloadFence->Release();
                m_payloadFence = nullptr;
            }
            if (m_readbackBuffer)
            {
                m_readbackBuffer->Release();
                m_readbackBuffer = nullptr;
            }
            if (m_queryHeap)
            {
                m_queryHeap->Release();
                m_queryHeap = nullptr;
            }
        }

        bool WaitForPendingPayloads(uint32_t timeoutMs)
        {
            if (!m_payloadFence || m_activePayload == 0)
            {
                return true;
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                const auto completed = m_payloadFence->GetCompletedValue();
                if (completed >= m_activePayload)
                {
                    return true;
                }
                if (completed == UINT64_MAX || (m_device && FAILED(m_device->GetDeviceRemovedReason())))
                {
                    return false;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        void RecalibrateClocks(uint64_t connectionId)
        {
            UINT64 cpuTimestamp;
            UINT64 gpuTimestamp;
            if (FAILED(m_queue->GetClockCalibration(&gpuTimestamp, &cpuTimestamp)))
            {
                TracyD3D12Error("Failed to obtain queue clock calibration counters.");
                return;
            }

            int64_t cpuDeltaTicks = cpuTimestamp - m_prevCalibrationTicksCPU;
            if (cpuDeltaTicks > 0)
            {
                static const int64_t nanosecodsPerTick = int64_t(1000000000) / GetFrequencyQpc();
                int64_t cpuDeltaNS = cpuDeltaTicks * nanosecodsPerTick;
                // Save the device cpu timestamp, not the Tracy profiler timestamp:
                m_prevCalibrationTicksCPU = cpuTimestamp;

                cpuTimestamp = Profiler::GetTime();

                auto* item = Profiler::QueueSerialForConnection(connectionId);
                if (!item)
                {
                    return;
                }
                MemWrite(&item->hdr.type, QueueType::GpuCalibration);
                MemWrite(&item->gpuCalibration.gpuTime, gpuTimestamp);
                MemWrite(&item->gpuCalibration.cpuTime, cpuTimestamp);
                MemWrite(&item->gpuCalibration.cpuDelta, cpuDeltaNS);
                MemWrite(&item->gpuCalibration.context, GetId());
                Profiler::QueueSerialFinish();
            }
        }

        tracy_force_inline void SubmitDeferredQueueItem(tracy::QueueItem* item)
        {
#ifdef TRACY_ON_DEMAND
            GetProfiler().DeferItem(*item);
#endif
            Profiler::QueueSerialFinish();
        }

    public:
        D3D12QueueCtx(ID3D12Device* device, ID3D12CommandQueue* queue)
            : m_device(device)
            , m_queue(queue)
        {
            if (!device || !queue)
            {
                TracyD3D12Error("Cannot create a context for a null device or command queue.");
                return;
            }

            // Verify we support timestamp queries on this queue.

            if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY)
            {
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 featureData{};

                HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &featureData, sizeof(featureData));
                if (FAILED(hr) || (featureData.CopyQueueTimestampQueriesSupported == FALSE))
                {
                    TracyD3D12Error("Platform does not support profiling of copy queues.");
                    return;
                }
            }

            static constexpr uint32_t MaxQueries = 64 * 1024;  // Must be even, because queries are (begin, end) pairs
            m_queryLimit = MaxQueries;

            D3D12_QUERY_HEAP_DESC heapDesc{};
            heapDesc.Type = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            heapDesc.Count = m_queryLimit;
            heapDesc.NodeMask = 0;  // #TODO: Support multiple adapters.

            while (m_queryLimit >= 2 && FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
            {
                m_queryLimit = (m_queryLimit / 2) & ~1u;
                heapDesc.Count = m_queryLimit;
            }
            if (!m_queryHeap)
            {
                TracyD3D12Error("Failed to create a timestamp query heap.");
                return;
            }
            m_queryConnection.resize(m_queryLimit);

            // Create a readback buffer, which will be used as a destination for the query data.

            D3D12_RESOURCE_DESC readbackBufferDesc{};
            readbackBufferDesc.Alignment = 0;
            readbackBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackBufferDesc.Width = m_queryLimit * sizeof(uint64_t);
            readbackBufferDesc.Height = 1;
            readbackBufferDesc.DepthOrArraySize = 1;
            readbackBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            readbackBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;  // Buffers are always row major.
            readbackBufferDesc.MipLevels = 1;
            readbackBufferDesc.SampleDesc.Count = 1;
            readbackBufferDesc.SampleDesc.Quality = 0;
            readbackBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            D3D12_HEAP_PROPERTIES readbackHeapProps{};
            readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
            readbackHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            readbackHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            readbackHeapProps.CreationNodeMask = 0;
            readbackHeapProps.VisibleNodeMask = 0;  // #TODO: Support multiple adapters.

            if (FAILED(device->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE, &readbackBufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuffer))))
            {
                TracyD3D12Error("Failed to create query readback buffer.");
                return;
            }

            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_payloadFence))))
            {
                TracyD3D12Error("Failed to create payload fence.");
                return;
            }

            float period = [queue]()
            {
                uint64_t timestampFrequency;
                if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)))
                {
                    return 0.0f;
                }
                return static_cast<float>( 1E+09 / static_cast<double>(timestampFrequency) );
            }();

            if (period == 0.0f)
            {
                TracyD3D12Error("Failed to get timestamp frequency.");
                return;
            }

            uint64_t cpuTimestamp;
            uint64_t gpuTimestamp;
            if (FAILED(queue->GetClockCalibration(&gpuTimestamp, &cpuTimestamp)))
            {
                TracyD3D12Error("Failed to get queue clock calibration.");
                return;
            }

            // Save the device cpu timestamp, not the profiler's timestamp.
            m_prevCalibrationTicksCPU = cpuTimestamp;

            cpuTimestamp = Profiler::GetTime();

            // All checked: ready to roll. Context id 255 is reserved by the protocol.
            if (!AllocateContextId(m_contextId))
            {
                TracyD3D12Error("GPU context id space is exhausted.");
                return;
            }
            m_valid = true;

            auto* item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuNewContext);
            MemWrite(&item->gpuNewContext.cpuTime, cpuTimestamp);
            MemWrite(&item->gpuNewContext.gpuTime, gpuTimestamp);
            MemWrite(&item->gpuNewContext.thread, decltype(item->gpuNewContext.thread)(0)); // #TODO: why 0 instead of GetThreadHandle()?
            MemWrite(&item->gpuNewContext.period, period);
            MemWrite(&item->gpuNewContext.context, GetId());
            MemWrite(&item->gpuNewContext.flags, GpuContextCalibration);
            MemWrite(&item->gpuNewContext.type, GpuContextType::Direct3D12);
            SubmitDeferredQueueItem(item);
        }

        ~D3D12QueueCtx()
        {
            // Never let profiler teardown hang forever after device removal or a failed queue signal.
            bool releaseResources = true;
            if (m_valid)
            {
                const bool payloadsReady = WaitForPendingPayloads(2000);
                const bool deviceRemoved = m_device && FAILED(m_device->GetDeviceRemovedReason());
                if (payloadsReady)
                {
                    Collect();
                }
                if ((!payloadsReady || m_submissionFailed) && !deviceRemoved)
                {
                    // The GPU may still reference these objects. A bounded leak is safer than
                    // releasing resources that may still be in use after a timeout.
                    releaseResources = false;
                    TracyD3D12Error("GPU timestamp shutdown did not complete; retaining context resources.");
                }
            }
            while (!m_payloadQueue.empty())
            {
                m_payloadQueue.pop();
            }
            if (releaseResources)
            {
                ReleaseResources();
            }
            else
            {
                m_payloadFence = nullptr;
                m_readbackBuffer = nullptr;
                m_queryHeap = nullptr;
            }
        }


        bool NewFrame()
        {
            if (!m_valid || m_submissionFailed)
            {
                return false;
            }

            std::vector<D3D12QueryPayload> payloads;
            {
                std::lock_guard<std::mutex> lock(m_queryLock);
                const auto queryCount = m_queryCounter.exchange(0);
                if (queryCount == 0)
                {
                    return true;
                }

                uint32_t queryOffset = 0;
                while (queryOffset < queryCount)
                {
                    D3D12QueryPayload payload;
                    payload.m_queryIdStart = (m_previousQueryCounter + queryOffset) % m_queryLimit;
                    payload.m_connectionId = m_queryConnection[payload.m_queryIdStart];
                    payload.m_queryCount = 1;
                    while (queryOffset + payload.m_queryCount < queryCount)
                    {
                        const auto queryId = (m_previousQueryCounter + queryOffset + payload.m_queryCount) % m_queryLimit;
                        if (m_queryConnection[queryId] != payload.m_connectionId)
                        {
                            break;
                        }
                        payload.m_queryCount++;
                    }
                    queryOffset += payload.m_queryCount;
                    payloads.emplace_back(payload);
                }
                m_previousQueryCounter = (m_previousQueryCounter + queryCount) % m_queryLimit;
            }

            if (m_activePayload == UINT64_MAX - 1)
            {
                m_submissionFailed = true;
                TracyD3D12Error("Timestamp payload fence value space is exhausted.");
                return false;
            }
            const auto nextPayload = m_activePayload + 1;
            if (FAILED(m_queue->Signal(m_payloadFence, nextPayload)))
            {
                m_submissionFailed = true;
                TracyD3D12Error("Failed to signal the timestamp payload fence.");
                return false;
            }

            for (auto& payload : payloads)
            {
                payload.m_fenceValue = nextPayload;
                m_payloadQueue.emplace(payload);
            }
            m_activePayload = nextPayload;
            return true;
        }

        void Name( const char* name, uint16_t len )
        {
            if (!m_valid || !name || len == 0)
            {
                return;
            }

            auto ptr = (char*)tracy_malloc( len );
            memcpy( ptr, name, len );

            auto item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuContextName );
            MemWrite( &item->gpuContextNameFat.context, GetId());
            MemWrite( &item->gpuContextNameFat.ptr, (uint64_t)ptr );
            MemWrite( &item->gpuContextNameFat.size, len );
            SubmitDeferredQueueItem(item);
        }

        void Collect()
        {
            if (!m_valid || !m_payloadFence || m_payloadQueue.empty())
            {
                return;
            }

            const auto newestReadyPayload = m_payloadFence->GetCompletedValue();
            if (newestReadyPayload == UINT64_MAX)
            {
                while (!m_payloadQueue.empty())
                {
                    m_payloadQueue.pop();
                }
                std::lock_guard<std::mutex> lock(m_queryLock);
                m_allocatedQueries = 0;
                m_submissionFailed = true;
                return;
            }

            if (m_payloadQueue.front().m_fenceValue > newestReadyPayload)
            {
                return;
            }

#ifdef TRACY_ON_DEMAND
            const bool connected = GetProfiler().IsConnected();
#else
            const bool connected = true;
#endif
            const auto connectionId = CurrentConnectionId();

            void* readbackBufferMapping = nullptr;
            if (connected)
            {
                D3D12_RANGE mapRange{ 0, m_queryLimit * sizeof(uint64_t) };
                if (FAILED(m_readbackBuffer->Map(0, &mapRange, &readbackBufferMapping)))
                {
                    TracyD3D12Error("Failed to map the timestamp readback buffer; discarding ready payloads.");
                }
            }
            auto* timestampData = static_cast<uint64_t*>(readbackBufferMapping);

            bool timestampsSent = false;
            while (!m_payloadQueue.empty() && m_payloadQueue.front().m_fenceValue <= newestReadyPayload)
            {
                const auto& payload = m_payloadQueue.front();

                for (uint32_t j = 0; j < payload.m_queryCount; ++j)
                {
                    const auto counter = (payload.m_queryIdStart + j) % m_queryLimit;
                    if (timestampData && payload.m_connectionId == connectionId)
                    {
                        auto* item = Profiler::QueueSerialForConnection(payload.m_connectionId);
                        if (item)
                        {
                            MemWrite(&item->hdr.type, QueueType::GpuTime);
                            MemWrite(&item->gpuTime.gpuTime, timestampData[counter]);
                            MemWrite(&item->gpuTime.queryId, static_cast<uint16_t>(counter));
                            MemWrite(&item->gpuTime.context, GetId());
                            Profiler::QueueSerialFinish();
                            timestampsSent = true;
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_queryLock);
                    assert(m_allocatedQueries >= payload.m_queryCount);
                    m_allocatedQueries -= payload.m_queryCount;
                }
                m_payloadQueue.pop();
            }

            if (readbackBufferMapping)
            {
                m_readbackBuffer->Unmap(0, nullptr);
            }

            if (timestampsSent)
            {
                // Recalibrate to account for drift.
                RecalibrateClocks(connectionId);
            }
        }

        tracy_force_inline bool IsValid() const
        {
            return m_valid && !m_submissionFailed;
        }

    private:
        tracy_force_inline uint32_t NextQueryId(uint64_t connectionId)
        {
            std::lock_guard<std::mutex> lock(m_queryLock);
            auto queryCounter = m_queryCounter.load(std::memory_order_relaxed);
            if (m_allocatedQueries > m_queryLimit - 2)
            {
                TracyD3D12Error("Submitted too many GPU queries; dropping the zone.");
                return UINT32_MAX;
            }

            const uint32_t id = (m_previousQueryCounter + queryCounter) % m_queryLimit;
            m_queryConnection[id] = connectionId;
            m_queryConnection[id + 1] = connectionId;
            m_queryCounter.store(queryCounter + 2, std::memory_order_relaxed);
            m_allocatedQueries += 2;
            return id;
        }

        tracy_force_inline uint8_t GetId() const
        {
            return m_contextId;
        }
    };

    class D3D12ZoneScope
    {
        bool m_active = false;
        D3D12QueueCtx* m_ctx = nullptr;
        ID3D12GraphicsCommandList* m_cmdList = nullptr;
        uint32_t m_queryId = 0;  // Used for tracking in nested zones.
        uint64_t m_connectionId = 0;

        tracy_force_inline void WriteQueueItem(QueueItem* item, QueueType type, uint64_t srcLocation)
        {
            MemWrite(&item->hdr.type, type);
            MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
            MemWrite(&item->gpuZoneBegin.srcloc, srcLocation);
            MemWrite(&item->gpuZoneBegin.thread, GetThreadHandle());
            MemWrite(&item->gpuZoneBegin.queryId, static_cast<uint16_t>(m_queryId));
            MemWrite(&item->gpuZoneBegin.context, m_ctx->GetId());
            Profiler::QueueSerialFinish();
        }

        tracy_force_inline D3D12ZoneScope(D3D12QueueCtx* ctx, ID3D12GraphicsCommandList* cmdList, bool active)
        {
            if (!active || !ctx || !cmdList || !ctx->IsValid())
            {
                return;
            }
#ifdef TRACY_ON_DEMAND
            if (!GetProfiler().IsConnected())
            {
                return;
            }
#endif

            m_ctx = ctx;
            m_cmdList = cmdList;
            m_connectionId = m_ctx->CurrentConnectionId();
            if (!m_ctx->IsConnectionActive(m_connectionId))
            {
                return;
            }

            m_queryId = m_ctx->NextQueryId(m_connectionId);
            if (m_queryId == UINT32_MAX)
            {
                return;
            }
            m_active = true;
            m_cmdList->EndQuery(m_ctx->m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, m_queryId);
        }

    public:
        tracy_force_inline D3D12ZoneScope(D3D12QueueCtx* ctx, ID3D12GraphicsCommandList* cmdList, const SourceLocationData* srcLocation, bool active)
            : D3D12ZoneScope(ctx, cmdList, active)
        {
            if (!m_active) return;

            auto* item = Profiler::QueueSerialForConnection(m_connectionId);
            if (item)
            {
                WriteQueueItem(item, QueueType::GpuZoneBeginSerial, reinterpret_cast<uint64_t>(srcLocation));
            }
        }

        tracy_force_inline D3D12ZoneScope(D3D12QueueCtx* ctx, ID3D12GraphicsCommandList* cmdList, const SourceLocationData* srcLocation, int32_t depth, bool active)
            : D3D12ZoneScope(ctx, cmdList, active)
        {
            if (!m_active) return;

            auto* item = Profiler::QueueSerialCallstackForConnection(Callstack(depth), m_connectionId);
            if (item)
            {
                WriteQueueItem(item, QueueType::GpuZoneBeginCallstackSerial, reinterpret_cast<uint64_t>(srcLocation));
            }
        }

        tracy_force_inline D3D12ZoneScope(D3D12QueueCtx* ctx, uint32_t line, const char* source, size_t sourceSz, const char* function, size_t functionSz, const char* name, size_t nameSz, ID3D12GraphicsCommandList* cmdList, bool active)
            : D3D12ZoneScope(ctx, cmdList, active)
        {
            if (!m_active) return;

            const auto sourceLocation = Profiler::AllocSourceLocation(line, source, sourceSz, function, functionSz, name, nameSz);

            auto* item = Profiler::QueueSerialForConnection(m_connectionId);
            if (item)
            {
                WriteQueueItem(item, QueueType::GpuZoneBeginAllocSrcLocSerial, sourceLocation);
            }
            else
            {
                tracy_free_fast(reinterpret_cast<void*>(sourceLocation));
            }
        }

        tracy_force_inline D3D12ZoneScope(D3D12QueueCtx* ctx, uint32_t line, const char* source, size_t sourceSz, const char* function, size_t functionSz, const char* name, size_t nameSz, ID3D12GraphicsCommandList* cmdList, int32_t depth, bool active)
            : D3D12ZoneScope(ctx, cmdList, active)
        {
            if (!m_active) return;

            const auto sourceLocation = Profiler::AllocSourceLocation(line, source, sourceSz, function, functionSz, name, nameSz);

            auto* item = Profiler::QueueSerialCallstackForConnection(Callstack(depth), m_connectionId);
            if (item)
            {
                WriteQueueItem(item, QueueType::GpuZoneBeginAllocSrcLocCallstackSerial, sourceLocation);
            }
            else
            {
                tracy_free_fast(reinterpret_cast<void*>(sourceLocation));
            }
        }

        tracy_force_inline ~D3D12ZoneScope()
        {
            if (!m_active) return;

            const auto queryId = m_queryId + 1;  // Our end query slot is immediately after the begin slot.
            m_cmdList->EndQuery(m_ctx->m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryId);

            auto* item = Profiler::QueueSerialForConnection(m_connectionId);
            if (item)
            {
                MemWrite(&item->hdr.type, QueueType::GpuZoneEndSerial);
                MemWrite(&item->gpuZoneEnd.cpuTime, Profiler::GetTime());
                MemWrite(&item->gpuZoneEnd.thread, GetThreadHandle());
                MemWrite(&item->gpuZoneEnd.queryId, static_cast<uint16_t>(queryId));
                MemWrite(&item->gpuZoneEnd.context, m_ctx->GetId());
                Profiler::QueueSerialFinish();
            }

            m_cmdList->ResolveQueryData(m_ctx->m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, m_queryId, 2, m_ctx->m_readbackBuffer, m_queryId * sizeof(uint64_t));
        }
    };

    static inline D3D12QueueCtx* CreateD3D12Context(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        auto* ctx = static_cast<D3D12QueueCtx*>(tracy_malloc(sizeof(D3D12QueueCtx)));
        new (ctx) D3D12QueueCtx{ device, queue };
        if (!ctx->IsValid())
        {
            ctx->~D3D12QueueCtx();
            tracy_free(ctx);
            return nullptr;
        }

        return ctx;
    }

    static inline void DestroyD3D12Context(D3D12QueueCtx* ctx)
    {
        if (!ctx)
        {
            return;
        }
        ctx->~D3D12QueueCtx();
        tracy_free(ctx);
    }

}

#undef TracyD3D12Panic
#undef TracyD3D12Error

using TracyD3D12Ctx = tracy::D3D12QueueCtx*;

#define TracyD3D12Context(device, queue) tracy::CreateD3D12Context(device, queue);
#define TracyD3D12Destroy(ctx) tracy::DestroyD3D12Context(ctx);
#define TracyD3D12ContextName(ctx, name, size) ctx->Name(name, size);

#define TracyD3D12NewFrame(ctx) ctx->NewFrame();

#define TracyD3D12UnnamedZone ___tracy_gpu_d3d12_zone
#define TracyD3D12SrcLocSymbol TracyConcat(__tracy_d3d12_source_location,TracyLine)
#define TracyD3D12SrcLocObject(name, color) static constexpr tracy::SourceLocationData TracyD3D12SrcLocSymbol { name, TracyFunction, TracyFile, (uint32_t)TracyLine, color };

#if defined TRACY_HAS_CALLSTACK && defined TRACY_CALLSTACK
#  define TracyD3D12Zone(ctx, cmdList, name) TracyD3D12NamedZoneS(ctx, TracyD3D12UnnamedZone, cmdList, name, TRACY_CALLSTACK, true)
#  define TracyD3D12ZoneC(ctx, cmdList, name, color) TracyD3D12NamedZoneCS(ctx, TracyD3D12UnnamedZone, cmdList, name, color, TRACY_CALLSTACK, true)
#  define TracyD3D12NamedZone(ctx, varname, cmdList, name, active) TracyD3D12SrcLocObject(name, 0); tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyD3D12SrcLocSymbol, TRACY_CALLSTACK, active };
#  define TracyD3D12NamedZoneC(ctx, varname, cmdList, name, color, active) TracyD3D12SrcLocObject(name, color); tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyD3D12SrcLocSymbol, TRACY_CALLSTACK, active };
#  define TracyD3D12ZoneTransient(ctx, varname, cmdList, name, active) TracyD3D12ZoneTransientS(ctx, varname, cmdList, name, TRACY_CALLSTACK, active)
#else
#  define TracyD3D12Zone(ctx, cmdList, name) TracyD3D12NamedZone(ctx, TracyD3D12UnnamedZone, cmdList, name, true)
#  define TracyD3D12ZoneC(ctx, cmdList, name, color) TracyD3D12NamedZoneC(ctx, TracyD3D12UnnamedZone, cmdList, name, color, true)
#  define TracyD3D12NamedZone(ctx, varname, cmdList, name, active) TracyD3D12SrcLocObject(name, 0); tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyD3D12SrcLocSymbol, active };
#  define TracyD3D12NamedZoneC(ctx, varname, cmdList, name, color, active) TracyD3D12SrcLocObject(name, color); tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyD3D12SrcLocSymbol, active };
#  define TracyD3D12ZoneTransient(ctx, varname, cmdList, name, active) tracy::D3D12ZoneScope varname{ ctx, TracyLine, TracyFile, strlen(TracyFile), TracyFunction, strlen(TracyFunction), name, strlen(name), cmdList, active };
#endif

#ifdef TRACY_HAS_CALLSTACK
#  define TracyD3D12ZoneS(ctx, cmdList, name, depth) TracyD3D12NamedZoneS(ctx, TracyD3D12UnnamedZone, cmdList, name, depth, true)
#  define TracyD3D12ZoneCS(ctx, cmdList, name, color, depth) TracyD3D12NamedZoneCS(ctx, TracyD3D12UnnamedZone, cmdList, name, color, depth, true)
#  define TracyD3D12NamedZoneS(ctx, varname, cmdList, name, depth, active) TracyD3D12SrcLocObject(name, 0); tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyD3D12SrcLocSymbol, depth, active };
#  define TracyD3D12NamedZoneCS(ctx, varname, cmdList, name, color, depth, active) TracyD3D12SrcLocObject(name, color); tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyD3D12SrcLocSymbol, depth, active };
#  define TracyD3D12ZoneTransientS(ctx, varname, cmdList, name, depth, active) tracy::D3D12ZoneScope varname{ ctx, TracyLine, TracyFile, strlen(TracyFile), TracyFunction, strlen(TracyFunction), name, strlen(name), cmdList, depth, active };
#else
#  define TracyD3D12ZoneS(ctx, cmdList, name, depth) TracyD3D12Zone(ctx, cmdList, name)
#  define TracyD3D12ZoneCS(ctx, cmdList, name, color, depth) TracyD3D12Zone(ctx, cmdList, name, color)
#  define TracyD3D12NamedZoneS(ctx, varname, cmdList, name, depth, active) TracyD3D12NamedZone(ctx, varname, cmdList, name, active)
#  define TracyD3D12NamedZoneCS(ctx, varname, cmdList, name, color, depth, active) TracyD3D12NamedZoneC(ctx, varname, cmdList, name, color, active)
#  define TracyD3D12ZoneTransientS(ctx, varname, cmdList, name, depth, active) TracyD3D12ZoneTransient(ctx, varname, cmdList, name, active)
#endif

#define TracyD3D12Collect(ctx) ctx->Collect();

#endif

#endif
