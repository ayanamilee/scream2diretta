// ScreamDirettaSync — DIRETTA::Sync subclass that pulls PCM frames from an
// externally-owned ordered PCM queue (PcmRing).
//
// Current architecture: the unified PCM queue is owned by DirettaState (in
// diretta.cpp) and lives across the Sync lifecycle. The Scream receiver
// thread writes into the queue continuously (during cooldown / open /
// handshake / steady state). The Diretta SDK send thread pulls from the
// queue via this Sync's getNewStream(), subject to a prefill / rebuffer
// gate so the very first cycles output silence until the queue has
// accumulated startup_queue_ms (or prefill_ms) of PCM.
//
// The ring is externally owned so it can persist across Sync open/close.
// Format changes rebuild the backend-owned queue once, and the new Sync reads
// from it directly. There is no staging drain step.

#ifndef SCREAM_DIRETTA_SYNC_H
#define SCREAM_DIRETTA_SYNC_H

#include "diretta_ring.h"
extern "C" {
#include "pcm_dump.h"
#include "pcm_startup.h"
}

#include <Diretta/Sync>
#include <Diretta/Stream>
#include <Diretta/Format>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace scream_diretta {

struct SyncTuning {
    // Steady-state ring buffer in milliseconds (preallocated; converted to
    // bytes once we know the format).
    int ring_buffer_ms = 1000;
    // Bytes (in ring units) to accumulate before allowing playback to begin.
    // Expressed as ms; converted using the active format's bytes_per_second.
    int prefill_ms = 500;
    // After an underrun we hold silence until this fraction of the ring is
    // filled. 0 disables rebuffering (let underruns be handled per-buffer).
    float rebuffer_percent = 0.5f;
    //  absolute rebuffer target in ms after an underrun. When >0 this
    // OVERRIDES rebuffer_percent during the underrun recovery hold so a
    // small transient hiccup does not force a refill to half the ring (~500
    // ms at default ring_buffer_ms=1000). 0 disables the override and falls
    // back to rebuffer_percent. Range conceptually 0..ring_buffer_ms.
    int underrun_rebuffer_ms = 0;
    //  minimum queue fill (ms) required before getNewStream outputs
    // real PCM after a fresh Sync open. Defaults to prefill_ms. Allows
    // overriding the startup gate threshold independently of steady-state
    // prefill semantics. Effective threshold = max(prefill_ms,
    // startup_queue_ms). 0 means "fall back to prefill_ms only".
    int startup_queue_ms = 0;
    // Compatibility: forced silent-warmup window (ms). After a
    // fresh Sync open the SDK send thread will output zero PCM (real
    // Diretta pull cycles, not a preload) for this duration BEFORE the
    // prefill/queue gate is allowed to open. Default is 0; the current
    // startup path defers the Sync open itself
    // (diretta.cpp open-fill gate) instead of muting after open. Kept
    // for CLI back-compat; not recommended.
    int startup_mute_ms = 0;
    //  post-play "real delay" window (ms). After the prefill gate
    // would release, getNewStream() emits silence for this many ms of
    // real pull cycles WITHOUT popping from the ring. The head of the
    // track waits intact -- audio is delayed, not dropped. Defaults to 0
    // (disabled). Diagnostic for target/DAC stabilization after play.
    int startup_real_delay_ms = 0;
};

// Inherits directly from DIRETTA::Sync (not SyncBuffer). The SDK's TestSync
// sample (DirettaHostSDK_149/SinHost/SinHost.cpp) demonstrates that pure
// callback / pull mode is implemented by subclassing Sync, overriding
// getNewStream(), and using Sync::connect(int) — not SyncBuffer::connect().
// SyncBuffer's BufferWorker is for push-mode setStream() flows only.
class ScreamDirettaSync : public DIRETTA::Sync {
public:
    ScreamDirettaSync();
    ~ScreamDirettaSync();

    // Wire the Sync to an externally-owned PCM queue. The queue's lifetime
    // is managed by the caller (DirettaState in diretta.cpp); the Sync just
    // pops from it. Must be called BEFORE configureFormat(); a null ring
    // makes getNewStream() emit silence forever.
    void attachRing(PcmRing* ring) { m_ring = ring; }

    // Real-time priority for the SDK worker thread (getNewStream caller).
    // Applied once on the first call into getNewStream(). -1 = disabled.
    void setRtPriority(int p) { m_rtPriority.store(p, std::memory_order_relaxed); }

