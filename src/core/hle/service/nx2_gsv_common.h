// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <set>
#include <span>
#include <string>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include "common/alignment.h"
#include "core/core.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "video_core/host1x/nx2_video_decoder.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"


namespace Service {

// Creates a session served by `iface` and returns its client end. Unlike PushIpcInterface this
// works from a domain too, for callers that want a raw session handle.
template <class T>
inline Kernel::KClientSession* CreateRawSession(Core::System& system, HLERequestContext& ctx,
                                                std::shared_ptr<T> iface) {
    auto& kernel = system.Kernel();
    ASSERT(Kernel::GetCurrentProcess(kernel).GetResourceLimit()->Reserve(
        kernel, Kernel::LimitableResource::SessionCountMax, 1));

    auto* session = Kernel::KSession::Create(kernel);
    session->Initialize(kernel, nullptr, 0);
    Kernel::KSession::Register(kernel, session);

    auto manager =
        std::make_shared<SessionRequestManager>(kernel, ctx.GetManager()->GetServerManager());
    manager->SetSessionHandler(std::move(iface));
    ctx.GetManager()->GetServerManager().RegisterSession(&session->GetServerSession(), manager);
    return &session->GetClientSession();
}

// Replies to a TIPC request with success and raw u32 data words. ResponseBuilder::Push(Result)
// writes the CMIF two-word result, which shifts TIPC data one word past where the client reads
// it (and past the copied size); TIPC has a one-word result followed directly by the data.
inline void TipcReplyWords(HLERequestContext& ctx, std::span<const u32> words) {
    IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
    rb.PushRaw<u32>(ResultSuccess.raw);
    for (const u32 w : words) {
        rb.Push<u32>(w);
    }
}

inline void TipcReplyWords(HLERequestContext& ctx, std::initializer_list<u32> words) {
    TipcReplyWords(ctx, std::span<const u32>{words.begin(), words.size()});
}

// TIPC 0(u64 token) on the Switch 2 sessions: a handshake newer SDKs send first; the reply echoes
// the token. (The auto stub's reply only echoed it by leaving the request's words in place.)
inline void TipcHandshake(HLERequestContext& ctx, std::string_view session) {
    const auto* w = ctx.CommandBuffer();
    LOG_INFO(Service, "{} TIPC 0 handshake {:#x}", session, u64{w[2]} | (u64{w[3]} << 32));
    TipcReplyWords(ctx, {w[2], w[3]});
}

// Replies with success and a copied handle to a new session served by `iface`.
template <class T>
inline void ReturnRawSession(Core::System& system, HLERequestContext& ctx, std::shared_ptr<T> iface) {
    auto* client = CreateRawSession(system, ctx, std::move(iface));
    IPC::ResponseBuilder rb{ctx, 2, 1, 0};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, *client);
}

inline bool Nx2VideoTraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_VDEC_TRACE") != nullptr;
    return enabled;
}

// Native memory and images handed out by the Switch 2 graphics server (gsv 111/114/423/429/315),
// shared with the vi2 session so presents can find an image's memory. Command meanings follow
// the shared changes.patch (IGraphicsSupervisor), re-checked against title 4's SDK.
struct Nx2NativeObjects {
    struct VideoTrace {
        u32 syncpoint{};
        u64 decoded_frames{};
        u32 image{};
        u64 gpu_address{};
        u64 device_address{};
        u64 pts{};
        std::string codec;
    };
    struct Memory {
        u64 size{};
        u64 address{}; // CPU (== GPU) address once bound by 114/423
        u32 parent{};
        u64 parent_offset{};
    };
    struct Image {
        u32 memory{};
        std::array<u32, 16> preference{};
        u64 size{};
        u32 count{1}; // images in the pool; handle id + i is image i
        u64 offset{}; // into its memory (images of a decoder pool share one)
        bool window_pool{};
        bool decoder{}; // a video decoder output image (gsv 911)
        // A decoder image the client reads on the GPU (gsv 428/426): written block-linear.
        bool gpu_read{};
        u32 gpu_maps{};     // outstanding 426 maps
        bool mapped_here{}; // 426 added its GPU mapping (no other mapping covered it)
    };
    std::mutex mutex;
    std::map<u32, Memory> memories;
    std::map<u32, Image> images;
    std::optional<VideoTrace> last_video_write;
    u32 next_memory{1};
    u32 next_image{1};
};

// Video decoder output images (nn::codec image pools, gsv 991/911/961). The preference
// (MakeDecodeImagePreference) holds an output layout code at word 1 and (width - 1) |
// (height - 1) << 16 at word 2. Codes come in pairs, one per CodecMode: 1/4 NV12 (format 3, what
// the movie players use), 2/5 BGRA, 3/6 RGBA; the HDR codes (10-13) are written as RGBA. The
// server picks the layout and describes it to the client with gsv 961: here pitch-linear rows,
// NV12's half-height chroma plane below the luma plane, each plane padded to 128 rows.
struct Nx2DecoderImageLayout {
    u32 code;
    u32 width;
    u32 height;
    Tegra::Host1x::Nx2PictureFormat format;
    u64 pitch;
    u64 chroma_offset;
    u64 size;
};

inline Nx2DecoderImageLayout Nx2CalculateDecoderImageLayout(const u32* preference) {
    using Tegra::Host1x::Nx2PictureFormat;
    Nx2DecoderImageLayout layout{};
    layout.code = preference[1];
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    switch (layout.code) {
    case 1:
    case 4:
        layout.format = Nx2PictureFormat::Nv12;
        break;
    case 2:
    case 5:
        layout.format = Nx2PictureFormat::Bgra8;
        break;
    default:
        layout.format = Nx2PictureFormat::Rgba8;
        break;
    }
    const bool nv12 = layout.format == Nx2PictureFormat::Nv12;
    constexpr u64 BlockRows = 128;
    layout.pitch = Common::AlignUp(u64{layout.width} * (nv12 ? 1 : 4), 64);
    layout.chroma_offset = layout.pitch * Common::AlignUp(u64{layout.height}, BlockRows);
    layout.size = layout.chroma_offset +
                  (nv12 ? layout.pitch * Common::AlignUp((u64{layout.height} + 1) / 2, BlockRows) : 0);
    return layout;
}

// True the first time a Switch 2 video command is seen, so each one is logged once in full and new
// or unexpected commands stand out.
inline bool Nx2FirstVideoCommand(std::string_view service, u32 command) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, u32>> seen;
    std::scoped_lock lk{mutex};
    return seen.emplace(std::string{service}, command).second;
}

} // namespace Service
