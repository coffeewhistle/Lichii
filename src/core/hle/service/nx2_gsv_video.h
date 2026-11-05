// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <span>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "common/game_presets.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/nx2_video_decoder.h"
#include "video_core/textures/decoders.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nx2_image_layout.h"
#include "core/hle/service/nx2_video_input_queue.h"
#include "core/hle/service/nx2_video_ready.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"

namespace Service {

// The Switch 2 video decoder (nn::codec::video::detail::ipc). gsv 902/906/1002 hand out a session
// whose TIPC 1 (CreateVideoDecoderService: handle at word 3, result at word 4) creates a decoder
// service, another session of this class. Layouts read from the SDK's nn::codec VP9 decoder:
// - 200(copy handle: work buffer transfer memory, u64 size, u32, u32, u32): initialize. 201 adds a
//   fourth u32 (a flag that 200 leaves at 1) and 202 that and a u64: the same initialization.
//   210(copy handle, u32 x4) replies only a result.
// - 101(A: codec name, "video/vp9\0"; u32 width, u32 height, u32 rate, u32): configure.
// - 3(): the submission-ready event (copy handle at word 3, result at word 4).
// - 6 / 207 / 8: start / stop / flush; 115: finalize.
// - 311(u32 image id): register an output image (gsv 911 creates it); reply its NativeSyncHandle,
//   signalled once a frame has been decoded into the image. The client acquires output images in
//   registration/release order from a ring of its own (no IPC), so frames fill them in that order.
// - 314(u32 image id, u32, NativeSyncHandle): give an output image back; the image may be
//   rewritten once that handle (the client's reads) has passed. Reply the image's next handle.
// - 212(u32 image id): unregister it.
// - 109(A: one compressed frame, u64 pts in us, u8 end of stream): submit input; reply a u64.
//   400 is the same with a second A buffer after the frame, a 0x838-byte block of frame
//   information; the decoder here only needs the frame.
// - 213(): the next finished frame (16 words): u32 image id, u32 width, u32 height, u32 width,
//   u32 height, u32 colour space, TimeSpan pts (ns), u8 end of stream.
// A NativeSyncHandle is {u32 SyncCounterId (~syncpoint), u32 mode, u64 value}; clients wait on it
// with nx2:syncpt 103/201. Each decoder service owns a syncpoint that counts its finished frames.
// FFmpeg decodes on a thread of the session's own (a 1440p VP9 frame takes milliseconds, and the
// service thread also serves presents and syncpoint waits) into the layout gsv 961 describes.
class INx2VideoSession final : public ServiceFramework<INx2VideoSession> {
public:
    explicit INx2VideoSession(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_,
                              const char* name)
        : ServiceFramework{system_, name}, m_context{system_, name},
          m_objects{std::move(objects_)} {
        RegisterHandlersTipc({
            {1, &INx2VideoSession::CreateDecoderService, "CreateVideoDecoderService"},
            {3, &INx2VideoSession::GetEvent, "GetEvent"},
            {6, &INx2VideoSession::Start, "Start"},
            {8, &INx2VideoSession::Flush, "Flush"},
            {101, &INx2VideoSession::Configure, "Configure"},
            {109, &INx2VideoSession::SubmitInput, "SubmitInputBuffer"},
            {115, &INx2VideoSession::Finalize, "Finalize"},
            {200, &INx2VideoSession::Succeed, "Initialize"},
            {201, &INx2VideoSession::Succeed, "InitializeWithFlag"},
            {202, &INx2VideoSession::Succeed, "InitializeWithFlagAndValue"},
            {207, &INx2VideoSession::Flush, "Stop"},
            {210, &INx2VideoSession::Succeed, "Unknown210"},
            {212, &INx2VideoSession::UnregisterBuffer, "UnregisterBuffer"},
            {213, &INx2VideoSession::GetOutputInfo, "GetOutputInfo"},
            {311, &INx2VideoSession::RegisterBuffer, "RegisterBuffer"},
            {314, &INx2VideoSession::ReleaseBuffer, "ReleaseBuffer"},
            {400, &INx2VideoSession::SubmitInput, "SubmitInputBufferWithInfo"},
        });
    }

