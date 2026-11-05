// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <set>
#include <span>
#include <cstdlib>
#include <limits>
#include <atomic>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/gpu.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_system_resource.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/svc_results.h"
#include "core/memory.h"
#include "video_core/nx2.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"

namespace Service {

// Placeholder for the Switch 2 GPU session (nn::gpu::sdk), handed out by gsv 497.
// - TIPC 11: [first, last] id range (first <= min(last, 64)); ids outside it are masked out.
// - TIPC 12(id): event handle for that id (handle at word 3, result at word 4); the SDK attaches
//   it as a SystemEvent.
// - TIPC 21(id): starts an operation; the SDK then waits on the id's event. Nothing runs here, so
//   the event is signalled at once.
// - TIPC 22: another handle the SDK keeps like a session; a placeholder session is returned.
class INx2GpuSession final : public ServiceFramework<INx2GpuSession> {
public:
    explicit INx2GpuSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu"}, m_context{system_, "nx2:gpu"} {
        RegisterHandlersTipc({
            {0, &INx2GpuSession::Handshake, "Handshake"},
            {11, &INx2GpuSession::GetIdRange, "GetIdRange (guessed)"},
            {12, &INx2GpuSession::GetIdEvent, "GetIdEvent (guessed)"},
            {21, &INx2GpuSession::Start, "Start (guessed)"},
            {22, &INx2GpuSession::OpenSubSession, "OpenSubSession (guessed)"},
            {33, &INx2GpuSession::GetCount33, "Unknown33 (u32 count)"},
            {41, &INx2GpuSession::CreateGraphicsDevice, "CreateGraphicsDevice"},
            {42, &INx2GpuSession::DestroyGraphicsDevice, "DestroyGraphicsDevice"},
            {201, &INx2GpuSession::GetUserPartition, "GetUserPartition (newer SDKs)"},
            {202, &INx2GpuSession::GetGsvPartition, "GetGsvPartition (newer SDKs)"},
            {203, &INx2GpuSession::GetVaRange, "GetVaRange (guessed)"},
            // Sub-session commands (gpu 22 hands out a second session to this same object).
            {101, &INx2GpuSession::SubRegisterMemory, "CreateGpuAccessibleMemory"},
            {102, &INx2GpuSession::SubDestroyMemory, "DestroyGpuAccessibleMemory (guessed)"},
            {211, &INx2GpuSession::SubSubmitJob, "NormalMapGpuMemory4K"},
            {212, &INx2GpuSession::SubModifyKind, "NormalModifyMemoryKind4K"},
            {213, &INx2GpuSession::SubUnmapJob, "NormalUnmapGpuMemory4K"},
            {214, &INx2GpuSession::SubCreateGpuOnlyRegion, "CreateSparseRegion"},
            {215, &INx2GpuSession::SubDestroySparseRegion, "DestroySparseRegion"},
            {216, &INx2GpuSession::SubSparseMap, "ExecuteSparseMapRequests"},
            // Firmware commands without a known meaning: zeroed replies.
            {23, &INx2GpuSession::StubReplyZeroed, "Unknown23"},
            {24, &INx2GpuSession::StubReplyZeroed, "Unknown24"},
            {25, &INx2GpuSession::StubReplyZeroed, "Unknown25"},
            {34, &INx2GpuSession::StubReplyZeroed, "Unknown34"},
            {51, &INx2GpuSession::StubReplyZeroed, "Unknown51"},
            {8001, &INx2GpuSession::StubReplyZeroed, "Unknown8001"},
            {8002, &INx2GpuSession::StubReplyZeroed, "Unknown8002"},
            {8003, &INx2GpuSession::StubReplyZeroed, "Unknown8003"},
            {8004, &INx2GpuSession::StubReplyZeroed, "Unknown8004"},
        });
        if (LazySparseRegions()) {
            Tegra::Nx2SetSparseBacking(
                [this](u64 gpu_address, u64 size, const u8* data) {
                    BackSparseWrite(gpu_address, size, data);
                });
        }
    }

    ~INx2GpuSession() override {
        // Waits for a backing the GPU thread may be doing.
        Tegra::Nx2SetSparseBacking({});
        FreeReleasedRegionsAtExit();
        {
            // Live backings go back too. Emulation is stopping or the client is gone, so nothing
            // queued after the wait above still renders into them.
            std::scoped_lock lk{m_region_mutex};
            if (!m_gpu_only_regions.empty()) {
                LOG_DEBUG(Service, "nx2:gpu freeing {} live GPU-only backings",
                          m_gpu_only_regions.size());
                const auto done = std::make_shared<std::atomic<bool>>(true);
                for (auto& entry : m_gpu_only_regions) {
                    m_released_regions.push_back({std::move(entry.second), done});
                }
                m_gpu_only_regions.clear();
                FreeReleasedRegions();
            }
        }
        for (auto* event : m_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    void Handshake(HLERequestContext& ctx) {
        TipcHandshake(ctx, "nx2:gpu");
    }

    static constexpr u32 NumIds = 32;

    Kernel::KEvent* EventFor(u32 id) {
        id %= NumIds;
        if (m_events[id] == nullptr) {
            m_events[id] = m_context.CreateEvent(fmt::format("nx2:gpu:Event{}", id));
        }
        return m_events[id];
    }

    void GetIdRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 11, returning [0, {}]", NumIds - 1);
        TipcReplyWords(ctx, {0, NumIds - 1});
    }

    void GetIdEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 12 id={}", id);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, EventFor(id)->GetReadableEvent());
    }

