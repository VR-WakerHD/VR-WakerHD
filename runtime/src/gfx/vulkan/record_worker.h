#pragma once
#include "pod_commands.h"
#include "handoff_trace.h"
#include "fallback_stats.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace gfxvk {
// Worker jobs contain owned POD bytes, sequence and diagnostic publication-frame
// metadata only (not a guarantee that all commands belong to that frame). Vulkan objects
// remain owned by the producer's submission slot until its GPU fence completes.
class RecordWorker {
public:
    RecordWorker();
    ~RecordWorker();
    RecordWorker(const RecordWorker&) = delete;
    RecordWorker& operator=(const RecordWorker&) = delete;
    uint64_t publish(PodCommands& commands);
    void wait(uint64_t serial, handoff_trace::Reason reason = handoff_trace::Reason::Unspecified);
    bool emitted(uint64_t serial);
    void drain();
    void stop();
private:
    struct Job { uint64_t serial; uint64_t producerFrame; std::vector<std::byte> bytes; };
    static constexpr size_t maxQueued = 4;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    uint64_t published_ = 0, completed_ = 0;
    bool stopping_ = false;
    std::exception_ptr error_;
    std::thread thread_;
    void run();
};
// Producer-only, under the GX2 execution mutex. Drain the previous worker
// prefix before touching its command buffer, bypass both raw begin routing and
// the deferred facade, and restore routing after producer submission/present.
class ProducerPresentationRecording {
public:
    ProducerPresentationRecording();
    ~ProducerPresentationRecording();
    ProducerPresentationRecording(const ProducerPresentationRecording&) = delete;
    ProducerPresentationRecording& operator=(const ProducerPresentationRecording&) = delete;
private:
    bool previousDispatch_ = false;
};
// The saved Graphics > Advanced choice ("Record graphics on a second thread"), set by the
// renderer before it starts; WWHD_VK_RECORD_WORKER (exactly 1 on, anything else off) wins.
void set_record_worker_saved_choice(bool on);
bool record_worker_requested(); // no thread while disabled
// Default of Graphics > Advanced "Record graphics on a second thread" when the player never chose:
// on for Android (phones are usually limited by the render thread), off elsewhere
// Off by default everywhere again (docs/VR_PERFORMANCE.md, fourth round, 2026-10-26). On Windows it was on from
// round three: with an RTX 5080 and a Ryzen 9 3900X it took 1 to 1.5 ms of CPU a frame off the render thread, frames
// byte-identical on the desktop. But the headset sessions since then showed the VR view shaking and the arms
// flickering, which the sessions before it (the worker off) did not; and the frame rate in the headset did not change
// (two frames a step either way). Until the headset shows which change caused that, the stable configuration is the
// default: WWHD_VK_RECORD_WORKER=1 (or Graphics > Advanced) turns it on. On a Xiaomi 14 (Snapdragon 8 Gen 3) it made
// typical frames ~0.6 ms faster but the slowest 1% ~3 ms slower than the single-thread path.
// On by default on Windows again (docs/VR_PERFORMANCE.md, "PERF-05", 2026-10-27), with multiview, the atlas thread and
// PERF-07a, for a headset A/B: WWHD_VK_RECORD_WORKER=0 (or Graphics > Advanced) turns it off.
// On by default on desktop Linux as well (2026-10-27): desktop stereo at the headset's 90 fps schedule, Ryzen +
// RTX 5080, from the same state, it took the frames over 1.5x the budget from 760-816 to 398 standing still (63 to 74
// fps) and from 422 to 360 turning the camera (docs/VR_PERFORMANCE.md, "Linux frame pacing").
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
constexpr bool record_worker_default() { return true; }
#else
constexpr bool record_worker_default() { return false; }
#endif
bool producer_presentation_recording(); // producer-only scoped routing
bool record_worker_active(); // producer only, under GX2 execution mutex
void start_record_worker();
void stop_record_worker();
PodCommands* record_worker_commands();
uint64_t publish_record_commands();
// Append a detached eye's owned POD stream after the producer prefix without
// waiting for either stream to finish recording. Submission ownership is unchanged.
bool publish_held_record_commands(PodCommands& commands);
void publish_record_commands_at_draw_boundary();
void wait_record_commands(uint64_t serial, handoff_trace::Reason reason = handoff_trace::Reason::Unspecified);
bool record_commands_emitted(uint64_t serial);
void drain_record_commands(fallback_stats::Boundary boundary = fallback_stats::Boundary::ExplicitState,
    uint32_t opcode = 0, PodFailureReason reason = PodFailureReason::UnclassifiedEncoderReject);
uint64_t submit_record_commands(VkDevice device, VkQueue queue, VkCommandBuffer cmd,
    VkFence fence, VkSemaphore wait, VkSemaphore signal,
    VkSemaphore extraWait = VK_NULL_HANDLE, VkSemaphore extraSignal = VK_NULL_HANDLE);
}