    ~INx2VideoSession() override {
        m_ready->SetRunning(system.Kernel(), false);
        if (m_worker.joinable()) {
            {
                std::scoped_lock lk{m_mutex};
                m_quit = true;
            }
            m_cv.notify_all();
            m_worker.join();
        }
        // The ready timer runs on the HostTiming thread: clear its event under its lock first.
        {
            std::scoped_lock lk{m_ready->mutex};
            m_ready->event = nullptr;
        }
        if (m_ready_timer) {
            system.CoreTiming().UnscheduleEvent(m_ready_timer);
        }
        if (m_event != nullptr) {
            m_context.CloseEvent(m_event);
        }
        if (m_syncpoint != NoSyncpoint) {
            NvdrvSyncpoints().FreeSyncpoint(m_syncpoint);
        }
    }

private:
    using Input = Nx2VideoInputQueue::Input;
    struct Decoded {
        std::optional<Tegra::Host1x::Nx2Picture> picture; // none: an empty end-of-stream frame
        u64 pts;
        bool end;
    };
    struct Output {
        u32 image;
        u32 width;
        u32 height;
        u64 pts;
        bool end;
    };
    struct Fence {
        u32 syncpoint;
        u32 value;
    };

    static constexpr u32 NoSyncpoint = ~0U;
    static constexpr size_t MaxDecodedAhead = 2; // decoded frames held while no image is free
    static constexpr u32 HostSyncpoints = 192;   // Tegra::Host1x::SyncpointManager

