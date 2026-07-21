// Implementation of ScreamDirettaSync. See diretta_sync.h for context.
//
//  the ring is externally owned; the Sync only reads from it. The
// prefill / rebuffer / underrun gates are unchanged in shape — they decide
// whether each getNewStream cycle outputs silence or pops from the ring.

#include "diretta_sync.h"
#include "diretta_diag.h"

#include <Diretta/Sync>
#include <Diretta/Stream>

#include <cstring>
#include <iostream>
#include <thread>

extern int verbosity;

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace scream_diretta {

namespace {
// RAII guard that pairs with ScreamDirettaSync::deactivate(). Mirrors
// DRUP's RingAccessGuard / beginReconfigure pattern: increments the
// ring-user counter only after observing m_active==true, then re-checks
// to close the race window where deactivate() flipped m_active=false
// between the first check and the increment. ~Guard releases the
// counter so deactivate()'s spin loop can make progress.
class RingUserGuard {
public:
    RingUserGuard(std::atomic<int>& users, const std::atomic<bool>& active)
        : users_(users), active_(false) {
        if (!active.load(std::memory_order_acquire)) return;
        // acq_rel ensures the increment is visible to deactivate()'s
        // load(acquire) and that we observe a subsequent store of
        // m_active==false.
        users_.fetch_add(1, std::memory_order_acq_rel);
        if (!active.load(std::memory_order_acquire)) {
            // Lost the race: deactivate() set m_active=false either
            // before or concurrently with our increment. Roll back; we
            // never entered the guarded section, so relaxed is fine.
            users_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        active_ = true;
    }

    ~RingUserGuard() {
        if (active_) {
            // release ensures all ring ops complete before the
            // decrement is visible to deactivate().
            users_.fetch_sub(1, std::memory_order_release);
        }
    }

    bool active() const { return active_; }

    RingUserGuard(const RingUserGuard&) = delete;
    RingUserGuard& operator=(const RingUserGuard&) = delete;

private:
    std::atomic<int>& users_;
    bool active_;
};
} // anonymous namespace

ScreamDirettaSync::ScreamDirettaSync() = default;
ScreamDirettaSync::~ScreamDirettaSync() = default;

void ScreamDirettaSync::deactivate() {
    // Two-phase shutdown: flip the gate first so new getNewStream cycles
    // bail at Gate 0, then spin until any cycle that already passed the
    // first m_active check has exited its RingUserGuard. After this
    // returns the owner (DirettaState) is guaranteed no SDK pull is
    // inside m_ring access and may safely resize / free the
    // externally-owned ring buffer.
    //
    // The caller may run under SCHED_FIFO (e.g. the receiver thread with
    // --rt-priority). A pure yield loop can starve the non-RT SDK worker
    // that holds the guard, especially when both are pinned to the same
    // core. We therefore busy-yield briefly and then sleep, giving the
    // holder a chance to run and exit even under priority inversion.
    //
    // Also clear the steady-state fast-path flag: taking the Sync out of
    // normal play means any future getNewStream cycles (if the Sync is
    // later re-activated) must re-run the full gate cascade instead of
    // assuming the previous steady-state invariants still hold.
    exitSteadyState();
    m_active.store(false, std::memory_order_release);
    int spins = 0;
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        if (spins < 1000) {
            std::this_thread::yield();
            ++spins;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void ScreamDirettaSync::configureFormat(uint32_t sampleRate,
                                        uint32_t channels,
                                        uint32_t bytesPerSample,
                                        const SyncTuning& tuning) {
    const size_t bytesPerFrame  = static_cast<size_t>(channels) * bytesPerSample;
    const size_t bytesPerSecond = static_cast<size_t>(sampleRate) * bytesPerFrame;
    if (bytesPerSecond == 0) return;

    m_bytesPerFrame.store(static_cast<uint32_t>(bytesPerFrame), std::memory_order_release);
    m_bytesPerSecond.store(static_cast<uint32_t>(bytesPerSecond), std::memory_order_release);

    //  cache the active format scalars for the egress dumper. Stored
    // plain (not atomic) because configureFormat() runs on the control
    // thread between play()/stop() -- the SDK send thread is paused during
    // reconfigure so a non-atomic write is safe.
    m_egress_rate     = sampleRate;
    m_egress_channels = channels;
    m_egress_bits     = bytesPerSample * 8u;

    //  configure the egress startup analyser + fader for this open.
    // Safe to call regardless of enabled state (the helpers no-op when
    // disabled). The SDK send thread is paused during reconfigure so
    // resetting the running counters here is race-free.
    if (m_egress_analyzer) {
        pcm_startup_analyzer_configure(m_egress_analyzer,
                                       sampleRate, channels, m_egress_bits);
    }
    if (m_egress_fader) {
        pcm_startup_fader_configure(m_egress_fader,
                                    sampleRate, channels, m_egress_bits);
    }

    // Ring sizing happened externally; pick a silence byte for the gate.
    const uint8_t silence = (m_ring ? m_ring->silenceByte() : 0x00);
    m_silenceByte.store(silence, std::memory_order_release);

    // Prefill / startup gate target in bytes. Effective threshold is
    // max(prefill_ms, startup_queue_ms) so callers can override the open-
    // time gate independently of steady-state prefill, but the default
    // (startup_queue_ms == 0) preserves prior behaviour exactly.
    int prefillMs = tuning.prefill_ms;
    if (prefillMs < 0) prefillMs = 0;
    int startupMs = tuning.startup_queue_ms;
    if (startupMs < 0) startupMs = 0;
    int gateMs = (startupMs > prefillMs) ? startupMs : prefillMs;
    int ringMs = tuning.ring_buffer_ms;
    if (ringMs < 50)   ringMs = 50;
    if (ringMs > 5000) ringMs = 5000;
    if (gateMs > ringMs) gateMs = ringMs / 2;
    size_t prefillBytes = (bytesPerSecond * static_cast<size_t>(gateMs)) / 1000;
    if (bytesPerFrame > 0) {
        prefillBytes = (prefillBytes / bytesPerFrame) * bytesPerFrame;
    }
    m_prefillBytes.store(prefillBytes, std::memory_order_release);

    float pct = tuning.rebuffer_percent;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 0.95f) pct = 0.95f;
    m_rebufferPct.store(pct, std::memory_order_release);

    //  absolute underrun-rebuffer target in bytes (from
    // tuning.underrun_rebuffer_ms). Clamped to the ring capacity. 0 means
    // "fall back to rebuffer_percent". Used by Gate 2 in getNewStream().
    int urMs = tuning.underrun_rebuffer_ms;
    if (urMs < 0) urMs = 0;
    size_t urBytes = (bytesPerSecond * static_cast<size_t>(urMs)) / 1000;
    if (bytesPerFrame > 0 && urBytes > 0) {
        urBytes = (urBytes / bytesPerFrame) * bytesPerFrame;
    }
    // Clamp to a bit under the ring so we can actually reach it.
    if (m_ring) {
        size_t cap = m_ring->capacity();
        if (cap > 0 && urBytes > (cap - bytesPerFrame)) {
            urBytes = (cap > bytesPerFrame * 2) ? (cap - bytesPerFrame * 2) : 0;
        }
    }
    m_underrunRebufferBytes.store(urBytes, std::memory_order_release);

    // Forced silent warmup. Convert ms -> bytes and clamp to the ring
    // capacity-equivalent window (no need to clamp further; emitting silent
    // cycles is a no-op on the queue). bytesPerFrame alignment matters only
    // for the emit counter -- the silence pattern itself is a byte fill.
    int muteMs = tuning.startup_mute_ms;
    if (muteMs < 0)    muteMs = 0;
    if (muteMs > 2000) muteMs = 2000;
    size_t muteBytes = (bytesPerSecond * static_cast<size_t>(muteMs)) / 1000;
    if (bytesPerFrame > 0) {
        muteBytes = (muteBytes / bytesPerFrame) * bytesPerFrame;
    }
    m_muteBytes.store(muteBytes, std::memory_order_release);

    // startup_real_delay window: silence cycles AFTER the prefill gate
    // releases, without consuming the queue. Convert ms -> bytes aligned to
    // frame size.
    int realDelayMs = tuning.startup_real_delay_ms;
    if (realDelayMs < 0)    realDelayMs = 0;
    if (realDelayMs > 5000) realDelayMs = 5000;
    size_t realDelayBytes = (bytesPerSecond * static_cast<size_t>(realDelayMs)) / 1000;
    if (bytesPerFrame > 0) {
        realDelayBytes = (realDelayBytes / bytesPerFrame) * bytesPerFrame;
    }
    m_realDelayBytes.store(realDelayBytes, std::memory_order_release);

    // Cycle size from the SDK is the number of bytes the send thread will ask
    // for each call. getCycleSize() is valid as soon as setSinkConfigure()
    // and a transfer-mode (configTransferAuto/etc.) have been applied -- the
    // caller orders configureFormat() after those steps.
    size_t raw_cycle = getCycleSize();
    // Defensive frame alignment: SDK normally returns frame-aligned cycle sizes
    // (cycle_us * bytesPerSec / 1e6 is naturally aligned for integer cycle_us
    // and frame-aligned formats), but align here anyway for consistency with
    // every other threshold above and to harden against any future SDK change.
    // The producer (diretta_output_send) only ever pushes whole frames, so the
    // consumer must also pop in whole-frame multiples to avoid L/R drift.
    size_t cycle = raw_cycle;
    if (bytesPerFrame > 0 && raw_cycle > 0) {
        cycle = (raw_cycle / bytesPerFrame) * bytesPerFrame;
    }
    if (cycle == 0) {
        // Fallback to ~1ms worth of audio rounded to a frame.
        cycle = bytesPerSecond / 1000;
        if (bytesPerFrame > 0) {
            cycle = (cycle / bytesPerFrame) * bytesPerFrame;
        }
        if (cycle == 0) cycle = bytesPerFrame;
        raw_cycle = cycle;   // fallback is already frame-aligned
    }

    // Fractional-frame drift compensation. getCycleSize() may be slightly
    // larger than an integer number of frames (e.g. 44.1k over standard MTU).
    // We truncate the cycle to whole frames above, but remember how many
    // bytes were truncated and add an extra frame every time the accumulated
    // truncation reaches a full frame. This keeps the long-term average
    // bytes-per-cycle equal to the SDK's intended value.
    size_t remainder = 0;
    if (bytesPerFrame > 0 && raw_cycle > cycle) {
        remainder = raw_cycle - cycle;
    }
    m_cycleRemainder.store(remainder, std::memory_order_release);
    m_cycleRemainderAccumulator.store(0, std::memory_order_release);
    m_streamBytes.store(cycle, std::memory_order_release);

    // Preallocate one extra frame so the occasional +1-frame compensation
    // cycle fits without a heap allocation on the audio hot path.
    const size_t streamDataSize = cycle + (bytesPerFrame > 0 ? bytesPerFrame : 0);
    m_streamData.assign(streamDataSize, silence);

    resetGate();
}

void ScreamDirettaSync::resetGate() {
    m_prefillDone.store(false, std::memory_order_release);
    m_rebuffering.store(false, std::memory_order_release);
    exitSteadyState();   // reconfigure / fresh open: see contract in header
    m_streamCount.store(0, std::memory_order_release);
    m_silentCycles.store(0, std::memory_order_release);
    m_realCycles.store(0, std::memory_order_release);
    m_muteBytesEmitted.store(0, std::memory_order_release);
    const size_t target = m_muteBytes.load(std::memory_order_acquire);
    m_muteDone.store(target == 0, std::memory_order_release);
    m_underrunEvents.store(0, std::memory_order_release);
    m_rebufferTargetBytes.store(0, std::memory_order_release);
    m_poppedBytes.store(0, std::memory_order_release);
    m_realDelayEmitted.store(0, std::memory_order_release);
    m_realDelayCycles.store(0, std::memory_order_release);
    const size_t realDelayTarget = m_realDelayBytes.load(std::memory_order_acquire);
    m_realDelayDone.store(realDelayTarget == 0, std::memory_order_release);
}

bool ScreamDirettaSync::getNewStream(diretta_stream& s) {
    const size_t baseWant = m_streamBytes.load(std::memory_order_acquire);

    // Fractional-frame drift compensation. getCycleSize() is not necessarily
    // a whole multiple of bytesPerFrame; we truncated it to whole frames in
    // configureFormat() to avoid L/R drift. Accumulate the truncated bytes and
    // add one extra frame whenever the accumulator reaches a full frame. This
    // keeps the long-term average bytes-per-cycle equal to the SDK's intended
    // value without changing cycle timing. The extra frame fits in m_streamData
    // because configureFormat() preallocated one additional frame.
    size_t want = baseWant;
    const size_t bpf = m_bytesPerFrame.load(std::memory_order_relaxed);
    const size_t remainder = m_cycleRemainder.load(std::memory_order_relaxed);
    if (bpf > 0 && remainder > 0) {
        size_t acc = m_cycleRemainderAccumulator.load(std::memory_order_relaxed);
        acc += remainder;
        if (acc >= bpf) {
            acc -= bpf;
            want += bpf;   // consume one extra frame this cycle
        }
        m_cycleRemainderAccumulator.store(acc, std::memory_order_relaxed);
    }

    // Gate 0: if the Sync is being torn down, emit silence without touching
    // the externally-owned ring (which may be resizing or destroyed).
    //
    // RingUserGuard pairs with deactivate(): once it returns active(),
    // deactivate() is guaranteed to wait for our destructor before
    // letting the owner resize/free m_ring. The two-phase check inside
    // the guard closes the race where deactivate() flips m_active=false
    // between our load and our increment.
    RingUserGuard ringGuard(m_ringUsers, m_active);
    if (!ringGuard.active()) {
        if (want > 0) {
            if (m_streamData.size() < want) {
                m_streamData.assign(want, m_silenceByte.load(std::memory_order_acquire));
            } else {
                std::memset(m_streamData.data(), m_silenceByte.load(std::memory_order_acquire), want);
            }
            s.Data.P = m_streamData.data();
            s.Size   = want;
        } else {
            s.Data.P = nullptr;
            s.Size = 0;
        }
        return true;
    }

    // Apply SCHED_FIFO once on the first real call, running on the SDK
    // worker thread so the priority affects the actual audio pull path.
    // Retry a few times on failure instead of giving up permanently,
    // because the failure is often transient (e.g. EPERM before caps are
    // applied, or the thread inheriting the wrong policy at creation).
    if (__builtin_expect(!m_rtPriorityApplied.load(std::memory_order_acquire), 0)) {
        const int prio = m_rtPriority.load(std::memory_order_relaxed);
        if (prio >= 1) {
#if defined(__linux__)
            struct sched_param param;
            param.sched_priority = prio;
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0) {
                std::cout << "[diretta] SDK worker set to SCHED_FIFO priority "
                          << prio << std::endl;
                m_rtPriorityApplied.store(true, std::memory_order_release);
            } else {
                const int attempts = m_rtPriorityAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
                std::cerr << "[diretta] WARNING: Failed to set SDK worker "
                          << "SCHED_FIFO priority " << prio
                          << " (errno=" << errno << ") attempt " << attempts << "/"
                          << RT_PRIORITY_MAX_ATTEMPTS << std::endl;
                if (attempts >= RT_PRIORITY_MAX_ATTEMPTS) {
                    m_rtPriorityApplied.store(true, std::memory_order_release);
                }
            }
#else
            m_rtPriorityApplied.store(true, std::memory_order_release);
#endif
        } else {
            m_rtPriorityApplied.store(true, std::memory_order_release);
        }
    }

    if (__builtin_expect(want == 0 || !m_ring, 0)) {
        // Not configured yet; hand back an empty buffer.
        s.Data.P = nullptr;
        s.Size = 0;
        return true;
    }
    if (__builtin_expect(m_streamData.size() < want, 0)) {
        // configureFormat preallocates at least one extra frame so the
        // fractional-frame drift compensation cycle fits; in steady state
        // this branch is never taken. Only grow, never shrink, so we do not
        // reallocate on cycles that use the base cycle size.
        m_streamData.assign(want, m_silenceByte.load(std::memory_order_acquire));
    }
    s.Data.P = m_streamData.data();
    s.Size   = want;

    uint8_t* dest = m_streamData.data();
    const uint8_t silence = m_silenceByte.load(std::memory_order_acquire);
    m_streamCount.fetch_add(1, std::memory_order_relaxed);

    // Fast path: once all four startup gates have passed at least once
    // and a real-PCM pop has succeeded, m_steadyState is latched true.
    // Until an underrun or a reconfigure clears it, every cycle takes
    // this single-load path and skips the gate cascade entirely.
    //
    // The acquire here pairs with the release at the slow-path tail
    // where m_steadyState is set; observing true also makes all prior
    // gate stores (m_muteDone, m_prefillDone, m_realDelayDone,
    // m_rebuffering=false) visible to this thread, so the gate cascade
    // is provably redundant on this path.
    //
    // Underrun handling stays here: if the ring cannot satisfy a full
    // cycle, drop the flag and fall through to the slow path so Gate 3
    // can arm rebuffering exactly as before.
    if (__builtin_expect(m_steadyState.load(std::memory_order_acquire), 1)) {
        if (__builtin_expect(m_ring->available() >= want, 1)) {
            m_ring->popOrSilence(dest, want);
            m_poppedBytes.fetch_add(want, std::memory_order_relaxed);
            m_realCycles.fetch_add(1, std::memory_order_relaxed);
            // Egress diagnostics: same single-point gate as the slow
            // path. DCE'd to nothing in NO_DIAGNOSTICS builds.
            if (__builtin_expect(diretta_diag_armed(), 0)) {
                if (m_egress_analyzer && !pcm_startup_analyzer_done(m_egress_analyzer)) {
                    pcm_startup_analyzer_feed(m_egress_analyzer, dest, want);
                }
                if (m_egress_fader && !pcm_startup_fader_done(m_egress_fader)) {
                    pcm_startup_fader_apply(m_egress_fader, dest, want);
                }
                if (m_egress_dumper && pcm_dumper_enabled(m_egress_dumper)) {
                    if (pcm_dumper_open_or_rotate(m_egress_dumper,
                                                  m_egress_rate, m_egress_channels,
                                                  m_egress_bits)) {
                        pcm_dumper_write(m_egress_dumper, dest, want);
                    }
                }
            }
            return true;
        }
        // Steady-state underrun: leave the flag clear so subsequent
        // cycles re-enter the slow path until rebuffering completes.
        // release inside the helper is paired with the slow-path
        // acquire of the gate flags it is about to consult.
        exitSteadyState();   // Gate 3 underrun arm: see contract in header
        // fall through to Gate 3
    }

    // Gate 0: forced silent warmup. Until we have emitted muteBytes
    // worth of zero PCM through real Diretta pull cycles, every cycle is
    // silent and does NOT pop the ring. This sits BEFORE the prefill gate
    // so even if the queue is already huge at open() time (e.g. ~1.2s
    // accumulated during the format-change cooldown), the target/DAC sees
    // genuine silent cycles first. Click mitigation that the fill-only
    // gate cannot provide.
    if (__builtin_expect(!m_muteDone.load(std::memory_order_acquire), 0)) {
        const size_t target = m_muteBytes.load(std::memory_order_acquire);
        std::memset(dest, silence, want);
        m_silentCycles.fetch_add(1, std::memory_order_relaxed);
        if (target == 0) {
            m_muteDone.store(true, std::memory_order_release);
        } else {
            size_t emitted = m_muteBytesEmitted.fetch_add(want, std::memory_order_relaxed) + want;
            if (emitted >= target) {
                m_muteDone.store(true, std::memory_order_release);
            }
        }
        return true;
    }

    // Gate 1: still in startup priming. The queue is filling but has not
    // reached the configured threshold yet. Emit silence WITHOUT popping
    // from the ring so the head of the track survives. This is what makes
    // the unified-queue design work: the same queue carries the head
    // of the track through cooldown/open/handshake, and the SDK simply
    // outputs silence until enough is buffered to start playback.
    if (__builtin_expect(!m_prefillDone.load(std::memory_order_acquire), 0)) {
        size_t target = m_prefillBytes.load(std::memory_order_acquire);
        if (target == 0 || m_ring->available() >= target) {
            m_prefillDone.store(true, std::memory_order_release);
        } else {
            std::memset(dest, silence, want);
            m_silentCycles.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Gate 1.5: startup_real_delay. After the prefill gate releases
    // (queue has enough audio), still emit silence for a configurable
    // window WITHOUT popping from the ring. The head of the track waits
    // intact; the target/DAC sees silence cycles to settle on. This
    // differs from the startup_mute (Gate 0) which sits before the
    // prefill gate; here we know the queue is primed and just delay the
    // first real PCM byte deterministically.
    if (__builtin_expect(!m_realDelayDone.load(std::memory_order_acquire), 0)) {
        const size_t target = m_realDelayBytes.load(std::memory_order_acquire);
        if (target == 0) {
            m_realDelayDone.store(true, std::memory_order_release);
        } else {
            std::memset(dest, silence, want);
            m_silentCycles.fetch_add(1, std::memory_order_relaxed);
            m_realDelayCycles.fetch_add(1, std::memory_order_relaxed);
            const size_t emitted = m_realDelayEmitted.fetch_add(want, std::memory_order_relaxed) + want;
            if (emitted >= target) {
                m_realDelayDone.store(true, std::memory_order_release);
            }
            return true;
        }
    }

    // Gate 2: rebuffering after a sustained underrun. Hold silence until
    // queue fill recovers to the armed target ( prefer the absolute
    // underrun_rebuffer_ms target if configured, else rebuffer_percent).
    if (__builtin_expect(m_rebuffering.load(std::memory_order_acquire), 0)) {
        const size_t threshold = m_rebufferTargetBytes.load(std::memory_order_acquire);
        if (m_ring->available() >= threshold) {
            m_rebuffering.store(false, std::memory_order_release);
            // Fall through to a normal pop.
        } else {
            std::memset(dest, silence, want);
            m_silentCycles.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Gate 3: arm rebuffering if a single cycle cannot be satisfied.
    // pick the rebuffer target. Prefer the absolute byte target derived
    // from underrun_rebuffer_ms (smaller, faster recovery for transient
    // hiccups); else use the rebuffer_percent of capacity. We arm
    // the gate even when both knobs evaluate to 0 bytes so the underrun
    // event count stays meaningful -- but in that case we recover next
    // cycle (threshold == 0 always satisfied).
    if (__builtin_expect(m_ring->available() < want, 0)) {
        size_t target = m_underrunRebufferBytes.load(std::memory_order_acquire);
        if (target == 0) {
            const float pct = m_rebufferPct.load(std::memory_order_acquire);
            target = static_cast<size_t>(m_ring->capacity() * pct);
        }
        m_rebufferTargetBytes.store(target, std::memory_order_release);
        m_rebuffering.store(true, std::memory_order_release);
        m_underrunEvents.fetch_add(1, std::memory_order_relaxed);
        const size_t avail_before = m_ring->available();
        m_ring->popOrSilence(dest, want);
        if (avail_before > 0) {
            m_poppedBytes.fetch_add(avail_before, std::memory_order_relaxed);
            // Egress diagnostics: analyser, fader, dumper. All three are
            // gated by the single diretta_diag_armed() flag. In
            // SCREAM2DIRETTA_NO_DIAGNOSTICS builds the accessor folds to
            // constant 0 and the entire block is DCE'd, so the SDK send
            // thread sees zero diagnostic instructions per cycle.
            if (__builtin_expect(diretta_diag_armed(), 0)) {
                //  feed the egress analyser (real PCM only). Runs BEFORE
                // any fade so the analyser observes raw queue contents.
                if (m_egress_analyzer && !pcm_startup_analyzer_done(m_egress_analyzer)) {
                    pcm_startup_analyzer_feed(m_egress_analyzer, dest, avail_before);
                }
                //  apply the egress fade IN PLACE before dumping, so the
                // egress WAV reflects what the SDK actually receives.
                if (m_egress_fader && !pcm_startup_fader_done(m_egress_fader)) {
                    pcm_startup_fader_apply(m_egress_fader, dest, avail_before);
                }
                //  dump only the bytes that were actually real PCM (the
                // part popOrSilence padded with silence at the tail does not
                // count as egress real PCM).
                if (m_egress_dumper && pcm_dumper_enabled(m_egress_dumper)) {
                    if (pcm_dumper_open_or_rotate(m_egress_dumper,
                                                  m_egress_rate, m_egress_channels,
                                                  m_egress_bits)) {
                        pcm_dumper_write(m_egress_dumper, dest, avail_before);
                    }
                }
            }
        }
        m_realCycles.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    m_ring->popOrSilence(dest, want);
    m_poppedBytes.fetch_add(want, std::memory_order_relaxed);
    m_realCycles.fetch_add(1, std::memory_order_relaxed);
    // Latch steady-state. Reaching this point proves: m_active=true,
    // m_muteDone=true, m_prefillDone=true, m_realDelayDone=true,
    // m_rebuffering=false, AND ring->available() >= want. The release
    // pairs with the fast-path acquire and makes those gate states
    // visible to the next cycle's reader (this same thread, but the
    // pairing is still required by the C++ memory model).
    m_steadyState.store(true, std::memory_order_release);
    // Egress diagnostics: analyser, fader, dumper. Same single-point
    // gate as the rebuffering branch above; DCE'd in NO_DIAGNOSTICS.
    if (__builtin_expect(diretta_diag_armed(), 0)) {
        //  feed the egress analyser BEFORE the fade so we see raw queue
        // contents, then apply the fade IN PLACE so dump + SDK see post-fade.
        if (m_egress_analyzer && !pcm_startup_analyzer_done(m_egress_analyzer)) {
            pcm_startup_analyzer_feed(m_egress_analyzer, dest, want);
        }
        if (m_egress_fader && !pcm_startup_fader_done(m_egress_fader)) {
            pcm_startup_fader_apply(m_egress_fader, dest, want);
        }
        //  dump the full cycle as real egress PCM. We reached this branch
        // only when ring->available() >= want, so the bytes are guaranteed
        // real.
        if (m_egress_dumper && pcm_dumper_enabled(m_egress_dumper)) {
            if (pcm_dumper_open_or_rotate(m_egress_dumper,
                                          m_egress_rate, m_egress_channels,
                                          m_egress_bits)) {
                pcm_dumper_write(m_egress_dumper, dest, want);
            }
        }
    }
    return true;
}

} // namespace scream_diretta