    // Lifecycle gate. activate() is called once the Sync is fully open and
    // ready for the SDK send thread to pull. deactivate() is called before
    // tearing the Sync down so getNewStream() can no longer touch the ring
    // (which may be resized / destroyed by the receiver thread).
    //
    // deactivate() is two-phase: it sets m_active=false AND then spins
    // until any in-flight getNewStream() cycle that already passed the
    // first m_active check has exited the guarded section. Without that
    // wait, an SDK pull cycle could still be inside m_ring access while
    // the owner reallocates the ring buffer (UB). Pattern mirrors DRUP's
    // beginReconfigure() / RingAccessGuard.
    void activate()   { m_active.store(true,  std::memory_order_release); }
    void deactivate();

    //  attach the egress PCM dumper. The Sync writes the bytes returned
    // to the SDK (real-PCM cycles only, after gates pass) to this dumper.
    // Pass nullptr to disable. The dumper must outlive the Sync.
    void attachEgressDumper(pcm_dumper_t* dumper) { m_egress_dumper = dumper; }

    //  attach the egress startup analyser + fader. The analyser inspects
    // the first <window> ms of real PCM after each format/open. The fader
    // applies an optional 0..1 ramp to the first <fade> ms IN PLACE, before
    // the egress dumper writes (so the egress WAV captures post-fade PCM).
    // Both pointers may be nullptr (no-op). They must outlive the Sync.
    void attachEgressAnalyzer(pcm_startup_analyzer_t* a) { m_egress_analyzer = a; }
    void attachEgressFader(pcm_startup_fader_t* f)       { m_egress_fader = f; }

    // Set up per-format state (bytes_per_frame, prefill threshold, cycle
    // size). The ring must already have been sized by the caller and
    // attached via attachRing(). Called from the control thread on Sync
    // open (initial or format-change). Not safe to call while the SDK send
    // thread is running.
    void configureFormat(uint32_t sampleRate,
                         uint32_t channels,
                         uint32_t bytesPerSample,
                         const SyncTuning& tuning);

    // Reset prefill / rebuffer state to "starting". Called on reconfigure.
    void resetGate();

    // Stats accessors. Safe to read from any thread (atomic loads).
    uint64_t pushedBytes()  const { return m_ring ? m_ring->pushedBytes() : 0; }
    uint64_t dropBytes()    const { return m_ring ? m_ring->dropBytes() : 0; }
    size_t   ringBytes()    const { return m_ring ? m_ring->capacity() : 0; }
    size_t   ringFill()     const { return m_ring ? m_ring->available() : 0; }
    uint64_t getStreamCount() const { return m_streamCount.load(std::memory_order_acquire); }
    uint64_t silentCycles()   const { return m_silentCycles.load(std::memory_order_acquire); }
    uint64_t realCycles()     const { return m_realCycles.load(std::memory_order_acquire); }
    bool     prefillDone()    const { return m_prefillDone.load(std::memory_order_acquire); }
    bool     rebuffering()    const { return m_rebuffering.load(std::memory_order_acquire); }
    size_t   prefillBytes()   const { return m_prefillBytes.load(std::memory_order_acquire); }
    bool     muteDone()       const { return m_muteDone.load(std::memory_order_acquire); }
    //  bytes consumed (popped) from the ring. Used by stats interval
    // logging to derive drain rate without snooping ring internals.
    uint64_t poppedBytes()   const { return m_poppedBytes.load(std::memory_order_acquire); }
    //  per-Sync underrun and rebuffer accounting. underrunEvents counts
    // distinct underrun episodes (one per begin); rebufferTargetBytes is the
    // effective threshold (max(percent*cap, underrun_rebuffer_ms-bytes))
    // currently armed for the rebuffering hold.
    uint64_t underrunEvents()      const { return m_underrunEvents.load(std::memory_order_acquire); }
    size_t   rebufferTargetBytes() const { return m_rebufferTargetBytes.load(std::memory_order_acquire); }

    // Current ring fill in milliseconds, or 0 if format not yet known.
    uint64_t ringFillMs() const {
        uint32_t bps = m_bytesPerSecond.load(std::memory_order_acquire);
        if (bps == 0 || !m_ring) return 0;
        return (static_cast<uint64_t>(m_ring->available()) * 1000ULL) / bps;
    }

protected:
    // SDK pull-mode callback. Single override needed.
    bool getNewStream(diretta_stream& s) override;