    void Log(HLERequestContext& ctx, std::string_view what) {
        const auto* w = ctx.CommandBuffer();
        if (Nx2FirstVideoCommand(GetServiceName(), ctx.GetCommand())) {
            LOG_INFO(Service, "{} TIPC {} {} (first use): words {:08x}", GetServiceName(),
                     ctx.GetCommand(), what, fmt::join(std::span<const u32>{w, 12}, " "));
        } else if (Nx2VideoTraceEnabled()) {
            // EDEN_NX2_VDEC_TRACE=1: every video decoder call at Info level.
            LOG_INFO(Service, "{} TIPC {} {}: [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                     GetServiceName(), ctx.GetCommand(), what, w[2], w[3], w[4], w[5]);
        } else {
            LOG_DEBUG(Service, "{} TIPC {} {}: [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                      GetServiceName(), ctx.GetCommand(), what, w[2], w[3], w[4], w[5]);
        }
    }

    Nvidia::NvCore::SyncpointManager& NvdrvSyncpoints() {
        return system.ServiceManager()
            .GetService<Nvidia::NVDRV>("nvdrv", true)
            ->GetModule()
            ->GetContainer()
            .GetSyncpointManager();
    }

    void CreateDecoderService(HLERequestContext& ctx) {
        Log(ctx, "create decoder service");
        ReturnRawSession(system, ctx,
                         std::make_shared<INx2VideoSession>(system, m_objects, "nx2:vdecsvc"));
    }

    void Succeed(HLERequestContext& ctx) {
        Log(ctx, "");
        TipcReplyWords(ctx, {});
    }

    void Start(HLERequestContext& ctx) {
        Log(ctx, "start");
        if (Nx2VideoTraceEnabled()) {
            LOG_INFO(Service, "nx2:vdec trace start syncpoint={}", m_syncpoint);
        }
        m_ready->SetRunning(system.Kernel(), true);
        m_ready->Signal(system.Kernel());
        TipcReplyWords(ctx, {});
    }

    void Configure(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::string codec;
        std::span<const u8> name_buf;
        if (!ctx.BufferDescriptorA().empty()) {
            name_buf = ctx.ReadBufferA(0);
            codec.assign(reinterpret_cast<const char*>(name_buf.data()),
                         strnlen(reinterpret_cast<const char*>(name_buf.data()), name_buf.size()));
        }
        if (name_buf.empty() && !ctx.BufferDescriptorX().empty()) {
            name_buf = ctx.ReadBufferX(0);
            codec.assign(reinterpret_cast<const char*>(name_buf.data()),
                         strnlen(reinterpret_cast<const char*>(name_buf.data()), name_buf.size()));
        }
        if (!name_buf.empty()) {
            if (codec.empty()) {
                const std::string_view sv{reinterpret_cast<const char*>(name_buf.data()), name_buf.size()};
                for (const auto& candidate : {"video/avc", "video/vp9", "video/vp8", "video/hevc"}) {
                    if (sv.find(candidate) != std::string_view::npos) {
                        codec = candidate;
                        break;
                    }
                }
            }
            if (codec.empty()) {
                LOG_INFO(Service, "{} configure: no codec name in {:02x}", GetServiceName(),
                         fmt::join(name_buf, " "));
            }
        }
        Log(ctx, "configure");
        LOG_INFO(Service, "{} configure codec \"{}\" {}x{} rate {}", GetServiceName(), codec, w[5],
                 w[6], w[7]);
        {
            std::scoped_lock lk{m_mutex};
            m_codec = std::move(codec);
            m_width = w[5];
            m_height = w[6];
        }
        if (!m_worker.joinable()) {
            m_worker = std::thread{[this] { DecodeLoop(); }};
        }
        TipcReplyWords(ctx, {});
    }

    void GetEvent(HLERequestContext& ctx) {
        Log(ctx, "event");
        if (m_event == nullptr) {
            m_event = m_context.CreateEvent(fmt::format("{}:Ready", GetServiceName()));
            // The client attaches it auto-clearing, so one signal is consumed by the first wait:
            // while the input queue has room, re-signal every 4 ms; a spurious wake only makes it
            // check again.
            {
                std::scoped_lock lk{m_ready->mutex};
                m_ready->event = m_event;
            }
            m_ready_timer = Core::Timing::CreateEvent(
                "nx2:vdec ready",
                [ready = m_ready, &kernel = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    ready->Signal(kernel);
                    return std::nullopt;
                });
            constexpr std::chrono::nanoseconds ReadyInterval{std::chrono::milliseconds{4}};
            system.CoreTiming().ScheduleLoopingEvent(ReadyInterval, ReadyInterval, m_ready_timer);
        }
        m_ready->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, m_event->GetReadableEvent());
    }

    // Queues an output image for the next decoded frame; returns the NativeSyncHandle words that
    // signal once that frame is in it.
    std::array<u32, 4> HandOut(u32 image, std::optional<Fence> release_fence) {
        if (m_syncpoint == NoSyncpoint) {
            m_syncpoint = NvdrvSyncpoints().AllocateSyncpoint(false);
            m_sync_base = system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(m_syncpoint);
            LOG_INFO(Service, "{} counts decoded frames on syncpoint {} from {}",
                     GetServiceName(), m_syncpoint, m_sync_base);
        }
        u32 value;
        {
            std::scoped_lock lk{m_mutex};
            m_free.push_back(image);
            if (release_fence) {
                m_release_fences[image] = *release_fence;
            }
            // Mario Kart World waits for returned images after a movie and acquires a frame for
            // each completed fence, so its released images need synthetic end-of-stream frames.
            static const bool release_now = [] {
                const char* env_value = std::getenv("EDEN_NX2_VDEC_EOS_RELEASE_NOW");
                return env_value != nullptr && env_value[0] != '0';
            }();
            const bool drain_eos =
                system.GetApplicationProcessProgramID() == Settings::MarioKartWorldProgramId;
            if ((drain_eos || release_now) && release_fence && m_stream_ended && m_inputs.Empty() &&
                m_decoded.empty()) {
                const u32 reached =
                    drain_eos ? DrainFreeAsEndOfStream()
                              : static_cast<u32>(system.Host1x().GetSyncpointManager()
                                                     .GetGuestSyncpointValue(m_syncpoint));
                m_sync_base = reached;
                m_handed = 0;
                value = reached;
            } else {
                value = m_sync_base + ++m_handed;
            }
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service,
                         "nx2:vdec trace handout syncpoint={} image={} release={} fence={} "
                         "current={} eos={} inputs={} decoded={} outputs={}",
                         m_syncpoint, image, release_fence.has_value(), value,
                         system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(m_syncpoint),
                         m_stream_ended, m_inputs.Size(), m_decoded.size(), m_outputs.size());
            }
        }
        m_cv.notify_one();
        return {~m_syncpoint, 0, value, 0};
    }

