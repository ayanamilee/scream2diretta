# DRUP vs s2d: Ring Buffer Data Entry and Optimization Analysis

**Date**: 2026-06 (consolidated from technical discussion)  
**Context**: Comparison between scream2diretta (s2d) and DirettaRendererUPnP (DRUP) on how PCM data enters their respective ring buffers, DRUP-specific optimizations for plain PCM entry, internal implementation details, and what s2d can reasonably borrow.

This document consolidates analysis for future refactoring/optimization decisions in s2d. It focuses on the "PCM entering ring" path, excluding decoding, bit-depth conversions, DSD handling, and SIMD/AVX optimizations unless they directly affect the entry mechanism.

---

## 1. Data Entry Paths: High-Level Comparison

### s2d (scream2diretta)
- **Ring Name**: `PcmRing` (unified PCM queue, `src/diretta_ring.h`)
- **Producer**: Scream UDP receiver thread (in `src/scream.c` → `diretta_output_send()` in `src/diretta.cpp`)
- **Entry Point**: `g_st.queue.pushFrames(data, bytes, bpf)` (after partial frame carry handling and per-packet format detection from Scream header bytes 0-3)
- **Characteristics**:
  - Input is "raw-ish" Scream packets (network-driven, potentially variable per-packet format).
  - Strong emphasis on frame alignment (`pushFrames` guarantees multiples of `bytesPerFrame` or drops to fit).
  - Ring lives across format changes (unified queue design) to avoid losing head-of-track data during reconfigure.
  - Direct push of final-format PCM in most cases (minimal conversion at this layer).

### DRUP (DirettaRendererUPnP)
- **Ring Name**: `DirettaRingBuffer` (lock-free SPSC ring with format conversion support, `src/DirettaRingBuffer.h`; also a variant in `src/sync/`)
- **Producer**: AudioEngine decode thread (FFmpeg-based, via registered `AudioCallback`)
- **Entry Point**: `DirettaRenderer` callback → `m_direttaSync->sendAudio(buffer.data(), samples)` → `DirettaSync::sendAudio()` (with `RingAccessGuard`) → `m_ringBuffer.push*()` (plain `push()` for direct PCM)
- **Characteristics**:
  - Input is decoded audio from FFmpeg (per-track stable format).
  - `sendAudio` acts as a dispatcher based on cached format flags (DSD vs PCM, 24-bit packing, 16→32 upsample, etc.).
  - For plain PCM (no conversion): calls `m_ringBuffer.push(data, totalBytes)`.
  - Ring is more tightly coupled to current `DirettaSync` instance/format (reconfigured on track/format change).
  - Heavy internal support for on-the-fly conversions inside various `pushXXX()` methods.

**Key Architectural Difference**:
- s2d: Network packet → per-packet detection + partial carry → direct frame-aligned push. Format changes are "normal" and handled asynchronously.
- DRUP: Decoded stream (stable per track) → `sendAudio` dispatcher → ring push (with conversion if needed). Format changes are track-oriented.

---

## 2. DRUP Optimizations in the Plain PCM Entry Path (Excluding Conversions)

Focusing strictly on the "plain PCM data enters ring" path (i.e., the direct `push()` call after dispatch):

1. **Direct Contiguous Write Fast-Path (Highest-Value Optimization)**
   - In `DirettaRingBuffer::push(const uint8_t* data, size_t len)`:
     ```cpp
     // Fast path: try direct write (no wraparound)
     uint8_t* region; size_t available;
     if (getDirectWriteRegion(len, region, available)) {
         memcpy_audio(region, data, len);
         commitDirectWrite(len);
         return len;
     }
     // Slow path: handle wraparound with two-chunk memcpy
     ```
   - `getDirectWriteRegion()` computes contiguous space from write pos (to end or to read pos) and returns a direct pointer if `contiguous >= needed`.
   - `commitDirectWrite()` simply advances `writePos_`.
   - **Benefit**: In the common non-wrapping case (large buffer, not near full), avoids repeated chunk calculations and potential second memcpy. Uses one linear write.
   - This is exposed as a public API for potential zero-copy upstream writes.

