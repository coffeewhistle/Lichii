// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <set>
#include "core/core.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"

namespace Service {

// Placeholder for the Switch 2 NVIDIA driver server (nn::nvsrv). It has no handlers, so every
// command (CMIF or TIPC) gets an empty success and is logged through the nx2: prefix.
class INx2NvsrvSession final : public ServiceFramework<INx2NvsrvSession> {
public:
    explicit INx2NvsrvSession(Core::System& system_) : ServiceFramework{system_, "nx2:nvsrv"} {}
};

// GPU time session handed out by gsv 1121. Welcome Tour opens it during graphics setup and
// terminated with InvalidHandle (0xe401) when the zeroed reply carried no session handle.
class INx2GpuTimeSession final : public ServiceFramework<INx2GpuTimeSession> {
public:
    explicit INx2GpuTimeSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu_time"} {
        RegisterHandlersTipc({
            {0, &INx2GpuTimeSession::Handshake, "Handshake"},
            {11, &INx2GpuTimeSession::QueryApplicationGpuTimeSpan, "QueryApplicationGpuTimeSpan"},
        });
    }

private:
    void Handshake(HLERequestContext& ctx) {
        TipcHandshake(ctx, "nx2:gpu_time");
    }

    void QueryApplicationGpuTimeSpan(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.PushRaw<u32>(IPC::ResultNotSupported.raw);
    }
};

// nn::camera::sdk camera context (gsv 602). GameChat builds its local camera video storage from it
// and polls it every frame. Nothing about the commands is known beyond the arguments seen (3 and
// 21 take the camera name "Default", 29 a u32 0x180 = 384, the width of the 384x216 camera images),
// and the game runs on an empty success, so they answer that without a log line per call.
class INx2CameraSession final : public ServiceFramework<INx2CameraSession> {
public:
    explicit INx2CameraSession(Core::System& system_) : ServiceFramework{system_, "nx2:camera"} {
        RegisterHandlersTipc({
            {3, &INx2CameraSession::Stub, "Unknown3 (camera name \"Default\")"},
            {21, &INx2CameraSession::Stub, "Unknown21 (camera name \"Default\")"},
            {29, &INx2CameraSession::Stub, "Unknown29 (u32 0x180, polled every frame)"},
        });
    }

private:
    void Stub(HLERequestContext& ctx) {
        const u32 command = ctx.GetCommand();
        if (m_logged.insert(command).second) {
            LOG_WARNING(Service, "(STUBBED) nx2:camera TIPC {} [2]={:#x} [3]={:#x}, returning success",
                        command, ctx.CommandBuffer()[2], ctx.CommandBuffer()[3]);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    std::set<u32> m_logged;
};

} // namespace Service