    void Start(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 21 id={}, signalling completion", id);
        EventFor(id)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetCount33(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 33 [2]={:#x}, returning 1", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {1});
    }

    // Used by nn::gpu::sdk::CreateGpuAddressSpace: three u64s after the result, a kind (< 5, 0 =
    // none) and the GPU VA range of the in-process VirtualMachine that serves AllocSpace
    // (nn::gpu::sdk::util::ExecuteVirtualMachineAllocSpace). NVN reserves fixed VA from 0x8000000
    // up to 512 GiB, so report the same range NvRm gives AllocAsEx: [0x8000000, 0xC000000000).
    void GetVaRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 203 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 kind = 1;
        constexpr u64 start = 0x8000000ULL;
        constexpr u64 size = 0xC000000000ULL - start;
        TipcReplyWords(ctx, {static_cast<u32>(kind), static_cast<u32>(kind >> 32),
                             static_cast<u32>(start), static_cast<u32>(start >> 32),
                             static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    // Newer SDKs (title 1) query the address space's user and gsv partitions with 201/202(id):
    // two u64s each, stored as the partition's min/max. The user value works whether it is read
    // as (start, size) or (min, max). Title 4 never needed the gsv partition, so it stays empty.
    void GetUserPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 201 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 first = 0x8000000ULL;
        constexpr u64 second = 0xC000000000ULL - first;
        TipcReplyWords(ctx, {static_cast<u32>(first), static_cast<u32>(first >> 32),
                             static_cast<u32>(second), static_cast<u32>(second >> 32)});
    }

    // An empty partition is min = ~0, max = 0 (min > max). Fire Emblem: Fortune's Weave's SDK
    // reads (0, 0) as a one-byte partition at address 0 and aborts in nn::gpu::sdk.
    void GetGsvPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 202 [2]={:#x}, empty", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {~0U, ~0U, 0, 0});
    }

    // Fire Emblem: Fortune's Weave: 41(name u64, flags u64, debug label u64) returns a device id
    // that later NvRm calls name; 0 is not a valid id. Only flags 0 has been seen.
    void CreateGraphicsDevice(HLERequestContext& ctx) {
        const auto* b = ctx.CommandBuffer();
        const u64 name = u64{b[2]} | (u64{b[3]} << 32);
        const u64 flags = u64{b[4]} | (u64{b[5]} << 32);
        u32 id = 0;
        for (u32 candidate = 1; candidate < m_devices.size(); ++candidate) {
            if (!m_devices[candidate]) {
                m_devices[candidate] = true;
                id = candidate;
                break;
            }
        }
        LOG_INFO(Service, "nx2:gpu 41 create graphics device name={:#x} flags={:#x} -> id {}", name,
                 flags, id);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(id != 0 ? ResultSuccess : Kernel::ResultOutOfResource);
        rb.Push(id);
    }

    void DestroyGraphicsDevice(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2] & 0xff;
        const bool existed = id < m_devices.size() && std::exchange(m_devices[id], false);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(existed ? ResultSuccess : Kernel::ResultNotFound);
    }

    void OpenSubSession(HLERequestContext& ctx) {
        // Served by this object so sub-session jobs can signal the per-id events.
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 22, returning a sub-session on nx2:gpu");
        ReturnRawSession(system, ctx, std::static_pointer_cast<INx2GpuSession>(shared_from_this()));
    }

    // Sub 101: (start u64, last u64, flags...) after CreateGpuAccessibleMemory's svc 0x80.
    void SubRegisterMemory(HLERequestContext& ctx) {
        const auto* b = ctx.CommandBuffer();
        const u64 start = u64{b[2]} | (u64{b[3]} << 32);
        const u64 last = u64{b[4]} | (u64{b[5]} << 32);
        LOG_DEBUG(Service, "nx2:gpu sub 101 start={:#x} last={:#x} [6]={:#x} [7]={:#x}", start,
                    last, b[6], b[7]);
        if (last > start) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(start, last - start + 1);
            // Two-page regions hold queue semaphores: GPU releases go to the second page, waits
            // read the first (see video_core/nx2.h).
            if (last - start + 1 == 0x2000) {
                Tegra::Nx2AddSemaphoreRegion(start);
                LOG_DEBUG(Service, "nx2:gpu semaphore page {:#x} mirrored to {:#x}", start + 0x1000,
                         start);
            }
        }
        // nn::gpu::sdk::CreateGpuAccessibleMemory stores word 3 of the reply as the
        // GpuMemoryHandle that later map jobs (211) name.
        const u32 handle = m_next_memory_handle++;
        m_memory[handle] = {start, last - start + 1};
        TipcReplyWords(ctx, {handle});
    }

    // Jobs that complete through a job-slot event (word Slot of the request), like 211.
    template <size_t Slot>
    void SubSlotJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "(STUBBED) nx2:gpu {} words {:08x} slot={}", ctx.GetCommand(),
                 fmt::join(std::span<const u32>{w + 2, 18}, " "), w[Slot]);
        EventFor(w[Slot])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 211: job submission (nn::gpu::sdk job tracking). Word 2 is a constant id; word 17 is
    // the job slot whose event (gpu TIPC 12) the SDK then waits on. Nothing runs here, so
    // complete it at once.
    void SubSubmitJob(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        const u32 slot = cmd_buf[17];
        const u64 a = u64{cmd_buf[3]} | (u64{cmd_buf[4]} << 32);
        const u64 b = u64{cmd_buf[5]} | (u64{cmd_buf[6]} << 32);
        const u64 c = u64{cmd_buf[8]} | (u64{cmd_buf[9]} << 32);
        // Words 3-4: first GPU address, 5-6: last, 7: memory handle, 8-9: offset into it.
        const u32 handle = cmd_buf[7];
        LOG_DEBUG(Service, "nx2:gpu 211 map [{:#x}, {:#x}] handle={} offset={:#x} slot={}", a, b,
                 handle, c, slot);
        if (const auto it = m_memory.find(handle); it != m_memory.end() && b > a) {
            // Also at the memory's own address: an earlier 213 may have unmapped the mapping 101
            // made there.
            const u64 cpu = it->second.address + c;
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(handle, c, a, cpu, b - a + 1);
        } else {
            LOG_WARNING(Service, "nx2:gpu 211 map of unknown handle {}", handle);
        }
        EventFor(slot)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 212 (nn::gpu::sdk::NormalModifyMemoryKind4K, behind NvRmGpuMappingModify): word 2 the
    // address space, 3-4 first GPU address, 5-6 last, 7 the wanted (compressible) kind, 8 the
    // fallback kind, 9 a sync flag, 10 the job slot. The reply's word 3 is the kind applied. Kinds
    // do not matter to the emulated GPU, so keep the mapping and report the wanted kind.
    void SubModifyKind(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        const u32 kind = w[7] & 0xff;
        LOG_DEBUG(Service, "nx2:gpu 212 modify [{:#x}, {:#x}] kind={:#x} fallback={:#x} slot={}",
                  first, last, kind, w[8] & 0xff, w[10]);
        EventFor(w[10])->Signal(system.Kernel());
        TipcReplyWords(ctx, {kind});
    }

    // Sub 213: words 3-4 first GPU address, 5-6 last, job slot in word 8.
    void SubUnmapJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        LOG_DEBUG(Service, "nx2:gpu 213 unmap [{:#x}, {:#x}] slot={}", first, last, w[8]);
        if (last > first) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
        }
        SignalAfterGpu(w[8]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Signals a job slot's event once the GPU thread has run everything queued so far, as the
    // console's graphics server completes unmap jobs behind the submitted work.
    void SignalAfterGpu(u32 slot) {
        auto* const event = EventFor(slot);
        system.GPU().RunInOrder([this, event] { event->Signal(system.Kernel()); });
    }

    // Sub 102: word 2 is the memory handle 101 returned, word 3 the job slot. The game frees the
    // memory (svc 0x80 clears GPU access) and reuses it as ordinary heap right after, so nothing
    // on the GPU side may keep it: drop the mapping 101 made at its own address.
    void SubDestroyMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 handle = w[2];
        if (const auto it = m_memory.find(handle); it != m_memory.end()) {
            const auto [address, size] = it->second;
            LOG_DEBUG(Service, "nx2:gpu 102 destroy handle={} [{:#x}, +{:#x}] slot={}", handle,
                      address, size, w[3]);
            if (size != 0) {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(address, size);
            }
            if (size == 0x2000) {
                Tegra::Nx2RemoveSemaphoreRegion(address);
            }
            m_memory.erase(it);
        } else {
            LOG_WARNING(Service, "nx2:gpu 102 destroy of unknown handle {}", handle);
        }
        SignalAfterGpu(w[3]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 214: GPU-only memory [start, last] (words 3-6, job slot in word 11). The graphics
    // server backs it itself: the client never maps anything there but renders into it (title 4
    // keeps its render targets in six 300 MB regions at 0x81_0000_0000). Back each region with
    // kernel memory placed in the process's alias code region and map that for the GPU.
    void SubCreateGpuOnlyRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 start = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        const u32 slot = w[11];
        LOG_DEBUG(Service, "nx2:gpu 214 words {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
                 w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12]);
        if (last > start && ((last + 1 - start) % Core::Memory::YUZU_PAGESIZE) == 0) {
            const u64 size = last + 1 - start;
            std::scoped_lock lk{m_region_mutex};
            FreeReleasedRegions();
            if ((w[7] & SparseRegionFlag) == 0) {
                m_gpu_only_ranges[start] = size; // kept out of 216, however it is backed
            }
            if ((w[7] & SparseRegionFlag) != 0 && LazySparseRegions()) {
                // Title 1's terrain texture arrays and layer pools (170 MB-5.8 GB, mostly empty):
                // backed whole, they ran the pool dry and the later ones got no memory at all.
                // The region gets a device range of its own, unbacked pages read zero, and
                // BackSparseWrite gives a block memory before the GPU writes into it.
                m_sparse_regions[start] = {
                    .size = size,
                    .blocks = std::vector<u8>(Common::DivCeil(size, SparseBlockSize)),
                };
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().CreateSparse(start, size);
                Tegra::Nx2SparseRegionCreated(start, size, SparseBlockSize);
                Tegra::Nx2AddSparseSpan(start, size);
                LOG_INFO(Service, "nx2:gpu 214 sparse region [{:#x}, {:#x}] backed as the GPU writes it",
                         start, last);
            } else if (LazyGpuOnlyRegions()) {
                // The rest is written by the host GPU, not through guest memory (title 1's and
                // title 4's render targets, 285-300 MB regions): like a sparse region it gets a
                // device range of its own and memory per block only when something writes guest
                // memory there (downloads, copies). The texture cache sees ordinary memory.
                m_sparse_regions[start] = {
                    .gpu_only = true,
                    .size = size,
                    .blocks = std::vector<u8>(Common::DivCeil(size, SparseBlockSize)),
                };
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().CreateSparse(start, size);
                Tegra::Nx2AddSparseSpan(start, size);
                LOG_INFO(Service, "nx2:gpu 214 GPU-only region [{:#x}, {:#x}] backed as it is written",
                         start, last);
            } else if (const auto address = BackGpuOnlyRegion(start, size)) {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(start, *address, size);
                LOG_INFO(Service, "nx2:gpu 214 GPU-only region [{:#x}, {:#x}] backed at {:#x} "
                                  "({} MiB of GPU-only memory in use, peak {} MiB)",
                         start, last, *address, m_gpu_only_bytes >> 20, m_gpu_only_peak >> 20);
            }
        } else {
            LOG_WARNING(Service, "nx2:gpu 214 unexpected region [{:#x}, {:#x}]", start, last);
        }
        EventFor(slot)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 215 (nn::gpu::sdk::DestroySparseRegion): words 3-6 the range, word 8 the job slot, like
    // 213. Drop the GPU mapping, then the backing 214 added: its SMMU mapping (in GPU-thread
    // order) and, once the GPU thread is past that, its pages. Kept allocated, title 1 ran the
    // pool dry within minutes and later regions had no memory at all (GPU writes to them lost).
    void SubDestroySparseRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        LOG_DEBUG(Service, "nx2:gpu 215 destroy sparse region [{:#x}, {:#x}] words {:#x} {:#x} {:#x} {:#x}",
                 first, last, w[1], w[2], w[7], w[8]);
        std::scoped_lock lk{m_region_mutex};
        FreeReleasedRegions();
        if (m_gpu_only_ranges.erase(first) != 0 && last > first) {
            DropGpuOnlyTiles(first, last - first + 1, false);
            m_shared_regions.erase(first);
            std::erase_if(m_shared_memory, [first](const auto& entry) { return entry.second == first; });
        }
        if (last > first) {
            if (const auto region = m_sparse_regions.find(first); region != m_sparse_regions.end()) {
                m_tile_bytes -= Core::Memory::YUZU_PAGESIZE *
                                static_cast<u64>(std::count(region->second.tile_pages.begin(),
                                                            region->second.tile_pages.end(), true));
                m_sparse_regions.erase(region);
                Tegra::Nx2SparseRegionDestroyed(first);
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DestroySparse(first, last - first + 1);
            } else {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
            }
            // The region's backing: all of it, or the blocks a sparse region got as it was written.
            std::shared_ptr<std::atomic<bool>> done;
            for (auto region = m_gpu_only_regions.lower_bound(first);
                 region != m_gpu_only_regions.end() && region->first <= last;) {
                if (region->first + region->second.size - 1 > last) {
                    ++region;
                    continue;
                }
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().ReleaseBacking(
                    region->second.cpu_address, region->second.size);
                if (!done) {
                    done = std::make_shared<std::atomic<bool>>(false);
                    system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
                }
                m_released_regions.push_back({std::move(region->second), done});
                region = m_gpu_only_regions.erase(region);
            }
        }
        SignalAfterGpu(w[8]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 216 (nn::gpu::sdk::ExecuteSparseMapRequests, behind nvnMemoryPoolMapVirtual and NvRm's
    // remap): an A buffer of SparseMapRequests and a B buffer that gets the kind applied to each;
    // word 8-9 is 1, 10 the address space, 11-12 the request size (40), 13 a sync flag, 14 the job
    // slot. The reply is the number of requests done. This is how the game commits and releases
    // its sparse texture tiles: a map points a tile at its own memory (sub 101), an unmap makes it
    // read zero again. A region the game maps tiles into needs no memory of its own.
    void SubSparseMap(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 slot = w[14];
        const auto data = ctx.BufferDescriptorA().empty() ? std::span<const u8>{} : ctx.ReadBufferA(0);
        const size_t count = data.size() / sizeof(SparseMapRequest);
        std::vector<u8> kinds(count);
        {
            std::scoped_lock lk{m_region_mutex};
            FreeReleasedRegions();
            ++m_tile_calls;
            for (size_t i = 0; i < count; ++i) {
                SparseMapRequest request;
                std::memcpy(&request, data.data() + i * sizeof(SparseMapRequest), sizeof(request));
                kinds[i] = request.kind;
                ApplySparseMapRequest(request);
            }
            // The first request, then at most every 30 s.
            const auto now = std::chrono::steady_clock::now();
            if (count != 0 && (m_tile_requests == count ||
                               now - m_tile_log_time >= std::chrono::seconds{30})) {
                m_tile_log_time = now;
                LOG_INFO(Service, "nx2:gpu 216: {} calls, {} maps, {} unmaps; {} MiB of sparse tiles "
                                  "and {} MiB in GPU-only regions mapped, {} maps into regions that "
                                  "keep their own memory left out; {} MiB of sparse blocks backed, {} "
                                  "MiB released",
                         m_tile_calls, m_tile_maps, m_tile_unmaps, m_tile_bytes >> 20,
                         m_gpu_only_tile_bytes >> 20, m_tile_kept, m_sparse_backed_bytes >> 20,
                         m_sparse_released_bytes >> 20);
            }
        }
        if (!ctx.BufferDescriptorB().empty() && count != 0) {
            ctx.WriteBufferB(kinds.data(), kinds.size(), 0);
        }
        SignalAfterGpu(slot);
        TipcReplyWords(ctx, {static_cast<u32>(count), static_cast<u32>(u64{count} >> 32)});
    }

    static bool IsAllZero(const u8* data, size_t size) {
        size_t i = 0;
        for (; i + sizeof(u64) <= size; i += sizeof(u64)) {
            u64 word;
            std::memcpy(&word, data + i, sizeof(word));
            if (word != 0) {
                return false;
            }
        }
        for (; i < size; ++i) {
            if (data[i] != 0) {
                return false;
            }
        }
        return true;
    }

    // Returns the pages of destroyed regions the GPU thread is done with to the kernel; with
    // `force`, also those it may not be done with yet.
    void FreeReleasedRegions(bool force = false) {
        auto* const process = system.ApplicationProcess();
        std::erase_if(m_released_regions, [&](ReleasedRegion& released) {
            if (!force && !released.done->load(std::memory_order_acquire)) {
                return false;
            }
            auto& region = released.region;
            if (process != nullptr &&
                R_FAILED(process->GetPageTable().UnmapPageGroup(
                    region.cpu_address, *region.pages, Kernel::KMemoryState::IoMemory))) {
                LOG_ERROR(Service, "nx2:gpu could not unmap GPU-only backing {:#x}+{:#x}",
                          region.cpu_address, region.size);
                return true; // still mapped: keep its pages
            }
            region.pages->Close(system.Kernel());
            m_gpu_only_bytes -= region.size;
            return true;
        });
    }

    // Session teardown: waits (bounded) for the GPU thread to pass the released regions, then
    // frees them all. When emulation stops, the GPU thread is stopped before the services close
    // and never gets there; their pages are freed anyway rather than lost.
    void FreeReleasedRegionsAtExit() {
        {
            std::scoped_lock lk{m_region_mutex};
            FreeReleasedRegions();
            if (m_released_regions.empty()) {
                return;
            }
        }
        if (system.IsPoweredOn()) {
            struct Wake {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{};
            };
            auto wake = std::make_shared<Wake>();
            // Queued after the RunInOrder that set each pending `done`, so it runs after them.
            system.GPU().RunInOrder([wake] {
                {
                    std::scoped_lock lk{wake->mutex};
                    wake->done = true;
                }
                wake->cv.notify_all();
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            std::unique_lock lk{wake->mutex};
            while (!wake->done && system.IsPoweredOn()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    break;
                }
                wake->cv.wait_for(lk, (std::min<std::chrono::steady_clock::duration>)(
                                          deadline - now, std::chrono::milliseconds{50}));
            }
        }
        std::scoped_lock lk{m_region_mutex};
        FreeReleasedRegions();
        if (!m_released_regions.empty()) {
            LOG_WARNING(Service, "nx2:gpu freeing {} released GPU-only backings the GPU thread has "
                                 "not passed",
                        m_released_regions.size());
            FreeReleasedRegions(true);
        }
    }

    struct MappedBacking {
        u64 address;
        u64 size;
        std::unique_ptr<Kernel::KPageGroup> pages;
    };

    // Allocates kernel memory and maps it in the alias code region. Touches no region state, so
    // it needs no lock: the GPU back thread calls it without m_region_mutex, because mapping can
    // wait on the guest page table lock and a guest thread holding that lock must not be stalled
    // behind a host mutex the service thread is waiting for.
    std::optional<MappedBacking> AllocateGpuOnlyBacking(u64 size, bool log_failure, bool clear) {
        using Core::Memory::YUZU_PAGESIZE;
        auto& kernel_core = system.Kernel();
        auto* const process = system.ApplicationProcess();
        if (process == nullptr) {
            return std::nullopt;
        }
        auto pg = std::make_unique<Kernel::KPageGroup>(
            kernel_core, std::addressof(kernel_core.GetSystemSystemResource().GetBlockInfoManager()));
        // The graphics server's own memory, not the application's: taken from the application
        // pool, title 4's six regions left it 1.8 GB less than the kernel reports to it. Not
        // cleared: the GPU writes these before reading them, and touching 300 MB would commit it
        // on the host.
        if (R_FAILED(kernel_core.MemoryManager().AllocateAndOpen(
                pg.get(), size / YUZU_PAGESIZE,
                Kernel::KMemoryManager::EncodeOption(Kernel::KMemoryManager::Pool::SystemNonSecure,
                                                     Kernel::KMemoryManager::Direction::FromBack)))) {
            if (log_failure) {
                LOG_ERROR(Service, "nx2:gpu 214 could not allocate {:#x} bytes", size);
            }
            return std::nullopt;
        }
        if (clear) {
            // Cheap for memory that's still zero (most of it): only reused pages get cleared.
            for (const auto& block : *pg) {
                system.DeviceMemory().buffer.ClearBackingRegion(
                    GetInteger(block.GetAddress()) - Core::DramMemoryMap::Base, block.GetSize(), 0);
            }
        }
        auto& page_table = process->GetPageTable();
        const u64 region_start = GetInteger(page_table.GetAliasCodeRegionStart());
        const u64 region_size = page_table.GetAliasCodeRegionSize();
        constexpr u64 Alignment = 0x200000; // 2 MiB
        // From where the last backing went (sparse regions back hundreds of blocks), then wrapping.
        const u64 slots = region_size / Alignment;
        for (u64 i = 0; i < slots; ++i) {
            const u64 offset = ((m_alias_hint.load(std::memory_order_relaxed) + i) % slots) * Alignment;
            if (offset + size > region_size) {
                continue;
            }
            const Common::ProcessAddress address{region_start + offset};
            if (R_SUCCEEDED(page_table.MapPageGroup(address, *pg, Kernel::KMemoryState::IoMemory,
                                                    Kernel::KMemoryPermission::UserReadWrite))) {
                m_alias_hint.store((offset + Common::AlignUp(size, Alignment)) / Alignment,
                                   std::memory_order_relaxed);
                return MappedBacking{GetInteger(address), size, std::move(pg)};
            }
        }
        LOG_ERROR(Service, "nx2:gpu 214 no room for {:#x} bytes in the alias code region "
                           "[{:#x}, +{:#x})", size, region_start, region_size);
        pg->Close(kernel_core);
        return std::nullopt;
    }

    // Records a mapped backing for the region at gpu_address. Needs m_region_mutex.
    void RegisterGpuOnlyBacking(u64 gpu_address, MappedBacking&& backing) {
        // A title may create the same region again from the same start with a different size
        // (Mario Kart World walks 300 MB -> 600 MB -> 1 GB on the same address). Overwriting the
        // entry used to drop the previous mapping without undoing it: the backing stayed mapped
        // in the alias code region and its pages never went back to the pool, so the GPU-only
        // total grew without bound (250 MiB -> 6.5 GiB in one session) until the process died.
        // Release the old backing the same way 215 does, i.e. only once the GPU thread is past it.
        if (const auto it = m_gpu_only_regions.find(gpu_address); it != m_gpu_only_regions.end()) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(gpu_address, it->second.size);
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().ReleaseBacking(it->second.cpu_address,
                                                                        it->second.size);
            auto done = std::make_shared<std::atomic<bool>>(false);
            system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
            m_released_regions.push_back({std::move(it->second), done});
            m_gpu_only_regions.erase(it);
        }
        m_gpu_only_regions[gpu_address] = {backing.address, backing.size, std::move(backing.pages)};
        m_gpu_only_bytes += backing.size;
        m_gpu_only_peak = (std::max)(m_gpu_only_peak, m_gpu_only_bytes);
    }

    // Needs m_region_mutex.
    std::optional<u64> BackGpuOnlyRegion(u64 gpu_address, u64 size) {
        auto backing = AllocateGpuOnlyBacking(size, true, false);
        if (!backing) {
            return std::nullopt;
        }
        const u64 address = backing->address;
        RegisterGpuOnlyBacking(gpu_address, std::move(*backing));
        return address;
    }

    // Runs before a GPU-side write that may land in a sparse region (Tegra::Nx2BackSparseWrite,
    // usually on the GPU thread): the blocks the write touches get memory, cleared, behind the
    // region's device pages. The GPU mapping stays as it is, so the images over the region keep
    // their storage and only see the write.
    void BackSparseWrite(u64 gpu_address, u64 size, const u8* data) {
        struct Pending {
            u64 region;
            u64 block;
        };
        std::vector<Pending> todo;
        const u64 end = gpu_address + size;
        // Calls fn(region_start, region, first_block, last_block) for each region the write touches.
        auto for_regions = [&](auto&& fn) {
            auto it = m_sparse_regions.upper_bound(gpu_address);
            if (it != m_sparse_regions.begin()) {
                --it;
            }
            for (; it != m_sparse_regions.end() && it->first < end; ++it) {
                auto& [start, region] = *it;
                if (start + region.size <= gpu_address) {
                    continue;
                }
                fn(start, region, ((std::max)(gpu_address, start) - start) / SparseBlockSize,
                   ((std::min)(end, start + region.size) - 1 - start) / SparseBlockSize);
            }
        };
        bool waits_for_other = false;
        u64 zero_bytes = 0;
        {
            std::unique_lock lk{m_region_mutex};
            for_regions([&](u64 start, SparseRegion& region, u64 first, u64 last) {
                for (u64 block = first; block <= last; ++block) {
                    if (region.blocks[block] == BlockEmpty && region.tiles) {
                        // The write lands in the game's own memory (a tile it mapped) or nowhere;
                        // the texture cache only learns that the block has contents now.
                        region.blocks[block] = BlockWritten;
                        Tegra::Nx2SparseBlockWritten(start, block);
                    } else if (region.blocks[block] == BlockEmpty) {
                        // Zeros over a block nobody wrote change nothing: it reads zero without
                        // memory. Title 1 clears ~1 GB of them at boot.
                        const u64 block_begin = start + block * SparseBlockSize;
                        const u64 write_begin = (std::max)(gpu_address, block_begin);
                        const u64 write_end = (std::min)(
                            {end, block_begin + SparseBlockSize, start + region.size});
                        if (data != nullptr &&
                            IsAllZero(data + (write_begin - gpu_address), write_end - write_begin) &&
                            !Tegra::Nx2SparseBlockIsWritten(start, block)) {
                            zero_bytes += write_end - write_begin;
                            continue;
                        }
                        region.blocks[block] = BlockPending;
                        todo.push_back({start, block});
                    } else if (region.blocks[block] == BlockPending) {
                        waits_for_other = true;
                    }
                }
            });
        }
        if (zero_bytes != 0) {
            std::scoped_lock lk{m_region_mutex};
            const u64 before = m_sparse_zero_bytes;
            m_sparse_zero_bytes += zero_bytes;
            if (before == 0 || (before >> 28) != (m_sparse_zero_bytes >> 28)) {
                LOG_INFO(Service, "nx2:gpu {} MiB of zeros over unwritten sparse blocks left out "
                                  "(they read zero without memory)",
                         m_sparse_zero_bytes >> 20);
            }
        }
        // Allocating and mapping happens outside the lock (see AllocateGpuOnlyBacking).
        for (const auto& [start, block] : todo) {
            u64 block_size = SparseBlockSize;
            {
                std::scoped_lock lk{m_region_mutex};
                const auto it = m_sparse_regions.find(start);
                if (it != m_sparse_regions.end()) {
                    block_size = (std::min)(SparseBlockSize, it->second.size - block * SparseBlockSize);
                }
            }
            auto backing = AllocateGpuOnlyBacking(block_size, false, true);
            bool stale = false;
            {
                std::scoped_lock lk{m_region_mutex};
                const auto it = m_sparse_regions.find(start);
                if (it == m_sparse_regions.end() || block >= it->second.blocks.size()) {
                    stale = true; // the region was destroyed while its block was being backed
                } else if (it->second.tiles) {
                    // The game started mapping its tiles there meanwhile (216): no block.
                    stale = true;
                    it->second.blocks[block] = BlockWritten;
                    Tegra::Nx2SparseBlockWritten(start, block);
                } else if (!backing) {
                    auto& region = it->second;
                    region.blocks[block] = BlockFailed; // out of memory: not again for every write
                    if (!region.warned) {
                        region.warned = true;
                        LOG_WARNING(Service, "nx2:gpu sparse region at {:#x}: pool exhausted, GPU writes "
                                             "to its unbacked blocks are dropped",
                                    start);
                    }
                } else {
                    const u64 block_address = start + block * SparseBlockSize;
                    const u64 address = backing->address;
                    it->second.blocks[block] = BlockBacked;
                    RegisterGpuOnlyBacking(block_address, std::move(*backing));
                    Nvidia::Devices::Nx2GpuMemoryRegistry::Get().BackSparse(block_address, address,
                                                                            block_size);
                    Tegra::Nx2SparseBlockWritten(start, block);
                    m_sparse_backed_bytes += block_size;
                    if (++m_sparse_blocks == 1 || (m_sparse_blocks % 64) == 0) {
                        LOG_INFO(Service, "nx2:gpu {} sparse blocks backed ({} MiB); {} MiB of GPU-only "
                                          "memory in use, peak {} MiB",
                                 m_sparse_blocks, m_sparse_backed_bytes >> 20, m_gpu_only_bytes >> 20,
                                 m_gpu_only_peak >> 20);
                    }
                }
            }
            m_region_cv.notify_all();
            if (stale && backing) {
                // Never handed to the GPU: unmap and free right away.
                if (auto* const process = system.ApplicationProcess(); process != nullptr) {
                    process->GetPageTable().UnmapPageGroup(Common::ProcessAddress{backing->address},
                                                           *backing->pages,
                                                           Kernel::KMemoryState::IoMemory);
                }
                backing->pages->Close(system.Kernel());
            }
        }
        if (waits_for_other) {
            // Another thread is backing a block this write needs: wait until it is done.
            std::unique_lock lk{m_region_mutex};
            m_region_cv.wait(lk, [&] {
                bool pending = false;
                for_regions([&](u64, SparseRegion& region, u64 first, u64 last) {
                    for (u64 block = first; block <= last; ++block) {
                        pending |= region.blocks[block] == BlockPending;
                    }
                });
                return !pending;
            });
        }
    }

    // EDEN_NX2_SPARSE_LAZY=0 backs sparse regions whole, as before.
    static bool LazySparseRegions() {
        static const bool lazy = [] {
            const char* value = std::getenv("EDEN_NX2_SPARSE_LAZY");
            return value == nullptr || value[0] != '0';
        }();
        return lazy;
    }

    // EDEN_NX2_GPU_ONLY_LAZY=0 backs GPU-only regions whole, as before.
    static bool LazyGpuOnlyRegions() {
        static const bool lazy = [] {
            const char* value = std::getenv("EDEN_NX2_GPU_ONLY_LAZY");
            return value == nullptr || value[0] != '0';
        }();
        return lazy;
    }

    static constexpr u32 SparseRegionFlag = 0x200; ///< Word 7 of 214 (0x60c arrays, 0xa0c pools)
    static constexpr u64 SparseBlockSize = 4ULL << 20;
    static constexpr u8 BlockEmpty = 0;
    static constexpr u8 BlockBacked = 1;
    static constexpr u8 BlockFailed = 2;
    static constexpr u8 BlockPending = 3; ///< Being allocated and mapped by BackSparseWrite
    static constexpr u8 BlockWritten = 4; ///< Written in a region the game maps tiles into
    struct SparseRegion {
        bool gpu_only{};        ///< A GPU-only region (no sparse flag), backed lazily as well
        u64 size;
        std::vector<u8> blocks; ///< BlockEmpty / Backed / Failed / Pending / Written per block
        bool warned{};          ///< Logged that the pool ran out for its blocks
        bool tiles{};           ///< The game maps its own memory into it (216): no blocks
        std::vector<bool> tile_pages; ///< Pages a tile maps, once `tiles`
    };
    std::map<u64, SparseRegion> m_sparse_regions; ///< By GPU address
    u64 m_sparse_blocks{};       ///< Sparse blocks backed so far
    u64 m_sparse_backed_bytes{}; ///< Sparse region memory given out so far
    u64 m_sparse_zero_bytes{};   ///< Zeros written over unwritten sparse blocks, left out
    u64 m_sparse_released_bytes{}; ///< Blocks given back once the game mapped tiles there

    // nn::gpu::sdk::SparseMapRequest (216), filled from NvRm's nvgpu_as_remap_op.
    struct SparseMapRequest {
        u64 gpu_address;
        u64 size;
        u8 no_write;
        u8 cacheable;
        u8 unknown_12;
        u8 kind;          ///< Wanted: the compressible kind if there is one
        u8 fallback_kind; ///< The incompressible kind
        u8 unknown_15;    ///< Remap op flag bit 15
        u16 padding_16;
        u32 memory_handle; ///< Sub 101's handle; 0 unmaps the range
        u32 padding_1c;
        u64 offset; ///< Into that memory
    };
    static_assert(sizeof(SparseMapRequest) == 40);
    u64 m_tile_calls{};
    u64 m_tile_requests{};
    std::chrono::steady_clock::time_point m_tile_log_time{};
    u64 m_tile_maps{};
    u64 m_tile_unmaps{};
    u64 m_tile_kept{}; ///< Maps into GPU-only regions that keep their own memory, left out
    u64 m_tile_bytes{}; ///< Sparse region pages a tile maps right now

    void ApplySparseMapRequest(const SparseMapRequest& request) {
        ++m_tile_requests;
        if (request.size == 0) {
            return;
        }
        const u64 gpu_address = request.gpu_address;
        const u64 size = request.size;
        if (size > std::numeric_limits<u64>::max() - gpu_address) {
            LOG_WARNING(Service, "nx2:gpu 216 [{:#x}, +{:#x}] wraps the address space", gpu_address,
                        size);
            return;
        }
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        auto it = m_sparse_regions.upper_bound(gpu_address);
        SparseRegion* region = nullptr;
        u64 start = 0;
        if (it != m_sparse_regions.begin()) {
            --it;
            if (size <= it->second.size && gpu_address - it->first <= it->second.size - size) {
                start = it->first;
                region = &it->second;
            }
        }
        if (const auto gpu_only = GpuOnlyRegionOf(gpu_address, size)) {
            ApplyGpuOnlyRequest(*gpu_only, request);
            return;
        }
        if (request.memory_handle == 0) {
            ++m_tile_unmaps;
            LOG_DEBUG(Service, "nx2:gpu 216 unmap [{:#x}, +{:#x}]", gpu_address, size);
            if (region != nullptr) {
                Tegra::Nx2SparseTilesMapped(start, gpu_address - start, size, false);
                registry.MapSparseTile(gpu_address, 0, size);
                MarkTiles(*region, gpu_address - start, size, false);
            } else {
                registry.Unmap(gpu_address, size);
            }
            return;
        }
        ++m_tile_maps;
        const auto memory = m_memory.find(request.memory_handle);
        if (memory == m_memory.end() || request.offset > memory->second.size || size > memory->second.size - request.offset) {
            LOG_WARNING(Service, "nx2:gpu 216 map [{:#x}, +{:#x}] of unknown memory {} offset {:#x}",
                        gpu_address, size, request.memory_handle, request.offset);
            return;
        }
        const u64 cpu_address = memory->second.address + request.offset;
        LOG_DEBUG(Service, "nx2:gpu 216 map [{:#x}, +{:#x}] -> memory {} offset {:#x} kind {:#x}",
                  gpu_address, size, request.memory_handle, request.offset, request.kind);
        if (region == nullptr) {
            registry.MapAt(gpu_address, cpu_address, size);
            return;
        }
        if (!region->tiles) {
            StartTiles(start, *region);
        }
        // The game's memory already holds what the tile shows (title 1 fills nine world-sized
        // masks, R8/RG16/R16 at 8192-16384 texels, in its own memory before mapping them): the
        // blocks count as written, so the texture cache gives the images over them memory and
        // loads them. Left unwritten, those images read zero and the static world was unlit.
        const u64 first_block = (gpu_address - start) / SparseBlockSize;
        const u64 last_block = (gpu_address + size - 1 - start) / SparseBlockSize;
        for (u64 block = first_block; block <= last_block; ++block) {
            if (region->blocks[block] == BlockEmpty) {
                region->blocks[block] = BlockWritten;
            }
            Tegra::Nx2SparseBlockWritten(start, block);
        }
        Tegra::Nx2SparseTilesMapped(start, gpu_address - start, size, true);
        registry.MapSparseTile(gpu_address, cpu_address, size);
        MarkTiles(*region, gpu_address - start, size, true);
    }

    // Start of the region 214 made without the sparse flag that holds the range (by its flags,
    // not by how it is backed).
    std::optional<u64> GpuOnlyRegionOf(u64 gpu_address, u64 size) const {
        auto it = m_gpu_only_ranges.upper_bound(gpu_address);
        if (it == m_gpu_only_ranges.begin()) {
            return std::nullopt;
        }
        --it;
        if (size > it->second || gpu_address - it->first > it->second - size) {
            return std::nullopt;
        }
        return it->first;
    }

    // 216 into a GPU-only region: render targets, and with remap support also some texture arrays
    // (title 1: 5.8 GB and 1.4 GB). The game backs those with its own memory too, which it fills
    // through its own address, so a map points the range at that memory the way 211 does: both
    // addresses reach the same device pages and the texture cache sees the writes. (Re-pointing the
    // region's own device pages instead, as for sparse tiles, left the arrays stale: black terrain.)
    // Left alone:
    // - Regions whose memory the game shares with another GPU-only region (render targets): the
    //   texture cache would flush and re-upload such overlapping render targets every frame. All of
    //   a group keep the memory 214 gave them.
    // - Regions 214 backs as they are written: the texture cache reads an image's memory in full
    //   when it uploads it, which makes the game's pages behind it resident on the host, while
    //   unwritten blocks read as zero for free (title 1: ~1 GB less resident at 150 s).
    void ApplyGpuOnlyRequest(u64 region, const SparseMapRequest& request) {
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        const u64 gpu_address = request.gpu_address;
        const u64 size = request.size;
        if (request.memory_handle == 0) {
            ++m_tile_unmaps;
            DropGpuOnlyTiles(gpu_address, size, true);
            return;
        }
        ++m_tile_maps;
        if (m_sparse_regions.contains(region)) {
            ++m_tile_kept;
            return;
        }
        if (m_shared_regions.contains(region)) {
            m_shared_memory[{request.memory_handle, request.offset}] = region;
            ++m_tile_kept;
            return;
        }
        const auto memory = m_memory.find(request.memory_handle);
        if (memory == m_memory.end() || request.offset > memory->second.size || size > memory->second.size - request.offset) {
            LOG_WARNING(Service, "nx2:gpu 216 map [{:#x}, +{:#x}] of unknown memory {} offset {:#x}",
                        gpu_address, size, request.memory_handle, request.offset);
            return;
        }
        if (const auto other = RegionSharing(request.memory_handle, request.offset, size, region)) {
            ++m_tile_kept;
            for (const u64 shared : {region, *other}) {
                if (m_shared_regions.insert(shared).second) {
                    const auto range = m_gpu_only_ranges.find(shared);
                    if (range != m_gpu_only_ranges.end()) {
                        DropGpuOnlyTiles(shared, range->second, true);
                    }
                    LOG_INFO(Service, "nx2:gpu GPU-only region at {:#x} shares memory with another: "
                                      "it keeps its own",
                             shared);
                }
            }
            return;
        }
        DropGpuOnlyTiles(gpu_address, size, false);
        registry.MapAt(gpu_address, memory->second.address + request.offset, size);
        m_gpu_only_tiles[gpu_address] = {size, region, request.memory_handle, request.offset};
        m_gpu_only_memory[{request.memory_handle, request.offset}] = {request.offset + size, region};
        m_gpu_only_tile_max = (std::max)(m_gpu_only_tile_max, size);
        m_gpu_only_tile_bytes += size;
    }

    // Another GPU-only region whose tiles hold any of [offset, +size) of the memory, if one does.
    std::optional<u64> RegionSharing(u32 handle, u64 offset, u64 size, u64 region) const {
        auto it = m_gpu_only_memory.lower_bound({handle, offset + size});
        while (it != m_gpu_only_memory.begin()) {
            --it;
            const auto& [key, value] = *it;
            const auto& [end, owner] = value;
            if (key.first != handle || key.second + m_gpu_only_tile_max <= offset) {
                break;
            }
            if (end > offset && owner != region) {
                return owner;
            }
        }
        // Regions that already keep their own memory record no tiles: compare with their requests.
        if (const auto kept = m_shared_memory.find({handle, offset}); kept != m_shared_memory.end() &&
                                                                     kept->second != region) {
            return kept->second;
        }
        return std::nullopt;
    }

    // The memory 214 gave a GPU-only region at [gpu_address, +size), when it was backed whole.
    std::optional<u64> OwnBackingOf(u64 gpu_address, u64 size) const {
        auto it = m_gpu_only_regions.upper_bound(gpu_address);
        if (it == m_gpu_only_regions.begin()) {
            return std::nullopt;
        }
        --it;
        if (size > it->second.size || gpu_address - it->first > it->second.size - size) {
            return std::nullopt;
        }
        return it->second.cpu_address + (gpu_address - it->first);
    }

    // Forgets the game's tiles in [gpu_address, +size) of GPU-only regions; with `restore` (an
    // unmap) those ranges show the region's own memory again, or nothing where it has none.
    void DropGpuOnlyTiles(u64 gpu_address, u64 size, bool restore) {
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        auto it = m_gpu_only_tiles.upper_bound(gpu_address);
        if (it != m_gpu_only_tiles.begin()) {
            const auto previous = std::prev(it);
            if (previous->first + previous->second.size > gpu_address) {
                it = previous;
            }
        }
        while (it != m_gpu_only_tiles.end() && it->first < gpu_address + size) {
            const auto& [tile_address, tile] = *it;
            if (restore) {
                if (const auto backing = OwnBackingOf(tile_address, tile.size)) {
                    registry.MapAt(tile_address, *backing, tile.size);
                } else {
                    registry.Unmap(tile_address, tile.size);
                }
            }
            if (const auto owner = m_gpu_only_memory.find({tile.memory_handle, tile.offset});
                owner != m_gpu_only_memory.end() && owner->second.second == tile.region) {
                if (m_shared_regions.contains(tile.region)) {
                    m_shared_memory[owner->first] = tile.region;
                }
                m_gpu_only_memory.erase(owner);
            }
            m_gpu_only_tile_bytes -= tile.size;
            it = m_gpu_only_tiles.erase(it);
        }
    }

    // The game maps tiles into this region itself (216): writes outside its tiles are dropped, as
    // on the console, so it gets no more blocks, and the blocks it got before (written while no
    // tile was mapped, which the console would have dropped) go back.
    void StartTiles(u64 start, SparseRegion& region) {
        using Core::Memory::YUZU_PAGESIZE;
        region.tiles = true;
        region.tile_pages.assign(Common::DivCeil(region.size, YUZU_PAGESIZE), false);
        std::shared_ptr<std::atomic<bool>> done;
        u64 released = 0;
        for (u64 block = 0; block < region.blocks.size(); ++block) {
            if (region.blocks[block] != BlockBacked) {
                continue;
            }
            region.blocks[block] = BlockWritten;
            const u64 block_address = start + block * SparseBlockSize;
            const auto backing = m_gpu_only_regions.find(block_address);
            if (backing == m_gpu_only_regions.end()) {
                continue;
            }
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapSparseTile(block_address, 0,
                                                                        backing->second.size);
            if (!done) {
                done = std::make_shared<std::atomic<bool>>(false);
                system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
            }
            released += backing->second.size;
            m_released_regions.push_back({std::move(backing->second), done});
            m_gpu_only_regions.erase(backing);
        }
        m_sparse_released_bytes += released;
        LOG_INFO(Service, "nx2:gpu sparse region at {:#x}: the game maps its tiles, {} MiB of blocks "
                          "released",
                 start, released >> 20);
    }

    // Counts the bytes of tiles mapped, page by page.
    void MarkTiles(SparseRegion& region, u64 offset, u64 size, bool mapped) {
        using Core::Memory::YUZU_PAGESIZE;
        if (!region.tiles) {
            return;
        }
        const u64 last = (std::min)(Common::DivCeil(offset + size, YUZU_PAGESIZE),
                                    static_cast<u64>(region.tile_pages.size()));
        for (u64 page = offset / YUZU_PAGESIZE; page < last; ++page) {
            if (region.tile_pages[page] != mapped) {
                region.tile_pages[page] = mapped;
                if (mapped) {
                    m_tile_bytes += YUZU_PAGESIZE;
                } else {
                    m_tile_bytes -= YUZU_PAGESIZE;
                }
            }
        }
    }

    u64 m_gpu_only_bytes{};      ///< Backing of GPU-only regions and sparse blocks right now
    u64 m_gpu_only_peak{};
    std::atomic<u64> m_alias_hint{}; ///< 2 MiB slot after the last backing in the alias code region
    /// Regions are created and destroyed on the service thread and backed on the GPU thread.
    std::recursive_mutex m_region_mutex;
    std::condition_variable_any m_region_cv; ///< Signalled when a pending sparse block resolves

    struct GpuOnlyRegion {
        u64 cpu_address;
        u64 size;
        std::unique_ptr<Kernel::KPageGroup> pages;
    };
    struct ReleasedRegion {
        GpuOnlyRegion region;
        std::shared_ptr<std::atomic<bool>> done; ///< Set by the GPU thread past the region's last use
    };
    std::map<u64, GpuOnlyRegion> m_gpu_only_regions; ///< By GPU address
    std::map<u64, u64> m_gpu_only_ranges; ///< 214 regions without the sparse flag: GPU address -> size
    struct GpuOnlyTile {
        u64 size;
        u64 region; ///< Start of its GPU-only region
        u32 memory_handle;
        u64 offset;
    };
    std::map<u64, GpuOnlyTile> m_gpu_only_tiles; ///< The game's memory 216 put in GPU-only regions
    /// (memory handle, offset) -> (end offset, region) of those tiles, to find shared memory
    std::map<std::pair<u32, u64>, std::pair<u64, u64>> m_gpu_only_memory;
    /// Memory mapped into regions that keep their own (handle, offset) -> region
    std::map<std::pair<u32, u64>, u64> m_shared_memory;
    std::set<u64> m_shared_regions; ///< GPU-only regions sharing memory: they keep their own
    u64 m_gpu_only_tile_max{};   ///< Largest such tile, bounds the search
    u64 m_gpu_only_tile_bytes{}; ///< GPU-only region memory the game maps right now
    std::vector<ReleasedRegion> m_released_regions;

    struct GpuMemory {
        u64 address;
        u64 size;
    };

    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, NumIds> m_events{};
    std::map<u32, GpuMemory> m_memory;
    u32 m_next_memory_handle{1};
    std::array<bool, 64> m_devices{}; // gpu 41 graphics device ids (0 unused)

    // Every 30 s: the backing of sparse blocks and GPU-only regions is marked cold. The game
    // writes it once (texture uploads by 2D blits: title 1 fills 2-4 GB of texture pools) and it
    // is read again only when the texture cache builds an image over it, so the OS may compress or
    // page it out before anything else. Last member: stops before the regions are destroyed.
    void HintColdBacking() {
        struct Range {
            u64 physical_offset;
            u64 virtual_address;
            u64 size;
        };
        std::vector<Range> ranges;
        {
            std::scoped_lock lk{m_region_mutex};
            for (const auto& [gpu_address, region] : m_gpu_only_regions) {
                u64 virtual_address = region.cpu_address;
                for (const auto& block : *region.pages) {
                    ranges.push_back({GetInteger(block.GetAddress()) - Core::DramMemoryMap::Base,
                                      virtual_address, block.GetSize()});
                    virtual_address += block.GetSize();
                }
            }
        }
        auto& buffer = system.DeviceMemory().buffer;
        for (const Range& range : ranges) {
            buffer.HintCold(range.physical_offset, range.size, range.virtual_address);
        }
    }
    std::jthread m_cold_hint_thread{[this](std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:ColdHint");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        while (!stop.stop_requested()) {
            for (int i = 0; i < 30 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds{1});
            }
            if (!stop.stop_requested()) {
                HintColdBacking();
            }
        }
    }};
};

} // namespace Service