2. **Generation Counter for Config Caching (at sendAudio Layer)**
   - In `DirettaSync::sendAudio()` (before calling ring push):
     ```cpp
     uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
     if (gen != m_cachedFormatGen) {
         // Reload all format flags from atomics (dsdMode, pack24bit, upsample..., channels, bytesPerSample, etc.)
         m_cached... = ...
         m_cachedFormatGen = gen;
     }
     // Then use *cached* values (near-zero atomic loads in steady state)
     if (...) ... else { written = m_ringBuffer.push(data, totalBytes); }
     ```
   - **Benefit**: For stable per-track formats, the hot path in `sendAudio` (the entry to the ring) avoids 5-7 atomic loads per call. Only one generation check.
   - Plain PCM path benefits indirectly (cheaper dispatch before the actual `push()`).

3. **Other Supporting Details in Plain Path**
   - Consistent power-of-2 size + `& mask_` for all position math.
   - `writePos_` / `readPos_` are `alignas(64)` (cache-line separated).
   - Uses `memcpy_audio()` (fast path, potentially SIMD-accelerated) even for plain copies.
   - Some paths include `prefetch_audio_buffer()`.
   - Exposed direct-write API allows callers to potentially bypass even the internal `push()` copy in the future.

**Note**: These optimizations are tuned for DRUP's "stable per-track decoded input" model. The generation counter shines because format rarely changes mid-track.

---

## 3. Internal Implementation Details

### Write Pointer + Mask Implementation (DirettaRingBuffer)
Standard power-of-2 ring buffer design for speed:

- On `resize()`: `size_ = roundUpPow2(newSize); mask_ = size_ - 1;`
- Write: `writePos_.store( (wp + len) & mask_ , std::memory_order_release );`
- Available: `return (wp - rp) & mask_;`
- Free space calculations use similar masked arithmetic.
- **Why?** `& mask_` is dramatically faster than `% size_` (especially on variable sizes or older/embedded CPUs). Wrap handling is cheap.
- Positions are cache-line aligned to prevent false sharing between producer (write) and consumer (read) threads.
- Similar (but not identical) implementation exists in s2d's `PcmRing` (`& m_mask`, separate `alignas(64)` atomics for producer/consumer stats).

### RingAccessGuard Concurrency Protection
RAII guard to safely access the ring while allowing reconfiguration (format change, resize, clear, etc.) without races or UAF.

**Core Logic (two-phase check to close race window)**:
```cpp
class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) return;  // Phase 1: early bail
        users_.fetch_add(1, std::memory_order_acq_rel);             // Phase 2: increment
        if (reconfiguring.load(std::memory_order_acquire)) {        // Phase 3: re-check (critical!)
            users_.fetch_sub(1, std::memory_order_relaxed);         // Bail-out: never entered
            return;
        }
        active_ = true;
    }
    ~RingAccessGuard() {
        if (active_) users_.fetch_sub(1, std::memory_order_release);
    }
    bool active() const { return active_; }
};
```

**Reconfigure Side (typical pattern)**:
- Set `reconfiguring = true`
- Spin/wait: `while (m_ringUsers.load(acquire) > 0) yield();`
- Safely mutate ring (resize, clear, etc.)
- Set `reconfiguring = false`

**Why the two-phase check?** Classic check-then-act race:
- Thread A sees `!reconfiguring`
- Thread B starts reconfig and sets flag
- Thread A increments `users`
- B thinks "no users" and mutates ring → UB

The post-increment re-check + relaxed bail-out closes the window. `acq_rel` on increment ensures visibility; `release` on decrement ensures prior ring ops are visible before the count drops.

**s2d Equivalent**: `RingUserGuard` (in `src/diretta_sync.cpp`) is nearly identical in spirit and code (two-phase check, acq_rel/release ordering, bail-out with relaxed). The comment explicitly says it "Mirrors DRUP's RingAccessGuard / beginReconfigure pattern." s2d places the guard primarily on the consumer side (`getNewStream`) because the producer (receiver) controls ring lifetime and must continue pushing during cooldown.

---

## 4. Borrowable Optimizations for s2d (Value Assessment)

