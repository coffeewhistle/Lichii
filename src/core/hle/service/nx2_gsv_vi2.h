// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "common/dkb_frame_trace.h"
#include "common/game_presets.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/framebuffer_config.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc_results.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nx2_image_layout.h"
#include "core/hle/service/nx2_present_pacer.h"
#include "core/hle/service/service.h"

#include "core/hle/service/nx2_gsv_common.h"

namespace Service {

// Placeholder for the Switch 2 display session (nn::vi2::sdk), handed out by gsv 305. Reply
// shapes read from the SDK: TIPC 1 -> u32 < 0x400, TIPC 3 -> u32 < 8, TIPC 11/31(u32) and
// 32(handle, u32) -> one handle at word 3 then the result. Handles returned here are events (a
// guess); everything else is auto-stubbed through the nx2: prefix.
class INx2Vi2Session final : public ServiceFramework<INx2Vi2Session> {
public:
    explicit INx2Vi2Session(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_)
        : ServiceFramework{system_, "nx2:vi2"}, m_context{system_, "nx2:vi2"},
          m_objects{std::move(objects_)} {
        RegisterHandlersTipc({
            {0, &INx2Vi2Session::Handshake, "Handshake"},
            {1, &INx2Vi2Session::ReturnOne, "Unknown1"},
            {3, &INx2Vi2Session::ReturnOne, "GetDisplayCount (guessed)"},
            {5, &INx2Vi2Session::GetTimestampRecorderCount, "GetTimestampRecorderCount (guessed)"},
            {11, &INx2Vi2Session::ReturnEvent, "Unknown11"},
            {31, &INx2Vi2Session::AcquireNativeEvent, "AcquireNativeEvent"},
            {32, &INx2Vi2Session::DuplicateNativeEvent, "DuplicateNativeEvent"},
            {261, &INx2Vi2Session::BindVsync, "BindDisplayVsyncEvent"},
            {262, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayVsyncEvent"},
            {263, &INx2Vi2Session::BindChange, "BindDisplayRefreshRateEvent"},
            {264, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayRefreshRateEvent"},
            {265, &INx2Vi2Session::BindChange, "BindDisplayHdrStatusEvent"},
            {266, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayHdrStatusEvent"},
            {267, &INx2Vi2Session::BindChange, "BindDisplayHdrPaperWhiteEvent"},
            {268, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayHdrPaperWhiteEvent"},
            {269, &INx2Vi2Session::BindChange, "BindDisplayBufferMaxResolutionEvent"},
            {270, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayBufferMaxResolutionEvent"},
            {271, &INx2Vi2Session::BindChange, "BindDisplayModeCapabilityEvent"},
            {272, &INx2Vi2Session::UnbindNativeEvent, "UnbindDisplayModeCapabilityEvent"},
            // Title 1 calls these ~75 times a second with the display name "default"; quiet
            // handlers (the auto stub logged two lines per call: a 50 MB log in an 18 minute
            // session).
            {103, &INx2Vi2Session::QueryDisplayRefreshRate, "QueryDisplayRefreshRate"},
            {104, &INx2Vi2Session::QueryDisplayHdrStatus, "QueryDisplayHdrStatus"},
            {105, &INx2Vi2Session::QueryDisplayHdrPaperWhite, "QueryDisplayHdrPaperWhite"},
            {212, &INx2Vi2Session::GetLayerBufferBits, "GetLayerBufferBits"},
            {106, &INx2Vi2Session::GetBufferMaxResolution, "GetBufferMaxResolution"},
            {228, &INx2Vi2Session::CancelClientBuffer, "CancelClientBuffer"},
            {229, &INx2Vi2Session::PresentClientBuffer, "PresentClientBuffer"},
            // Firmware commands without a known meaning: zeroed replies.
            {13, &INx2Vi2Session::StubReplyZeroed, "Unknown13"},
            {51, &INx2Vi2Session::StubReplyZeroed, "Unknown51"},
            {101, &INx2Vi2Session::StubReplyZeroed, "Unknown101"},
            {102, &INx2Vi2Session::StubReplyZeroed, "Unknown102"},
            {107, &INx2Vi2Session::StubReplyZeroed, "Unknown107"},
            {108, &INx2Vi2Session::StubReplyZeroed, "Unknown108"},
            {111, &INx2Vi2Session::StubReplyZeroed, "Unknown111"},
            {112, &INx2Vi2Session::StubReplyZeroed, "Unknown112"},
            {113, &INx2Vi2Session::StubReplyZeroed, "Unknown113"},
            {202, &INx2Vi2Session::StubReplyZeroed, "Unknown202"},
            {230, &INx2Vi2Session::StubReplyZeroed, "Unknown230"},
            {231, &INx2Vi2Session::StubReplyZeroed, "Unknown231"},
            {232, &INx2Vi2Session::StubReplyZeroed, "Unknown232"},
            {251, &INx2Vi2Session::StubReplyZeroed, "Unknown251"},
            {252, &INx2Vi2Session::StubReplyZeroed, "Unknown252"},
            {261, &INx2Vi2Session::StubReplyZeroed, "Unknown261"},
            {262, &INx2Vi2Session::StubReplyZeroed, "Unknown262"},
            {263, &INx2Vi2Session::StubReplyZeroed, "Unknown263"},
            {264, &INx2Vi2Session::StubReplyZeroed, "Unknown264"},
            {265, &INx2Vi2Session::StubReplyZeroed, "Unknown265"},
            {266, &INx2Vi2Session::StubReplyZeroed, "Unknown266"},
            {267, &INx2Vi2Session::StubReplyZeroed, "Unknown267"},
            {268, &INx2Vi2Session::StubReplyZeroed, "Unknown268"},
            {269, &INx2Vi2Session::StubReplyZeroed, "Unknown269"},
            {270, &INx2Vi2Session::StubReplyZeroed, "Unknown270"},
            {271, &INx2Vi2Session::StubReplyZeroed, "Unknown271"},
            {272, &INx2Vi2Session::StubReplyZeroed, "Unknown272"},
            {301, &INx2Vi2Session::StubReplyZeroed, "Unknown301"},
            {302, &INx2Vi2Session::StubReplyZeroed, "Unknown302"},
            {351, &INx2Vi2Session::StubReplyZeroed, "Unknown351"},
            {352, &INx2Vi2Session::StubReplyZeroed, "Unknown352"},
            {353, &INx2Vi2Session::StubReplyZeroed, "Unknown353"},
            {354, &INx2Vi2Session::StubReplyZeroed, "Unknown354"},
            {355, &INx2Vi2Session::StubReplyZeroed, "Unknown355"},
            {356, &INx2Vi2Session::StubReplyZeroed, "Unknown356"},
            {358, &INx2Vi2Session::StubReplyZeroed, "Unknown358"},
            {359, &INx2Vi2Session::StubReplyZeroed, "Unknown359"},
        });
    }

    ~INx2Vi2Session() override {
        // The vsync callback may be running on the HostTiming thread: empty its event list under
        // its lock first (it holds the state, not this session), then close the events.
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.clear();
        }
        if (m_vsync_timer) {
            system.CoreTiming().UnscheduleEvent(m_vsync_timer);
        }
        for (auto* event : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    void Handshake(HLERequestContext& ctx) {
        TipcHandshake(ctx, "nx2:vi2");
    }

    // 229(u64 client token, u32 slot, u32 image, ...fences): result, u64 mask of buffer slots the
    // client may acquire next, four zero words. Nothing is shown yet; every slot is handed back
    // at once (changes.patch's default mask is 0x7fff).
    void PresentClientBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        ++m_presents;
        if (Common::DkbFrameTrace::Enabled()) {
            m_vsync->trace_present.store(m_presents, std::memory_order_relaxed);
        }
        Core::g_nx2_present_count.fetch_add(1, std::memory_order_relaxed);
        if (m_presents <= 4 || (m_presents % 600) == 0) {
            LOG_INFO(Service, "nx2:vi2 229 present #{} token={:#x} slot={} image={}", m_presents,
                     u64{w[2]} | (u64{w[3]} << 32), w[4], w[5]);
        }
        // Wait for this frame's fence, so the game doesn't write the next frame's data (constant
        // buffers holding texture handles, vertex data, Pokopia's glyph atlas) while the GPU
        // thread still reads this frame's. But title 1's GPU waits on semaphores its CPU releases
        // only after this present returns: while a channel stays suspended on one, wait for the
        // previous frame's fence instead and let the game prepare frame N+1 meanwhile (waiting
        // for this frame there cost 8-56 ms of every 34-105 ms frame, or never finished).
        // EDEN_NX2_FRAMES_AHEAD=1 always waits for the previous frame, =0 always for this one.
        static const int frames_ahead = [] {
            const char* value = std::getenv("EDEN_NX2_FRAMES_AHEAD");
            return value == nullptr ? -1 : value[0] == '1' ? 1 : 0;
        }();
        const auto entered = std::chrono::steady_clock::now();
        if (frames_ahead == 1 || (frames_ahead == -1 && !WaitForFrame(~w[22], w[24], true))) {
            LOG_DEBUG(Service, "nx2:vi2 present #{} fence ({}, {:#x}), waiting for ({}, {:#x}); now {:#x}",
                      m_presents, ~w[22], w[24], m_previous_fence_id, m_previous_fence_value,
                      ~w[22] < 192 ? system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(~w[22]) : 0U);
            // With EDEN_NX2_FRAMES_AHEAD=1, only run a frame ahead while the GPU keeps up. With
            // the previous frame still on the GPU here, the GPU is the limit and running ahead
            // gains nothing; and Splatoon Raiders, which reuses its command memory as if the GPU
            // were at most a frame behind, rewrote command lists Eden had not read yet (misread
            // commands, then crashes, with a slow GPU). Wait for this frame too then. Not when a
            // suspended channel brought us here: this frame can't finish before the present
            // returns.
            const bool gpu_behind = frames_ahead == 1 && m_previous_fence_id < 192 &&
                                    !system.Host1x().GetSyncpointManager().IsReadyGuest(
                                        m_previous_fence_id, m_previous_fence_value);
            WaitForFrame(m_previous_fence_id, m_previous_fence_value);
            if (gpu_behind) {
                WaitForFrame(~w[22], w[24]);
            }
        } else if (frames_ahead == 0) {
            WaitForFrame(~w[22], w[24]);
        }
        m_previous_fence_id = ~w[22];
        m_previous_fence_value = w[24];
        const auto waited = std::chrono::steady_clock::now();
        Composite(w[5], ~w[22], w[24], w[7]);
        const auto composited = std::chrono::steady_clock::now();
        PaceToVblank(w[12]);
        const auto paced = std::chrono::steady_clock::now();
        if (Common::DkbFrameTrace::Enabled()) {
            const auto ms = [](auto duration) {
                return std::chrono::duration<double, std::milli>(duration).count();
            };
            if (m_frame_trace.rate.Ready(paced)) {
                LOG_INFO(Service,
                         "DKB frame present={} word12={:#x} requested_interval={} "
                         "requested_hz={} effective_interval={} effective_hz={} forced_interval={} "
                         "speed_limit={} speed_percent={} switches={} last_switch={:#x}->{:#x} "
                         "cpu_frame_wall_ms={:.3f} cpu_present_wall_ms={:.3f} fence_wall_ms={:.3f} "
                         "composite_wall_ms={:.3f} target_ms={:.3f} sleep_requested_ms={:.3f} "
                         "sleep_actual_wall_ms={:.3f} pace_wall_ms={:.3f} deadline_reset={}",
                         m_presents, w[12], w[12] >> 16, w[12] & 0xffff,
                         m_frame_trace.interval, m_frame_trace.refresh, m_frame_trace.forced,
                         Settings::values.use_speed_limit.GetValue(), Settings::SpeedLimit(),
                         m_frame_trace.switches, m_frame_trace.switch_from,
                         m_frame_trace.switch_to,
                         m_frame_trace.last_present == std::chrono::steady_clock::time_point{}
                             ? -1.0 : ms(entered - m_frame_trace.last_present),
                         ms(paced - entered), ms(waited - entered), ms(composited - waited),
                         m_frame_trace.target_ms, m_frame_trace.sleep_requested_ms,
                         m_frame_trace.sleep_actual_ms, ms(paced - composited),
                         m_frame_trace.deadline_reset);
                m_frame_trace.switches = 0;
            }
            m_frame_trace.last_present = entered;
        }
        // Where each frame's time goes on the present thread, averaged per 600 presents: the
        // game is blocked here while Eden's GPU thread catches up (fence) and composites.
        {
            using us = std::chrono::microseconds;
            // long long like the stats: microseconds::rep is long on Linux (std::max needs one type).
            const auto to_us = [](auto d) -> long long {
                return std::chrono::duration_cast<us>(d).count();
            };
            m_stats.fence_us += to_us(waited - entered);
            m_stats.composite_us += to_us(composited - waited);
            m_stats.pace_us += to_us(paced - composited);
            m_stats.max_fence_us = (std::max<long long>)(m_stats.max_fence_us, static_cast<long long>(to_us(waited - entered)));
            if (m_stats.last_present != std::chrono::steady_clock::time_point{}) {
                const long long frame = to_us(entered - m_stats.last_present);
                m_stats.frame_us += frame;
                m_stats.max_frame_us = (std::max)(m_stats.max_frame_us, frame);
                m_stats.hitches += frame > 100'000 ? 1 : 0;
            }
            m_stats.last_present = entered;
            if (++m_stats.presents == 600) {
                LOG_INFO(Service,
                         "nx2:vi2 last 600 presents: frame {} us (max {}, {} over 100 ms), "
                         "blocked on the GPU fence {} us (max {}), composite {} us, vblank "
                         "pacing {} us",
                         m_stats.frame_us / 599, m_stats.max_frame_us, m_stats.hitches,
                         m_stats.fence_us / 600, m_stats.max_fence_us, m_stats.composite_us / 600,
                         m_stats.pace_us / 600);
                m_stats = {.last_present = entered};
            }
        }
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    // The console's compositor frees a buffer only once its frame's GPU fence has passed, which
    // is what keeps a game from running ahead of the GPU. This reply always reports every buffer
    // free, so pace the game here instead: wait (bounded, in case the GPU waits on the CPU) until
    // Eden's GPU thread has processed the frame. Otherwise the game laps the GPU thread and
    // rewrites command memory it hasn't read yet.
    u32 m_wait_timeouts{};
    struct PresentStats {
        long long frame_us{}, fence_us{}, composite_us{}, pace_us{}, max_fence_us{};
        long long max_frame_us{}, hitches{};
        u32 presents{};
        std::chrono::steady_clock::time_point last_present{};
    } m_stats{};
    u32 m_previous_fence_id{~0U}; // none yet (WaitForFrame ignores ids >= 192)
    u32 m_previous_fence_value{};

    // The console flips one frame per 60 Hz vblank and keeps the rest queued, so a game can't
    // present faster than that. Every slot is reported free here: hold each present until the
    // next vblank instead (title 4 ran at up to 160 fps, its timing and audio with it).
    Nx2PresentPacer m_present_pacer;
    u32 m_swap_word{};

    struct FrameTrace {
        Common::DkbFrameTrace::RateLimit rate;
        std::chrono::steady_clock::time_point last_present{};
        std::optional<u32> previous_word;
        u64 switches{};
        u32 switch_from{}, switch_to{}, interval{}, refresh{}, forced{};
        double target_ms{}, sleep_requested_ms{}, sleep_actual_ms{};
        bool deadline_reset{};
    } m_frame_trace;

    // timing: present word 12, (swap interval << 16) | refresh rate. Fire Emblem: Fortune's Weave
    // sends 0x1003c during its boot logos and 0x2003c (two vblanks per frame, 30 fps) after; paced
    // at every vblank it ran at double speed. EDEN_NX2_SWAP_INTERVAL=<n> forces the interval.
    void PaceToVblank(u32 timing) {
        const bool trace = Common::DkbFrameTrace::Enabled();
        if (trace) {
            if (m_frame_trace.previous_word && *m_frame_trace.previous_word != timing) {
                ++m_frame_trace.switches;
                m_frame_trace.switch_from = *m_frame_trace.previous_word;
                m_frame_trace.switch_to = timing;
            }
            m_frame_trace.previous_word = timing;
            m_frame_trace.target_ms = 0;
            m_frame_trace.sleep_requested_ms = 0;
            m_frame_trace.sleep_actual_ms = 0;
            m_frame_trace.deadline_reset = false;
        }
        static const u32 forced_interval = [] {
            const char* value = std::getenv("EDEN_NX2_SWAP_INTERVAL");
            return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
        }();
        const auto& process = Kernel::GetCurrentProcess(kernel);
        const bool mkw_dynamic =
            process.GetProgramId() == Settings::MarioKartWorldProgramId &&
            process.IsMkwDynamicFpsPatchApplied() && Settings::values.mkw_dynamic_fps.GetValue();
        u32 swap_interval = std::clamp<u32>(timing >> 16, 1, 4);
        if (mkw_dynamic) {
            swap_interval = 1;
        }
        if (forced_interval != 0) {
            swap_interval = forced_interval;
        }
        const u32 refresh_rate = (timing & 0xffff) >= 30 && (timing & 0xffff) <= 240 ? timing & 0xffff : 60;
        if (trace) {
            m_frame_trace.interval = swap_interval;
            m_frame_trace.refresh = refresh_rate;
            m_frame_trace.forced = forced_interval;
        }
        if (!Settings::values.use_speed_limit.GetValue() || Settings::SpeedLimit() == 0) {
            return;
        }
        if (timing != std::exchange(m_swap_word, timing)) {
            LOG_INFO(Service, "nx2:vi2 present #{} swap interval {} at {} Hz (word 12 = {:#x})",
                     m_presents, swap_interval, refresh_rate, timing);
        }
        const auto interval = std::chrono::nanoseconds{
            100'000'000'000LL * swap_interval / refresh_rate / Settings::SpeedLimit()};
        if (trace) {
            m_frame_trace.target_ms = std::chrono::duration<double, std::milli>(interval).count();
        }
        const auto now = std::chrono::steady_clock::now();
        const auto pacing = m_present_pacer.Advance(now, interval);
        if (trace) {
            m_frame_trace.deadline_reset = pacing.reset;
        }
        if (now < pacing.deadline) {
            // Sleep this service thread in the guest kernel. It runs on guest core 3, and a host
            // sleep held that core for the whole wait: game threads scheduled there ran only
            // between presents (Fortune's Weave's loader read one RomFS block per frame).
            const auto wait =
                std::chrono::duration_cast<std::chrono::nanoseconds>(pacing.deadline - now);
            const auto sleep_start = trace ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            static_cast<void>(Kernel::GetCurrentThread(kernel).Sleep(
                kernel, kernel.HardwareTimer().GetTick() + wait.count() + 2));
            if (trace) {
                m_frame_trace.sleep_requested_ms =
                    std::chrono::duration<double, std::milli>(wait).count();
                m_frame_trace.sleep_actual_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - sleep_start).count();
            }
        }
    }

    /// Waits until the fence is reached. With `unless_gpu_waits`, gives up after a few ms once a
    /// GPU channel was seen suspended on a semaphore (it may be one this thread releases later).
    /// Returns whether the fence was reached.
    bool WaitForFrame(u32 fence_id, u32 fence_value, bool unless_gpu_waits = false) {
        if (fence_id >= 192) {
            return true;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const auto start = std::chrono::steady_clock::now();
        // Long enough to cover shader/pipeline compile stalls: a game that gets more than a few
        // command buffers ahead misses the equal-value gates its other channels acquire.
        const auto deadline = start + std::chrono::milliseconds{4000};
        if (!syncpoints.IsReadyGuest(fence_id, fence_value)) {
            // Woken by the GPU thread as it reaches the fence. Polling with 200 us sleeps slept
            // about 1 ms per check on Windows, added to every frame. An action left registered
            // after a timeout fires later on a state nobody waits on.
            struct Wake {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{};
            };
            auto wake = std::make_shared<Wake>();
            syncpoints.RegisterGuestAction(fence_id, fence_value, [wake] {
                {
                    std::scoped_lock lk{wake->mutex};
                    wake->done = true;
                }
                wake->cv.notify_all();
            });
            std::unique_lock lk{wake->mutex};
            // Title 1's channels resume and suspend again while they wait on this thread, so any
            // suspension counts, not only an unbroken one.
            bool gpu_waited = false;
            while (!wake->done && system.IsPoweredOn() &&
                   std::chrono::steady_clock::now() < deadline) {
                if (unless_gpu_waits) {
                    gpu_waited |= system.GPU().HasSuspendedChannels();
                    if (gpu_waited &&
                        std::chrono::steady_clock::now() - start > std::chrono::milliseconds{3}) {
                        return false;
                    }
                }
                wake->cv.wait_for(lk, std::chrono::milliseconds{unless_gpu_waits ? 1 : 50});
            }
        }
        if (unless_gpu_waits && !syncpoints.IsReadyGuest(fence_id, fence_value)) {
            return false;
        }
        if (!syncpoints.IsReadyGuest(fence_id, fence_value) && m_wait_timeouts++ < 64) {
            LOG_WARNING(Service, "nx2:vi2 present #{} fence ({}, {}) not reached in 4 s (now {})",
                        m_presents, fence_id, fence_value, syncpoints.GetGuestSyncpointValue(fence_id));
        }
        if (m_presents <= 8 || (m_presents % 600) == 0) {
            LOG_INFO(Service, "nx2:vi2 present #{} fence ({}, {}) now {}, waited {} us",
                     m_presents, fence_id, fence_value, syncpoints.GetGuestSyncpointValue(fence_id),
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
        }
        return syncpoints.IsReadyGuest(fence_id, fence_value);
    }

    // Shows a presented native image: word 5 is the image id, ~word 22 / word 24 the GPU fence
    // (syncpoint, value) to wait for (layout from changes.patch). The image is a block-linear
    // RGBA8 surface in the console's native GOB layout at GPU address == CPU address.
    // shown_size: the present's word 7, ((height - 1) << 16) | (width - 1) of the area the game drew
    // into from the image's top-left corner (0 when it gives none).
    void Composite(u32 image_id, u32 fence_id, u32 fence_value, u32 shown_size) {
        u64 address{};
        u32 parent{};
        u64 parent_offset{};
        std::optional<Nx2NativeObjects::VideoTrace> video_trace;
        Nx2ImageLayout layout{};
        u32 image_format{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            if (image == m_objects->images.end()) {
                return;
            }
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory == m_objects->memories.end() || memory->second.address == 0) {
                return;
            }
            address = memory->second.address + image->second.offset;
            parent = memory->second.parent;
            parent_offset = memory->second.parent_offset + image->second.offset;
            layout = Nx2CalculateImageLayout(image->second.preference.data());
            image_format = image->second.preference[6];
            if (Nx2VideoTraceEnabled()) {
                video_trace = m_objects->last_video_write;
            }
        }
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        auto gpu_address = registry.GpuAddressOf(parent, parent_offset);
        auto device = gpu_address ? registry.DeviceAddressOfGpu(*gpu_address) : std::nullopt;
        if (!device) {
            device = registry.DeviceAddressOf(address);
        }
        if (video_trace) {
            if (!gpu_address && device && registry.DeviceAddressOfGpu(address) == device) {
                gpu_address = address;
            }
            LOG_INFO(Service,
                     "nx2:vdec trace present={} image={} cpu={:#x} gpu={:#x} device={:#x} "
                     "gpu_known={} mapped={} latest_decode_syncpoint={} decoded_frames={} codec={} "
                     "decoded_image={} decoded_gpu={:#x} decoded_device={:#x} pts={} "
                     "same_image={} same_device={}",
                     m_presents, image_id, address, gpu_address.value_or(0),
                     device.value_or(0), gpu_address.has_value(), device.has_value(), video_trace->syncpoint,
                     video_trace->decoded_frames, video_trace->codec, video_trace->image,
                     video_trace->gpu_address, video_trace->device_address, video_trace->pts,
                     image_id == video_trace->image,
                     device && *device == video_trace->device_address);
        }
        if (!device) {
            LOG_WARNING(Service, "nx2:vi2 present: image {} at {:#x} has no device mapping",
                        image_id, address);
            return;
        }
        u32 crop_width = layout.width;
        u32 crop_height = layout.height;
        // The present says how much of the image the game drew: Kirby Air Riders draws 1920x1080
        // into 2560x1440 images, Mario Kart World 2560x1440 into 3840x2160 ones. Showing the whole
        // image put the frame in the top-left corner.
        const u32 shown_width = (shown_size & 0xFFFF) + 1, shown_height = (shown_size >> 16) + 1;
        const bool has_shown_size = shown_size != 0 && shown_width <= layout.width &&
                                    shown_height <= layout.height;
        if (has_shown_size) {
            crop_width = shown_width;
            crop_height = shown_height;
        }
        // Handheld titles keep their docked present images (up to 3840x2160) and draw the frame
        // into the top-left 1920x1080, the handheld panel's resolution; the console's display
        // scans out only that region. Showing whole images put the frame in a corner inside black
        // margins that scaled with the window (seen on Mario Kart World and Splatoon Raiders).
        if (!Settings::IsDockedMode()) {
            crop_width = (std::min)(crop_width, 1920U);
            crop_height = (std::min)(crop_height, 1080U);
        } else if (!has_shown_size &&
                   system.GetApplicationProcessProgramID() == 0x0400C3F00006E000ULL &&
                   layout.width == 3840 && layout.height == 2160) {
            // Mario Kart World docked draws 2560x1440 into the top-left of its 4K images.
            crop_width = 2560;
            crop_height = 1440;
        }
        static const u32 env_crop_w = [] {
            const char* v = std::getenv("EDEN_NX2_CROP_WIDTH");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0U;
        }();
        static const u32 env_crop_h = [] {
            const char* v = std::getenv("EDEN_NX2_CROP_HEIGHT");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0U;
        }();
        if (env_crop_w != 0 && env_crop_h != 0) {
            crop_width = env_crop_w;
            crop_height = env_crop_h;
        }
        const u64 shown = (u64{layout.width} << 48) | (u64{layout.height} << 32) |
                          (u64{crop_width} << 16) | crop_height;
        if (shown != m_shown_layout) {
            m_shown_layout = shown;
            LOG_INFO(Service,
                     "nx2:vi2 composite image {} layout {}x{}, drawn area {:#x}, crop {}x{}, docked={}",
                     image_id, layout.width, layout.height, shown_size, crop_width, crop_height,
                     Settings::IsDockedMode());
        }
        std::vector<Tegra::FramebufferConfig> layers{Tegra::FramebufferConfig{
            .address = *device,
            .offset = 0,
            .width = layout.width,
            .height = layout.height,
            .stride = static_cast<u32>(layout.pitch / 4),
            // Preference word 6: 1 = RGBA8, 4 = RGB10A2.
            .pixel_format = image_format == 4 ? android::PixelFormat::Rgba1010102
                                              : android::PixelFormat::Rgba8888,
            .transform_flags = android::BufferTransformFlags::Unset,
            .crop_rect = {0, 0, static_cast<int>(crop_width), static_cast<int>(crop_height)},
            .blending = Tegra::BlendMode::Opaque,
            .native_tiling = true,
        }};
        std::vector<Nvidia::NvFence> fences;
        if (fence_id < 192) {
            fences.push_back({static_cast<s32>(fence_id), fence_value});
        }
        system.GPU().RequestComposite(std::move(layers), std::move(fences));
        CaptureFrame();
        DumpRanges();
        system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
        system.GetPerfStats().EndSystemFrame();
        system.GetPerfStats().BeginSystemFrame();
    }

    // EDEN_NX2_FRAME_CAPTURE=<dir>: every 120th present, save what the renderer shows as
    // nx2-frame-<n>.tga in <dir> (lets hidden test runs check the output; idea from changes.patch).
    void CaptureFrame() {
        static const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        static const u64 every = [] {
            const char* value = std::getenv("EDEN_NX2_FRAME_CAPTURE_EVERY");
            return value != nullptr ? (std::max)(u64{1}, static_cast<u64>(std::strtoull(value, nullptr, 10))) : u64{120};
        }();
        if (directory == nullptr || (m_presents % every) != 1 % every || m_capture_pending->exchange(true)) {
            return;
        }
        const auto layout = Layout::DefaultFrameLayout(1280, 720);
        auto pixels = std::make_shared<std::vector<u8>>(layout.width * layout.height * 4);
        const std::string path =
            fmt::format("{}/nx2-frame-{:05}.tga", directory, m_presents);
        auto pending = m_capture_pending; // the screenshot callback can outlive the session
        system.GPU().Renderer().RequestScreenshot(
            pixels->data(),
            [pixels, layout, path, pending](bool invert_y) {
                std::array<u8, 18> header{};
                header[2] = 2; // uncompressed true colour
                header[12] = static_cast<u8>(layout.width);
                header[13] = static_cast<u8>(layout.width >> 8);
                header[14] = static_cast<u8>(layout.height);
                header[15] = static_cast<u8>(layout.height >> 8);
                header[16] = 32;
                // Descriptor bit 5: rows top to bottom. invert_y means the data is bottom-up.
                header[17] = invert_y ? 0x08 : 0x28;
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(header.data(), 1, header.size(), file);
                    std::fwrite(pixels->data(), 1, pixels->size(), file);
                    std::fclose(file);
                }
                pending->store(false);
            },
            layout);
    }

    // EDEN_NX2_DUMP=<present>:<hex address>:<hex size>[,<hex address>:<hex size>...]: at that
    // present, flush those GPU ranges back from the host GPU and save their bytes as
    // nx2-dump-<address>.bin in the EDEN_NX2_FRAME_CAPTURE directory (to inspect what a pass
    // rendered, e.g. a 3D volume, offline).
    void DumpRanges() {
        static const std::string spec = [] {
            const char* text = std::getenv("EDEN_NX2_DUMP");
            return text ? std::string{text} : std::string{};
        }();
        if (spec.empty() || m_presents != std::strtoull(spec.c_str(), nullptr, 10)) {
            return;
        }
        const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        size_t position = spec.find(':');
        while (position != std::string::npos) {
            char* end{};
            const u64 address = std::strtoull(spec.c_str() + position + 1, &end, 16);
            const u64 size = *end == ':' ? std::strtoull(end + 1, &end, 16) : 0;
            position = spec.find(',', position + 1); // the next "address:size"
            const std::string path = fmt::format("{}/nx2-dump-{:x}.bin", directory ? directory : ".", address);
            // On the GPU thread: waiting for it here can deadlock with a GPU wait on this present.
            static_cast<void>(system.GPU().RunOnGpuThread([&system = system, address, size, path] {
                std::vector<u8> bytes(size);
                for (u64 current = address; current < address + size;) {
                    const u64 chunk = std::min((current | 0xfff) + 1, address + size) - current;
                    if (const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(current)) {
                        system.GPU().Renderer().ReadRasterizer()->FlushRegion(*device, chunk);
                        system.Host1x().MemoryManager().ReadBlock(*device, bytes.data() + (current - address), chunk);
                    }
                    current += chunk;
                }
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), file);
                    std::fclose(file);
                }
                LOG_INFO(Service, "nx2: dumped {:#x} bytes at {:#x} to {}", size, address, path);
            }));
        }
    }

    // 228(u64 token, u32 slot): return a buffer without presenting it.
    void CancelClientBuffer(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    // nn::vi2::QueryDisplayRefreshRate(display name): u32 refresh rate in Hz, then a bool (a u64
    // in all). nn::vi::detail::DefaultDisplay::QueryRefreshRate takes only -1, 60 or 120 and fails
    // on anything else; Minecraft polls it and never draws while it fails. A one-word reply left
    // the bool to whatever the message buffer held.
    void QueryDisplayRefreshRate(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {60, 0});
    }

    // nn::vi2::QueryDisplayHdrPaperWhite(display name): u16 paper white in nits. The empty reply
    // left it to the message buffer's leftovers; answer the BT.2408 reference white.
    void QueryDisplayHdrPaperWhite(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {203});
    }

    // nn::vi2::QueryDisplayHdrStatus(display name): u64 HdrOutputFormat, u16. The sdk's
    // DefaultDisplay::QueryHdrStatus takes format 1 as HDR output and fails with 0x272 on
    // anything above 1. With the empty auto stub reply it read the request's leftover words as
    // the format; Kirby Air Riders re-queries whenever its HDR status event fires and rebuilt its
    // window images every few frames, which showed black. Report SDR output.
    // nn::vi2::QueryDisplayHdrStatus(display name): u64 output format, u16 peak luminance in
    // nits. The sdk takes format 1 as SDR and 2 as HDR and fails the query on any other value.
    // Report SDR output with a typical SDR panel's peak.
    void QueryDisplayHdrStatus(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {1, 0, 400});
    }

    // 212(u64 layer): u64 bits of the layer's buffer slots that are ready. The client polls it once
    // a second until some are set; it only got going because the auto stub's reply happened to
    // hold non-zero leftovers. Report the same slots 228/229 hand back.
    void GetLayerBufferBits(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots), static_cast<u32>(AvailableSlots >> 32)});
    }

    void ReturnOne(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {}, returning 1", ctx.GetCommand());
        TipcReplyWords(ctx, {1});
    }

    // The highest buffer resolution mode of the "default" display. Fire Emblem: Fortune's Weave picks
    // its output from it: 1 -> 1280x720, 2 -> 1920x1080, 4 and 8 -> 2560x1440; 0 (the auto stub's
    // reply), 3 and 5 fall back to 720p. Docked reports 4, handheld 2 (the screen is 1080p);
    // EDEN_VI2_MAX_RESOLUTION_MODE=<n> overrides.
    void GetBufferMaxResolution(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        const u64 name = u64{packet[2]} | (u64{packet[3]} << 32);
        constexpr u64 DefaultDisplay = 0x00746c7561666564ULL; // "default"
        Result result = Kernel::ResultInvalidCombination;
        u64 mode{};
        if (packet[0] == 0x7a && packet[1] == 2) {
            result = Result{ErrorModule::VI, 7};
            if (name == DefaultDisplay) {
                static const char* forced = std::getenv("EDEN_VI2_MAX_RESOLUTION_MODE");
                mode = forced ? std::strtoull(forced, nullptr, 0) : Settings::IsDockedMode() ? 4 : 2;
                result = ResultSuccess;
            }
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(result.raw);
        rb.Push(mode);
        LOG_DEBUG(Service_VI,
                  "nx2:vi2 max resolution: name={:#x} docked={} result={:#x} mode={}",
                  name, Settings::IsDockedMode(), result.raw, mode);
    }

    // Must be < 8; nn::vi::detail::LayerHistory creates 7 timestamp recorders per layer.
    void GetTimestampRecorderCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC 5, returning 7");
        TipcReplyWords(ctx, {7});
    }

    void ReturnEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {} [2]={:#x} [3]={:#x}, returning an event",
                    ctx.GetCommand(), ctx.CommandBuffer()[2], ctx.CommandBuffer()[3]);
        auto* event = m_context.CreateEvent(fmt::format("nx2:vi2:Event{}", m_events.size()));
        m_events.push_back(event);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // 31(u32 index < 8): the session's event for that slot (the sdk picks a free index). The
    // 261..272 commands bind a slot to a display event source and unbind it again. Until a slot
    // is bound it fires on every vblank, as all of them used to.
    void AcquireNativeEvent(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[2];
        if (index >= m_native_events.size()) {
            LOG_ERROR(Service, "nx2:vi2 31 index {} out of range", index);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown);
            return;
        }
        LOG_INFO(Service, "nx2:vi2 31 native event {}", index);
        ReplyNativeEvent(ctx, index);
    }

    // 32(u32 index, copy handle): the client hands back its handle for an acquired slot and gets
    // the slot's event again; the handle descriptor and handle come before the index. Games call
    // it for slot 0 right after binding it to vsync. Title 1's main thread waits on this event
    // until it has fired twice before it submits any GPU work, and hung there while it never
    // fired.
    void DuplicateNativeEvent(HLERequestContext& ctx) {
        const u32* words = ctx.CommandBuffer();
        u32 data = 2;
        if (words[1] >> 31) {
            const u32 descriptor = words[2];
            data = 3 + ((descriptor >> 1) & 0xf) + ((descriptor >> 5) & 0xf) + (descriptor & 1 ? 2 : 0);
        }
        const u32 index = words[data];
        if (index >= m_native_events.size()) {
            LOG_ERROR(Service, "nx2:vi2 32 index {} out of range", index);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown);
            return;
        }
        LOG_INFO(Service, "nx2:vi2 32 native event {}", index);
        ReplyNativeEvent(ctx, index);
    }

    // 261(u64 display name, u32 index): fire the slot on every vblank.
    void BindVsync(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[4];
        LOG_INFO(Service, "nx2:vi2 261 native event {} -> vsync", index);
        if (index < m_native_events.size() && m_native_events[index]) {
            SetVsyncSlot(index, true);
        }
        TipcReplyWords(ctx, {});
    }

    // 263/265/267/269/271(u64 display name, u32 index): fire the slot when the display's refresh
    // rate, HDR status, HDR paper white, buffer max resolution or mode capability changes, so the
    // game re-queries it. None of them change here: fire once so the game reads the current
    // state, then leave the slot quiet. Kirby Air Riders re-queried its HDR status on every
    // present while this slot fired with vsync.
    void BindChange(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[4];
        LOG_INFO(Service, "nx2:vi2 {} native event {} -> change notification", ctx.GetCommand(), index);
        if (index < m_native_events.size() && m_native_events[index]) {
            SetVsyncSlot(index, false);
            m_native_events[index]->Signal(system.Kernel());
        }
        TipcReplyWords(ctx, {});
    }

    // 262..272 even (u64 display name, u32 index): detach the slot from its source.
    void UnbindNativeEvent(HLERequestContext& ctx) {
        const u32 index = ctx.CommandBuffer()[4];
        LOG_INFO(Service, "nx2:vi2 {} native event {} unbound", ctx.GetCommand(), index);
        if (index < m_native_events.size() && m_native_events[index]) {
            SetVsyncSlot(index, false);
        }
        TipcReplyWords(ctx, {});
    }

    void ReplyNativeEvent(HLERequestContext& ctx, u32 index) {
        auto*& event = m_native_events[index];
        if (!event) {
            event = m_context.CreateEvent(fmt::format("nx2:vi2:Native{}", index));
            m_events.push_back(event);
            SetVsyncSlot(index, true);
            StartVsyncTimer();
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    void SetVsyncSlot(u32 index, bool on) {
        auto* event = m_native_events[index];
        std::scoped_lock lk{m_vsync->mutex};
        auto& list = m_vsync->events;
        const auto it = std::find(list.begin(), list.end(), event);
        if (on && it == list.end()) {
            list.push_back(event);
        } else if (!on && it != list.end()) {
            list.erase(it);
        }
    }

    void StartVsyncTimer() {
        if (!m_vsync_timer) {
            // EDEN_NX2_VSYNC_HZ=<rate> (experiment): the game starts frames on this event, so a
            // frame that misses a 60 Hz tick idles until the next one.
            static const std::chrono::nanoseconds VblankInterval{[] {
                const char* text = std::getenv("EDEN_NX2_VSYNC_HZ");
                const long long hz = text ? (std::max)(std::strtoll(text, nullptr, 10), 1LL) : 60LL;
                return 1'000'000'000LL / hz;
            }()};
            m_vsync_timer = Core::Timing::CreateEvent(
                "nx2:vi2 vsync",
                [vsync_state = m_vsync, &kernel = system.Kernel()](
                    s64 scheduled,
                    std::chrono::nanoseconds late) -> std::optional<std::chrono::nanoseconds> {
                    std::scoped_lock lk{vsync_state->mutex};
                    for (auto* vsync : vsync_state->events) {
                        vsync->Signal(kernel);
                    }
                    if (Common::DkbFrameTrace::Enabled()) {
                        const s64 observed = scheduled + late.count();
                        ++vsync_state->trace_callbacks;
                        vsync_state->trace_late_callbacks +=
                            VblankInterval.count() > 0 && late >= VblankInterval;
                        vsync_state->trace_max_late =
                            (std::max)(vsync_state->trace_max_late, late.count());
                        if (vsync_state->trace_last_callback) {
                            const s64 gap = observed - *vsync_state->trace_last_callback;
                            vsync_state->trace_min_gap = vsync_state->trace_min_gap
                                ? (std::min)(*vsync_state->trace_min_gap, gap) : gap;
                        }
                        vsync_state->trace_last_callback = observed;
                        if (vsync_state->trace_rate.Ready(std::chrono::steady_clock::now())) {
                            LOG_INFO(Service,
                                     "DKB vsync present={} callbacks={} bound_events={} interval_ns={} "
                                     "late_callbacks={} max_late_ns={} min_gap_ns={} "
                                     "observed_global_ns={} guest_ticks={}",
                                     vsync_state->trace_present.load(std::memory_order_relaxed),
                                     vsync_state->trace_callbacks, vsync_state->events.size(),
                                     VblankInterval.count(), vsync_state->trace_late_callbacks,
                                     vsync_state->trace_max_late,
                                     vsync_state->trace_min_gap.value_or(-1), observed,
                                     kernel.System().CoreTiming().GetClockTicks());
                            vsync_state->trace_callbacks = 0;
                            vsync_state->trace_late_callbacks = 0;
                            vsync_state->trace_max_late = 0;
                            vsync_state->trace_min_gap.reset();
                        }
                    }
                    return std::nullopt;
                });
            system.CoreTiming().ScheduleLoopingEvent(VblankInterval, VblankInterval, m_vsync_timer);
        }
    }

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    std::vector<Kernel::KEvent*> m_events;
    std::array<Kernel::KEvent*, 8> m_native_events{}; ///< 31's slots, also listed in m_events
    // Shared with the vsync timer callback, which can outlive the session by one call.
    struct VsyncState {
        std::mutex mutex;
        std::vector<Kernel::KEvent*> events;
        Common::DkbFrameTrace::RateLimit trace_rate;
        std::optional<s64> trace_last_callback, trace_min_gap;
        s64 trace_max_late{};
        u64 trace_callbacks{}, trace_late_callbacks{};
        std::atomic<u64> trace_present{};
    };
    std::shared_ptr<VsyncState> m_vsync = std::make_shared<VsyncState>();
    std::shared_ptr<Core::Timing::EventType> m_vsync_timer;
    u64 m_presents{};
    u64 m_shown_layout{}; // image and crop size last logged
    std::shared_ptr<std::atomic<bool>> m_capture_pending = std::make_shared<std::atomic<bool>>(false);
};

} // namespace Service
