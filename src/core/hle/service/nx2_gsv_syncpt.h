// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "video_core/host1x/host1x.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"

namespace Service {

// Placeholder for the Switch 2 syncpoint server (nn::syncpt), handed out by gsv 202. TIPC 1
// returns a u32 the SDK requires to be < 0x400 and then passes min(value, 63) to TIPC 21.
// Everything else is auto-stubbed through the nx2: prefix.
// - TIPC 0(u64): handshake, echo the token (newer SDKs).
// - TIPC 22(u32 slot): an event handle for that slot (copy handle at word 3, result at word 4).
//   NVN's NvOsEventPool asks for slots and multi-waits on them.
// - TIPC 201(slot, counter, u64 value, mode): signal the slot's event once the counter reaches
//   value. The counter is a SyncCounterId, the bitwise NOT of the host1x syncpoint id; mode 0
//   compares 32-bit wrapping, mode 1 64-bit. Eden's guest syncpoints back it.
// - TIPC 202(slot): cancel the slot's request.
class INx2SyncptSession final : public ServiceFramework<INx2SyncptSession> {
public:
    explicit INx2SyncptSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:syncpt"}, m_context{system_, "nx2:syncpt"} {
        RegisterHandlersTipc({
            {0, &INx2SyncptSession::Handshake, "Handshake"},
            {1, &INx2SyncptSession::GetCount, "GetCount (guessed)"},
            {22, &INx2SyncptSession::GetSlotEvent, "GetSlotEvent (guessed)"},
            {103, &INx2SyncptSession::QueryCounter, "QueryCounter"},
            {201, &INx2SyncptSession::RequestSlotEvent, "RequestSlotEvent (guessed)"},
            {202, &INx2SyncptSession::CancelSlotEvent, "CancelSlotEvent (guessed)"},
            // Firmware commands without a known meaning: zeroed replies.
            {21, &INx2SyncptSession::StubReplyZeroed, "Unknown21"},
        });
    }

    ~INx2SyncptSession() override {
        // A registered action may be running on the GPU thread right now: mark the session dead
        // under the lock it signals under, so it can't touch a request or event closed below.
        {
            std::scoped_lock lk{m_shared->mutex};
            m_shared->alive = false;
        }
        for (auto& [slot, request] : m_requests) {
            Cancel(request);
        }
        for (auto& [slot, event] : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    // Newer SDKs (title 1) open with TIPC 0(u64 token) and abort unless the reply echoes it.
    void Handshake(HLERequestContext& ctx) {
        TipcHandshake(ctx, "nx2:syncpt");
    }

    void GetCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 1, returning 32");
        TipcReplyWords(ctx, {32});
    }

    // From changes.patch: the counter is the bitwise NOT of the host1x syncpoint id.
    void QueryCounter(HLERequestContext& ctx) {
        const u32 syncpoint = ~ctx.CommandBuffer()[2];
        const u64 value = syncpoint < HostSyncpoints
                              ? system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(syncpoint)
                              : 0;
        LOG_DEBUG(Service, "nx2:syncpt 103 syncpoint={} value={:#x}", syncpoint, value);
        TipcReplyWords(ctx, {static_cast<u32>(value), static_cast<u32>(value >> 32), 0});
    }

    void GetSlotEvent(HLERequestContext& ctx) {
        const u32 slot = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 22 slot={:#x}, returning an event", slot);
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // Map nodes are stable, so a registered action can hold a reference to its request.
    struct SlotRequest {
        u32 syncpoint{};
        std::atomic<bool> pending{};
        Tegra::Host1x::SyncpointManager::ActionHandle handle{};
    };

    // Outlives the session (actions hold a reference): signalling happens under `mutex` and only
    // while `alive`.
    struct SharedState {
        std::mutex mutex;
        bool alive{true};
    };

    void RequestSlotEvent(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        const u32 slot = cmd_buf[2];
        const u32 syncpoint = ~cmd_buf[3];
        const u64 value = u64{cmd_buf[4]} | (u64{cmd_buf[5]} << 32);
        const u32 mode = cmd_buf[6];
        LOG_DEBUG(Service, "nx2:syncpt TIPC 201 slot={:#x} syncpoint={} value={:#x} mode={}", slot,
                  syncpoint, value, mode);
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
        }
        if (syncpoint >= HostSyncpoints) {
            LOG_WARNING(Service, "nx2:syncpt TIPC 201 syncpoint {} out of range, signalling",
                        syncpoint);
            event->Signal(system.Kernel());
            TipcReplyWords(ctx, {});
            return;
        }
        auto& request = m_requests[slot];
        Cancel(request);
        // A new request starts unsignalled. Slots are reused by different client objects: title 1's
        // movie player got slot 0 after NVN's fence waits had left it signalled, took that as a
        // decoded frame and aborted when the decoder had none.
        event->Clear(system.Kernel());
        request.syncpoint = syncpoint;
        request.pending = true;
        auto& manager = system.Host1x().GetSyncpointManager();
        // The action runs on the GPU thread under the manager's lock, or inline if the value is
        // already reached.
        request.handle = manager.RegisterGuestAction(
            syncpoint, static_cast<u32>(value),
            [shared = m_shared, &kernel = system.Kernel(), &request, event] {
                std::scoped_lock lk{shared->mutex};
                if (!shared->alive) {
                    return;
                }
                request.pending = false;
                event->Signal(kernel);
            });
        TipcReplyWords(ctx, {});
    }

    void CancelSlotEvent(HLERequestContext& ctx) {
        const u32 slot = ctx.CommandBuffer()[2];
        LOG_DEBUG(Service, "nx2:syncpt TIPC 202 slot={:#x}", slot);
        if (auto it = m_requests.find(slot); it != m_requests.end()) {
            Cancel(it->second);
        }
        // nn::syncpt::sdk's cancel always waits on the slot's event after this reply, so a cancel
        // signals it like a completion, also when nothing was pending; the client rechecks the
        // counter (TIPC 103) itself. Title 1 hung forever there when the GPU thread ran a frame
        // behind (EDEN_NX2_FRAMES_AHEAD) and a mid-frame wait outlasted NvOsEventPool's 100 ms,
        // and its movie player cancels each slot before its first request: signalled only for
        // pending requests, the player never got to start a movie.
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
        }
        event->Signal(system.Kernel());
        TipcReplyWords(ctx, {});
    }

    /// Returns true if the request was still pending.
    bool Cancel(SlotRequest& request) {
        if (request.pending.exchange(false)) {
            system.Host1x().GetSyncpointManager().DeregisterGuestAction(request.syncpoint,
                                                                        request.handle);
            return true;
        }
        return false;
    }

    static constexpr u32 HostSyncpoints = 192; // Tegra::Host1x::SyncpointManager

    KernelHelpers::ServiceContext m_context;
    std::map<u32, Kernel::KEvent*> m_events;
    std::map<u32, SlotRequest> m_requests;
    std::shared_ptr<SharedState> m_shared = std::make_shared<SharedState>();
};

} // namespace Service
