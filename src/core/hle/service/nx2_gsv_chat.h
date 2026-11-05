// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"

namespace Service {

// GameChat's graphics side (nn::chat::gsv), handed out by gsv 1202. Read from the SDK's
// nn::chat::gsv::Initialize: TIPC 1() returns a u32 id at word 3 (it aborts with 2162-0001 if
// the id is over 0x3ff), then TIPC 11(u32 id) returns an event (copy handle at word 3, result at
// word 4) that it attaches as a system event. Everything else falls through to the nx2 auto stub.
class INx2ChatGsvSession final : public ServiceFramework<INx2ChatGsvSession> {
public:
    explicit INx2ChatGsvSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:chat:gsv"}, m_context{system_, "nx2:chat:gsv"} {
        RegisterHandlersTipc({
            {1, &INx2ChatGsvSession::GetEventId, "GetEventId (guessed)"},
            {11, &INx2ChatGsvSession::GetEvent, "GetEvent (guessed)"},
        });
    }

    ~INx2ChatGsvSession() override {
        for (auto& [id, event] : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    void GetEventId(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:chat:gsv TIPC 1, returning id 0");
        TipcReplyWords(ctx, {0});
    }

    void GetEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:chat:gsv TIPC 11 id={}, returning an event", id);
        auto*& event = m_events[id];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:chat:gsv:Event{}", id));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext m_context;
    std::map<u32, Kernel::KEvent*> m_events;
};

} // namespace Service