    // SDK info-exchange callback. Intentionally left empty.
    //
    // Diretta Host SDK 149 does NOT invoke statusUpdate() in practice: the
    // SDK<->Target info/feedback exchange (verified at ~10 Hz on the wire,
    // 48B info + 16B feedback packets per --info-cycle) is consumed entirely
    // inside the SDK to drive cycle adaptation / feedback averaging, and the
    // upper-layer hook is never called. Relying on it for telemetry produced
    // a permanently silent log. DRUP overrides this the same way: `{}`.
    //
    // NOTE: SDK 149 exposes NO way to read live Target runtime state from the
    // application layer. statusUpdate() (the only push hook) is dead, and the
    // const getters getCycleTime()/getMinCycleTime()/getCycleSize()/
    // getCyclePackets()/getMode() return the *host-side negotiated send
    // profile* captured at sink-open/re-negotiation -- a static snapshot, not
    // live Target telemetry. They only change on format re-negotiation and
    // are already printed once at open, so polling them adds no information.
    // A '--target-info' polling option was prototyped and then removed for
    // exactly this reason.
    void statusUpdate() override {}

private:
    // Externally owned. Owner (DirettaState) guarantees the pointer remains
    // valid for the Sync's lifetime — it is sized once per format-change
    // and not freed until the Sync is gone.
    PcmRing* m_ring = nullptr;

    // Lifecycle gate. Set to false by deactivate() before the owner tears
    // down or resizes the ring, so getNewStream() cannot touch m_ring
    // while it is being reallocated.
    std::atomic<bool> m_active{false};

    // In-flight getNewStream() cycle counter. Incremented inside
    // getNewStream() AFTER passing the m_active check, decremented when
    // the cycle exits. deactivate() spins on this until it reaches 0
    // before returning, so the owner is guaranteed no SDK pull cycle is
    // still inside m_ring access. Mirrors DRUP's m_ringUsers /
    // RingAccessGuard pattern.
    std::atomic<int> m_ringUsers{0};

    // Real-time priority state. Set by the owner (DirettaState) before the
    // SDK send thread starts pulling. Applied inside getNewStream() on the
    // first few calls so it runs on the SDK worker thread. Retried up to
    // RT_PRIORITY_MAX_ATTEMPTS on failure instead of giving up permanently.
    std::atomic<int>  m_rtPriority{-1};
    std::atomic<bool> m_rtPriorityApplied{false};
    std::atomic<int>  m_rtPriorityAttempts{0};
    static constexpr int RT_PRIORITY_MAX_ATTEMPTS = 5;

    //  egress PCM dumper. Owned by DirettaState in diretta.cpp; the
    // Sync just calls pcm_dumper_write() from getNewStream() when the cycle
    // returns real PCM (silence emitted by Gate 0/1/1.5/2 is NOT written).
    pcm_dumper_t* m_egress_dumper = nullptr;
    uint32_t m_egress_rate = 0;
    uint32_t m_egress_channels = 0;
    uint32_t m_egress_bits = 0;

    // Egress startup analyser + fader. Both externally owned; configured
    // in configureFormat() and consumed on every real-PCM cycle in
    // getNewStream() BEFORE the egress dumper write so the dump reflects
    // any applied fade.
    pcm_startup_analyzer_t* m_egress_analyzer = nullptr;
    pcm_startup_fader_t*    m_egress_fader    = nullptr;

    // Per-format state, written by configureFormat() and read by getNewStream
    // / pushPcm. Atomics so we don't need a mutex on the audio path; format
    // changes happen between play()/stop() so we tolerate a small staleness.
    std::atomic<size_t>   m_streamBytes{0};      // SDK cycle stream size in bytes
    std::atomic<size_t>   m_prefillBytes{0};     // bytes needed before opening gate
    std::atomic<float>    m_rebufferPct{0.5f};
    std::atomic<uint8_t>  m_silenceByte{0};
    std::atomic<uint32_t> m_bytesPerSecond{0};
    std::atomic<uint32_t> m_bytesPerFrame{0};