**High-Value / Recommended for s2d**:
- **Direct contiguous write fast-path in ring push**: Add to `PcmRing::push` / `pushFrames` (early `getDirectWriteRegion`-style check for single memcpy when no wrap). High ROI for receiver hot path (wraps are infrequent with large buffers). Low code change risk. s2d's current `pushFrames` always does chunk math + potential two-memcpy.
- **Generation counter for stable per-format state**: Introduce in the push path or DirettaState for things that only change on reconfigure (e.g., current bpf, silence byte, certain thresholds). Snapshot once per generation; hot path uses cache. Medium-high value if more state is added later.
- **Expose direct-write API**: Public `getDirectWriteRegion` + `commitDirectWrite` as an extension point (for potential future zero-copy scenarios).

**Medium / Situational**:
- More aggressive prefetch or fast-memcpy wrappers in plain push (s2d already uses `std::memcpy` in ring; could align with DRUP's `memcpy_audio`).

**Low Value or Already Present**:
- Power-of-2 + mask and cache-line separation: Already implemented in s2d's `PcmRing` (see `& m_mask`, `alignas(64)` producer/consumer atomics).
- RingAccessGuard / two-phase guard pattern: Already adopted 1:1 as `RingUserGuard` (with s2d-specific adjustments for its async producer-driven reconfig model).

---

## 5. Architecture Differences That Limit Borrowing

These make certain DRUP techniques hard to apply directly or low-value in s2d:

- **Input Model**:
  - DRUP: Decoded audio from AudioEngine (per-track stable format). `sendAudio` can safely assume format constancy for many calls → generation counter for flags shines.
  - s2d: Scream UDP packets (header bytes 0-3 can signal format change per packet). Requires per-packet detection + partial carry logic. Format changes are a designed-in "normal" case handled via async reconfigure.
  - **Impact**: DRUP's heavy format-flag caching in the entry dispatcher would be incorrect or overly complex in s2d. s2d's "unified queue + per-packet handling" already solves the equivalent problem differently.

- **Reconfigure / Ring Lifetime Model**:
  - s2d: Async reconfigure (separate thread). Receiver (producer) must *continue pushing* during cooldown (to pre-fill new format's ring). Ring is "unified" and outlives individual `Sync` instances. Guard is deliberately consumer-side only (`RingUserGuard` in `getNewStream`); producer controls lifetime via `deactivate()`.
  - DRUP: More track-oriented reconfig. Guards on both producer (`sendAudio`) and consumer sides to drain users before mutating ring.
  - **Impact**: Symmetric "guard on producer side for drain" does not fit s2d's "receiver must keep pushing" requirement. s2d's `RingUserGuard` + `deactivate` + bounded disconnect is already the adapted equivalent.

- **Conversion vs Plain Path**:
  - DRUP: Many `push*` methods exist for format conversions (24-bit packing, 16→32, DSD planar). Direct-write API helps avoid extra staging copies *inside conversions*.
  - s2d: Plain PCM push (data is already normalized to ring format upstream). No equivalent conversion layer at ring entry.
  - **Impact**: Direct-write benefits are smaller for s2d's plain path (though still useful for the non-wrap case).

- **Hot Path Characteristics**:
  - s2d receiver push: Extremely latency-sensitive, pinned core, UDP-driven, must handle partial frames.
  - DRUP sendAudio: From decode thread, more "media player" pacing.
  - **Impact**: Optimizations assuming stable long-running formats or callback-driven flow don't map 1:1.

**Summary of Fit**:
s2d's core strengths (minimal per-packet input model + async reconfig without data loss + producer-controlled ring lifetime) mean it does **not** need (and cannot safely adopt) DRUP's "assume format stable across many pushes" or "block producer on reconfig" patterns. The best opportunities are localized ring-internal improvements (fast contiguous path) and state-caching patterns that respect s2d's per-packet reality.

---

## 6. Recommendations & Next Steps (Revised after detailed entry + consumer analysis)

**Revised Priority for s2d (after atomic/branch/dispatch/lifecycle comparison on both producer entry and consumer exit paths)**:

The initial "high ROI" items for the ring entry (Direct contiguous fast-path via getDirectWriteRegion + Generation counter) were based on surface similarity. After precise accounting (DRUP fast-path push actually performs getFreeSpace + getDirectWriteRegion + commit load, i.e. more acquires than s2d's single freeSpace + (likely CSE'd) wp load; the "saved" branch is highly predictable and near-zero cost on modern CPUs; s2d format dispatch happens upstream via non-atomic Scream header bytes + plain bpf param passed to pushFrames, with no equivalent 7-atomic dispatch in the hot push; s2d recreates ScreamDirettaSync per format so consumer scalars are configured once per Sync rather than long-lived) — **these are not recommended as pure ring-layer performance changes**. They would add complexity or atomic traffic for no (or negative) net gain in s2d's model, violating the "Avoid over-engineering" spirit and hot-path rules in CLAUDE.md.

1. **Do not** implement Direct Contiguous Fast-Path or Generation Counter as ring entry (producer) optimizations. s2d's current `pushFrames` (frame-aligned drop-newest + two-chunk with predictable wrap branch + plain param) is already lean and appropriate. See the user's detailed load-count critique and source cross-check for the reasoning.
2. Consumer-side generation counter (DRUP's `m_consumerStateGen` + cached scalars in `getNewStream`) provides value in DRUP's long-lived Sync model, but is lower priority / lower value for s2d (Sync is intentionally short-lived per format; runtime gate state like prefill/rebuffer must be checked atomically every cycle anyway; the unified queue + async Sync replacement is a core strength for no data loss on format change).
3. Leave Ring*Guard (consumer-only in s2d), power-of-2 + mask, cache-line separation, and explicit memory orders as-is — these were correctly borrowed/adapted.
4. Consider borrowing *ideas* (not code) from DRUP's MTU + DSD-multiplier-aware stabilization buffer count calculation (in their post-online gate) to refine s2d's startup tuning, open-gate logic, or --startup-*-ms defaults/auto modes. This is cold-path / config only.
5. The post-pop producer notification pattern (try_lock + cv in DRUP getNewStream) is documented for reference but **not applicable** — s2d's live UDP producer policy is explicitly drop-newest / never stall receiver.
6. Keep the diagnostics model (strictly gated by `diretta_diag_armed()` + dual binaries for DCE, statusUpdate for --info-cycle observability with zero impact on receiver or SDK audio worker hot paths).

**Consumer-side guard adaptation note**: The two-phase RingAccessGuard pattern is mirrored as `RingUserGuard` (exact memory orders, bail-out relaxed). The key correct decision in s2d is placing protection *only on the consumer* (`getNewStream`) because the producer (receiver thread) must continue pushing into the unified queue during format-change cooldown. DRUP guards both sides because their model allows draining the producer.

This document (and the parallel Chinese version) now records both the producer-entry and consumer-exit comparisons for future "后续斟酌如何改善".

---

## 7. Consumer Side: PcmRing pop / getNewStream → Diretta SDK / Target

### High-Level Comparison

**s2d (ring → Target path)**:
- Producer keeps pushing to the *unified* PcmRing even while a Sync is being torn down / new one is opening asynchronously.
- `ScreamDirettaSync::getNewStream(diretta_stream&)` (SDK send thread, pinned via cpu-audio / OCCUPIED) is the consumer.
- First thing: `RingUserGuard` (two-phase) to safely access the externally-owned ring during reconfig.
- Pre-allocate `m_streamData`, set `s.Data.P` / `s.Size` directly (SDK 148 app-managed buffer model).
- Sophisticated multi-gate logic *before* any pop (all using acquire atomics):
  - Gate 0: forced silent warmup (`m_muteDone`, no pop from ring — preserves head).
  - Gate 1: prefill (wait for queue fill, emit silence, *no pop*).
  - Gate 1.5: `startup_real_delay` (configurable no-pop delay after prefill for click mitigation).
  - Gate 2/3: rebuffer arming + underrun (if `available() < want`, arm rebuffer target, call `popOrSilence` which pads + counts).
- Real path: `m_ring->popOrSilence(dest, want)` — two-chunk memcpy (plain `std::memcpy`), possible silence pad + `underrunCount`, release store to rp.
- Update `poppedBytes`, `realCycles` / `silentCycles` (relaxed).
- Egress diags (analyser/fader/dumper) strictly gated by `diretta_diag_armed()` (DCE'd in `scream2diretta` production binary).
- Separate `statusUpdate()` override (called by SDK on info-packet cycles) for live `getCycleTime` / mode / latency capture — completely off the data hot path; used for the `| live-cycle=...` stats suffix (only when different from open-time value, and mainly interesting under TargetProfile).

**DRUP (ring → Target path)**:
- Longer-lived `DirettaSync` + ring across formats.
- `DirettaSync::getNewStream(diretta_stream&)` (similar app-managed buffer for SDK 148).
- `RingAccessGuard` (identical two-phase pattern) — used on *both* producer and consumer.
- Dedicated **consumer generation counter** (`m_consumerStateGen` incremented on reconfigure, alongside producer `m_formatGeneration`):
  - Hot path: single acquire load of gen; only on mismatch reload 6+ atomics (bytesPerBuffer, silenceByte, isDsd, sampleRate, bytesPerFrame, framesPerBufferRemainder for 44.1k-family cycle drift fix) into cached plain values.
- Gates (similar but not identical): silence remaining, stop, prefillComplete, elaborate post-online stabilization (MTU-aware + DSD multiplier scaling of required silence buffer count to achieve consistent wall-time warmup), rebuffering.
- Underrun arms rebuffer.
- `m_ringBuffer.pop(dest, ...)` — always two-chunk with `memcpy_audio` (the audio-optimized wrapper, even on pop).
- After successful pop: `try_lock` + `notify_one` on space-available cv (never blocks the consumer thread; signals producer for flow control / jitter reduction).
- Throttled async logging (`DIRETTA_LOG_ASYNC`).

**Core lifecycle / model difference (consumer side)**:
- s2d deliberately destroys the old `ScreamDirettaSync` on format change and opens a fresh one (async, via worker), while the PcmRing (unified queue) and receiver continue uninterrupted. This enables "head of track survives reconfig" + explicit multi-phase click mitigation (mute + prefill + realDelay) without data loss. RingUserGuard + deactivate() closes the in-flight getNewStream safety window.
- DRUP keeps the Sync object; uses reconfiguring flag + guard drain (beginReconfigure spins until ringUsers==0) + gen bump so the *same* getNewStream can pick up new cached scalars. More emphasis on signaling back to producer and precise stabilization timing.

### Borrowable Points on the Consumer / Exit Path (with assessment)

**Medium / situational value (ideas, not direct port)**:
- Consumer generation counter for stable scalars in getNewStream (DRUP comment calls it "C1: single atomic load vs 5-6 loads").
  - **Assessment for s2d**: Lower ROI than in DRUP. Because s2d replaces the entire Sync object on format change (see `reconfigure()`, `open_sync_for_format()`, async open in `diretta.cpp`), the per-format scalars (`m_streamBytes`, silence) are written once in `configureFormat()` and the object is new. The hot-path checks that happen every cycle (prefillDone, realDelayDone, rebuffering, muteDone, `ring->available()`) are *dynamic gate state* required for the unified-queue design — they cannot be "cached away". A gen snapshot would add an extra atomic + branch per getNewStream for only a few scalar loads that are already cheap. DRUP needs the pattern because their Sync lives long-term.
- MTU + DSD rate scaling for post-online / stabilization silence buffer count (DRUP computes cycleTimeUs from effectiveMTU / bytesPerSec, then buffersNeeded = targetWarmupMs / cycleTimeUs, with DSD multiplier special case).
  - **Assessment**: Useful reference for s2d's startup logic. s2d already has rich CLI controls (`--startup-mute-ms`, `--startup-real-delay-ms`, `--open-gate-*`, rebuffer percent, etc.) and byte-based open gate. Borrowing the *calculation approach* could improve auto-computed defaults or an "adaptive" mode, especially for high-MTU or high-rate DSD. Cold path only — zero impact on pop hot path.
- Post-pop producer notification with try_lock (never block consumer).
  - **Assessment**: Not applicable. s2d's producer is a live Scream UDP source; the explicit policy (PcmRing docs) is drop-newest on full, never stall the receiver thread (would risk kernel socket overflow or added latency on the pinned scream core). DRUP's decode-thread producer can safely wait. The pattern is good engineering for their jitter-reduction work but conflicts with s2d's "PCM transport, not media player, live source" model.

**Low value (same as producer side)**:
- Specialized `memcpy_audio` (and prefetch) even on the pop path.
  - For the small, regular cycle-sized copies performed by the SDK worker, benefit is marginal on modern platforms. s2d's plain `std::memcpy` in `popOrSilence` is simple and sufficient. (Same conclusion as the entry-side analysis.)

**Already correctly done / mirrored**:
- Two-phase reconfig guard on the consumer (`getNewStream`): `RingUserGuard` is a faithful port of DRUP's `RingAccessGuard` (same acquire on flag, acq_rel on add, acquire re-check, relaxed bail sub, release on dec). The s2d comment explicitly says "Mirrors DRUP's...". The *difference* (guard only on consumer, not on push) is the correct adaptation for s2d.
- App-managed stream buffer: both pre-allocate persistent storage (`m_streamData`) and directly set `diretta_stream.Data.P` + `Size` instead of relying on SDK resize in the hot callback. s2d does this explicitly for SDK 148 compatibility.
- "Emit silence without consuming from ring" during prefill / startup phases (core to s2d's unified queue preserving the head of track).
- Atomic stats for real/silent cycles, underruns, popped bytes (exposed via stats; DRUP has analogous counters).
- Strict avoidance of steady-state logging / work in the real-PCM pop path (s2d is stricter than DRUP's throttled async logs; aligns with CLAUDE.md "No logging in steady state").

**Not worth borrowing (architecture conflicts)**:
- Long-lived Sync + gen snapshot across format changes (DRUP's model). s2d's "tear down Sync, keep unified ring alive, open fresh Sync" is a deliberate strength for zero data loss + click mitigation on song/format switches. Porting the full gen + drain machinery would fight this.
- Producer-side RingAccessGuard draining in sendAudio equivalent (already analyzed on entry side — receiver must keep draining UDP).
- Direct-read region / zero-copy pop into the SDK buffer (DRUP never implemented a symmetric "getDirectReadRegion"; pop is always two-chunk. In s2d it would be risky due to external ring ownership, possible resize under guard, wrap handling, and the fact that we still need to hand a stable pointer for the SDK cycle. No compelling zero-copy win on the read side.)
- Blocking or waiting on the consumer thread for any reason (DRUP is careful with try_lock; s2d already has no such waits in the data path).

### Consumer-Side Conclusion

Parallel to the producer (PCM entry) side: after inspecting the actual DRUP `getNewStream` + consumer gen + RingAccessGuard + beginReconfigure + post-pop notify, plus s2d's full gated `getNewStream` + `popOrSilence` + `RingUserGuard` + `statusUpdate`, there is **no very high-ROI "copy this DRUP mechanism into the pop path" win**.

DRUP's consumer generation and stabilization scaling are valuable *in their long-lived Sync + decode-producer + jitter-sensitive media-player context*. s2d's consumer path is intentionally more "gated" (to make the unified queue + async reconfig + head preservation + multi-phase click mitigation work) and the ring pop primitive itself stays deliberately simple.

The comparison's main lasting value is:
- Confirmation that the guard was adapted *correctly* (consumer only).
- Understanding why certain DRUP patterns (both-sides guards, gen on both producer and consumer, producer notification) do not map.
- Validation that s2d's exit path, while different in shape, is coherent with the overall architecture described in CLAUDE.md.

No changes to the hot pop path are recommended on the basis of this comparison.

---

*End of document. For code references, see:*
- s2d: `src/diretta_ring.h` (PcmRing push/pushFrames + popOrSilence), `src/diretta_sync.cpp` (getNewStream with full gates + RingUserGuard + popOrSilence calls + statusUpdate for info-cycle), `src/diretta.cpp` (queue_push_frames + reconfigure + async Sync lifecycle + configureFormat).
- DRUP: `Projects/DirettaRendererUPnP/src/DirettaRingBuffer.h` (push + getDirectWriteRegion + commitDirectWrite + pop + memcpy_audio), `src/DirettaSync.cpp` (sendAudio + m_formatGeneration + getNewStream + m_consumerStateGen + RingAccessGuard on both sides + beginReconfigure/endReconfigure + post-pop notify), `src/DirettaRenderer.cpp` (callback to sendAudio).

---

## 8. Scream Receiver Side (Native UDP Path) — No DRUP Equivalent, Native Improvement Opportunities

This side is 100% s2d-native (the original Scream UDP receiver from the scream project, adapted). There is no DRUP equivalent to borrow from — DRUP receives decoded audio from an AudioEngine/FFmpeg callback into sendAudio, not raw Scream UDP datagrams.

### Current Architecture (from scream.c + network.c)
- Main thread = Scream receiver loop (pinnable via --cpu-scream, optional SCHED_FIFO via --rt-priority).
- Unified thin loop:
  ```c
  while (!g_shutdown_pending) {
      rcv_xxx(&receiver_data);   // network (unicast / multicast) ...
      if (data) {
          if (receiver_tap_any_armed()) { ... tap ... }   // compile-time 0 in NO_DIAGNOSTICS prod binary
          output_send_fn(&data);  // for Diretta: diretta_output_send (the format delta + partial carry + push to PcmRing)
      }
  }
  ```
- `rcv_network` (the common UDP path):
  - select(500ms timeout) for responsive shutdown (deliberate design after EINTR problems).
  - recvfrom into fixed small context buffer (MAX_SO_PACKETSIZE ≈ 1.1 KiB).
  - Optional --allowed-source-ip userspace filter (post-recv, silent drop).
  - Parse 5-byte Scream header (rate_byte, sample_size, channels, channel_map) into receiver_data_t.
  - Deliver **zero-copy pointer**: `audio = &buf[5]`, `audio_size = n-5`. No data copy at receiver layer.
- SO_RCVBUF: default 4 MiB for Diretta output (to absorb bursts/scheduler latency before userspace drains into PcmRing). User-overridable, logged at -v. Kernel may double the value.
- Per-packet: always re-parse header (Scream can signal format change on any packet; necessary for song switches / screamalsa behavior).
- Handoff to "push into PcmRing": the receiver just delivers the datagram. The actual linkage work (per-packet format change detection against last, partial-frame carry to guarantee whole target-bpf frames, DSD transform or bit-depth conversion when sink < source, then queue_push_frames → PcmRing::pushFrames) lives in the Diretta backend (diretta_output_send and helpers in diretta.cpp). This was already analyzed in prior sections as the "entry" path.
- All heavy diagnostics (payload tap, raw-entry tap, startup analyzer, comparator, dumps) are behind a single-point `receiver_tap_any_armed()` gate (plain int set once at init; constant-0 + DCE in production `scream2diretta` binary built with -DSCREAM2DIRETTA_NO_DIAGNOSTICS).
- Shutdown: flag checked before blocking and on EINTR/timeout; no SA_RESTART.

This keeps the receiver backend-agnostic (same delivery works for `-o raw` and `-o diretta`) and the hot path extremely lean.

### Worthwhile Native Optimizations / Improvements (No DRUP to Copy)
Prioritized by value vs. risk to hot-path cleanliness, shutdown correctness, and "no over-engineering" (CLAUDE.md).

**High-ROI, realistic**:
- **recvmmsg batch receive (Linux)** on the network path.
  - Current: one select + one recvfrom per datagram (even when data is backlogged).
  - Proposal: when available, use recvmmsg(..., MSG_DONTWAIT or with timeout) to pull up to 16–64 datagrams per syscall into an array of iovecs, then process the batch in a tight inner loop (same pointer handoff, same gated tap, same output_send per packet).
  - Fallback to the existing single-packet path on non-Linux or error.
  - Why valuable: syscall amortization is the classic win for high-pps UDP receivers. Scream packet rates can be high (high SR + channels, or screamalsa sending frequent updates). Gives the pinned --cpu-scream thread more headroom.
  - Hot path impact: none in steady state (batch loop is the same work); only when data arrives in bursts.
  - Shutdown: check g_shutdown_pending between packets in the batch; the mmsg call itself can be made interruptible.
  - Risk: medium (new code path, need to handle mixed-format in a batch gracefully — rare but possible on transitions). Well-contained.
  - Status: not implemented; would be a nice native improvement.

- **Lightweight, always-on receiver-side observability in --stats**.
  - Add cheap relaxed atomics (or just non-atomic since single producer) for packets_received, bytes_received, perhaps a "last receive ts" for gap detection.
  - Increment in rcv_network on every successful delivery (before or after the tap check).
  - Expose in the existing periodic stats (format_stats_line / maybe_print_periodic_stats) or a new small receiver stats line, alongside the Diretta queue stats (pushed/dropped/underruns/ring fill). Rate-limited.
  - Also surface effective SO_RCVBUF and any observable kernel drops (via getsockopt or simple /proc query at stats time).
  - Why valuable: users (and prior logs in this conversation) care about "UDP buffer situation", correlation of underruns with source gaps vs. ring behavior, packet loss at kernel vs. userspace drops. Currently stats are very Diretta/backend-centric.
  - Cost: 1–2 relaxed ops per packet — same order as the existing pushedBytes etc. in the ring. Safe in both prod and debug binaries.
  - No impact on hot path cleanliness or logs (stats printing is already conditional and throttled).
  - Complements the existing --udp-rcvbuf-bytes knob and ss -m / watch ss usage.

**Medium / situational (only if measured)**:
- Reduce per-packet syscall overhead from the select(500ms) design.
  - Current select is a deliberate tradeoff for shutdown latency when idle (instead of pure blocking recvfrom + relying on EINTR).
  - Alternatives: self-pipe/eventfd wakeup on shutdown (block on recvfrom most of the time, poll on pipe+socket when needed), or ppoll with sigmask, or simply accept a shorter timeout (e.g. 50–100ms) and the tiny idle wakeup cost.
  - When streaming, select usually returns immediately (fd readable), so cost is the extra syscall + fdset setup.
  - Only pursue if profiling the receiver thread shows it as material. Current design is robust and well-commented.

**Low / not worth**:
- Per-packet header re-parse in receiver: 5 byte loads + struct stores. Necessary (format can legitimately change per packet) and trivial cost. Backend re-checks for "effective change" anyway.
- Moving partial carry / frame alignment into the pure receiver: no — it depends on the *target* bytes-per-frame negotiated by the active backend (Diretta sink after setSinkConfigure). Raw backend doesn't need the same guarantee. The carry + push linkage is correctly backend-specific.
- Kernel-level source filtering (BPF/iptables) for --allowed-source-ip: overkill for the normal 1-sender use case; current userspace check after recv is simple and sufficient.
- Any per-packet allocation, logging, or heavy computation in the receive loop: forbidden by design and CLAUDE.md hot-path rules. Already clean.
- Zero-copy from kernel UDP buffer all the way into PcmRing: extremely complex (circular buffer, frame alignment, partial carry, reconfig resize safety, wrap handling) for marginal gain at audio data rates. Not aligned with "keep the receiver clean" goal.

### Conclusion for Scream Receiver Side
The native receiver is intentionally minimal and correct: thin loop, zero-copy delivery of raw datagrams + header, strong shutdown semantics, gated diagnostics, good defaults for feeding the userspace PcmRing (large SO_RCVBUF), and backend-agnostic.

The interesting "push to PCM ring linkage" work (the part that actually interacts with PcmRing) lives one layer down in the Diretta backend and has already been analyzed (in the entry-side sections) as clean for s2d's constraints.

The most attractive native improvements are:
- recvmmsg batching (real reduction in per-packet syscall cost for high-rate cases).
- First-class receive counters and UDP health in the --stats output (directly addresses user observability needs without adding noise or complexity to the default path).

These respect all project rules: hot path stays lean, no new steady-state logs/allocs, diags remain off-by-default, receiver thread never blocks on full ring.

No DRUP patterns apply here — this layer simply does not exist in the DRUP architecture.

(End of receiver-side analysis. The preceding sections 6–7 cover the ring entry and exit in depth.)