    void RegisterBuffer(HLERequestContext& ctx) {
        Log(ctx, "register output");
        const auto handle = HandOut(ctx.CommandBuffer()[2], std::nullopt);
        TipcReplyWords(ctx, {handle[0], handle[1], handle[2], handle[3]});
    }

    void ReleaseBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        Log(ctx, "release output");
        const auto handle = HandOut(w[2], Fence{~w[4], w[6]});
        TipcReplyWords(ctx, {handle[0], handle[1], handle[2], handle[3]});
    }

    void UnregisterBuffer(HLERequestContext& ctx) {
        Log(ctx, "unregister output");
        const u32 image = ctx.CommandBuffer()[2];
        {
            std::scoped_lock lk{m_mutex};
            std::erase(m_free, image);
            m_release_fences.erase(image);
        }
        TipcReplyWords(ctx, {});
    }

    void SubmitInput(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        // Words 2-4 are the A descriptor of the compressed frame (400: 5-7 that of its information
        // block), the data words follow.
        const size_t words = 2 + 3 * ctx.BufferDescriptorA().size();
        const u64 pts = u64{w[words]} | (u64{w[words + 1]} << 32);
        const bool end = (w[words + 2] & 0xFF) != 0;
        Log(ctx, "submit");
        Input input{{}, pts, end};
        if (!ctx.BufferDescriptorA().empty()) {
            const auto data = ctx.ReadBufferA(0);
            input.data.assign(data.begin(), data.end());
        }
        const auto input_size = input.data.size();
        {
            std::scoped_lock lk{m_mutex};
            m_inputs.Push(std::move(input));
            m_stream_ended = false;
            m_end_queued |= end;
            m_ready->room.store(!m_end_queued && m_inputs.BelowReadyWatermark(),
                                std::memory_order_relaxed);
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service,
                         "nx2:vdec trace submit syncpoint={} input={} bytes={} pts={} eos={} queued={}",
                         m_syncpoint, m_submitted + 1, input_size, pts, end,
                         m_inputs.Size());
            }
        }
        m_cv.notify_one();
        const u64 id = ++m_submitted;
        TipcReplyWords(ctx, {static_cast<u32>(id), static_cast<u32>(id >> 32)});
    }

    // Flush (8) and Stop (207): drop queued input and frames not yet in an image. Images already
    // handed out keep their place, so the sync values stay in step with the client's ring.
    void Flush(HLERequestContext& ctx) {
        Log(ctx, ctx.GetCommand() == 8 ? "flush" : "stop");
        if (ctx.GetCommand() == 207) {
            m_ready->SetRunning(system.Kernel(), false);
        }
        {
            std::scoped_lock lk{m_mutex};
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service,
                         "nx2:vdec trace {} syncpoint={} inputs={} decoded={} outputs={} "
                         "decoded_frames={} frames_out={} eos={} eos_queued={}",
                         ctx.GetCommand() == 8 ? "flush" : "stop", m_syncpoint, m_inputs.Size(),
                         m_decoded.size(), m_outputs.size(), m_decoded_frames, m_frames_out,
                         m_stream_ended, m_end_queued);
            }
            m_inputs.Clear();
            m_flush = true;
            m_stream_ended = false;
            m_end_queued = false;
            m_ready->room.store(true, std::memory_order_relaxed);
            // Only Stop: Bananza flushes (8) after its movies end and treats every completed
            // fence as a frame to acquire.
            if (ctx.GetCommand() == 207) {
                CompleteHandedFences();
            }
        }
        m_cv.notify_one();
        TipcReplyWords(ctx, {});
    }

    void Finalize(HLERequestContext& ctx) {
        Log(ctx, "finalize");
        m_ready->SetRunning(system.Kernel(), false);
        {
            std::scoped_lock lk{m_mutex};
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service,
                         "nx2:vdec trace finalize syncpoint={} inputs={} decoded={} outputs={} "
                         "decoded_frames={} frames_out={} eos={} eos_queued={}",
                         m_syncpoint, m_inputs.Size(), m_decoded.size(), m_outputs.size(),
                         m_decoded_frames, m_frames_out, m_stream_ended, m_end_queued);
            }
            CompleteHandedFences();
        }
        TipcReplyWords(ctx, {});
    }

    // Advances the decoder's syncpoint to the last value handed out (m_mutex held).
    void CompleteHandedFences() {
        if (m_syncpoint == NoSyncpoint) {
            return;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const u32 target = m_sync_base + m_handed;
        u32 current = static_cast<u32>(syncpoints.GetGuestSyncpointValue(m_syncpoint));
        if (Nx2VideoTraceEnabled()) {
            LOG_INFO(Service, "nx2:vdec trace complete fences syncpoint={} current={} target={}",
                     m_syncpoint, current, target);
        }
        while (static_cast<s32>(target - current) > 0) {
            syncpoints.IncrementGuest(m_syncpoint);
            ++current;
        }
    }

    void GetOutputInfo(HLERequestContext& ctx) {
        Log(ctx, "output info");
        std::optional<Output> output;
        {
            std::scoped_lock lk{m_mutex};
            if (!m_outputs.empty()) {
                output = m_outputs.front();
                m_outputs.pop_front();
            }
        }
        if (!output) {
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service, "nx2:vdec trace output empty syncpoint={} result=0x1c286f",
                         m_syncpoint);
            }
            // Nothing finished: the code the client's own Acquire uses for an empty ring.
            IPC::ResponseBuilder rb{ctx, 2};
            rb.PushRaw<u32>(0x1c286f);
            return;
        }
        const u64 pts_ns = output->pts * 1000;
        if (Nx2VideoTraceEnabled()) {
            LOG_INFO(Service, "nx2:vdec trace output syncpoint={} image={} size={}x{} pts={} eos={}",
                     m_syncpoint, output->image, output->width, output->height, output->pts,
                     output->end);
        }
        TipcReplyWords(ctx, {output->image, output->width, output->height, output->width,
                             output->height, 0, static_cast<u32>(pts_ns),
                             static_cast<u32>(pts_ns >> 32), output->end ? 1U : 0U, 0, 0, 0, 0,
                             0, 0, 0});
    }

    // The decode thread: turns queued input into decoded frames and decoded frames into output
    // images, in order.
    void DecodeLoop() {
        Common::SetCurrentThreadName("nx2:vdec");
        std::unique_ptr<Tegra::Host1x::Nx2VideoDecoder> decoder;
        std::vector<Tegra::Host1x::Nx2Picture> pictures;
        std::vector<u8> bytes;
        std::unique_lock lk{m_mutex};
        while (true) {
            m_cv.wait(lk, [&] {
                return m_quit || m_flush || (!m_decoded.empty() && !m_free.empty()) ||
                       (!m_inputs.Empty() && m_decoded.size() < MaxDecodedAhead);
            });
            if (m_quit) {
                return;
            }
            if (m_flush) {
                m_flush = false;
                m_decoded.clear();
                if (decoder) {
                    lk.unlock();
                    decoder->Reset();
                    lk.lock();
                }
                continue;
            }
            if (!m_decoded.empty() && !m_free.empty()) {
                Decoded frame = std::move(m_decoded.front());
                m_decoded.pop_front();
                const u32 image = m_free.front();
                m_free.pop_front();
                std::optional<Fence> fence;
                if (const auto it = m_release_fences.find(image); it != m_release_fences.end()) {
                    fence = it->second;
                    m_release_fences.erase(it);
                }
                lk.unlock();
                if (fence) {
                    WaitForRelease(*fence);
                }
                const auto [width, height] = WriteImage(decoder.get(), image, frame, bytes);
                lk.lock();
                // Even an image written during a flush counts: the client waits for its value.
                m_outputs.push_back({image, width, height, frame.pts, frame.end});
                if (frame.end) {
                    m_stream_ended = true;
                    m_end_queued = false;
                    m_ready->room.store(false, std::memory_order_relaxed);
                }
                system.Host1x().GetSyncpointManager().IncrementGuest(m_syncpoint);
                m_last_output = m_outputs.back();
                if (frame.end && m_inputs.Empty() && m_decoded.empty() &&
                    system.GetApplicationProcessProgramID() == Settings::MarioKartWorldProgramId) {
                    DrainFreeAsEndOfStream();
                }
                ++m_frames_out;
                if (m_frames_out == 1 || (m_frames_out % 600) == 0 || frame.end) {
                    LOG_INFO(Service, "{} frame {} ({}x{}, pts {} us{}) in image {}",
                             GetServiceName(), m_frames_out, width, height, frame.pts,
                             frame.end ? ", end of stream" : "", image);
                }
                continue;
            }
            Input input = m_inputs.Pop();
            if (!m_stream_ended && !m_end_queued) {
                m_ready->room.store(m_inputs.BelowReadyWatermark(), std::memory_order_relaxed);
            }
            std::string codec = m_codec;
            if (codec.empty() && Tegra::Host1x::IsH264Stream(input.data)) {
                codec = "video/avc";
                m_codec = codec;
                LOG_INFO(Service, "{} empty codec, detected H.264 stream: using \"{}\"",
                         GetServiceName(), codec);
            }
            lk.unlock();
            if (!decoder || decoder->Name() != codec) {
                if (codec.empty()) {
                    LOG_WARNING(Service,
                                "{} input without codec: size {}, pts {}, end {}, first bytes {:02x}",
                                GetServiceName(), input.data.size(), input.pts, input.end,
                                fmt::join(std::span<const u8>{input.data}.first(
                                              std::min(input.data.size(), size_t{16})),
                                          " "));
                }
                decoder = std::make_unique<Tegra::Host1x::Nx2VideoDecoder>(codec);
            }
            pictures.clear();
            decoder->Decode(input.data, input.pts, pictures);
            if (input.end) {
                decoder->Drain(pictures);
            }
            lk.lock();
            m_decoded_frames += pictures.size();
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service,
                         "nx2:vdec trace decode syncpoint={} codec={} open={} pts={} eos={} "
                         "pictures={} decoded_frames={} discarded_by_flush={}",
                         m_syncpoint, decoder->Name(), decoder->IsOpen(), input.pts, input.end,
                         pictures.size(), m_decoded_frames, m_flush);
            }
            if (m_flush) {
                continue;
            }
            for (auto& picture : pictures) {
                const u64 pts = picture.pts;
                m_decoded.push_back({std::move(picture), pts, false});
            }
            if (input.end) {
                // The client stops at the frame marked end of stream; give it one even when the
                // last input held no picture.
                if (m_decoded.empty()) {
                    m_decoded.push_back({std::nullopt, input.pts, true});
                } else {
                    m_decoded.back().end = true;
                }
            }
        }
    }

    u32 DrainFreeAsEndOfStream() {
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        if (Nx2VideoTraceEnabled()) {
            LOG_INFO(Service, "nx2:vdec trace synthetic EOS syncpoint={} images={}",
                     m_syncpoint, m_free.size());
        }
        while (!m_free.empty()) {
            const u32 image = m_free.front();
            m_free.pop_front();
            m_release_fences.erase(image);
            m_outputs.push_back({image, m_last_output.width, m_last_output.height, m_last_output.pts, true});
            syncpoints.IncrementGuest(m_syncpoint);
        }
        return static_cast<u32>(syncpoints.GetGuestSyncpointValue(m_syncpoint));
    }

    // Waits (bounded) until the client's reads of an image it gave back have passed. Runs on the
    // decode thread without m_mutex. A release more than 500 ms late means the client's GPU work
    // is stalled (the decoder only runs MaxDecodedAhead frames ahead), so the frame goes out.
    void WaitForRelease(const Fence& fence) {
        if (fence.syncpoint >= HostSyncpoints) {
            return;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        if (!syncpoints.IsReadyGuest(fence.syncpoint, fence.value)) {
            struct Wake {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{};
            };
            auto wake = std::make_shared<Wake>();
            // Runs inline if the value was reached meanwhile, else on the incrementing thread.
            const auto handle =
                syncpoints.RegisterGuestAction(fence.syncpoint, fence.value, [wake] {
                    {
                        std::scoped_lock lk{wake->mutex};
                        wake->done = true;
                    }
                    wake->cv.notify_all();
                });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
            bool done = false;
            while (true) {
                {
                    std::unique_lock lk{wake->mutex};
                    done = wake->cv.wait_for(lk, std::chrono::milliseconds{10},
                                             [&] { return wake->done; });
                }
                // m_mutex is not taken under wake->mutex: actions run under the syncpoint
                // manager's lock, which the service thread takes under m_mutex.
                if (done || std::chrono::steady_clock::now() >= deadline || !system.IsPoweredOn()) {
                    break;
                }
                std::scoped_lock lk{m_mutex};
                if (m_quit) {
                    break;
                }
            }
            if (!done) {
                syncpoints.DeregisterGuestAction(fence.syncpoint, handle);
                static std::atomic<u32> timeouts{};
                if (!syncpoints.IsReadyGuest(fence.syncpoint, fence.value) &&
                    (timeouts.fetch_add(1, std::memory_order_relaxed) % 64) == 0) {
                    LOG_WARNING(Service, "{} gave up waiting for image release ({}, {}), current "
                                         "value={}",
                                GetServiceName(), fence.syncpoint, fence.value,
                                syncpoints.GetGuestSyncpointValue(fence.syncpoint));
                }
            }
        }
        if (Nx2VideoTraceEnabled()) {
            LOG_INFO(Service, "nx2:vdec trace release wait syncpoint={} value={} reached={}",
                     fence.syncpoint, fence.value,
                     syncpoints.IsReadyGuest(fence.syncpoint, fence.value));
        }
    }

    // Writes a decoded frame into an output image; returns the size the frame reports.
    std::pair<u32, u32> WriteImage(Tegra::Host1x::Nx2VideoDecoder* decoder, u32 image_id,
                                   const Decoded& frame, std::vector<u8>& bytes) {
        u64 address{};
        u32 parent{};
        u64 parent_offset{};
        Nx2DecoderImageLayout layout{};
        bool block_linear{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            if (image == m_objects->images.end()) {
                if (Nx2VideoTraceEnabled()) {
                    LOG_INFO(Service, "nx2:vdec trace write skipped syncpoint={} image={} destroyed",
                             m_syncpoint, image_id);
                }
                return {m_width, m_height};
            }
            layout = Nx2CalculateDecoderImageLayout(image->second.preference.data());
            block_linear = image->second.gpu_read &&
                           layout.format != Tegra::Host1x::Nx2PictureFormat::Nv12;
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory != m_objects->memories.end() && memory->second.address != 0) {
                address = memory->second.address + image->second.offset;
                parent = memory->second.parent;
                parent_offset = memory->second.parent_offset + image->second.offset;
            }
        }
        if (!frame.picture || decoder == nullptr || address == 0 ||
            system.ApplicationProcess() == nullptr) {
            if (Nx2VideoTraceEnabled()) {
                LOG_INFO(Service,
                         "nx2:vdec trace write skipped syncpoint={} image={} cpu={:#x} picture={} eos={}",
                         m_syncpoint, image_id, address, frame.picture.has_value(), frame.end);
            }
            return {std::min(m_width, layout.width), std::min(m_height, layout.height)};
        }
        const auto trace_write = [&] {
            if (!Nx2VideoTraceEnabled()) {
                return;
            }
            auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
            auto gpu_address = registry.GpuAddressOf(parent, parent_offset);
            const auto device = registry.DeviceAddressOf(address);
            if (!gpu_address && device && registry.DeviceAddressOfGpu(address) == device) {
                gpu_address = address;
            }
            LOG_INFO(Service,
                     "nx2:vdec trace write syncpoint={} decoded_frames={} output={} codec={} "
                     "image={} cpu={:#x} gpu={:#x} gpu_known={} device={:#x} mapped={} "
                     "format={} block_linear={} pitch={} chroma={:#x} pts={} eos={}",
                     m_syncpoint, m_decoded_frames, m_frames_out + 1, decoder->Name(), image_id,
                     address, gpu_address.value_or(0), gpu_address.has_value(),
                     device.value_or(0), device.has_value(), static_cast<u32>(layout.format),
                     block_linear, layout.pitch, layout.chroma_offset, frame.pts, frame.end);
            std::scoped_lock lk{m_objects->mutex};
            m_objects->last_video_write = Nx2NativeObjects::VideoTrace{
                m_syncpoint, m_decoded_frames, image_id, gpu_address.value_or(0),
                device.value_or(0), frame.pts, decoder->Name()};
        };
        // A frame larger than the image is scaled down to it.
        const u32 width = std::min(frame.picture->width, layout.width);
        const u32 height = std::min(frame.picture->height, layout.height);
        bytes.resize(layout.size);
        auto& memory = system.ApplicationMemory();
        if (block_linear) {
            // Read on the GPU: RGBA (the blit's format 0xd5) in the native block-linear layout,
            // 16-GOB blocks, as our Eden tree writes these images.
            decoder->Render(*frame.picture, Tegra::Host1x::Nx2PictureFormat::Rgba8, width, height,
                            layout.pitch, 0, bytes);
            DumpFrame(image_id, bytes, layout, width, height);
            m_tiled.resize(layout.size);
            Tegra::Texture::SwizzleTexture(m_tiled, bytes, 4, static_cast<u32>(layout.pitch / 4),
                                           layout.height, 1, 4, 0, 1, true);
            memory.WriteBlock(address, m_tiled.data(), m_tiled.size());
            if (const auto device =
                    Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address)) {
                system.GPU().InvalidateRegion(*device, m_tiled.size());
            }
            trace_write();
            return {width, height};
        }
        decoder->Render(*frame.picture, layout.format, width, height, layout.pitch,
                        layout.chroma_offset, bytes);
        DumpFrame(image_id, bytes, layout, width, height);
        memory.WriteBlock(address, bytes.data(), layout.pitch * height);
        if (layout.format == Tegra::Host1x::Nx2PictureFormat::Nv12) {
            memory.WriteBlock(address + layout.chroma_offset, bytes.data() + layout.chroma_offset,
                              layout.pitch * ((height + 1) / 2));
        }
        // The game samples the image on the GPU: drop the texture cache's copy of the old frame.
        if (const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address)) {
            system.GPU().InvalidateRegion(*device, layout.size);
        }
        trace_write();
        return {width, height};
    }

    // EDEN_NX2_VDEC_DUMP=<dir>: save the luma of every 120th decoded frame, a quarter of its size,
    // as nx2-vdec-image-<id>-<frame>.pgm (apart from how the game shows it).
    void DumpFrame(u32 image_id, const std::vector<u8>& bytes, const Nx2DecoderImageLayout& layout,
                   u32 width, u32 height) {
        static const char* const directory = std::getenv("EDEN_NX2_VDEC_DUMP");
        if (directory == nullptr || (m_frames_out % 120) != 0) {
            return;
        }
        const u32 bpp = layout.format == Tegra::Host1x::Nx2PictureFormat::Nv12 ? 1 : 4;
        const u32 w = width / 4;
        const u32 h = height / 4;
        const std::string path =
            fmt::format("{}/nx2-vdec-image-{}-{:05}.pgm", directory, image_id, m_frames_out + 1);
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fprintf(file, "P5 %u %u 255\n", w, h);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    std::fputc(bytes[y * 4 * layout.pitch + x * 4 * bpp], file);
                }
            }
            std::fclose(file);
        }
    }

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    Kernel::KEvent* m_event{};
    std::shared_ptr<Nx2VideoReadyState> m_ready = std::make_shared<Nx2VideoReadyState>();
    std::shared_ptr<Core::Timing::EventType> m_ready_timer;
    u64 m_submitted{};
    u32 m_syncpoint{NoSyncpoint}; ///< Counts finished frames (allocated at the first register)
    u32 m_sync_base{};            ///< Its value before the first frame
    std::vector<u8> m_tiled;      ///< Decode thread: block-linear copy of a frame (WriteImage)

    // Shared with the decode thread.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_codec;
    u32 m_width{};
    u32 m_height{};
    u32 m_handed{}; ///< Output images handed out (registered or released) so far
    u64 m_frames_out{};
    u64 m_decoded_frames{};
    Nx2VideoInputQueue m_inputs;
    std::deque<Decoded> m_decoded;     ///< Waiting for a free output image
    std::deque<u32> m_free;            ///< Output images, in the order the client acquires them
    std::map<u32, Fence> m_release_fences;
    std::deque<Output> m_outputs;      ///< Finished, for 213
    Output m_last_output{};
    bool m_stream_ended{};             ///< End-of-stream frame handed out, nothing submitted since
    bool m_end_queued{};
    bool m_flush{};
    bool m_quit{};
    std::thread m_worker;
};

} // namespace Service