    // Fractional-frame drift compensation. getCycleSize() is not guaranteed
    // to be a whole multiple of bytesPerFrame. We truncate to an integer
    // number of frames to avoid L/R channel drift, then accumulate the
    // truncated bytes here and add one extra frame whenever the accumulator
    // reaches a full frame. This keeps the long-term average bytes-per-cycle
    // equal to the SDK's intended value without changing the cycle timing.
    std::atomic<size_t> m_cycleRemainder{0};            // bytes truncated per cycle
    std::atomic<size_t> m_cycleRemainderAccumulator{0}; // accumulated truncation

    std::atomic<bool> m_prefillDone{false};
    std::atomic<bool> m_rebuffering{false};

    // Fast-path flag. Once all four startup gates (mute / prefill /
    // real_delay / rebuffer-clear) have passed at least once and a
    // normal real-PCM pop has succeeded, getNewStream() flips this
    // to true and subsequent cycles take a single-load fast path
    // that skips the gate cascade. Gate 3 (underrun) clears it so
    // the slow path can arm rebuffering. resetGate() clears it on
    // every reconfigure. Acquire/release pairs with the gate stores
    // so a reader observing m_steadyState==true also observes all
    // prior gate transitions.
    //
    // Contract: every site that takes the Sync out of normal play —
    // (1) Gate 3 underrun arm,
    // (2) resetGate() on format reconfigure / fresh open,
    // (3) any future SDK offline/online or ring-resize event —
    // MUST go through exitSteadyState(). Direct stores to
    // m_steadyState are forbidden outside this single helper so the
    // exit set stays in one auditable place.
    std::atomic<bool> m_steadyState{false};
    void exitSteadyState() {
        m_steadyState.store(false, std::memory_order_release);
    }

    // Startup mute gate. m_muteBytes is the silent-warmup target in
    // bytes (computed from sample rate and the configured ms). The mute
    // gate sits BEFORE the prefill gate: until m_muteDone is true, every
    // getNewStream cycle outputs silence (no pop from the ring) regardless
    // of ring fill.
    std::atomic<size_t> m_muteBytes{0};
    std::atomic<size_t> m_muteBytesEmitted{0};
    std::atomic<bool>   m_muteDone{true};   // true == no mute requested

    // Persistent buffer the SDK reads from; resized on configureFormat to
    // match getCycleSize() (plus one extra frame for drift compensation)
    // so getNewStream() never allocates.
    std::vector<uint8_t> m_streamData;

    // Stats.
    std::atomic<uint64_t> m_streamCount{0};
    std::atomic<uint64_t> m_silentCycles{0};
    std::atomic<uint64_t> m_realCycles{0};

    // Underrun tracking. m_underrunEvents counts the number of times we
    // transitioned from "ok" to "rebuffering" (i.e. distinct underrun
    // episodes). m_underrunRebufferBytes is the configured absolute target
    // in bytes (from underrun_rebuffer_ms), or 0 to fall back to percent.
    // m_rebufferTargetBytes is whichever target is currently armed.
    std::atomic<uint64_t> m_underrunEvents{0};
    std::atomic<size_t>   m_underrunRebufferBytes{0};
    std::atomic<size_t>   m_rebufferTargetBytes{0};
    // Drain accounting: bytes the SDK has popped from the ring through
    // this Sync. Counts real-PCM pops only (silence cycles do not pop).
    std::atomic<uint64_t> m_poppedBytes{0};

    // startup_real_delay gate. After the prefill gate has released
    // (queue >= prefill_ms), getNewStream() will still emit silence
    // without popping the ring for m_realDelayBytes worth of audio, so
    // the target/DAC can settle on silence before the first real PCM is
    // popped. Queue is NOT consumed during this window. m_realDelayDone
    // is true if the feature is disabled (target == 0).
    std::atomic<size_t>   m_realDelayBytes{0};
    std::atomic<size_t>   m_realDelayEmitted{0};
    std::atomic<bool>     m_realDelayDone{true};
    std::atomic<uint64_t> m_realDelayCycles{0};
public:
    size_t   realDelayBytes()         const { return m_realDelayBytes.load(std::memory_order_acquire); }
    size_t   realDelayBytesEmitted()  const { return m_realDelayEmitted.load(std::memory_order_acquire); }
    bool     realDelayDone()          const { return m_realDelayDone.load(std::memory_order_acquire); }
    uint64_t realDelayCycles()        const { return m_realDelayCycles.load(std::memory_order_acquire); }
};

} // namespace scream_diretta

#endif // SCREAM_DIRETTA_SYNC_H
