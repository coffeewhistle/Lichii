// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <span>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "core/core.h"
#include "core/arm/dynarmic/window_trace.h"
#include "video_core/gpu.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/nx2_video_decoder.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/nx2_image_layout.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"
#include "core/hle/service/nx2_gsv_stubs.h"
#include "core/hle/service/nx2_gsv_syncpt.h"
#include "core/hle/service/nx2_gsv_gpu.h"
#include "core/hle/service/nx2_gsv_chat.h"
#include "core/hle/service/nx2_gsv_vi2.h"
#include "core/hle/service/nx2_gsv_video.h"

namespace Service {

// Placeholder for the Switch 2 graphics-system server that nn::gsv::BootWithRemoteMemory talks
// to over TIPC. Nothing behind it is emulated: commands succeed and are logged, so boot can show
// what comes next. Commands 202 and 1402 hand out the syncpt and nvsrv sessions the SDK uses.
class INx2GraphicsSystemSession final : public ServiceFramework<INx2GraphicsSystemSession> {
public:
    explicit INx2GraphicsSystemSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gsv"}, m_context{system_, "nx2:gsv"} {
        RegisterHandlersTipc({
            {4, &INx2GraphicsSystemSession::Stub, "Boot (Switch 2, guessed)"},
            {5, &INx2GraphicsSystemSession::Stub, "Unknown5"},
            {101, &INx2GraphicsSystemSession::Stub, "Unknown101"},
            {202, &INx2GraphicsSystemSession::OpenSyncptSession, "OpenSyncptSession (guessed)"},
            {204, &INx2GraphicsSystemSession::AttachSyncptMemory, "AttachSyncptMemory (guessed)"},
            {305, &INx2GraphicsSystemSession::OpenViSession, "OpenViSession (guessed)"},
            {497, &INx2GraphicsSystemSession::OpenGpuSession, "OpenGpuSession (guessed)"},
            {111, &INx2GraphicsSystemSession::CreateNativeMemory, "CreateNativeMemory"},
            {112, &INx2GraphicsSystemSession::DestroyNativeMemory, "DestroyNativeMemory"},
            {114, &INx2GraphicsSystemSession::BindNativeMemory, "BindNativeMemory"},
            {314, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "DestroyNativeWindowAliasImagePool"},
            {315, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "CreateNativeWindowAliasImagePool"},
            {423, &INx2GraphicsSystemSession::BindNativeGpuMemory, "BindNativeGpuMemory"},
            {429, &INx2GraphicsSystemSession::CreateNativeImage, "CreateNativeImage"},
            {430, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyNativeImage"},
            {426, &INx2GraphicsSystemSession::QueryNativeImage, "MapNativeImage"},
            {427, &INx2GraphicsSystemSession::UnmapNativeImage, "UnmapNativeImage"},
            {428, &INx2GraphicsSystemSession::QueryNativeImage, "QueryNativeImage"},
            {435, &INx2GraphicsSystemSession::CalculateNativeImageSize, "CalculateNativeImageSize"},
            {1402, &INx2GraphicsSystemSession::OpenNvsrvSession, "OpenNvsrvSession (guessed)"},
            {1510, &INx2GraphicsSystemSession::CalculateRequirements, "CalculateRequirements"},
            {991, &INx2GraphicsSystemSession::CalculateDecoderImagePoolBuffer,
             "CalculateDecoderImagePoolBuffer"},
            {901, &INx2GraphicsSystemSession::Stub, "VideoInitialize (guessed)"},
            {902, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSession (guessed)"},
            {905, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSessionWithFlag"},
            {906, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSessionWithVersion"},
            {1001, &INx2GraphicsSystemSession::Stub, "VideoInitialize2 (guessed)"},
            {1002, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSession2 (guessed)"},
            {601, &INx2GraphicsSystemSession::Stub, "CameraInitialize (guessed)"},
            {602, &INx2GraphicsSystemSession::OpenCameraSession, "OpenCameraContext (guessed)"},
            {1201, &INx2GraphicsSystemSession::Stub, "ChatInitialize (guessed)"},
            {1202, &INx2GraphicsSystemSession::OpenChatSession, "OpenChatSession (guessed)"},
            // Same request as 429: nn::chat::gsv::CreateImagePool (GameChat camera images).
            {1211, &INx2GraphicsSystemSession::CreateNativeImage, "CreateChatImagePool (guessed)"},
            {1291, &INx2GraphicsSystemSession::CalculateChatImagePoolSize,
             "CalculateChatImagePoolSize (guessed)"},
            {911, &INx2GraphicsSystemSession::CreateDecoderImage, "CreateDecoderImage"},
            // 912(u32 image id): the video group's x12, freeing one decoder output image (Switch
            // Sports frees its four when its movie ends). Unlisting it stops the decode thread
            // writing frames into memory the game reuses.
            {912, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyDecoderImage"},
            {961, &INx2GraphicsSystemSession::GetCpuImageDescriptor, "GetCpuUserImageDescriptor"},
            // Firmware commands without a known meaning: zeroed replies.
            {11, &INx2GraphicsSystemSession::RegisterClientHandle, "RegisterClientHandle"},
            {113, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown113"},
            {115, &INx2GraphicsSystemSession::QueryGrantedFeatures, "QueryGrantedFeatures"},
            {120, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown120"},
            {121, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown121"},
            {122, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown122"},
            {123, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown123"},
            {124, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown124"},
            {125, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown125"},
            {126, &INx2GraphicsSystemSession::GetSessionFlag, "GetSessionFlag"},
            {131, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown131"},
            {133, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown133"},
            {187, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown187"},
            {201, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission201"},
            {211, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown211"},
            {212, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown212"},
            {213, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown213"},
            {214, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown214"},
            {215, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown215"},
            {301, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission301"},
            {303, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown303"},
            {311, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool311"},
            {312, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool312"},
            {321, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool321"},
            {361, &INx2GraphicsSystemSession::GetCpuImageDescriptor, "GetFeatureCpuImageDescriptor361"},
            {391, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize391"},
            {401, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission401"},
            {402, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission402"},
            {403, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission403"},
            {424, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown424"},
            {425, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown425"},
            {427, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown427"},
            {434, &INx2GraphicsSystemSession::AttachGpuAddressSpace, "AttachGpuAddressSpace"},
            {436, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown436"},
            {461, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown461"},
            {462, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown462"},
            {463, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown463"},
            {464, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown464"},
            {465, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown465"},
            {466, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown466"},
            {467, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown467"},
            {468, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown468"},
            {493, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown493"},
            {494, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission494"},
            {603, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown603"},
            {612, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool612"},
            {613, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown613"},
            {614, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown614"},
            {615, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool615"},
            {691, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize691"},
            {801, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission801"},
            {802, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown802"},
            {803, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown803"},
            {811, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool811"},
            {812, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool812"},
            {813, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown813"},
            {814, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown814"},
            {861, &INx2GraphicsSystemSession::GetCpuImageDescriptor, "GetFeatureCpuImageDescriptor861"},
            {891, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize891"},
            {1003, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1003"},
            {1011, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1011"},
            {1012, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1012"},
            {1091, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1091"},
            {1101, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission1101"},
            {1102, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission1102"},
            {1111, &INx2GraphicsSystemSession::SetFeatureVersion, "SetFeatureVersion"},
            {1112, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1112"},
            {1113, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1113"},
            {1121, &INx2GraphicsSystemSession::OpenGpuTimeSession, "OpenGpuTimeSession"},
            {1203, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1203"},
            {1212, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool1212"},
            {1501, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission1501"},
            {1502, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission1502"},
            {1503, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission1503"},
            {1601, &INx2GraphicsSystemSession::CheckPermission, "CheckPermission1601"},
            {1602, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1602"},
            {1603, &INx2GraphicsSystemSession::StubReplyZeroed, "Unknown1603"},
            {1611, &INx2GraphicsSystemSession::CreateNativeImage, "CreateFeatureImagePool1611"},
            {1612, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyFeatureImagePool1612"},
            {1691, &INx2GraphicsSystemSession::CalculateChatImagePoolSize, "CalculateFeatureImagePoolSize1691"},
        });
    }

    ~INx2GraphicsSystemSession() override {
        if (m_syncpt_poll) {
            system.CoreTiming().UnscheduleEvent(m_syncpt_poll);
        }
        for (auto* event : m_syncpt_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    // Feature permission checks (201, 301, 401-403, 1101, 1102, 1501-1503): each tests one bit of
    // the permissions the session was opened with and fails when it is clear. Games only ask for
    // features they are granted.
    void CheckPermission(HLERequestContext& ctx) {
        LOG_DEBUG(Service, "nx2:gsv TIPC {} permission check", ctx.GetCommand());
        TipcReplyWords(ctx, {});
    }

    // 115(u64 requested feature flags): the subset that is granted, which is all of them here.
    // A zeroed reply would deny every feature.
    void QueryGrantedFeatures(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_INFO(Service, "nx2:gsv TIPC 115 features {:#x}", u64{w[2]} | (u64{w[3]} << 32));
        TipcReplyWords(ctx, {w[2], w[3]});
    }

    // 126: one bit of the session's state (meaning unknown); clear.
    void GetSessionFlag(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {0});
    }

    // 434(u32 id from nx2:gpu 33): the session takes a reference on that GPU address space.
    void AttachGpuAddressSpace(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_INFO(Service, "nx2:gsv TIPC 434 address space {:#x}", id);
        m_address_spaces.insert(id);
        TipcReplyWords(ctx, {});
    }

    // 11(copy handle): handed over once per session and kept; a second call fails.
    void RegisterClientHandle(HLERequestContext& ctx) {
        if (m_client_handle_registered) {
            LOG_WARNING(Service, "nx2:gsv TIPC 11 called twice");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown);
            return;
        }
        m_client_handle_registered = true;
        TipcReplyWords(ctx, {});
    }

    // 1111(u32): a version or mode number the server checks and records.
    void SetFeatureVersion(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:gsv TIPC 1111 value {:#x}", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {});
    }

    std::set<u32> m_address_spaces;        ///< Ids attached with 434
    bool m_client_handle_registered = false; ///< Set by 11

    // Index of the first raw data word of a TIPC request: header (2 words), then an optional
    // special header with pid and copy/move handle words.
    static size_t TipcDataOffset(const u32* cmd_buf) {
        if ((cmd_buf[1] & 0x80000000) == 0) {
            return 2;
        }
        const u32 desc = cmd_buf[2];
        return 3 + ((desc & 1) ? 2 : 0) + ((desc >> 1) & 0xF) + ((desc >> 5) & 0xF);
    }

    void Stub(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                    ctx.GetCommand(), cmd_buf[2], cmd_buf[3], cmd_buf[4], cmd_buf[5]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // nn::codec::video::detail::ipc::Initialize sends 901, 902, 1001, 1002. 902 and 1002 return
    // a session handle at word 3 and the result at word 4; left to the auto stub, the handle was 0
    // and InitializeVp9Decoder failed with 2001-0114 (title 1 aborts when a movie starts).
    // Newer SDKs (nn::codec::video::detail::InitializeServerContext) send 906(u8, u64 decoder
    // library version) instead of 902; title 4 sends version 0x3'00030000 and aborted with
    // InvalidHandle at its first movie while 906 was auto-stubbed. The SDK of later titles
    // (ipc::Initialize(bool)) sends 905(u8) instead of 902 when its flag is set; the reply is the
    // same. 1001/1002 open the encoder's creator session (CreateVideoEncoderService uses it).
    void OpenVideoSession(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const bool second = ctx.GetCommand() == 1002;
        LOG_INFO(Service, "nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x}, returning a video decoder session",
                 ctx.GetCommand(), w[2], w[3], w[4]);
        ReturnRawSession(system, ctx,
                         std::make_shared<INx2VideoSession>(system, m_objects,
                                                            second ? "nx2:vdec2" : "nx2:vdec"));
    }

    // nn::camera::sdk::CreateCameraContext sends 601() then 602(u64 1), which returns the camera
    // context session (handle at word 3, result at word 4). Auto-stubbed, 602 had no handle, the
    // SDK sent 603 (destroy) and title 1 (1.8.0) aborted with 2034-0000. Its commands are
    // answered by INx2CameraSession.
    void OpenCameraSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 602, returning a nx2:camera session");
        ReturnRawSession(system, ctx, std::make_shared<INx2CameraSession>(system));
    }

    // nn::chat::gsv::Initialize sends 1201 then 1202, the same pair as 901/902 and 1001/1002.
    // Left to the auto stub it aborted with 2162-0001 (GameChat), so 1202 gets a session too.
    void OpenChatSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1202, returning a nx2:chat session");
        ReturnRawSession(system, ctx, std::make_shared<INx2ChatGsvSession>(system));
    }

    void OpenSyncptSession(HLERequestContext& ctx) {
        // Used by nn::syncpt::sdk::InitializeEx; handle at word 3, result at word 4.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 202, returning a nx2:syncpt session");
        ReturnRawSession(system, ctx, std::make_shared<INx2SyncptSession>(system));
    }

    void OpenGpuTimeSession(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:gsv TIPC 1121, returning a nx2:gpu_time session");
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuTimeSession>(system));
    }

    // nn::syncpt::sdk::InitializeEx sends two transfer memories (0x1000 bytes RW, then 0x2000
    // bytes the client maps read-only) and expects the server to have written a header into the
    // second one: u32 version (must be 1) at +0, u32 base id at +4, u32 count at +8 (the client
    // requires ((count + 0x40) >> 6) - 1 < 0x40), then offsets of: the u64 syncpoint values (RO,
    // +0xc), the RW "armed" summary word (+0x10), the RO summary word (+0x14), the RW armed bits
    // (+0x18) and the RO acknowledge bits (+0x1c).
    // To wait, the client makes its RW bit differ from the RO bit, signals the first returned
    // event (it attaches that handle as writable), and waits on the second until the server makes
    // the bits equal again; it then rereads the value and rearms if the threshold isn't reached.
    // Reply: the two handles (words 3 and 4), then the result.
    static constexpr u32 SyncptValuesOffset = 0x40;   // RO, u64 per syncpoint
    static constexpr u32 SyncptRoSummaryOffset = 0x1040;
    static constexpr u32 SyncptRoBitsOffset = 0x1100; // RO, up to 0x40 u64 words
    static constexpr u32 SyncptRwSummaryOffset = 0x0;
    static constexpr u32 SyncptRwBitsOffset = 0x100;  // RW, up to 0x40 u64 words
    static constexpr u32 HostSyncpoints = 192;        // Tegra::Host1x::SyncpointManager

    void AttachSyncptMemory(HLERequestContext& ctx) {
        // RequestParser doesn't skip TIPC handle words, so read the raw data words.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        const u32 version_requested = cmd_buf[data];
        const u32 max_syncpoints = std::min<u32>(cmd_buf[data + 1], (0x1000 - 0x40) / 8);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 204, version={} max_syncpoints={:#x}",
                    version_requested, max_syncpoints);

        auto rw = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
        auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
        if (rw.IsNotNull() && ro.IsNotNull() && ro->GetSize() >= 0x2000) {
            m_syncpt_memory = &ro->GetOwner()->GetMemory();
            m_syncpt_rw = GetInteger(rw->GetSourceAddress());
            m_syncpt_ro = GetInteger(ro->GetSourceAddress());
            m_syncpt_count = max_syncpoints;
            auto& memory = *m_syncpt_memory;
            memory.Write32(m_syncpt_ro + 0x00, 1); // version
            memory.Write32(m_syncpt_ro + 0x04, 0); // base id (guess)
            memory.Write32(m_syncpt_ro + 0x08, max_syncpoints);
            memory.Write32(m_syncpt_ro + 0x0c, SyncptValuesOffset);
            memory.Write32(m_syncpt_ro + 0x10, SyncptRwSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x14, SyncptRoSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x18, SyncptRwBitsOffset);
            memory.Write32(m_syncpt_ro + 0x1c, SyncptRoBitsOffset);
            UpdateSyncptValues();
            if (!m_syncpt_poll) {
                m_syncpt_poll = Core::Timing::CreateEvent(
                    "nx2:gsv:SyncptPoll",
                    [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                        PollSyncpts();
                        return std::nullopt;
                    });
                system.CoreTiming().ScheduleLoopingEvent(SyncptPollInterval, SyncptPollInterval,
                                                         m_syncpt_poll);
            }
        } else {
            LOG_ERROR(Service, "nx2:gsv TIPC 204: missing syncpt transfer memory");
        }

        if (m_syncpt_events[0] == nullptr) {
            m_syncpt_events[0] = m_context.CreateEvent("nx2:gsv:SyncptRequest");
            m_syncpt_events[1] = m_context.CreateEvent("nx2:gsv:SyncptNotify");
        }
        IPC::ResponseBuilder rb{ctx, 2, 2, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, *m_syncpt_events[0]); // signalled by the client
        rb.PushCopyObjects(ctx, m_syncpt_events[1]->GetReadableEvent());
    }

    // Mirrors Eden's guest syncpoint values into the shared RO page. Returns true if any changed.
    bool UpdateSyncptValues() {
        auto& manager = system.Host1x().GetSyncpointManager();
        bool changed = false;
        for (u32 id = 0; id < std::min(m_syncpt_count, HostSyncpoints); ++id) {
            const u64 value = manager.GetGuestSyncpointValue(id);
            const u64 address = m_syncpt_ro + SyncptValuesOffset + id * 8;
            if (m_syncpt_memory->Read64(address) != value) {
                m_syncpt_memory->Write64(address, value);
                changed = true;
            }
        }
        return changed;
    }

    // Answers every armed wait: the value page is current, so the client rechecks its threshold
    // and rearms if it still has to wait. Acknowledging without a value change costs the client a
    // wakeup per poll, which keeps waits whose syncpoint we never see from hanging.
    void PollSyncpts() {
        if (m_syncpt_memory == nullptr) {
            return;
        }
        const u32 words = (m_syncpt_count + 63) / 64;
        std::array<u64, 0x40> armed{};
        for (u32 w = 0; w < words; ++w) {
            armed[w] = m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwBitsOffset + w * 8);
        }
        UpdateSyncptValues();
        bool acked = false;
        for (u32 w = 0; w < words; ++w) {
            const u64 ro_address = m_syncpt_ro + SyncptRoBitsOffset + w * 8;
            if (m_syncpt_memory->Read64(ro_address) != armed[w]) {
                m_syncpt_memory->Write64(ro_address, armed[w]);
                acked = true;
            }
        }
        m_syncpt_memory->Write64(m_syncpt_ro + SyncptRoSummaryOffset,
                                 m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwSummaryOffset));
        if (acked) {
            m_syncpt_events[1]->Signal(system.Kernel());
        }
    }

    // nn::gsv::shim::CalculateRequirements(u64* size, u64* alignment, preference, u32): the 0x40-byte
    // InterModuleUserImagePreference (words 2..17) and a u32 (word 18). A block-linear preference
    // (MakeInterModuleBlockUserImagePreference) holds kind 2 at word 3, (width - 1) | (height - 1)
    // << 16 at word 4, two u32s at words 6-7 and the pixel format at word 8. Reply: result, u64
    // size, u64 alignment; the client raises the alignment to at least 4 KiB and rounds the size.
    void CalculateRequirements(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateNativeImageRequirements(
            std::span<const u32, 16>{w + 2, 16}, w[18]);
        const auto trace = Core::WindowTrace::Enabled()
                               ? Core::WindowTrace::sessions.Find(ctx.GetThread().GetOwnerProcess())
                               : nullptr;
        if (trace && trace->TakeGsvLog()) {
            LOG_INFO(Service, "Star Fox gsv 1510: session={} tls={:#x} kind={} {}x{} raw_block={} raw_pitch_gobs={} format={} block_format_and_luminance={:#x} count={} -> pitch={:#x} block_rows_log2={} size={:#x} alignment={:#x}",
                     trace->id, GetInteger(ctx.GetThread().GetTlsAddress()), w[3], layout.width, layout.height,
                     w[6], w[7], w[8], w[9], w[18],
                     layout.pitch, layout.block_height_log2, layout.size, layout.alignment);
        }
        LOG_DEBUG(Service, "nx2:gsv 1510 kind={} {}x{} block={} pitch={:#x} format={} -> size={:#x} align={:#x}",
                  w[3], layout.width, layout.height, layout.block_height_log2, layout.pitch, w[8],
                  layout.size, layout.alignment);
        TipcReplyWords(ctx, {static_cast<u32>(layout.size), static_cast<u32>(layout.size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // nn::codec::CalculateDecoderImagePoolBuffer(size*, alignment*, VideoDecoderImagePreference,
    // u32 count) sends 991(preference[16], u32 count) and reads result, u64 size, u64 alignment;
    // the client raises the alignment to at least 4 KiB and rounds the size up to it. The
    // preference (MakeDecodeImagePreference) holds an output layout code at word 1 (1/4 for
    // format 3, which the movie player uses, read here as NV12) and (width - 1) | (height - 1) << 16
    // at word 2. Left to the auto stub, the reply words keep the request's, so the size became
    // 0x167027f'00000001 and title 1's movie player aborted (main+0x19e5b58) when its first movie
    // started. Sizes are block-linear NV12 (luma, then half-height chroma, 128-row blocks) or 4
    // bytes per pixel for the other codes; nothing is decoded into them yet.
    void CalculateDecoderImagePoolBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateDecoderImageLayout(w + 2);
        const u32 count = std::max<u32>(w[18], 1);
        const u64 size = layout.size * count;
        const u64 alignment = 64 * 128;
        LOG_INFO(Service, "nx2:gsv 991 decoder image pool code={} {}x{} count={} -> size={:#x}",
                 layout.code, layout.width, layout.height, count, size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(alignment), static_cast<u32>(alignment >> 32)});
    }

    // 435(preference[16], u32 count): result, u64 size of a GPU native image.
    void CalculateNativeImageSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Nx2CalculateImageLayout(w + 2).size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 435 count={} -> size={:#x}", w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    static u64 Pair(const u32* w, size_t i) {
        return u64{w[i]} | (u64{w[i + 1]} << 32);
    }

    // 111(u8 mode at word 2, u64 size): result, u32 native memory id.
    void CreateNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Pair(w, 3);
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_memory++;
        m_objects->memories[id] = {size, 0};
        LOG_INFO(Service, "nx2:gsv 111 CreateNativeMemory mode={} size={:#x} -> id {}", w[2] & 0xFF,
                 size, id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyNativeMemory(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::vector<std::optional<std::pair<u64, u64>>> unmaps;
        {
            std::scoped_lock lk{m_objects->mutex};
            for (auto& [image_id, image] : m_objects->images) {
                if (image.memory == id && image.mapped_here) {
                    unmaps.push_back(TakeDecoderMapping(image));
                }
            }
            m_objects->memories.erase(id);
        }
        for (const auto& range : unmaps) {
            DropDecoderMapping(range);
        }
        LOG_DEBUG(Service, "nx2:gsv 112 DestroyNativeMemory id {}", id);
        TipcReplyWords(ctx, {});
    }

    // 114(u32 id, u64 address, u64 size, u64 flags, u64 offset): back native memory with process
    // memory.
    void BindNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "114");
        TipcReplyWords(ctx, {});
    }

    // 423(u32 id, u64 address, u64 size, u32 parent GpuMemoryHandle, u64 offset, ...): back
    // native memory with part of GPU-accessible memory.
    void BindNativeGpuMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "nx2:gsv 423 parent={} offset={:#x}", w[7], Pair(w, 8));
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "423", w[7], Pair(w, 8));
        TipcReplyWords(ctx, {});
    }

    void BindMemory(u32 id, u64 address, u64 size, const char* command, u32 parent = 0,
                    u64 parent_offset = 0) {
        std::scoped_lock lk{m_objects->mutex};
        auto& memory = m_objects->memories[id];
        memory.address = address;
        memory.parent = parent;
        memory.parent_offset = parent_offset;
        if (memory.size == 0) {
            memory.size = size;
        }
        LOG_INFO(Service, "nx2:gsv {} bind native memory {} at {:#x}+{:#x}", command, id, address,
                 size);
    }

    // 429(preference[16], u32 count, u32 memory id): result, u32 native image id.
    void CreateNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_image;
        auto& image = m_objects->images[id];
        image.count = std::max<u32>(w[18], 1);
        // nn::gs::sdk::CalculateNativeImageHandle(pool, i) is pool + i, so keep those free.
        m_objects->next_image += image.count;
        image.memory = w[19];
        std::copy_n(w + 2, image.preference.size(), image.preference.begin());
        image.size = Nx2CalculateImageLayout(w + 2).size;
        LOG_INFO(Service, "nx2:gsv 429 CreateNativeImage memory={} count={} size={:#x} -> id {}",
                 w[19], w[18], image.size, id);
        TipcReplyWords(ctx, {id});
    }

    // Pool size of a feature's images (1291 here; 391, 691, 891 and 1691 for the other feature
    // groups, which share the server template).
    // nn::chat::gsv::CalculateRequiredBufferMemory: 1291(preference[16], u32 count), the request
    // of 1510, replies result, u64 size, u64 alignment. Auto-stubbed, the size became 0x1000 and
    // the camera pool (1211) got memory for none of its images.
    void CalculateChatImagePoolSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        const u64 size = layout.size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 1291 {}x{} count={} -> size={:#x}", layout.width,
                  layout.height, w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // 428(u32 native image handle) nn::gpu::sdk::QueryNativeImage and 426(u32 handle, u32, u32
    // address space, u8 permission) nn::gpu::sdk::MapNativeImage, used by NVN for images another
    // server created (GameChat's camera pool from 1211). Reply words 3-4 the GPU address of the
    // image (426 only; GPU == CPU address, see Nx2NativeObjects::Memory), 5-6 the u64 offset of
    // the image in its memory, 7-38 a GpuOunceUserImageDescriptor and 39 the flags. The SDK
    // requires descriptor byte 0x00 == 1, byte 0x20 == 1 and a non-zero size at 0x2c (offset at
    // 0x28); NVN reads (width - 1) | (height - 1) << 16 at 0x08 and the layout kind at 0x24 (1
    // takes RGBA formats 0x25/0x37/0x38, 2 the 0x99-0x9c formats). Block height and pitch use
    // the 429 preference words (0x10, 0x14), a guess.
    void QueryNativeImage(HLERequestContext& ctx) {
        const u32 handle = ctx.CommandBuffer()[2];
        if (QueryDecoderImage(ctx, handle)) {
            return;
        }
        std::array<u32, 16> preference{};
        u64 image_offset = 0;
        u64 memory_address = 0;
        {
            std::scoped_lock lk{m_objects->mutex};
            auto it = m_objects->images.upper_bound(handle);
            if (it != m_objects->images.begin()) {
                --it;
                if (handle - it->first < it->second.count) {
                    preference = it->second.preference;
                    image_offset = (handle - it->first) * it->second.size;
                    if (const auto memory = m_objects->memories.find(it->second.memory);
                        memory != m_objects->memories.end()) {
                        memory_address = memory->second.address;
                    }
                }
            }
        }
        const auto layout = Nx2CalculateImageLayout(preference.data());
        const u64 gpu_address = ctx.GetCommand() == 426 ? memory_address + image_offset : 0;
        std::array<u32, 37> words{};
        words[0] = static_cast<u32>(gpu_address);
        words[1] = static_cast<u32>(gpu_address >> 32);
        words[2] = static_cast<u32>(image_offset);
        words[3] = static_cast<u32>(image_offset >> 32);
        u32* const desc = words.data() + 4;
        desc[0] = 1;
        desc[1] = preference[1];
        desc[2] = preference[2];
        desc[4] = layout.block_height_log2;
        desc[5] = static_cast<u32>(layout.pitch / 64);
        desc[8] = 1;
        desc[9] = 1;
        desc[11] = static_cast<u32>(layout.size);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv {} handle={} {}x{} size={:#x} gpu={:#x}",
                    ctx.GetCommand(), handle, layout.width, layout.height, layout.size,
                    gpu_address);
        IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
        rb.PushRaw<u32>(ResultSuccess.raw);
        for (const u32 w : words) {
            rb.Push<u32>(w);
        }
    }

    // 428/426 for a decoder output image (Fire Emblem: Fortune's Weave's movie player blits each
    // one, block-linear RGBA, format 0xd5, block height 16 GOBs, into a texture of its own). The
    // reply follows our Eden tree, where the movies play: words 3-4 the GPU address, 5-6 the last
    // byte's address, then the descriptor {1, 0, dimensions, .., 0x01000001, 1, offset, size}
    // (offset 0 once mapped). 426 maps the image at its own address for the GPU: decoder pool
    // memory is process memory (114) that no GPU mapping covers, and the blit read zeros.
    bool QueryDecoderImage(HLERequestContext& ctx, u32 handle) {
        const bool map = ctx.GetCommand() == 426;
        u64 address{};
        u64 size{};
        u64 offset{};
        u32 dimensions{};
        bool register_mapping = false;
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(handle);
            if (image == m_objects->images.end() || !image->second.decoder) {
                return false;
            }
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory == m_objects->memories.end() || memory->second.address == 0) {
                return false;
            }
            image->second.gpu_read = true;
            offset = image->second.offset;
            size = image->second.size;
            dimensions = image->second.preference[2];
            if (map) {
                address = memory->second.address + offset;
                register_mapping = image->second.gpu_maps++ == 0 &&
                                   !Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address);
                image->second.mapped_here |= register_mapping;
            }
        }
        if (register_mapping) {
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(address, size);
        }
        LOG_INFO(Service, "nx2:gsv {} decoder image {} at {:#x}+{:#x}{}", ctx.GetCommand(), handle,
                 address, size, register_mapping ? ", mapped for the GPU" : "");
        std::array<u32, 37> words{};
        if (map) {
            const u64 last = address + size - 1;
            words[0] = static_cast<u32>(address);
            words[1] = static_cast<u32>(address >> 32);
            words[2] = static_cast<u32>(last);
            words[3] = static_cast<u32>(last >> 32);
        }
        u32* const desc = words.data() + 4;
        desc[0] = 1;
        desc[2] = dimensions;
        desc[8] = 0x01000001;
        desc[9] = 1;
        desc[10] = map ? 0 : static_cast<u32>(offset);
        desc[11] = static_cast<u32>(size);
        IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
        rb.PushRaw<u32>(ResultSuccess.raw);
        for (const u32 w : words) {
            rb.Push<u32>(w);
        }
        return true;
    }

    // 427(u32 handle, u32 address space, u64 address, u64 last address): undoes a 426. The last
    // unmap of a decoder image drops the GPU mapping 426 made.
    void UnmapNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 handle = w[2];
        std::optional<std::pair<u64, u64>> unmap;
        {
            std::scoped_lock lk{m_objects->mutex};
            if (const auto image = m_objects->images.find(handle);
                image != m_objects->images.end() && image->second.gpu_maps != 0 &&
                --image->second.gpu_maps == 0) {
                unmap = TakeDecoderMapping(image->second);
            }
        }
        LOG_DEBUG(Service, "nx2:gsv 427 image {} [{:#x}, {:#x}]", handle, Pair(w, 4), Pair(w, 6));
        DropDecoderMapping(unmap);
        TipcReplyWords(ctx, {});
    }

    // The range 426 mapped for an image, if it did; called with m_objects->mutex held.
    std::optional<std::pair<u64, u64>> TakeDecoderMapping(Nx2NativeObjects::Image& image) {
        image.gpu_maps = 0;
        if (!std::exchange(image.mapped_here, false)) {
            return std::nullopt;
        }
        const auto memory = m_objects->memories.find(image.memory);
        if (memory == m_objects->memories.end() || memory->second.address == 0) {
            return std::nullopt;
        }
        return std::pair{memory->second.address + image.offset, image.size};
    }

    static void DropDecoderMapping(const std::optional<std::pair<u64, u64>>& range) {
        if (range) {
            auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
            registry.Unmap(range->first, range->second);
            registry.ReleaseBacking(range->first, range->second);
        }
    }

    // 911(preference[16], u32 count, u32 memory id) (nn::codec::InitializeDecoderImagePool):
    // result, the id of the first of `count` decoder output images; the client names image i of
    // the pool id + i. They share the pool's memory, one image size apart.
    void CreateDecoderImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 count = std::max<u32>(w[18], 1);
        const auto layout = Nx2CalculateDecoderImageLayout(w + 2);
        std::scoped_lock lk{m_objects->mutex};
        const u32 first = m_objects->next_image;
        m_objects->next_image += count;
        for (u32 i = 0; i < count; ++i) {
            auto& image = m_objects->images[first + i];
            image.memory = w[19];
            std::copy_n(w + 2, image.preference.size(), image.preference.begin());
            image.size = layout.size;
            image.offset = layout.size * i;
            image.decoder = true;
        }
        LOG_INFO(Service, "nx2:gsv 911 decoder images {}..{} code={} {}x{} in memory {}", first,
                 first + count - 1, layout.code, layout.width, layout.height, w[19]);
        TipcReplyWords(ctx, {first});
    }

    // 961(u32 image id): result, the 128-byte nn::gsv::CpuUserImageDescriptor that
    // nn::gs::sdk::ReadUserImage* (behind nn::codec::ReadDecoderOutputImage*) copy an image out
    // with: u8 kind at +0; for the pitch-linear kinds (8 RGBA, 9 BGRA, 10 NV12) the plane's
    // offset into the pool memory (u32 +0x8) and row pitch (u64 +0x10), NV12's chroma plane at
    // +0x18/+0x20. 11-13 are the block-linear kinds, not used here.
    void GetCpuImageDescriptor(HLERequestContext& ctx) {
        using Tegra::Host1x::Nx2PictureFormat;
        const u32 id = ctx.CommandBuffer()[2];
        std::array<u32, 32> words{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(id);
            if (image != m_objects->images.end() && image->second.decoder) {
                const auto layout = Nx2CalculateDecoderImageLayout(image->second.preference.data());
                const u64 offset = image->second.offset;
                words[0] = layout.format == Nx2PictureFormat::Nv12    ? 0xa
                           : layout.format == Nx2PictureFormat::Bgra8 ? 0x9
                                                                      : 0x8;
                words[2] = static_cast<u32>(offset);
                words[4] = static_cast<u32>(layout.pitch);
                if (layout.format == Nx2PictureFormat::Nv12) {
                    words[6] = static_cast<u32>(offset + layout.chroma_offset);
                    words[8] = static_cast<u32>(layout.pitch);
                }
            }
        }
        if (Nx2FirstVideoCommand("nx2:gsv", 961) || words[0] == 0) {
            LOG_INFO(Service, "nx2:gsv 961 image {} -> kind {:#x} offset {:#x} pitch {:#x}", id,
                     words[0], words[2], words[4]);
        }
        TipcReplyWords(ctx, words);
    }

    void DestroyNativeImage(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        if (Nx2VideoTraceEnabled()) {
            LOG_INFO(Service, "nx2:vdec trace destroy image={} command={}", id, ctx.GetCommand());
        }
        std::optional<std::pair<u64, u64>> unmap;
        {
            std::scoped_lock lk{m_objects->mutex};
            if (const auto image = m_objects->images.find(id); image != m_objects->images.end()) {
                unmap = TakeDecoderMapping(image->second);
                m_objects->images.erase(image);
            }
        }
        DropDecoderMapping(unmap);
        TipcReplyWords(ctx, {});
    }

    // 315(u32 image, u32 count) / 314(u32 image): add/remove a window alias image pool.
    void ConfigureWindowImagePool(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        const bool create = ctx.GetCommand() == 315;
        std::scoped_lock lk{m_objects->mutex};
        if (auto it = m_objects->images.find(id); it != m_objects->images.end()) {
            it->second.window_pool = create;
        }
        LOG_INFO(Service, "nx2:gsv {} window image pool image {}", ctx.GetCommand(), id);
        TipcReplyWords(ctx, {});
    }

    void OpenViSession(HLERequestContext& ctx) {
        // Used by nn::vi2::sdk::Initialize; takes one u64, returns a session (handle at word 3,
        // result at word 4).
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 305 arg={:#x}, returning a nx2:vi2 session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2Vi2Session>(system, m_objects));
    }

    void OpenGpuSession(HLERequestContext& ctx) {
        // Used by nn::gpu::sdk::BootWithRemoteMemory; takes one u64, returns a session (handle at
        // word 3, result at word 4) that the GPU SDK keeps.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 497 arg={:#x}, returning a nx2:gpu session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuSession>(system));
    }

    void OpenNvsrvSession(HLERequestContext& ctx) {
        // The SDK reads the handle at word 3 and the result at word 4.
        // nvsrv speaks the nvdrv INvDrvServices protocol (Open/Ioctl/Close/...,
        // 13 = SetGraphicsFirmwareMemoryMarginEnabled), so serve it with Eden's nvdrv on the shared
        // module. The nx2: name keeps unknown commands auto-stubbed and logged.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1402, returning an nvdrv-backed nx2:nvsrv session");
        auto module = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv", true)->GetModule();
        auto nvsrv = std::make_shared<Nvidia::NVDRV>(system, module, "nx2:nvsrv");
        nvsrv->InitializeForProcess(ctx.GetThread().GetOwnerProcess());
        ReturnRawSession(system, ctx, std::move(nvsrv));
    }

    static constexpr std::chrono::nanoseconds SyncptPollInterval{std::chrono::microseconds{500}};

    std::shared_ptr<Nx2NativeObjects> m_objects{std::make_shared<Nx2NativeObjects>()};
    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, 2> m_syncpt_events{};
    Core::Memory::Memory* m_syncpt_memory{};
    u64 m_syncpt_rw{};
    u64 m_syncpt_ro{};
    u32 m_syncpt_count{};
    std::shared_ptr<Core::Timing::EventType> m_syncpt_poll;
};

} // namespace Service
