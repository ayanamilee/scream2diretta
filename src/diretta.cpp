// Diretta output backend for the Scream UDP receiver.
//
// Current data path:
//   Scream receiver -> receiver_data_t -> diretta_output_send()
//     -> PcmRing -> ScreamDirettaSync::getNewStream() -> Diretta SDK.
//
// This file owns the Diretta backend state, target discovery, format mapping,
// PCM frame alignment, partial-frame carry, PcmRing ingress, and bounded SDK
// open/cleanup lifecycle. Blocking SDK open/cleanup work runs outside the
// receiver hot path so UDP ingress can keep draining into the queue.

extern "C" {
#include "diretta.h"
#include "pcm_dump.h"
#include "pcm_startup.h"
#include "receiver_tap.h"
#include "diretta_diag.h"
}

#include "diretta_sync.h"
#include "diretta_ring.h"

#include <cmath>

#include <Diretta/Find>
#include <Diretta/Sync>
#include <Diretta/Stream>
#include <Diretta/Format>
#include <Diretta/SysLog>
#include <ACQUA/IPAddress>
#include <ACQUA/Clock>
#include <ACQUA/SysLog>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace DIRETTA;
using namespace ACQUA;

namespace {

// Largest PCM frame size we are willing to handle. Largest sane Scream
// frame is 32-bit / 16ch = 64 bytes/frame. 256 gives headroom and stays
// L1-friendly. Allocated once as a member — never on the audio hot path.
constexpr size_t PARTIAL_FRAME_MAX = 256;

// DSD constants (screamalsa sends DSD_U32_BE with sample_size == 1 sentinel).
constexpr uint8_t DSD_SILENCE_BYTE = 0x69;
constexpr int DSD_BUFFER_MS_DEFAULT = 1500;
constexpr int DSD_PREFILL_MS_DEFAULT = 200;

// Maximum Scream packet payload we ever expect on the wire. Even with jumbo
// frames the UDP payload stays well below this; we use a fixed upper bound so
// the DSD and 24-bit transform scratch buffers can be allocated once in init
// and never resized on the audio hot path.
constexpr size_t SCREAM_MAX_PACKET_BYTES = 65536;

struct DirettaState {
    bool initialized = false;
    bool sdk_open = false;

    diretta_config_t cfg{};

    IPAddress sink_addr;
    uint32_t mtu = 0;

    Find* finder = nullptr;
    scream_diretta::ScreamDirettaSync* sync = nullptr;

    // The unified ordered PCM queue. Owned here so it persists across the
    // Sync lifecycle: when a format change tears down the SDK Sync, the
    // queue is rebuilt fresh for the new format; while the new Sync's SDK
    // open is in cooldown / handshake, the receiver continues to push PCM
    // into the queue. When the new Sync opens, configureFormat() points
    // the Sync at the queue and getNewStream() reads from it directly.
    scream_diretta::PcmRing queue;
    bool queue_ready = false;
    uint32_t queue_bpf = 0;

    // Last *effective* PCM format we accepted. The fields that drive
    // Diretta config are compared (sample_rate, sample_size, channels).
    // wire_layout is also compared because for 24-bit PCM it selects between
    // S24_3LE (src_bpf = 3*ch, no pack) and S24_LE (src_bpf = 4*ch, pack to
    // 3-byte). A 3LE<->4LE switch at the same rate/size/channels would
    // otherwise leave src_bytes_per_frame / pcm_needs_pack_24 stale and
    // misalign every frame.
    uint32_t last_sample_rate = 0;
    uint8_t last_sample_size = 0;
    uint8_t last_channels = 0;
    uint8_t last_wire_layout = 0;
    bool have_last_fmt = false;

    bool sink_active = false;
    uint32_t bytes_per_frame = 0;        // destination bytes/frame (queue / SDK)
    uint32_t src_bytes_per_frame = 0;    // source bytes/frame from Scream header
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t bits_per_sample = 0;

    // Source format from Scream header (immutable per reconfigure).
    // The DAC must accept the source bit depth as-is; ingress down-conversion
    // was removed (was rarely correct, almost never triggered, and the SDK /
    // user are better placed to choose the right bit depth upstream).

    // DSD state (screamalsa sample_size == 1).
    bool is_dsd = false;
    uint32_t dsd_multiplier = 0;   // 1=DSD64, 2=DSD128, 4=DSD256, 8=DSD512
    uint32_t dsd_real_rate = 0;    // e.g. 2822400 for DSD64

    // DSD transformation flags. The Scream receiver (network.c)
    // is required to deliver DSD in standard ALSA DSD_U32_BE word-
    // interleaved order; the legacy network path (-L) performs the original
    // byte-interleaved -> word-interleaved conversion if needed. This backend
    // only applies the sink-dependent bit-order / byte-order transforms.
    // ALSA DSD_U32_BE convention places the DSD bit in the MSB (bit 7) of each
    // byte, i.e. MSB-first / big-endian.
    // Atomic because the receiver thread reads these on every DSD packet while
    // the async open worker negotiates/writes them; the receiver must never see
    // a torn or mixed transform.
    std::atomic<bool> dsd_needs_bit_reverse{false};
    std::atomic<bool> dsd_needs_byte_swap{false};
    // Staging fields written by the async open worker, committed to the live
    // globals in finalize_sync_open_on_receiver() (receiver thread only).
    std::atomic<bool> async_open_dsd_needs_bit_reverse{false};
    std::atomic<bool> async_open_dsd_needs_byte_swap{false};
    std::vector<uint8_t> dsd_transform_buffer; // scratch for DSD transform (fixed size, allocated once in init)

    // PCM format conversion state.
    // Diretta SDK has no native S24_LE (4-byte container); screamalsa sends
    // S24_LE when wire_layout == 1, so we pack it to S24_3LE (3-byte) on ingress.
    bool pcm_needs_pack_24 = false;
    std::vector<uint8_t> pcm_pack_buffer; // scratch for S24_LE -> S24_3LE (fixed size, allocated once in init)

    // Cached cycle values from the last successful open, for stats output.
    uint64_t target_cycle_us = 0;
    uint64_t sdk_cycle_us = 0;

    // Inferred protocol overhead (MTU - per-packet payload), learned from
    // the first successful SDK open.  -1 = not yet inferred.
    int inferred_overhead = -1;

    // Last format we successfully built a FormatConfigure for.
    FormatConfigure last_fc{};
    bool have_last_fc = false;

    // Reconnect / connection-loss handling.
    bool   conn_lost_logged = false;
    std::chrono::steady_clock::time_point next_reconnect_at{};
    uint32_t reconnect_backoff_ms = 500;

    // Format-change cooldown gives the target time to release the previous
    // stream before accepting a new FormatConfigure. SDK cleanup runs
    // asynchronously off the audio thread.
    std::chrono::steady_clock::time_point reconfigure_ready_at{};
    bool reconfigure_pending = false;

    // Hard wall-clock deadline for the open gate. Once crossed, the Sync is
    // opened even if queue fill is still below the configured threshold; the
    // Sync-side prefill gate then protects playback start.
    std::chrono::steady_clock::time_point open_gate_deadline_at{};
    // Pre-computed open-gate fill threshold in bytes for the current format.
    // 0 means "no queue gate, open as soon as cooldown elapses".
    size_t open_gate_threshold_bytes = 0;
    // Throttle the -vv "waiting for queue fill" log so we don't flood.
    long long last_open_gate_log_ms = 0;

    // Grace window after a successful open(): suppress sink-lost detection
    // for this duration so the new Sync has a chance to start the SDK send
    // thread before we tear it down on a transient is_connect()==false
    // flap.
    std::chrono::steady_clock::time_point sink_open_at{};
    std::chrono::steady_clock::time_point sink_open_origin{};
    bool have_sink_open_at = false;

    bool ever_connected = false;

    bool stream_started = false;
    uint64_t stream_count_at_open = 0;

    bool reconnect_pending = false;

    // Upstream-idle pause state (#1). True between an idle-pause (SDK
    // stop() + Sync deactivate(), connection kept) and the resume on the
    // next real PCM packet. Distinct from reconnect_pending, which means
    // the Sync was fully torn down (#2 release) and must be reopened.
    bool paused = false;

    // Tail of the latest receive that didn't end on a frame boundary;
    // carried forward into the next push so the queue only ever sees
    // whole frames. Fixed-size buffer — never heap-allocates on the hot
    // path.
    uint8_t  partial_frame[PARTIAL_FRAME_MAX] = {0};
    uint32_t partial_frame_len = 0;

    // Producer-side stats not already tracked inside PcmRing.
    std::atomic<uint64_t> partial_carry_count{0};
    std::atomic<uint64_t> format_changes{0};
    std::atomic<uint64_t> reconnect_attempts{0};

    // Idle/active state tracking for stats and verbose logging.
    uint64_t last_stats_pushed_frames = 0;
    bool was_active_last_period = false;

    // Periodic stats printer state.
    std::chrono::steady_clock::time_point next_stats_print{};
    bool stats_print_armed = false;

    // -vv: throttle "prefill gate" progress prints during priming.
    long long last_prefill_log_ms = 0;
    bool prefill_logged_open = false;
    // Track mute-gate completion so we log the transition exactly once per
    // Sync open and can correlate it with the prefill gate opening.
    bool mute_logged_complete = false;

    // Diretta debug phase tracing.
    // Anchor for relative timestamps. Reset on each format-change accepted;
    // updated to sync_open_begin once a fresh Sync open starts. All
    // [diretta-debug] phase lines print ms relative to this anchor so the
    // human listening for a click can align "click at +X ms" with the
    // emitted Diretta phase events.
    std::chrono::steady_clock::time_point dbg_anchor{};
    bool dbg_anchor_valid = false;
    // Secondary anchor: the moment the current sync_open_begin started.
    std::chrono::steady_clock::time_point dbg_open_anchor{};
    bool dbg_open_anchor_valid = false;

    // Per-open one-shot flags. Reset by dbg_reset_open_flags() at the
    // start of each open_sync_for_format() so only the first occurrence
    // of each event gets a "first_*" trace line.
    bool dbg_logged_is_connect_true = false;
    bool dbg_logged_first_getNewStream = false;
    bool dbg_logged_first_real_pcm = false;
    uint64_t dbg_last_stream_count = 0;
    uint64_t dbg_last_real_cycles = 0;

    // One-shot open_grace_end phase log. Reset alongside the debug flags.
    bool phase_logged_open_grace_begin = false;
    bool phase_logged_open_grace_end = false;

    // Non-blocking Sync open. The open / setSink / connect / connectWait /
    // play sequence can block in the SDK, so it runs on a worker thread. The
    // receiver continues pushing PCM into PcmRing and later adopts the opened
    // Sync from async_open_pending_sync.
    std::atomic<int> async_open_state{0}; // AOS_*
    scream_diretta::ScreamDirettaSync* async_open_pending_sync = nullptr;
    std::thread async_open_thread;
    FormatConfigure async_open_fc{};
    std::string async_open_reason;
    std::atomic<uint32_t> async_open_accepted_bits{0};
    // First receiver-side event after sync_open_begin (anchored by the
    // worker, observed by the receive thread). Used to confirm that
    // multicast ingestion is not stalled by the open sequence.
    bool dbg_logged_first_packet_after_open_begin = false;
    bool dbg_logged_first_push_during_open_grace = false;
    bool phase_logged_open_grace_nonblocking = false;

    // True once at least one Sync open has been attempted in this session.
    // First-open skips format-change cooldown because no previous stream
    // needs to be released by the target. Set by reconfigure(); never reset.
    bool first_open_done = false;
    // Effective format-change cooldown in ms applied at the most recent
    // reconfigure(). Snapshotted so logs match the value that was used
    // (not whatever the config currently holds).
    int last_cooldown_ms = 0;
    // One-shot warning logged when queue fill crosses 90% of capacity
    // before the SDK first pulls real PCM. Reset whenever we (re)build
    // the unified queue and after first_real_pcm is observed.
    bool startup_overflow_risk_logged = false;

    // Underrun diagnostics.
    // Monotonic timestamp of the most recent PCM packet received from the
    // network/upstream (any non-empty data->audio). Used to derive
    // source_gap_ms when an underrun begins -- distinguishes "the source
    // paused / changed track" from "we are starved while the source is
    // still producing".
    std::chrono::steady_clock::time_point last_pcm_packet_at{};
    bool have_last_pcm_packet_at = false;

    // NIC-arrival timestamp of the most recent PCM packet, in CLOCK_REALTIME
    // nanoseconds (set only when --enable-nic-timestamp is in effect).
    // Lets underrun_begin/recover separate upstream sender gaps (nic_gap_ms)
    // from local userspace wakeup latency (source_gap_ms - nic_gap_ms).
    // 0 = unavailable.
    uint64_t last_pcm_nic_ts_ns = 0;

    // Previous underrun-event count observed by the receive thread when it
    // last polled. We use this to detect "underrun begin" without racing
    // against the SDK thread: when current > previous we emit the begin
    // event with the producer/source-gap context known to the receive
    // thread.
    uint64_t last_observed_underrun_events = 0;
    // True while the Sync is currently rebuffering. Flipping this back to
    // false (the SDK exited the rebuffer hold) emits the recover event.
    bool rebuffering_logged = false;
    // Timestamp + fill at the most recent underrun_begin so the recover
    // event can report the silent_ms duration of the hiccup.
    std::chrono::steady_clock::time_point underrun_begin_at{};
    uint64_t underrun_begin_silent_cycles = 0;
    uint64_t underrun_begin_real_cycles = 0;

    // Periodic stats deltas. Snapshot pushed_bytes / popped_bytes /
    // ring_fill_bytes at the previous stats print so the stats line
    // can include push_delta_ms / drain_delta_ms / net_fill_delta_ms.
    uint64_t last_stats_pushed_bytes = 0;
    uint64_t last_stats_popped_bytes = 0;
    uint64_t last_stats_ring_fill_bytes = 0;
    bool     have_last_stats_snapshot = false;

    // startup_real_delay observer flags. The Sync owns the gate state;
    // the receive thread observes the transition real_delay_done from
    // false -> true so it can emit a single startup_real_delay_end phase
    // event with queue-fill before/after diagnostics.
    bool phase_logged_startup_real_delay_begin = false;
    bool phase_logged_startup_real_delay_end = false;
    uint64_t startup_real_delay_queue_fill_at_begin = 0;
    uint64_t startup_real_delay_pushed_at_begin = 0;
    uint64_t startup_real_delay_popped_at_begin = 0;

    // PCM dumpers. Configured once at diretta_output_init from the
    // CLI prefixes. ingress_dumper is written from the receive thread
    // immediately before queue_push_frames; egress_dumper is attached to
    // the Sync and written from the SDK send thread on every real-PCM
    // pop cycle. Each instance is single-producer so no locking is needed.
    pcm_dumper_t ingress_dumper{};
    pcm_dumper_t egress_dumper{};

    // Raw-entry tap dumper. Writes data->audio / data->audio_size
    // raw, before any frame-align / partial-carry / queue step -- the
    // exact bytes that `-o stdout` (raw.c) would have written, just
    // wrapped as a WAV using the active source format. Written from the
    // receive thread, single-producer.
    pcm_dumper_t raw_entry_dumper{};

    // In-RAM byte comparator between the raw-entry tap and the
    // ingress tap. Records the first compare_window_bytes of REAL PCM at
    // each tap into a fixed-size buffer. Once both buffers are full
    // (or the active format changes), emits a single stderr summary
    // describing whether the two streams are byte-identical and, if not,
    // the first mismatch offset and the maximum per-sample absolute
    // delta. Single-producer per buffer (receive thread for both -- the
    // taps share a thread, so a plain memcpy/atomic-flag flow is safe).
    std::vector<uint8_t> cmp_raw_entry_buf;
    std::vector<uint8_t> cmp_ingress_buf;
    size_t   cmp_raw_entry_filled = 0;
    size_t   cmp_ingress_filled = 0;
    size_t   cmp_window_bytes = 0; // total bytes we want to capture per side
    uint32_t cmp_sample_rate = 0;
    uint32_t cmp_channels = 0;
    uint32_t cmp_bits_per_sample = 0;
    uint32_t cmp_bytes_per_frame = 0;
    bool     cmp_armed = false;     // both taps enabled AND window > 0
    bool     cmp_summary_emitted = false;

    // Startup analysers + egress fader. The ingress analyser is driven
    // from the receive thread (in queue_push_frames) and emits a summary
    // of the first --startup-analyze-ms of real PCM after each format/open.
    // The egress analyser + fader are owned here but attached to the
    // ScreamDirettaSync; they are consumed on the SDK send thread.
    // Single-producer per instance: no locking required.
    pcm_startup_analyzer_t ingress_analyzer{};
    pcm_startup_analyzer_t egress_analyzer{};
    pcm_startup_fader_t    egress_fader{};
};

enum AsyncOpenState : int {
    AOS_Idle       = 0,
    AOS_InProgress = 1,
    AOS_DoneOk     = 2,
    AOS_DoneFail   = 3,
};

DirettaState g_st;

// Single-point armed gate for Diretta-backend-internal diagnostic
// facilities (ingress/egress/raw-entry dumpers, startup analysers,
// startup fader, ingress-tap comparator). Computed once at the end of
// diretta_output_init() and read-only thereafter. Read by
// queue_push_frames(), the raw-entry tap in diretta_output_send(), and
// the egress-side feeds in ScreamDirettaSync::getNewStream() via the
// static inline diretta_diag_armed() accessor in diretta_diag.h.
//
// In SCREAM2DIRETTA_NO_DIAGNOSTICS builds the accessor folds to a
// compile-time constant 0, so this symbol is unused in the production
// binary; we still define it to keep the linkage symmetrical.
#ifndef SCREAM2DIRETTA_NO_DIAGNOSTICS
extern "C" int g_diretta_diag_armed_flag = 0;
#endif

#define DLOG(level, fmt, ...) do { \
    if (verbosity >= (level)) { \
        std::fprintf(stderr, "[diretta] " fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

// Phase-trace helpers.
//
// Independent of -v / -vv: when --diretta-debug is set, every event below
// is emitted to stderr with monotonic timestamps relative to the most
// recent format-change-accept (the "global" anchor) and, when applicable,
// the most recent sync_open_begin (the "open" anchor). This gives the
// user a deterministic timeline to overlay against any audible click.
//
// Format: "[diretta-debug] +<ms>ms (+<open_ms>ms since open_begin) <event>: <details>".
// Events use stable snake_case names matching the spec so log greps stay
// simple.

static inline bool dbg_on() { return g_st.cfg.diretta_debug != 0; }

static void dbg_set_anchor() {
    g_st.dbg_anchor = std::chrono::steady_clock::now();
    g_st.dbg_anchor_valid = true;
    g_st.dbg_open_anchor_valid = false;
}

static void dbg_set_open_anchor() {
    g_st.dbg_open_anchor = std::chrono::steady_clock::now();
    g_st.dbg_open_anchor_valid = true;
    if (!g_st.dbg_anchor_valid) {
        g_st.dbg_anchor = g_st.dbg_open_anchor;
        g_st.dbg_anchor_valid = true;
    }
}

static void dbg_reset_open_flags() {
    g_st.dbg_logged_is_connect_true = false;
    g_st.dbg_logged_first_getNewStream = false;
    g_st.dbg_logged_first_real_pcm = false;
    g_st.dbg_last_stream_count = 0;
    g_st.dbg_last_real_cycles = 0;
    g_st.phase_logged_open_grace_begin = false;
    g_st.phase_logged_open_grace_end = false;
    g_st.dbg_logged_first_packet_after_open_begin = false;
    g_st.dbg_logged_first_push_during_open_grace = false;
    g_st.phase_logged_open_grace_nonblocking = false;
    g_st.phase_logged_startup_real_delay_begin = false;
    g_st.phase_logged_startup_real_delay_end = false;
    g_st.startup_real_delay_queue_fill_at_begin = 0;
    g_st.startup_real_delay_pushed_at_begin = 0;
    g_st.startup_real_delay_popped_at_begin = 0;
    // A fresh Sync open resets the per-Sync underrun counters, so the
    // observer state must reset too. Otherwise the first underrun on the
    // new Sync would be missed (cur_events would already equal the stale
    // last_observed value).
    g_st.last_observed_underrun_events = 0;
    g_st.rebuffering_logged = false;
}

// Always-on phase emitter for the small set of once-per-open
// events the user wants to see by default (setSink, connect, connectWait,
// play, is_connect_true, first_getNewStream, first_real_pcm,
// open_grace_*). Independent of --diretta-debug and of -v. Emits one
// short line per phase with a monotonic ms-relative timestamp anchored
// at the most recent sync_open_begin (falls back to dbg_anchor or "now").
__attribute__((format(printf, 2, 3)))
static void phase_event(const char* event, const char* fmt, ...) {
    // Phase events are detailed step-by-step diagnostics. Suppress by
    // default; enable only with --diretta-debug or -vv.
    if (!g_st.cfg.diretta_debug && verbosity < 2) return;
    auto now = std::chrono::steady_clock::now();
    long long rel_open_ms = -1;
    long long rel_ms = 0;
    if (g_st.dbg_open_anchor_valid) {
        rel_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_st.dbg_open_anchor).count();
    }
    if (g_st.dbg_anchor_valid) {
        rel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_st.dbg_anchor).count();
    }
    char details[384];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(details, sizeof(details), fmt, ap);
        va_end(ap);
    } else {
        details[0] = '\0';
    }
    if (rel_open_ms >= 0) {
        std::fprintf(stderr,
            "[diretta-phase] +%lldms (+%lldms since open_begin) %s%s%s\n",
            rel_ms, rel_open_ms, event,
            details[0] ? ": " : "", details);
    } else {
        std::fprintf(stderr,
            "[diretta-phase] %s%s%s\n",
            event, details[0] ? ": " : "", details);
    }
}

// printf-like phase event emitter. The first %s in the fmt is reserved
// for the event name; the rest is free-form details. Stays a no-op when
// --diretta-debug is off so the hot path pays a single bool load.
__attribute__((format(printf, 2, 3)))
static void dbg_event(const char* event, const char* fmt, ...) {
    if (!dbg_on()) return;
    auto now = std::chrono::steady_clock::now();
    long long rel_ms = 0;
    long long rel_open_ms = -1;
    if (g_st.dbg_anchor_valid) {
        rel_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_st.dbg_anchor).count();
    }
    if (g_st.dbg_open_anchor_valid) {
        rel_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_st.dbg_open_anchor).count();
    }
    char details[512];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(details, sizeof(details), fmt, ap);
        va_end(ap);
    } else {
        details[0] = '\0';
    }
    if (rel_open_ms >= 0) {
        std::fprintf(stderr,
            "[diretta-debug] +%lldms (+%lldms since open_begin) %s%s%s\n",
            rel_ms, rel_open_ms, event,
            details[0] ? ": " : "", details);
    } else {
        std::fprintf(stderr,
            "[diretta-debug] +%lldms %s%s%s\n",
            rel_ms, event, details[0] ? ": " : "", details);
    }
}

static bool stats_should_print() {
    return (g_st.cfg.stats_enabled || verbosity > 0);
}

// Default cooldown between tearing down a Sync and opening a
// fresh one. The actual value used at runtime is g_st.cfg.format_change_cooldown_ms
// (configurable via --format-change-cooldown-ms; default 200). The constant
// below is only the fallback for safety (used if config was 0 due
// to older callers); it is NOT the runtime default any more.
constexpr int FORMAT_CHANGE_COOLDOWN_MS_LEGACY = 1200;
constexpr int FORMAT_CHANGE_COOLDOWN_MS_DEFAULT = 200;

static inline int effective_cooldown_ms() {
    int v = g_st.cfg.format_change_cooldown_ms;
    if (v < 0) v = 0;
    if (v > 5000) v = 5000;
    // DSD targets need more time to release the previous stream.
    if (g_st.is_dsd && g_st.dsd_multiplier > 1) {
        v = static_cast<int>(v * g_st.dsd_multiplier);
        if (v > 5000) v = 5000;
    }
    return v;
}

// Fallback. If after the cooldown the queue still hasn't reached the
// open threshold (e.g. silent/sparse stream, very low prefill rate), we
// still open the Sync after this much extra wall time so we don't refuse
// to play forever. The Sync's own prefill gate then handles the rest.
constexpr int OPEN_GATE_MAX_WAIT_MS = 3000;

constexpr int RUNTIME_CLEANUP_BUDGET_MS = 1500;

// After connect() succeeds, suppress sink-lost detection for this window.
constexpr int SINK_OPEN_GRACE_MS = 2000;
constexpr int SINK_OPEN_GRACE_EXTEND_MS = 500;
constexpr int SINK_OPEN_GRACE_TOTAL_MS = 8000;

// Split a decoded sample rate (Hz) into base/multiple for FormatID selection.
// Supports 44.1k/48k families and their power-of-two multiples.
static void map_rate(uint32_t sample_rate_hz, uint32_t* base, uint32_t* mult) {
    if (sample_rate_hz % 44100u == 0) {
        *base = 44100u;
        *mult = sample_rate_hz / 44100u;
    } else if (sample_rate_hz % 48000u == 0) {
        *base = 48000u;
        *mult = sample_rate_hz / 48000u;
    } else if (sample_rate_hz == 8000u) {
        *base = 8000u;
        *mult = 1;
    } else {
        *base = 0;
        *mult = 0;
    }
    if (*mult == 0) *mult = 1;
}

static FormatID rate_base_id(uint32_t base) {
    if (base == 44100u) return FormatID::RAT_44100;
    if (base == 48000u) return FormatID::RAT_48000;
    if (base == 8000u)  return FormatID::RAT_8000;
    return FormatID::NONE;
}

static FormatID rate_mult_id(uint32_t mult) {
    // Cap PCM sample-rate multiples to 16x (768 kHz). Higher wire values are
    // not realistic Scream sources and were an easy DoS vector: a crafted
    // header could force a multi-gigabyte ring allocation.
    switch (mult) {
        case 1:    return FormatID::RAT_MP1;
        case 2:    return FormatID::RAT_MP2;
        case 4:    return FormatID::RAT_MP4;
        case 8:    return FormatID::RAT_MP8;
        case 16:   return FormatID::RAT_MP16;
        default:   return FormatID::NONE;
    }
}

static FormatID channel_id(uint32_t ch) {
    switch (ch) {
        case 1: return FormatID::CHA_1;
        case 2: return FormatID::CHA_2;
        case 4: return FormatID::CHA_4;
        case 6: return FormatID::CHA_6;
        case 8: return FormatID::CHA_8;
        case 16: return FormatID::CHA_16;
        default: return FormatID::NONE;
    }
}

static FormatID pcm_id_for_bits(unsigned char bits) {
    switch (bits) {
        case 8:  return FormatID::FMT_PCM_SIGNED_8;
        case 16: return FormatID::FMT_PCM_SIGNED_16;
        case 24: return FormatID::FMT_PCM_SIGNED_24;
        case 32: return FormatID::FMT_PCM_SIGNED_32;
        default: return FormatID::NONE;
    }
}

static FormatID pcm_id(unsigned char bits) {
    return pcm_id_for_bits(bits);
}

// Build a DSD FormatConfigure. Scream signals DSD via sample_size == 1.
// The driver transmits HALF the DSD_U32 container rate and s2d's network.c
// (unlike the screamalsa receiver) does NOT apply `rate *= 2`, so rf.sample_rate
// here is the halved wire value; we restore the full rate locally:
//   container_rate = rf.sample_rate * 2    (queue sizing; 44100 -> 88200 = DSD64)
//   dsd_real_rate  = container_rate * 32   (SDK setSpeed; == rf.sample_rate * 64)
static bool build_dsd_format(const receiver_format_t& rf, FormatConfigure& out_fc,
                             uint32_t* out_container_rate, uint32_t* out_channels,
                             uint32_t* out_bits, uint32_t* out_bpf,
                             uint32_t* out_dsd_multiplier, uint32_t* out_dsd_real_rate) {
    // map_rate runs on the halved wire value: base must be 44100/48000 and the
    // multiplier (DSD64=1, DSD128=2, DSD256=4, DSD512=8) is derived from it.
    // Reject non-power-of-two or out-of-range multipliers; they are not valid
    // DSD rates and were an easy DoS vector for huge ring allocations.
    uint32_t base = 0, mult = 0;
    map_rate(rf.sample_rate, &base, &mult);
    if (base != 44100u && base != 48000u) return false;
    if (mult == 0) mult = 1;
    if (mult > 8u || (mult & (mult - 1)) != 0) return false;
    const uint32_t container_rate = rf.sample_rate * 2u;
    const uint64_t dsd_real_rate64 = static_cast<uint64_t>(container_rate) * 32u;
    if (dsd_real_rate64 > 0xFFFFFFFFu) return false;
    const uint32_t dsd_real_rate = static_cast<uint32_t>(dsd_real_rate64);
    const uint32_t bpf = rf.channels * 4u;
    FormatID ch = channel_id(rf.channels);
    if (ch == FormatID::NONE) return false;

    out_fc = FormatConfigure();
    out_fc.setSpeed(dsd_real_rate);
    out_fc.setChannel(rf.channels);
    // Default to LSB | BIG (most common for DSF-style interleaved DSD_U32_BE).
    // Actual negotiation in open_sync_worker_blocking() tries all 4 modes.
    out_fc.setFormat(FormatID::FMT_DSD1 |
                     FormatID::FMT_DSD_SIZ_32 |
                     FormatID::FMT_DSD_LSB |
                     FormatID::FMT_DSD_BIG);
    if (!out_fc.isValid()) return false;
    *out_container_rate = container_rate;
    *out_channels = rf.channels;
    *out_bits = 32;
    *out_bpf = bpf;
    *out_dsd_multiplier = mult;
    *out_dsd_real_rate = dsd_real_rate;
    return true;
}

// Format-dependent flags derived from a Scream header. build_format() fills
// this purely from its input and never touches g_st; only reconfigure()
// commits an accepted DerivedFormat, so the validity probe in
// diretta_output_send() cannot corrupt live DSD / pack-24 state.
struct DerivedFormat {
    bool     is_dsd = false;
    uint32_t dsd_multiplier = 0;
    uint32_t dsd_real_rate = 0;
    bool     pcm_needs_pack_24 = false;
};

static bool build_format(const receiver_format_t& rf, FormatConfigure& out_fc,
                         uint32_t* out_rate, uint32_t* out_channels,
                         uint32_t* out_bits, uint32_t* out_bpf,
                         DerivedFormat* out_df) {
    *out_df = DerivedFormat{};
    // DSD sentinel: screamalsa sets sample_size == 1 for DSD_U32_BE.
    if (rf.sample_size == 1) {
        uint32_t dsd_mult = 0, dsd_real = 0;
        bool ok = build_dsd_format(rf, out_fc, out_rate, out_channels, out_bits, out_bpf,
                                   &dsd_mult, &dsd_real);
        if (ok) {
            out_df->is_dsd = true;
            out_df->dsd_multiplier = dsd_mult;
            out_df->dsd_real_rate = dsd_real;
        }
        return ok;
    }
    uint32_t base = 0, mult = 0;
    map_rate(rf.sample_rate, &base, &mult);
    FormatID rb = rate_base_id(base);
    FormatID rm = rate_mult_id(mult);
    FormatID ch = channel_id(rf.channels);
    FormatID pc = pcm_id(rf.sample_size);
    if (rb == FormatID::NONE || rm == FormatID::NONE ||
        ch == FormatID::NONE || pc == FormatID::NONE) {
        return false;
    }
    out_fc = FormatConfigure(ch | pc | rb | rm);
    if (!out_fc.isValid()) return false;
    *out_rate = rf.sample_rate;
    *out_channels = rf.channels;
    *out_bits = rf.sample_size;
    // wire_layout distinguishes S24_3LE (3 bytes) from S24_LE (4-byte container).
    // build_format returns the SOURCE bytes per frame; reconfigure() derives the
    // queue bytes per frame (Diretta SDK always wants packed 3-byte S24).
    *out_bpf = scream_bytes_per_sample(&rf) * rf.channels;
    out_df->pcm_needs_pack_24 = (rf.sample_size == 24 &&
                                 rf.wire_layout == SCREAM_WIRE_S24_LE);
    return true;
}

// Confirm the sink accepts the source format as-is.
// Returns the accepted bit depth on success, or 0 if the sink rejects it.
// Ingress down-conversion was removed: when the DAC rejects the source bit
// depth we fail loudly and tell the user to lower the bit depth at the
// sender. This keeps the bridge transparent and avoids the bug-prone
// scalar-truncation path that almost never triggered in practice anyway
// (Diretta DACs commonly accept 32-bit, which is the Scream default).
static uint32_t negotiate_sink_format(scream_diretta::ScreamDirettaSync* sync,
                                      const FormatConfigure& source_fc,
                                      uint32_t source_bits) {
    if (sync->checkSinkSupport(source_fc)) {
        return source_bits;
    }
    DLOG(0, "sink does not support %u-bit source format. "
         "Lower the sender bit depth (e.g. set Scream Windows driver or "
         "Album Player to a bit depth the DAC supports) and try again. "
         "Ingress down-conversion is intentionally not implemented.",
         source_bits);
    return 0;
}

static SysLog::SysLogLevel map_log_level(diretta_log_level_t l) {
    switch (l) {
        case DIRETTA_LOG_DEBUG: return SysLog::Debug;
        case DIRETTA_LOG_WARN:  return SysLog::Warning;
        default:                return SysLog::Notice;
    }
}

static void init_syslog(const diretta_config_t& cfg) {
    static bool done = false;
    if (done) return;
    SysLog::initialize(SysLog::local0, /*port*/ 0, /*stdout*/ true);
    // --diretta-debug forces the SDK's own SysLog level to Debug so the
    // underlying library's "log" archives (libDirettaHost / libACQUA, the
    // non-"-nolog" variants) emit their full event stream. With the -nolog
    // archives this is a no-op, but it costs nothing to set.
    const SysLog::SysLogLevel lvl = cfg.diretta_debug
                                    ? SysLog::Debug
                                    : map_log_level(cfg.log_level);
    SysLogDiretta::changeLevel(lvl, DIRETTA::SyslogPortHost);
    done = true;
}

struct DiscoveredTarget {
    IPAddress portAddr;
    Find::TargetConnectInfo info;
};

static bool enumerate_targets(Find& finder, std::vector<DiscoveredTarget>& out) {
    Find::PortResalts ports;
    if (!finder.findOutput(ports)) {
        return false;
    }
    out.clear();
    out.reserve(ports.size());
    for (const auto& kv : ports) {
        out.push_back({ kv.first, kv.second });
    }
    return true;
}

static std::string ip_to_str(const IPAddress& a) {
    std::string s = a.get_full_str();
    if (s.empty()) s = "<addr>";
    return s;
}

// Overhead cache helpers -----------------------------------------------------

static std::string overhead_cache_dir() {
    // Prefer $STATE_DIRECTORY when running under systemd with
    // `StateDirectory=scream2diretta`. systemd creates /var/lib/scream2diretta
    // and exports the path here, and the directory remains writable even
    // when `ProtectHome=true` / `ProtectSystem=strict` are active.
    // If multiple StateDirectory entries are configured, $STATE_DIRECTORY is
    // colon-separated; take the first.
    if (const char* sd = std::getenv("STATE_DIRECTORY")) {
        if (sd[0]) {
            std::string s(sd);
            const auto colon = s.find(':');
            if (colon != std::string::npos) s.resize(colon);
            return s;
        }
    }
    // Manual / non-systemd invocation: write under the user's config dir.
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        return std::string(home) + "/.config/scream2diretta";
    }
    return "/tmp/scream2diretta";
}

static std::string overhead_cache_file(const IPAddress& addr) {
    std::string s = ip_to_str(addr);
    // Sanitise for use as a filename: replace : % , with safe chars.
    for (char& c : s) {
        if (c == ':') c = '_';
        else if (c == '%') c = '-';
        else if (c == ',') c = '.';
    }
    return overhead_cache_dir() + "/overhead-" + s + ".txt";
}

static void ensure_cache_dir() {
    const std::string dir = overhead_cache_dir();
#if defined(__linux__)
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
        // Create parent ~/.config if needed, then our subdir.
        const char* home = std::getenv("HOME");
        if (home && home[0]) {
            std::string parent = std::string(home) + "/.config";
            if (stat(parent.c_str(), &st) != 0) {
                if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
                    DLOG(1, "ensure_cache_dir: mkdir(%s) failed: %s",
                         parent.c_str(), std::strerror(errno));
                }
            }
        }
        if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            DLOG(1, "ensure_cache_dir: mkdir(%s) failed: %s",
                 dir.c_str(), std::strerror(errno));
        }
    }
#endif
}

static int load_inferred_overhead(const IPAddress& addr) {
    const std::string path = overhead_cache_file(addr);
    std::ifstream f(path);
    if (!f) return -1;
    int val = -1;
    if (f >> val && val > 0 && val < 200) return val;
    return -1;
}

static void save_inferred_overhead(const IPAddress& addr, int overhead) {
    if (overhead <= 0 || overhead >= 200) return;
    ensure_cache_dir();
    const std::string path = overhead_cache_file(addr);
    std::ofstream f(path);
    if (f) {
        f << overhead << "\n";
        f.flush();
        if (f.good()) {
            DLOG(1, "overhead cache saved: %s = %d", path.c_str(), overhead);
        } else {
            DLOG(1, "overhead cache write failed: %s (errno=%s)",
                 path.c_str(), std::strerror(errno));
        }
    } else {
        const char* home = std::getenv("HOME");
        DLOG(1, "overhead cache open failed: %s (errno=%s, HOME=%s)",
             path.c_str(), std::strerror(errno),
             (home && home[0]) ? home : "<unset>");
    }
}

// Dynamic cycle-time calculation matching DRUP / slim2Diretta.
// When cycle_us == 0 (auto), compute the interval needed to send one
// full efficient-MTU packet worth of audio at the current format.
static unsigned int calculateCycleTime(uint32_t sampleRate,
                                       uint32_t channels,
                                       uint32_t bitsPerSample,
                                       uint32_t mtu,
                                       int inferredOverhead)
{
    // OVERHEAD depends on MTU measurement semantics:
    //  - Jumbo frames (MTU > 2000): measSendMTU returns IP-layer MTU,
    //    overhead ≈ 6 bytes (observed: 9014 → 9008).
    //  - Standard frames (MTU ≤ 2000): measSendMTU returns link-layer
    //    frame size (incl. Ethernet header + FCS + possible VLAN tag),
    //    overhead ≈ 22 bytes (observed: 1518 → 1496).
    // If we have previously inferred the actual overhead from SDK feedback,
    // use it; otherwise fall back to defaults.
    int overhead;
    if (inferredOverhead > 0 && inferredOverhead < 200) {
        overhead = inferredOverhead;
    } else {
        overhead = (mtu > 2000u) ? 6 : 22;  // jumbo: IP-layer; standard: link-layer
    }
    const uint32_t effMtu = (mtu > static_cast<uint32_t>(overhead))
                                ? (mtu - overhead)
                                : 1476u;
    const double bytesPerSecond = static_cast<double>(sampleRate)
                                  * static_cast<double>(channels)
                                  * static_cast<double>(bitsPerSample) / 8.0;
    if (bytesPerSecond <= 0.0) return 200u;
    const double cycleTimeUs = (static_cast<double>(effMtu) / bytesPerSecond)
                               * 1000000.0;
    unsigned int result = static_cast<unsigned int>(std::round(cycleTimeUs));
    return std::max(100u, std::min(result, 50000u));
}

// Returns the human-readable mode name actually applied (for -vv logging).
// out_effective_cycle_us receives the cycle value we actually hand to the SDK
// for this configuration (so callers/logging don't have to second-guess the
// AUTO branch decision).
static const char* apply_transfer_mode(Sync& sb,
                                       const diretta_config_t& cfg,
                                       uint32_t sample_rate,
                                       uint32_t channels,
                                       uint32_t bits_per_sample,
                                       bool is_dsd,
                                       unsigned int& out_effective_cycle_us) {
    const uint32_t effective_mtu = cfg.mtu_override > 0
        ? static_cast<uint32_t>(cfg.mtu_override)
        : g_st.mtu;
    // varmax_cycle = cycle time that exactly fills one effMtu packet at the
    // current format. Used both as the auto-default cycle and as the
    // "1 packet per cycle" upper bound when the user gives an explicit cycle.
    const unsigned int varmax_cycle = calculateCycleTime(sample_rate,
                                                         channels,
                                                         bits_per_sample,
                                                         effective_mtu,
                                                         g_st.inferred_overhead);
    const unsigned int cycle_us = (cfg.cycle_us > 0) ? cfg.cycle_us : varmax_cycle;
    const Clock cycle    = Clock::MicroSeconds(cycle_us);
    const Clock cycleMin = (cfg.cycle_min_us > 0)
        ? Clock::MicroSeconds(cfg.cycle_min_us)
        : Clock();
    // Default: the cycle we computed above is what we hand to the SDK. AUTO
    // sub-branches that override this (varmax-override) must update
    // out_effective_cycle_us before returning.
    out_effective_cycle_us = cycle_us;

    // Global sanity warning: cycle <200us is below typical target capability
    // and may produce unstable playback. We still honour the user value; in
    // auto+user-cycle mode it will additionally be checked against varmax_cycle
    // (1-packet bound) below.
    if (cfg.cycle_us > 0 && cfg.cycle_us < 200u) {
        DLOG(0, "[warn] cycle_time_us=%u < 200us is below typical target "
                "capability; sound may be unstable (format=%uHz/%ubit/%uch)",
             cfg.cycle_us, sample_rate, bits_per_sample, channels);
    }

    if (cfg.target_profile_limit_us > 0) {
        const Clock limitCycle = Clock::MicroSeconds(cfg.target_profile_limit_us);
        ProfileMaker pm = sb.getProfileMaker(limitCycle);
        const char* name;

        switch (cfg.transfer_mode) {
            case DIRETTA_TM_VARMAX:
                pm.configTransferSizeMax();
                name = "profile-varmax";
                break;
            case DIRETTA_TM_FIXAUTO:
                pm.configTransferFixAuto(cycle);
                name = "profile-fixauto";
                break;
            case DIRETTA_TM_AUTOFIX:
                // Under an active Target Profile, autofix == cycle-anchored
                // FixAuto (same as the explicit FIXAUTO path).
                pm.configTransferFixAuto(cycle);
                name = "profile-autofix";
                break;
            case DIRETTA_TM_RANDOM:
                pm.configTransferRandom(cycle, cycleMin, /*fragments*/ 1);
                name = "profile-random";
                break;
            case DIRETTA_TM_VARAUTO:
                pm.configTransferVarAuto(cycle);
                name = "profile-varauto";
                break;
            case DIRETTA_TM_AUTO:
            default:
                pm.configTransferAuto(cycle);
                name = "profile-auto";
                break;
        }
        sb.setConfigTransfer(static_cast<Profile>(pm));
        return name;
    }

    switch (cfg.transfer_mode) {
        case DIRETTA_TM_VARMAX:
            sb.configTransferVarMax(cycle);
            return "varmax";
        case DIRETTA_TM_VARAUTO:
            sb.configTransferVarAuto(cycle);
            return "varauto";
        case DIRETTA_TM_FIXAUTO:
            sb.configTransferFixAuto(cycle);
            return "fixauto";
        case DIRETTA_TM_RANDOM:
            sb.configTransferRandom(cycle, cycleMin, /*fragments*/ 1);
            return "random";
        case DIRETTA_TM_AUTOFIX: {
            // AUTOFIX: cycle-anchored variant of AUTO. This is the legacy
            // auto+cycletime behaviour, kept as an explicit mode so callers
            // who want FixAuto (cycle honored verbatim) instead of the new
            // VarAuto-based auto+cycletime carrier can opt in.
            //
            // A. cfg.cycle_us > 0:
            //      user_cycle <= safe_max  -> FixAuto(user_cycle)  (cycle-anchored)
            //      user_cycle >  safe_max  -> override to VarMax(varmax_cycle)
            // B. cfg.cycle_us == 0:
            //      same as AUTO B-branch (low-bitrate/DSD -> VarAuto,
            //      normal/high-rate PCM -> VarMax).
            if (cfg.cycle_us > 0) {
                const unsigned int safe_max = static_cast<unsigned int>(
                    static_cast<double>(varmax_cycle) * 0.97);
                if (cycle_us <= safe_max) {
                    sb.configTransferFixAuto(cycle);
                    // out_effective_cycle_us already = cycle_us (= user value)
                    return "autofix-fixauto";
                } else {
                    DLOG(0, "[warn] requested cycle_time_us=%u exceeds 1-packet "
                            "limit (varmax_cycle=%u, safe_max=%u) at "
                            "%uHz/%ubit/%uch; falling back to varmax",
                         cycle_us, varmax_cycle, safe_max,
                         sample_rate, bits_per_sample, channels);
                    sb.configTransferVarMax(Clock::MicroSeconds(varmax_cycle));
                    out_effective_cycle_us = varmax_cycle;
                    return "autofix-varmax-override";
                }
            }
            // cfg.cycle_us == 0: identical to AUTO B-branch.
            const bool isLowBitrate = (bits_per_sample <= 16
                                       && sample_rate <= 48000);
            if (isLowBitrate || is_dsd) {
                sb.configTransferVarAuto(cycle);
                return is_dsd ? "autofix-varauto-dsd" : "autofix-varauto";
            } else {
                sb.configTransferVarMax(cycle);
                return "autofix-varmax";
            }
        }
        case DIRETTA_TM_AUTO:
        default: {
            // AUTO logic (two sub-cases):
            //
            // A. User explicitly gave --cycle-time (cfg.cycle_us > 0):
            //    Treat user cycle as primary intent. Apply via VarAuto so the
            //    cycle is carried as the anchor, BUT only if 1 packet still
            //    fits within effMtu at that cycle. The 1-packet bound is
            //    varmax_cycle (cycle that exactly fills effMtu). We use a 3%
            //    safety margin to absorb overhead-inference jitter so we
            //    don't silently fall to 2 packets/cycle.
            //      user_cycle <= safe_max  -> VarAuto(user_cycle)
            //      user_cycle >  safe_max  -> override to VarMax(varmax_cycle)
            //    NOTE: VarAuto (size-anchored) may yield a slightly larger
            //    cycle offset than FixAuto (cycle-anchored), but in single-
            //    packet operation the measured offset is negligible (<1%).
            //    The cycle-anchored FixAuto variant is available via the
            //    explicit `autofix` mode below.
            //
            // B. No --cycle-time (cfg.cycle_us == 0):
            //    DRUP-style policy: low-bitrate (<=16bit/<=48k) or DSD →
            //    VarAuto; normal/high-rate PCM → VarMax. The cycle used is
            //    varmax_cycle (one effMtu worth of data at current format).
            if (cfg.cycle_us > 0) {
                const unsigned int safe_max = static_cast<unsigned int>(
                    static_cast<double>(varmax_cycle) * 0.97);
                if (cycle_us <= safe_max) {
                    sb.configTransferVarAuto(cycle);
                    // out_effective_cycle_us already = cycle_us (= user value)
                    return "auto-varauto-cycle";
                } else {
                    DLOG(0, "[warn] requested cycle_time_us=%u exceeds 1-packet "
                            "limit (varmax_cycle=%u, safe_max=%u) at "
                            "%uHz/%ubit/%uch; falling back to varmax",
                         cycle_us, varmax_cycle, safe_max,
                         sample_rate, bits_per_sample, channels);
                    sb.configTransferVarMax(Clock::MicroSeconds(varmax_cycle));
                    out_effective_cycle_us = varmax_cycle;
                    return "auto-varmax-override";
                }
            }
            // cfg.cycle_us == 0 path (original auto policy: low-bitrate/DSD →
            // VarAuto, normal/high-rate PCM → VarMax). out_effective_cycle_us
            // is already set to cycle_us (=varmax_cycle) at function entry.
            const bool isLowBitrate = (bits_per_sample <= 16
                                       && sample_rate <= 48000);
            if (isLowBitrate || is_dsd) {
                sb.configTransferVarAuto(cycle);
                return is_dsd ? "auto-varauto-dsd" : "auto-varauto";
            } else {
                sb.configTransferVarMax(cycle);
                return "auto-varmax";
            }
        }
    }
}

// Allocate the unified PCM queue for the current format. Called
// at format-accept time (from reconfigure()) and on sink-lost recovery
// so PCM during cooldown / handshake is captured directly into the queue
// that the next Sync will pull from. Safe to call before any Sync exists.
static void configure_unified_queue(uint32_t sample_rate,
                                    uint32_t channels,
                                    uint32_t bytes_per_sample,
                                    uint32_t bytes_per_frame,
                                    uint8_t silence_byte) {
    const uint64_t bps = static_cast<uint64_t>(sample_rate) * bytes_per_frame;
    if (bps == 0 || bytes_per_frame == 0) {
        g_st.queue_ready = false;
        g_st.queue_bpf = 0;
        return;
    }
    int ring_ms = g_st.cfg.ring_buffer_ms > 0 ? g_st.cfg.ring_buffer_ms : 1000;
    int prefill_ms = g_st.cfg.prefill_ms > 0 ? g_st.cfg.prefill_ms : 0;
    if (g_st.is_dsd) {
        ring_ms = g_st.cfg.dsd_buffer_ms > 0 ? g_st.cfg.dsd_buffer_ms : DSD_BUFFER_MS_DEFAULT;
        prefill_ms = g_st.cfg.dsd_prefill_ms > 0 ? g_st.cfg.dsd_prefill_ms : DSD_PREFILL_MS_DEFAULT;
    }
    if (ring_ms < 50)   ring_ms = 50;
    if (ring_ms > 5000) ring_ms = 5000;
    size_t ring_bytes = static_cast<size_t>((bps * static_cast<uint64_t>(ring_ms)) / 1000);
    // Hard cap: a single Scream packet cannot legitimately require more than
    // a few hundred MB, and uncapped sizing was a DoS vector (crafted rate
    // forcing tens of GB). 256 MiB is generous for all real formats
    // (DSD512 stereo ~23 MB/s * 5 s ~115 MB).
    const size_t RING_BYTES_MAX = 256u * 1024u * 1024u;
    if (ring_bytes > RING_BYTES_MAX) {
        DLOG(0, "requested ring size %zu B exceeds cap %zu B; clamping",
             ring_bytes, RING_BYTES_MAX);
        ring_bytes = RING_BYTES_MAX;
    }
    g_st.queue.resize(ring_bytes, silence_byte);
    g_st.queue_bpf = bytes_per_frame;
    g_st.queue_ready = true;

    // Pre-compute the open-gate threshold in bytes for this format.
    // The Sync open is deferred until queue fill >= this value (or the
    // OPEN_GATE_MAX_WAIT_MS fallback fires). The threshold mirrors the
    // Sync's own prefill gate: max(prefill_ms, startup_queue_ms).
    int startup_ms  = g_st.cfg.startup_queue_ms > 0 ? g_st.cfg.startup_queue_ms : 0;
    int gate_ms = (startup_ms > prefill_ms) ? startup_ms : prefill_ms;
    if (gate_ms > ring_ms) gate_ms = ring_ms / 2;
    if (gate_ms < 0) gate_ms = 0;
    size_t thr = static_cast<size_t>((bps * static_cast<uint64_t>(gate_ms)) / 1000);
    if (bytes_per_frame > 0) {
        thr = (thr / bytes_per_frame) * bytes_per_frame;
    }
    g_st.open_gate_threshold_bytes = thr;

    DLOG(1, "PcmRing armed: %zu bytes (%dms) for %u Hz, %u-bit, %u ch "
         "(bpf=%u); open gate=%zu B (~%d ms); receiver writes directly, SDK pulls directly",
         g_st.queue.capacity(), ring_ms,
         sample_rate, bytes_per_sample * 8, channels, bytes_per_frame,
         thr, gate_ms);
    (void)bytes_per_sample;
}

// Comparator helpers. Defined here (inside the anonymous namespace)
// so queue_push_frames and reconfigure can call them; the format
// (re)arm path is invoked from reconfigure().
static void cmp_rearm_for_format(uint32_t sample_rate,
                                 uint32_t channels,
                                 uint32_t bits_per_sample,
                                 uint32_t bytes_per_frame) {
    if (g_st.cfg.compare_ingress_taps_ms <= 0) {
        g_st.cmp_armed = false;
        return;
    }
    if (!pcm_dumper_enabled(&g_st.raw_entry_dumper) ||
        !pcm_dumper_enabled(&g_st.ingress_dumper)) {
        g_st.cmp_armed = false;
        return;
    }
    if (sample_rate == 0 || channels == 0 ||
        bits_per_sample == 0 || bytes_per_frame == 0) {
        g_st.cmp_armed = false;
        return;
    }
    const uint64_t bps =
        static_cast<uint64_t>(sample_rate) * static_cast<uint64_t>(bytes_per_frame);
    uint64_t bytes = (bps * static_cast<uint64_t>(g_st.cfg.compare_ingress_taps_ms)) / 1000ULL;
    bytes = (bytes / bytes_per_frame) * bytes_per_frame;
    if (bytes == 0) bytes = bytes_per_frame;
    g_st.cmp_raw_entry_buf.assign(bytes, 0);
    g_st.cmp_ingress_buf.assign(bytes, 0);
    g_st.cmp_raw_entry_filled  = 0;
    g_st.cmp_ingress_filled = 0;
    g_st.cmp_window_bytes   = bytes;
    g_st.cmp_sample_rate    = sample_rate;
    g_st.cmp_channels       = channels;
    g_st.cmp_bits_per_sample = bits_per_sample;
    g_st.cmp_bytes_per_frame = bytes_per_frame;
    g_st.cmp_armed = true;
    g_st.cmp_summary_emitted = false;
    DLOG(2, "cmp: armed for %u Hz / %u-bit / %u ch (bpf=%u): %zu B per tap "
         "(~%d ms)",
         sample_rate, bits_per_sample, channels, bytes_per_frame,
         g_st.cmp_window_bytes, g_st.cfg.compare_ingress_taps_ms);
}

static inline void cmp_capture(std::vector<uint8_t>& buf, size_t& filled,
                               const uint8_t* src, size_t bytes) {
    if (!g_st.cmp_armed || g_st.cmp_summary_emitted || bytes == 0) return;
    if (filled >= g_st.cmp_window_bytes) return;
    const size_t space = g_st.cmp_window_bytes - filled;
    const size_t take = (bytes < space) ? bytes : space;
    std::memcpy(buf.data() + filled, src, take);
    filled += take;
}

static int32_t cmp_pcm_sample_signed(const uint8_t* p, uint32_t bits) {
    switch (bits) {
        case 16: {
            int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            return (int32_t)v;
        }
        case 24: {
            int32_t v = (int32_t)((uint32_t)p[0] |
                                  ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16));
            if (v & 0x00800000) v |= 0xFF000000;
            return v;
        }
        case 32: {
            int32_t v = (int32_t)((uint32_t)p[0] |
                                  ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16) |
                                  ((uint32_t)p[3] << 24));
            return v;
        }
        default: return 0;
    }
}

static void cmp_maybe_emit_summary(void) {
    if (!g_st.cmp_armed || g_st.cmp_summary_emitted) return;
    if (g_st.cmp_raw_entry_filled  < g_st.cmp_window_bytes) return;
    if (g_st.cmp_ingress_filled < g_st.cmp_window_bytes) return;
    g_st.cmp_summary_emitted = true;

    const size_t N = g_st.cmp_window_bytes;
    const uint32_t bps_bytes = g_st.cmp_bytes_per_frame;
    const uint32_t bits      = g_st.cmp_bits_per_sample;
    const uint32_t bytes_per_sample = bits / 8;
    const uint32_t sample_rate = g_st.cmp_sample_rate;

    const uint8_t* a = g_st.cmp_raw_entry_buf.data();
    const uint8_t* b = g_st.cmp_ingress_buf.data();

    size_t first_mismatch_byte = N;
    for (size_t i = 0; i < N; ++i) {
        if (a[i] != b[i]) { first_mismatch_byte = i; break; }
    }
    const bool identical = (first_mismatch_byte == N);

    int64_t max_abs_delta = 0;
    size_t  max_abs_delta_frame = 0;
    uint32_t max_abs_delta_ch = 0;
    const size_t total_samples = (bytes_per_sample > 0) ? (N / bytes_per_sample) : 0;
    for (size_t s = 0; s < total_samples; ++s) {
        const uint8_t* pa = a + s * bytes_per_sample;
        const uint8_t* pb = b + s * bytes_per_sample;
        const int64_t va = (int64_t)cmp_pcm_sample_signed(pa, bits);
        const int64_t vb = (int64_t)cmp_pcm_sample_signed(pb, bits);
        const int64_t d = (va > vb) ? (va - vb) : (vb - va);
        if (d > max_abs_delta) {
            max_abs_delta = d;
            if (bps_bytes > 0) {
                max_abs_delta_frame = (s * bytes_per_sample) / bps_bytes;
                max_abs_delta_ch = (uint32_t)((s * bytes_per_sample) % bps_bytes) / bytes_per_sample;
            }
        }
    }

    const size_t first_mismatch_frame =
        (first_mismatch_byte < N && bps_bytes > 0)
            ? (first_mismatch_byte / bps_bytes)
            : 0;
    const double first_mismatch_time_ms =
        (sample_rate > 0)
            ? ((double)first_mismatch_frame * 1000.0 / (double)sample_rate)
            : 0.0;

    const double full_scale =
        (bits == 16) ? 32768.0 :
        (bits == 24) ? 8388608.0 :
        (bits == 32) ? 2147483648.0 : 1.0;
    const double max_abs_delta_norm = (double)max_abs_delta / full_scale;

    std::fprintf(stderr,
        "[cmp:ingress-taps] summary: window_ms=%d window_bytes=%zu "
        "rate=%u bits=%u ch=%u bpf=%u identical=%s "
        "first_mismatch_byte=%zu first_mismatch_frame=%zu "
        "first_mismatch_time_ms=%.3f "
        "max_abs_sample_delta=%lld (norm=%.6f) "
        "max_abs_delta_frame=%zu max_abs_delta_ch=%u\n",
        g_st.cfg.compare_ingress_taps_ms,
        N,
        sample_rate, bits, g_st.cmp_channels, bps_bytes,
        identical ? "yes" : "no",
        (identical ? N : first_mismatch_byte),
        (identical ? (N / (bps_bytes ? bps_bytes : 1)) : first_mismatch_frame),
        identical ? ((sample_rate > 0)
                     ? ((double)(N / (bps_bytes ? bps_bytes : 1)) * 1000.0 / (double)sample_rate)
                     : 0.0)
                  : first_mismatch_time_ms,
        (long long)max_abs_delta, max_abs_delta_norm,
        max_abs_delta_frame, max_abs_delta_ch);
}

// Push a frame-aligned PCM run into the unified queue. The caller has
// already taken care of partial-frame carry; len must be a whole multiple
// of bpf.
static inline void queue_push_frames(const uint8_t* data, size_t bytes, uint32_t bpf) {
    if (!g_st.queue_ready || bpf == 0 || bytes == 0) return;
    // Diagnostic block: ingress dumper + ingress-tap comparator + ingress
    // startup analyser. All three are owned by the Diretta backend and
    // gated by the single diretta_diag_armed() flag computed once at init.
    // In SCREAM2DIRETTA_NO_DIAGNOSTICS builds the accessor folds to
    // constant 0 and the entire block is DCE'd -- the per-packet hot path
    // is just the early-exit check above plus pushFrames() below.
    if (__builtin_expect(diretta_diag_armed(), 0)) {
        // Ingress PCM dump. Runs BEFORE pushFrames so that even if the
        // ring drops on overflow, we still capture the exact bytes that came
        // off the wire after frame-alignment. The ingress dumper is single-
        // producer (receive thread) so no locking is required.
        if (pcm_dumper_enabled(&g_st.ingress_dumper)) {
            if (pcm_dumper_open_or_rotate(&g_st.ingress_dumper,
                                          g_st.sample_rate, g_st.channels,
                                          g_st.bits_per_sample)) {
                pcm_dumper_write(&g_st.ingress_dumper, data, bytes);
            }
        }
        // Comparator: capture the post-frame-align ingress bytes. Same
        // window as the raw-entry tap; once both buffers are full the summary
        // line fires.
        cmp_capture(g_st.cmp_ingress_buf, g_st.cmp_ingress_filled, data, bytes);
        cmp_maybe_emit_summary();
        // Ingress startup analyser. Inspects the first N ms of PCM after
        // each format/open and emits a single summary line. The ingress
        // analyser observes the raw queue input -- never affected by the
        // egress fade (which only mutates the SDK cycle buffer).
        if (!pcm_startup_analyzer_done(&g_st.ingress_analyzer)) {
            pcm_startup_analyzer_feed(&g_st.ingress_analyzer, data, bytes);
        }
    }
    g_st.queue.pushFrames(data, bytes, bpf);
}

// Bit-reversal lookup table: reverses the order of bits within a byte.
// Generated by: lut[b] = reverse_bits(b)
static const uint8_t g_bit_reverse_lut[256] = {
    0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
    0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
    0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,
    0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
    0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,
    0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
    0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,
    0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
    0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,
    0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
    0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,
    0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
    0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,
    0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
    0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,
    0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF,
};

// Apply negotiated DSD bit-order / byte-order transformations to standard
// ALSA DSD_U32_BE data.
//
// The Scream receiver is required to deliver DSD in standard ALSA DSD_U32_BE
// frame order: each channel's 4-byte word is contiguous, and channels alternate
// by word ([L0 L1 L2 L3 R0 R1 R2 R3] for stereo). The legacy network receiver
// (network.c, -L mode) performs the original byte-interleaved -> word-interleaved
// conversion if needed, so by the time the data reaches this backend it is always
// in standard word-interleaved order.
//
// This function applies only the target-dependent transformations:
//   * bit-reversal (LSB <-> MSB) if the sink negotiated LSB-first DSD
//   * byte-swap (BIG <-> LITTLE) if the sink negotiated little-endian DSD
static inline void dsd_transform(const uint8_t* src, uint8_t* dst, size_t bytes,
                                 uint32_t channels, bool bit_reverse, bool byte_swap) {
    if (channels == 0 || bytes == 0) return;
    const uint32_t bpf = channels * 4; // DSD_SIZ_32: 4 bytes per channel
    const size_t num_frames = bytes / bpf;

    for (size_t f = 0; f < num_frames; ++f) {
        const uint8_t* s = src + f * bpf;
        uint8_t* d = dst + f * bpf;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            // Standard DSD_U32_BE: each channel's 4-byte word is contiguous.
            uint8_t b0 = s[ch * 4 + 0];
            uint8_t b1 = s[ch * 4 + 1];
            uint8_t b2 = s[ch * 4 + 2];
            uint8_t b3 = s[ch * 4 + 3];

            if (bit_reverse) {
                b0 = g_bit_reverse_lut[b0];
                b1 = g_bit_reverse_lut[b1];
                b2 = g_bit_reverse_lut[b2];
                b3 = g_bit_reverse_lut[b3];
            }

            if (byte_swap) {
                d[ch * 4 + 0] = b3;
                d[ch * 4 + 1] = b2;
                d[ch * 4 + 2] = b1;
                d[ch * 4 + 3] = b0;
            } else {
                d[ch * 4 + 0] = b0;
                d[ch * 4 + 1] = b1;
                d[ch * 4 + 2] = b2;
                d[ch * 4 + 3] = b3;
            }
        }
    }
}

// Pack S24_LE (4-byte little-endian container, 24-bit sample in low 3 bytes)
// into S24_3LE (packed 3-byte little-endian). Diretta SDK only exposes packed
// 24-bit PCM (FMT_PCM_SIGNED_24), so screamalsa's S24_LE wire_layout must be
// converted before queue ingress.
static inline void pcm_pack_24le(const uint8_t* src, uint8_t* dst, size_t bytes,
                                 uint32_t src_bpf, uint32_t channels) {
    if (channels == 0 || src_bpf == 0 || bytes == 0) return;
    const uint32_t dst_bpf = channels * 3;
    const size_t num_frames = bytes / src_bpf;
    for (size_t f = 0; f < num_frames; ++f) {
        const uint8_t* s = src + f * src_bpf;
        uint8_t* d = dst + f * dst_bpf;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            // S24_LE: low 3 bytes of a 32-bit LE word carry the sample.
            d[ch * 3 + 0] = s[ch * 4 + 0];
            d[ch * 3 + 1] = s[ch * 4 + 1];
            d[ch * 3 + 2] = s[ch * 4 + 2];
        }
    }
}

// Wrapper around queue_push_frames that performs DSD bit-reversal /
// byte-swap, or S24_LE -> S24_3LE packing, when required.
// src_bpf  = bytes per frame of the SOURCE data (Scream header format)
// dst_bpf  = bytes per frame of the TARGET data (queue / SDK format)
static inline void queue_push_frames_converted(const uint8_t* data, size_t bytes,
                                               uint32_t src_bpf, uint32_t dst_bpf) {
    if (!g_st.queue_ready || dst_bpf == 0 || bytes == 0) return;

    // DSD transformation path: the Scream receiver delivers standard ALSA
    // DSD_U32_BE word-interleaved data. The legacy network path (-L) already
    // converts original byte-interleaved DSD to this standard order. Here we
    // only apply the sink-dependent bit-order / byte-order transforms.
    if (g_st.is_dsd) {
        if (bytes > g_st.dsd_transform_buffer.size()) {
            // Defensive: input larger than our fixed scratch buffer. Drop rather
            // than allocate on the hot path; this should never happen in practice
            // because Scream packets are bounded by the network MTU.
            return;
        }
        const bool bit_reverse = g_st.dsd_needs_bit_reverse.load(std::memory_order_acquire);
        const bool byte_swap   = g_st.dsd_needs_byte_swap.load(std::memory_order_acquire);
        dsd_transform(data, g_st.dsd_transform_buffer.data(), bytes, g_st.channels,
                      bit_reverse, byte_swap);
        queue_push_frames(g_st.dsd_transform_buffer.data(), bytes, dst_bpf);
        return;
    }

    // S24_LE -> S24_3LE packing. Diretta SDK has no native 4-byte 24-bit
    // container; drop the padding byte from each sample.
    if (g_st.pcm_needs_pack_24) {
        if (src_bpf == 0) return;
        const size_t whole_frames = bytes / src_bpf;
        const size_t out_bytes = whole_frames * dst_bpf;
        if (out_bytes == 0) return;
        if (out_bytes > g_st.pcm_pack_buffer.size()) {
            // Defensive: output larger than our fixed scratch buffer. Drop rather
            // than allocate on the hot path.
            return;
        }
        pcm_pack_24le(data, g_st.pcm_pack_buffer.data(), whole_frames * src_bpf,
                      src_bpf, g_st.channels);
        queue_push_frames(g_st.pcm_pack_buffer.data(), out_bytes, dst_bpf);
        return;
    }

    queue_push_frames(data, bytes, dst_bpf);
}

// Runs blocking SDK open calls on a worker thread so the receiver can keep
// pushing PCM into PcmRing. Operates only on a local Sync*; never touches
// g_st.sync. On success the Sync is published via async_open_pending_sync.
// On failure it is destroyed before the worker reports DoneFail.
//
// All format scalars are passed explicitly as snapshots so the worker does not
// read mutable g_st fields while the receiver thread may be running.
static uint32_t open_sync_worker_blocking(scream_diretta::ScreamDirettaSync*& out_sync,
                                          const FormatConfigure& fc,
                                          uint32_t sample_rate,
                                          uint32_t channels,
                                          uint32_t bits_per_sample,
                                          bool is_dsd,
                                          uint32_t dsd_real_rate,
                                          uint32_t dsd_multiplier) {
    const diretta_config_t& cfg = g_st.cfg;
    auto* sync = new scream_diretta::ScreamDirettaSync();
    out_sync = nullptr;
    sync->setRtPriority(cfg.rt_priority);

    // Attach the persistent queue BEFORE configureFormat / connect so the
    // SDK send thread can never see a null ring.
    sync->attachRing(&g_st.queue);

    // Attach the egress PCM dumper (no-op if --dump-egress-wav was
    // not set). The Sync writes real-PCM bytes returned to the SDK into
    // the dumper on every pop cycle. Single-producer from the SDK send
    // thread; safe without locking.
    sync->attachEgressDumper(&g_st.egress_dumper);

    // Attach the egress startup analyser + fader. Both are owned by
    // DirettaState and reconfigured on every Sync open (configureFormat).
    sync->attachEgressAnalyzer(&g_st.egress_analyzer);
    sync->attachEgressFader(&g_st.egress_fader);

    const unsigned base_tmode = cfg.thread_mode != 0 ? cfg.thread_mode : (unsigned)Sync::CRITICAL;
    const unsigned occ_mask = (cfg.cpu_audio >= 0) ? (unsigned)Sync::OCCUPIED : 0u;
    const Sync::THRED_MODE tmode = static_cast<Sync::THRED_MODE>(base_tmode | occ_mask);
    const int info_us = cfg.info_cycle_us > 0 ? cfg.info_cycle_us : 100000;
    const int sdk_cpu_main  = cfg.cpu_audio >= 0 ? cfg.cpu_audio : 0;
    const int sdk_cpu_other = cfg.cpu_other >= 0 ? cfg.cpu_other : 0;

    if (cfg.cpu_audio >= 0) {
        DLOG(1, "CPU affinity: SDK thread pinned to core %d (OCCUPIED mode, thread_mode=0x%x)",
             cfg.cpu_audio, (unsigned)tmode);
    }
    dbg_event("sdk_sync_open_call", "thread_mode=0x%x info_us=%d cpuMain=%d cpuOther=%d",
              (unsigned)tmode, info_us, sdk_cpu_main, sdk_cpu_other);
    if (!sync->open(tmode,
                    Clock::MicroSeconds(info_us),
                    /*ifno*/ 0,
                    /*name*/ "scream2diretta",
                    /*venderId*/ 0,
                    /*cpuMain*/ sdk_cpu_main,
                    /*cpuOther*/ sdk_cpu_other,
                    /*rngOther*/ 0,
                    Sync::MSMODE_AUTO)) {
        DLOG(0, "Sync::open() failed");
        dbg_event("sdk_sync_open_return", "result=fail");
        delete sync;
        return 0;
    }
    dbg_event("sdk_sync_open_return", "result=ok");

    const uint32_t mtu_eff = cfg.mtu_override > 0 ? (uint32_t)cfg.mtu_override : g_st.mtu;
    const int eff_buf_ms = cfg.target_buffer_ms;
    phase_event("setSink_begin",
                "addr=%s buffer_ms=%d mtu=%u",
                ip_to_str(g_st.sink_addr).c_str(), eff_buf_ms, (unsigned)mtu_eff);
    dbg_event("setSink_begin",
              "addr=%s buffer_ms=%d mtu=%u nopBreak=false",
              ip_to_str(g_st.sink_addr).c_str(),
              eff_buf_ms,
              (unsigned)mtu_eff);
    if (!sync->setSink(g_st.sink_addr,
                       Clock::MilliSeconds(eff_buf_ms),
                       /*nopBreak*/ false,
                       mtu_eff)) {
        DLOG(0, "setSink() failed");
        phase_event("setSink_end", "result=fail");
        dbg_event("setSink_return", "result=fail");
        sync->close();
        delete sync;
        return 0;
    }
    phase_event("setSink_end", "result=ok buffer_ms=%d", eff_buf_ms);
    dbg_event("setSink_return", "result=ok");
    // Pace the handshake: target needs time between SDK calls or it
    // misses the first cycles and produces no audio. Mirrors DRUP /
    // slim2Diretta inter-call pauses.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Format negotiation: PCM falls back to lower bit depths; DSD tries
    // the four common LSB/MSB × BIG/LITTLE combinations.
    uint32_t accepted_bits = 0;
    if (is_dsd) {
        const char* dsd_mode_str = "unknown";
        FormatConfigure dsd_fc;
        dsd_fc.setSpeed(dsd_real_rate);
        dsd_fc.setChannel(channels);

        struct DsdMode { FormatID fmt; const char* name; };
        const DsdMode dsd_modes[] = {
            { FormatID::FMT_DSD1 | FormatID::FMT_DSD_SIZ_32 |
              FormatID::FMT_DSD_MSB | FormatID::FMT_DSD_BIG,   "MSB|BIG" },
            { FormatID::FMT_DSD1 | FormatID::FMT_DSD_SIZ_32 |
              FormatID::FMT_DSD_LSB | FormatID::FMT_DSD_BIG,   "LSB|BIG" },
            { FormatID::FMT_DSD1 | FormatID::FMT_DSD_SIZ_32 |
              FormatID::FMT_DSD_MSB | FormatID::FMT_DSD_LITTLE,"MSB|LITTLE" },
            { FormatID::FMT_DSD1 | FormatID::FMT_DSD_SIZ_32 |
              FormatID::FMT_DSD_LSB | FormatID::FMT_DSD_LITTLE,"LSB|LITTLE" },
        };
        bool dsd_ok = false;
        FormatID negotiated_dsd_fmt = FormatID::FMT_DSD1;
        for (const auto& mode : dsd_modes) {
            dsd_fc.setFormat(mode.fmt);
            if (sync->checkSinkSupport(dsd_fc)) {
                if (!sync->setSinkConfigure(dsd_fc)) {
                    DLOG(0, "setSinkConfigure(DSD %s) failed after checkSinkSupport succeeded", mode.name);
                    continue;
                }
                dsd_mode_str = mode.name;
                negotiated_dsd_fmt = mode.fmt;
                dsd_ok = true;
                break;
            }
        }
        if (!dsd_ok) {
            // Last resort: FMT_DSD1 only (let SDK infer the rest).
            dsd_fc.setFormat(FormatID::FMT_DSD1);
            if (sync->checkSinkSupport(dsd_fc) && sync->setSinkConfigure(dsd_fc)) {
                dsd_mode_str = "FMT_DSD1";
                negotiated_dsd_fmt = FormatID::FMT_DSD1;
                dsd_ok = true;
            }
        }
        if (!dsd_ok) {
            DLOG(0, "sink does not support any DSD format (tried MSB|BIG, LSB|BIG, MSB|LITTLE, LSB|LITTLE)");
            dbg_event("checkSinkSupport_return", "result=fail dsd");
            sync->close();
            delete sync;
            return 0;
        }
        accepted_bits = 32;  // DSD uses 32-bit container internally

        // Determine if the target needs bit-reversal or byte-swap relative to
        // the Scream receiver's DSD_U32_BE output. Standard ALSA DSD_U32_BE
        // places the DSD bit in the MSB (bit 7) of each byte, i.e. the source
        // is MSB+BIG.
        const bool target_is_lsb    = (negotiated_dsd_fmt & FormatID::FMT_DSD_LSB) != FormatID(0);
        const bool target_is_msb    = (negotiated_dsd_fmt & FormatID::FMT_DSD_MSB) != FormatID(0);
        const bool target_is_little = (negotiated_dsd_fmt & FormatID::FMT_DSD_LITTLE) != FormatID(0);
        // If neither LSB nor MSB is set (FMT_DSD1 fallback), assume MSB.
        const bool target_wants_lsb = target_is_lsb;
        g_st.async_open_dsd_needs_bit_reverse.store(target_wants_lsb, std::memory_order_release);
        g_st.async_open_dsd_needs_byte_swap.store(target_is_little, std::memory_order_release);
        DLOG(1, "DSD format negotiated: %s (real_rate=%u Hz) transform: bit_reverse=%s byte_swap=%s",
             dsd_mode_str, dsd_real_rate,
             g_st.async_open_dsd_needs_bit_reverse.load(std::memory_order_relaxed) ? "yes" : "no",
             g_st.async_open_dsd_needs_byte_swap.load(std::memory_order_relaxed) ? "yes" : "no");
        dbg_event("checkSinkSupport_return", "result=ok dsd_mode=%s", dsd_mode_str);
        dbg_event("setSinkConfigure_return", "result=ok dsd_mode=%s", dsd_mode_str);
    } else {
        accepted_bits = negotiate_sink_format(sync, fc, bits_per_sample);
        if (accepted_bits == 0) {
            DLOG(0, "sink does not support any compatible PCM format (tried %u, 24, 16)",
                 bits_per_sample);
            dbg_event("checkSinkSupport_return", "result=fail");
            sync->close();
            delete sync;
            return 0;
        }
        dbg_event("checkSinkSupport_return", "result=ok accepted_bits=%u", accepted_bits);

        // If negotiation kept the source format, we still need to call setSinkConfigure.
        if (accepted_bits == bits_per_sample) {
            dbg_event("setSinkConfigure_begin", "");
            if (!sync->setSinkConfigure(fc)) {
                DLOG(0, "setSinkConfigure() failed");
                dbg_event("setSinkConfigure_return", "result=fail");
                sync->close();
                delete sync;
                return 0;
            }
            dbg_event("setSinkConfigure_return", "result=ok");
        } else {
            // negotiate_sink_format already called setSinkConfigure with the fallback format.
            dbg_event("setSinkConfigure_return", "result=ok auto_downgrade=%u->%u",
                      bits_per_sample, accepted_bits);
        }
    }
    // Pace the handshake after setSinkConfigure -- DRUP sleeps 100ms here.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    dbg_event("inquirySupportFormat_begin", "");
    sync->inquirySupportFormat(g_st.sink_addr);
    if (dbg_on() || verbosity >= 2) {
        const Sync::Info& si = sync->getSinkInfo();
        const bool maxSize_unset = (si.maxSize == 65535u);
        const uint32_t inferred_overhead = (!maxSize_unset && si.maxSize > 0)
            ? ((unsigned)si.maxMTU > si.maxSize
               ? ((unsigned)si.maxMTU - si.maxSize)
               : 0)
            : 0;
        if (dbg_on()) {
            dbg_event("sink_info",
                      "supportPCM=0x%x latencyBuf=%u latencyMax=%u latencyHw=%u "
                      "maxSize=%u%s(min=%u,req=%u,max=%u)[inferred_overhead=%s] "
                      "supportMSmode=0x%x",
                      (unsigned)si.supportPCM, (unsigned)si.latencyBuffer,
                      (unsigned)si.latencyMax, (unsigned)si.latencyHw,
                      (unsigned)si.maxSize, maxSize_unset ? "(unset)" : "",
                      (unsigned)si.minMTU, (unsigned)si.reqMTU, (unsigned)si.maxMTU,
                      maxSize_unset ? "N/A" : std::to_string(inferred_overhead).c_str(),
                      (unsigned)si.supportMSmode);
        }
        if (verbosity >= 2) {
            const uint16_t msm = si.supportMSmode;
            const char* inferred_ms = "NONE";
            if (msm & 0x04)       inferred_ms = "MS3";
            else if (msm & 0x01)  inferred_ms = "MS1";
            DLOG(2, "sink caps: pcm=0x%x dsd_lsb=%s dsd_msb=%s ms_mode=%s "
                 "(supported:%s%s%s) latency_buf=%uus latency_max=%uus latency_hw=%uus "
                 "max_mtu=%u max_payload=%u%s inferred_overhead=%s",
                 (unsigned)si.supportPCM,
                 si.checkSinkSupportDSDlsb() ? "yes" : "no",
                 si.checkSinkSupportDSDmsb() ? "yes" : "no",
                 inferred_ms,
                 (msm & 0x01) ? " MS1" : "",
                 (msm & 0x02) ? " MS2" : "",
                 (msm & 0x04) ? " MS3" : "",
                 (unsigned)si.latencyBuffer * 100u,
                 (unsigned)si.latencyMax * 100u,
                 (unsigned)si.latencyHw * 100u,
                 (unsigned)si.maxMTU,
                 (unsigned)si.maxSize,
                 maxSize_unset ? "(unset)" : "",
                 maxSize_unset ? "N/A" : std::to_string(inferred_overhead).c_str());
        }
    }
    dbg_event("inquirySupportFormat_return", "");

    dbg_event("setConfigTransfer_begin",
              "mode=%d cycle_us=%d cycle_min_us=%d info_cycle_us=%d profile_limit_us=%d",
              (int)cfg.transfer_mode, cfg.cycle_us, cfg.cycle_min_us,
              cfg.info_cycle_us, cfg.target_profile_limit_us);
    unsigned int applied_cycle_us = 0;
    const char* applied_mode = apply_transfer_mode(*sync, cfg,
                                                   sample_rate, channels,
                                                   bits_per_sample, is_dsd,
                                                   applied_cycle_us);
    if (dbg_on()) {
        dbg_event("sdk_cycle_info",
                  "getCycleTime_us=%lld getMinCycleTime_us=%lld getCycleSize=%zu getCyclePackets=%zu",
                  (long long)(sync->getCycleTime().getMicroSeconds()),
                  (long long)(sync->getMinCycleTime().getMicroSeconds()),
                  (size_t)sync->getCycleSize(),
                  (size_t)sync->getCyclePackets());
    }
    // Compute cycle stats once. The DLOG below is verbose-only, but cycle
    // caching and overhead inference run unconditionally so a fresh service
    // start (without -vv) still populates the overhead-*.txt cache on the
    // first successful open. Cache location is resolved by overhead_cache_dir():
    // $STATE_DIRECTORY (systemd) → $HOME/.config/scream2diretta → /tmp.
    {
        const uint32_t eff_mtu = cfg.mtu_override > 0
            ? (uint32_t)cfg.mtu_override
            : g_st.mtu;
        // target_cycle = the cycle we actually handed to the SDK (returned via
        // out-param from apply_transfer_mode). This stays correct across all
        // AUTO sub-branches (varmax-override, fixauto, etc.) where the cycle
        // handed to SDK may differ from cfg.cycle_us.
        const int target_cycle = static_cast<int>(applied_cycle_us);
        const long long sdk_cycle = (long long)(sync->getCycleTime().getMicroSeconds());

        if (verbosity >= 2) {
            // mode_sdk: the send-profile ModeType the SDK quantized our config
            // into (read-back of getMode()). Maps Profile::ModeType:
            //   VARIABLE=0, FIX=1, RANDOM=2, TRIANGOLO=3.
            const int mode_raw = static_cast<int>(sync->getMode());
            const char* mode_sdk;
            switch (mode_raw) {
                case 0:  mode_sdk = "variable";  break;
                case 1:  mode_sdk = "fix";       break;
                case 2:  mode_sdk = "random";    break;
                case 3:  mode_sdk = "triangolo"; break;
                default: mode_sdk = "unknown";   break;
            }
            // min_cycle: SDK's minimum generated transmission interval
            // (getMinCycleTime). Only meaningful when >0; under SelfProfile
            // it stays 0, so suppress the field in that case to avoid noise.
            const long long min_cycle =
                (long long)(sync->getMinCycleTime().getMicroSeconds());
            char min_cycle_buf[32];
            if (min_cycle > 0) {
                snprintf(min_cycle_buf, sizeof(min_cycle_buf),
                         " min_cycle=%lldus", min_cycle);
            } else {
                min_cycle_buf[0] = '\0';
            }
            DLOG(2, "transfer: mtu=%u mode=%s mode_sdk=%s target_cycle=%dus "
                 "sdk_cycle=%lldus cycle_size=%zuB cycle_packets=%zu%s",
                 (unsigned)eff_mtu, applied_mode, mode_sdk, target_cycle, sdk_cycle,
                 (size_t)sync->getCycleSize(),
                 (size_t)sync->getCyclePackets(),
                 min_cycle_buf);
        }

        g_st.target_cycle_us = static_cast<uint64_t>(target_cycle);
        g_st.sdk_cycle_us = static_cast<uint64_t>(sdk_cycle);

        // Infer actual protocol overhead from SDK feedback.
        // cycle_size is the per-packet PCM payload; eff_mtu - cycle_size
        // equals the real overhead (Ethernet + DDS + FCS + padding).
        if (g_st.inferred_overhead < 0) {
            const size_t cs = sync->getCycleSize();
            const int inferred = static_cast<int>(eff_mtu) - static_cast<int>(cs);
            if (inferred > 0 && inferred < 200) {
                g_st.inferred_overhead = inferred;
                DLOG(1, "inferred overhead: %d (mtu=%u cycle_size=%zu packets=%zu)",
                     inferred, (unsigned)eff_mtu, cs,
                     (size_t)sync->getCyclePackets());
                save_inferred_overhead(g_st.sink_addr, inferred);
            } else {
                DLOG(1, "overhead inference skipped: mtu=%u cycle_size=%zu "
                     "raw_inferred=%d", (unsigned)eff_mtu, cs, inferred);
            }
        }
    }
    dbg_event("setConfigTransfer_return", "");
    // Pace before connectPrepare so the target sees the new transfer
    // configuration before the connect handshake starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    dbg_event("connectPrepare_begin", "");
    sync->connectPrepare();
    dbg_event("connectPrepare_return", "");
    // DRUP and slim2Diretta both pause between connectPrepare and
    // connect (CONNECT_DELAY_MS in slim2Diretta; ~100ms in DRUP).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Configure per-format Sync state (cycle size, prefill threshold). The
    // ring it reads from is g_st.queue, already sized by
    // configure_unified_queue().
    scream_diretta::SyncTuning tuning;
    int ring_ms  = cfg.ring_buffer_ms > 0 ? cfg.ring_buffer_ms : 1000;
    int prefill_ms = cfg.prefill_ms     > 0 ? cfg.prefill_ms     : 500;
    int mute_ms  = cfg.startup_mute_ms  > 0 ? cfg.startup_mute_ms  : 0;
    int real_delay_ms = cfg.startup_real_delay_ms > 0 ? cfg.startup_real_delay_ms : 0;
    if (is_dsd) {
        ring_ms    = cfg.dsd_buffer_ms  > 0 ? cfg.dsd_buffer_ms  : DSD_BUFFER_MS_DEFAULT;
        prefill_ms = cfg.dsd_prefill_ms > 0 ? cfg.dsd_prefill_ms : DSD_PREFILL_MS_DEFAULT;
        const uint32_t mult = dsd_multiplier > 1 ? dsd_multiplier : 1;
        mute_ms       = static_cast<int>(mute_ms * mult);
        real_delay_ms = static_cast<int>(real_delay_ms * mult);
        const int dsd_warmup = cfg.dsd_startup_warmup_ms > 0 ? cfg.dsd_startup_warmup_ms : 0;
        mute_ms += static_cast<int>(dsd_warmup * mult);
        if (mute_ms > 2000) mute_ms = 2000;
        if (real_delay_ms > 5000) real_delay_ms = 5000;
    }
    tuning.ring_buffer_ms = ring_ms;
    tuning.prefill_ms     = prefill_ms;
    tuning.rebuffer_percent = (cfg.rebuffer_percent >= 0.0f && cfg.rebuffer_percent <= 0.95f)
                              ? cfg.rebuffer_percent : 0.50f;
    tuning.underrun_rebuffer_ms = cfg.underrun_rebuffer_ms > 0 ? cfg.underrun_rebuffer_ms : 0;
    tuning.startup_queue_ms = cfg.startup_queue_ms > 0 ? cfg.startup_queue_ms : 0;
    tuning.startup_mute_ms  = mute_ms;
    tuning.startup_real_delay_ms = real_delay_ms;
    {
        uint32_t bps = bits_per_sample / 8;
        if (bps == 0) bps = 1;
        sync->configureFormat(sample_rate, channels, bps, tuning);
    }
    dbg_event("configureFormat_done",
              "ring_ms=%d prefill_ms=%d startup_queue_ms=%d startup_mute_ms=%d "
              "startup_real_delay_ms=%d rebuffer_pct=%.0f underrun_rebuffer_ms=%d "
              "sample_rate=%u channels=%u bps=%u",
              tuning.ring_buffer_ms, tuning.prefill_ms, tuning.startup_queue_ms,
              tuning.startup_mute_ms, tuning.startup_real_delay_ms,
              tuning.rebuffer_percent * 100.0f,
              tuning.underrun_rebuffer_ms,
              sample_rate, channels, bits_per_sample / 8);

    const int connect_cpu = cfg.cpu_audio >= 0 ? cfg.cpu_audio : 0;
    phase_event("connect_begin", "cpuMain=%d", connect_cpu);
    dbg_event("connect_begin", "cpuMain=%d", connect_cpu);
    if (!sync->connect(connect_cpu)) {
        DLOG(0, "connect() failed");
        phase_event("connect_end", "result=fail");
        dbg_event("connect_return", "result=fail");
        sync->close();
        delete sync;
        return 0;
    }
    phase_event("connect_end", "result=ok");
    dbg_event("connect_return", "result=ok");
    phase_event("connectWait_begin", "");
    dbg_event("connectWait_begin", "");
    if (!sync->connectWait()) {
        DLOG(0, "connectWait() failed");
        phase_event("connectWait_end", "result=fail");
        dbg_event("connectWait_return", "result=fail");
        sync->disconnect(false);
        sync->close();
        delete sync;
        return 0;
    }
    phase_event("connectWait_end",
                "is_connect=%d is_online=%d is_active=%d",
                sync->is_connect() ? 1 : 0,
                sync->is_online() ? 1 : 0,
                sync->is_active() ? 1 : 0);
    dbg_event("connectWait_return",
              "is_connect=%d is_online=%d is_active=%d",
              sync->is_connect() ? 1 : 0,
              sync->is_online() ? 1 : 0,
              sync->is_active() ? 1 : 0);

    // Wait for the target to actually transition to ONLINE before
    // starting playback. slim2Diretta uses
    //   while (!is_online()) sleep_for(5ms);
    // We bound the wait at 500ms so a non-responsive target cannot
    // wedge the open path. Without this poll, play() can fire before
    // the target has finished its CONNECT transition, and the first
    // cycles are lost (silent startup until something nudges the
    // target -- which is what -vv accidentally provides via log I/O).
    {
        const auto poll_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(500);
        while (!sync->is_online() &&
               std::chrono::steady_clock::now() < poll_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        dbg_event("online_poll_done", "is_online=%d", sync->is_online() ? 1 : 0);
    }

    phase_event("play_begin", "");
    dbg_event("play_begin", "");
    sync->play();
    DLOG(1, "play() ok, target is playing (isPlay=%d)", sync->isPlay() ? 1 : 0);
    phase_event("play_end", "isPlay=%d", sync->isPlay() ? 1 : 0);
    dbg_event("play_return", "isPlay=%d", sync->isPlay() ? 1 : 0);

    // The Sync is fully open and ready, but we do NOT activate it here.
    // Activation is deferred to finalize_sync_open_on_receiver() on the
    // receive thread so that DSD transform flags negotiated above can be
    // committed and any stale pre-negotiation PCM can be cleared from the
    // queue before the SDK send thread starts pulling.
    out_sync = sync;
    return accepted_bits;
}

// Called by the receive thread when it observes AOS_DoneOk. Installs the
// freshly-opened Sync into g_st, emits the open_grace phase markers, and
// re-arms the post-open state machine. This is the part of the old
// open_sync_for_format() that lived after play_end — it must run on the
// receive thread because it touches fields the receive thread also reads
// elsewhere (sink_open_at, ever_connected, stream_count_at_open, etc.).
static void finalize_sync_open_on_receiver(scream_diretta::ScreamDirettaSync* sync,
                                           uint32_t accepted_bits) {
    const diretta_config_t& cfg = g_st.cfg;
    const uint32_t source_bits = g_st.bits_per_sample;
    g_st.sync = sync;

    // negotiate_sink_format() now refuses any sink that cannot accept the
    // source bit depth, so accepted_bits must equal source_bits here.
    // A mismatch would indicate a logic error in the open path.
    if (accepted_bits != source_bits) {
        DLOG(0, "internal error: accepted_bits=%u != source_bits=%u "
             "(ingress conversion was removed); refusing open",
             accepted_bits, source_bits);
        sync->deactivate();
        sync->disconnect(false);
        sync->close();
        delete sync;
        g_st.sync = nullptr;
        return;
    }

    // Commit DSD transform flags negotiated by the async worker. The
    // receive thread may have been writing PCM into the queue while the
    // worker was opening the Sync; if the negotiated transform differs from
    // the default (no transform), the queued bytes were produced with the
    // wrong transform and must be discarded before playback starts.
    if (g_st.is_dsd) {
        const bool bit_reverse = g_st.async_open_dsd_needs_bit_reverse.load(std::memory_order_acquire);
        const bool byte_swap   = g_st.async_open_dsd_needs_byte_swap.load(std::memory_order_acquire);
        g_st.dsd_needs_bit_reverse.store(bit_reverse, std::memory_order_release);
        g_st.dsd_needs_byte_swap.store(byte_swap, std::memory_order_release);
        if (bit_reverse || byte_swap) {
            const size_t discarded = g_st.queue.available();
            g_st.queue.clear();
            DLOG(1, "DSD transform negotiated (bit_reverse=%s byte_swap=%s); "
                 "discarded %zu B of pre-negotiation PCM from queue",
                 bit_reverse ? "yes" : "no", byte_swap ? "yes" : "no", discarded);
        }
    }

    if (!g_st.phase_logged_open_grace_begin) {
        g_st.phase_logged_open_grace_begin = true;
        phase_event("open_grace_begin",
                    "grace_ms=%d total_cap_ms=%d nonblocking=1",
                    SINK_OPEN_GRACE_MS, SINK_OPEN_GRACE_TOTAL_MS);
    }
    if (!g_st.phase_logged_open_grace_nonblocking) {
        g_st.phase_logged_open_grace_nonblocking = true;
        phase_event("open_grace_nonblocking",
                    "queue_fill=%zu/%zu B receiver_first_packet_logged=%d "
                    "receiver_first_push_during_grace_logged=%d",
                    g_st.queue_ready ? g_st.queue.available() : 0,
                    g_st.queue_ready ? g_st.queue.capacity() : 0,
                    g_st.dbg_logged_first_packet_after_open_begin ? 1 : 0,
                    g_st.dbg_logged_first_push_during_open_grace ? 1 : 0);
    }
    g_st.sink_active = true;
    // bytes_per_frame was set in reconfigure() to the destination frame size
    // (e.g. packed 3-byte for S24_LE). Recompute only when there is no source-
    // to-queue conversion in progress; otherwise leave the conversion state alone.
    if (!g_st.pcm_needs_pack_24) {
        uint32_t bps = g_st.bits_per_sample / 8;
        g_st.bytes_per_frame = bps * g_st.channels;
    }
    g_st.sink_open_at = std::chrono::steady_clock::now();
    g_st.sink_open_origin = g_st.sink_open_at;
    g_st.have_sink_open_at = true;
    g_st.conn_lost_logged = false;
    g_st.ever_connected = g_st.sync->is_connect();
    g_st.stream_started = false;
    g_st.stream_count_at_open = g_st.sync->getStreamCount();
    g_st.reconnect_pending = false;
    g_st.prefill_logged_open = false;
    g_st.last_prefill_log_ms = 0;
    g_st.mute_logged_complete = (cfg.startup_mute_ms <= 0);

    int eff_prefill_ms = cfg.prefill_ms > 0 ? cfg.prefill_ms : 500;
    int eff_startup_mute_ms = cfg.startup_mute_ms > 0 ? cfg.startup_mute_ms : 0;
    if (g_st.is_dsd) {
        eff_prefill_ms = cfg.dsd_prefill_ms > 0 ? cfg.dsd_prefill_ms : DSD_PREFILL_MS_DEFAULT;
        const uint32_t mult = g_st.dsd_multiplier > 1 ? g_st.dsd_multiplier : 1;
        eff_startup_mute_ms = static_cast<int>(eff_startup_mute_ms * mult);
        const int dsd_warmup = cfg.dsd_startup_warmup_ms > 0 ? cfg.dsd_startup_warmup_ms : 0;
        eff_startup_mute_ms += static_cast<int>(dsd_warmup * mult);
        if (eff_startup_mute_ms > 2000) eff_startup_mute_ms = 2000;
    }
    const int eff_startup_queue_ms = cfg.startup_queue_ms > 0 ? cfg.startup_queue_ms : 0;
    const float eff_rebuffer_pct   = (cfg.rebuffer_percent >= 0.0f && cfg.rebuffer_percent <= 0.95f)
                                     ? cfg.rebuffer_percent : 0.50f;
    const int gate_ms = (eff_startup_queue_ms > 0 && eff_startup_queue_ms > eff_prefill_ms)
                         ? eff_startup_queue_ms
                         : eff_prefill_ms;
    DLOG(1, "sync open finalised on receive thread: queue=%zu/%zu B (~%llu ms), "
         "prefill_ms=%d, startup_queue_ms=%d, startup_mute_ms=%d, rebuffer=%.0f%%, "
         "gate_threshold=%d ms (format=%u Hz, %u-bit, %u ch). "
         "SDK pulls real PCM from cycle 1. open_grace is diagnostic-only and "
         "did NOT block the receive path during this open.",
         g_st.sync->ringFill(), g_st.sync->ringBytes(),
         (unsigned long long)g_st.sync->ringFillMs(),
         eff_prefill_ms, eff_startup_queue_ms, eff_startup_mute_ms,
         eff_rebuffer_pct * 100.0f, gate_ms,
         g_st.sample_rate, g_st.bits_per_sample, g_st.channels);
    if (eff_startup_mute_ms > 0 && verbosity >= 2) {
        DLOG(2, "startup mute: outputting silence for ~%d ms of "
             "real pull cycles before popping PCM. Not recommended with the current open gate — "
             "queue prebuffer at open=%llu ms",
             eff_startup_mute_ms,
             (unsigned long long)g_st.sync->ringFillMs());
    }
    phase_event("sync_open_end",
                "queue_fill=%zu/%zu B (~%llu ms) is_connect=%d stream_count=%llu",
                g_st.sync->ringFill(), g_st.sync->ringBytes(),
                (unsigned long long)g_st.sync->ringFillMs(),
                g_st.sync->is_connect() ? 1 : 0,
                (unsigned long long)g_st.sync->getStreamCount());
    dbg_event("sync_open_end",
              "queue_fill=%zu/%zu B (~%llu ms) is_connect=%d stream_count=%llu",
              g_st.sync->ringFill(), g_st.sync->ringBytes(),
              (unsigned long long)g_st.sync->ringFillMs(),
              g_st.sync->is_connect() ? 1 : 0,
              (unsigned long long)g_st.sync->getStreamCount());

    // Activate the Sync only after format negotiation has been committed
    // and any stale pre-negotiation PCM has been cleared. This guarantees
    // the first SDK pull cycle sees transform-correct data.
    sync->activate();
}

static void cleanup_sync_async(scream_diretta::ScreamDirettaSync* old,
                               const char* reason_in);

// Kick off a non-blocking Sync open. Returns false if a previous async open
// is still in flight; callers should then continue ingesting into PcmRing.
static bool start_async_sync_open(const FormatConfigure& fc, const char* reason) {
    const int prev = g_st.async_open_state.load(std::memory_order_acquire);
    if (prev == AOS_InProgress) {
        DLOG(2, "start_async_sync_open: open already in progress (%s)",
             reason ? reason : "");
        return false;
    }
    // Reap any prior thread that we never joined (DoneOk/DoneFail was
    // already consumed but the thread handle is still around).
    if (g_st.async_open_thread.joinable()) {
        g_st.async_open_thread.join();
    }
    // If a previous async open completed but was never installed (e.g. a
    // reconnect raced with poll_async_sync_open()), clean it up now instead
    // of leaking a live Sync + Target session.
    if (g_st.async_open_pending_sync) {
        DLOG(1, "start_async_sync_open: discarding completed but uninstalled Sync");
        scream_diretta::ScreamDirettaSync* stale = g_st.async_open_pending_sync;
        g_st.async_open_pending_sync = nullptr;
        stale->deactivate();
        stale->stop();
        cleanup_sync_async(stale, "completed-async-open-discarded");
        g_st.async_open_state.store(AOS_Idle, std::memory_order_release);
    }

    const diretta_config_t& cfg = g_st.cfg;
    g_st.async_open_fc = fc;
    g_st.async_open_reason = reason ? reason : "";
    g_st.async_open_pending_sync = nullptr;
    dbg_set_open_anchor();
    dbg_reset_open_flags();
    DLOG(1, "Sync open started: target=%s (reason=%s)",
         ip_to_str(g_st.sink_addr).c_str(),
         g_st.async_open_reason.c_str());
    phase_event("sync_open_begin",
                "target=%s mtu=%u target_buffer_ms=%d cycle_us=%d "
                "info_cycle_us=%d transfer_mode=%d format=%uHz %u-bit %uch "
                "queue_fill=%zu/%zu B nonblocking=1 reason=%s",
                ip_to_str(g_st.sink_addr).c_str(),
                (unsigned)(cfg.mtu_override > 0 ? (uint32_t)cfg.mtu_override : g_st.mtu),
                cfg.target_buffer_ms,
                cfg.cycle_us, cfg.info_cycle_us, (int)cfg.transfer_mode,
                g_st.sample_rate, g_st.bits_per_sample, g_st.channels,
                g_st.queue_ready ? g_st.queue.available() : 0,
                g_st.queue_ready ? g_st.queue.capacity() : 0,
                g_st.async_open_reason.c_str());
    dbg_event("sync_open_begin",
              "target=%s mtu=%u target_buffer_ms=%d thread_mode=0x%x cycle_us=%d "
              "info_cycle_us=%d transfer_mode=%d target_profile_limit_us=%d "
              "format=%uHz %u-bit %uch (bpf=%u) queue_fill=%zu/%zu B nonblocking=1",
              ip_to_str(g_st.sink_addr).c_str(),
              (unsigned)(cfg.mtu_override > 0 ? (uint32_t)cfg.mtu_override : g_st.mtu),
              cfg.target_buffer_ms,
              (unsigned)(cfg.thread_mode != 0 ? cfg.thread_mode : (unsigned)Sync::CRITICAL),
              cfg.cycle_us, cfg.info_cycle_us,
              (int)cfg.transfer_mode, cfg.target_profile_limit_us,
              g_st.sample_rate, g_st.bits_per_sample, g_st.channels,
              g_st.bytes_per_frame,
              g_st.queue_ready ? g_st.queue.available() : 0,
              g_st.queue_ready ? g_st.queue.capacity() : 0);
    // Snapshot the format scalars for the worker. The worker must not read
    // mutable g_st fields while the receiver thread is running; all inputs
    // are captured here before the thread is spawned.
    const uint32_t snap_sample_rate = g_st.sample_rate;
    const uint32_t snap_channels    = g_st.channels;
    const uint32_t snap_bits        = g_st.bits_per_sample;
    const bool     snap_is_dsd      = g_st.is_dsd;
    const uint32_t snap_dsd_rate    = g_st.dsd_real_rate;
    const uint32_t snap_dsd_mult    = g_st.dsd_multiplier;

    g_st.async_open_state.store(AOS_InProgress, std::memory_order_release);
    g_st.async_open_accepted_bits.store(0, std::memory_order_relaxed);
    g_st.async_open_thread = std::thread([fc, snap_sample_rate, snap_channels,
                                          snap_bits, snap_is_dsd, snap_dsd_rate,
                                          snap_dsd_mult]() {
#if defined(__linux__)
        // This worker is spawned from a context that may itself be running
        // SCHED_FIFO (e.g. the receiver thread at --rt-priority): a pthread
        // created there INHERITS the parent's SCHED_FIFO policy + priority.
        // open_sync_worker_blocking() does blocking network handshakes, so it
        // must NOT run at real-time priority competing with the audio threads.
        // Demote to SCHED_OTHER first so the subsequent nice(-10) actually
        // takes effect (nice is a no-op under SCHED_FIFO).
        {
            struct sched_param sp; sp.sched_priority = 0;
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
        }
        ::nice(-10);
#endif
        scream_diretta::ScreamDirettaSync* sync_local = nullptr;
        const uint32_t accepted_bits = open_sync_worker_blocking(
            sync_local, fc,
            snap_sample_rate, snap_channels, snap_bits,
            snap_is_dsd, snap_dsd_rate, snap_dsd_mult);
        if (accepted_bits != 0) {
            g_st.async_open_pending_sync = sync_local;
            g_st.async_open_accepted_bits.store(accepted_bits, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_release);
            g_st.async_open_state.store(AOS_DoneOk, std::memory_order_release);
        } else {
            g_st.async_open_pending_sync = nullptr;
            g_st.async_open_state.store(AOS_DoneFail, std::memory_order_release);
        }
    });
    return true;
}



static void cleanup_finder() {
    if (g_st.finder) {
        g_st.finder->close();
        delete g_st.finder;
        g_st.finder = nullptr;
    }
}

// Deterministic SDK disconnect using the canonical DRUP / slim2Diretta
// pattern: stop() then disconnect_flgset() then a brief 50ms pause so the
// bare-Ethernet disconnect notice can leave on the wire before the caller
// closes the socket. Replaces the older disconnect(true) + thread-detach +
// abandon pattern, which could block indefinitely on a stale target
// session and left the target waiting out an abandoned session whenever
// the disconnect did not complete in budget.
//
// Always returns true; callers can drop their failure-path branches.
// budget_ms and reason are kept for source-level ABI compatibility with
// existing call sites; they are intentionally unused.
static bool bounded_disconnect(scream_diretta::ScreamDirettaSync* s,
                               int /*budget_ms*/,
                               const char* /*reason*/) {
    if (!s) return true;
    s->stop();
    s->disconnect_flgset();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
}

static void teardown_sync_for_shutdown() {
    // If an async open is in flight or just finished, reap the worker first.
    // If it produced a Sync that was never installed, adopt it for teardown.
    if (g_st.async_open_thread.joinable()) {
        g_st.async_open_thread.join();
    }
    const int aos = g_st.async_open_state.load(std::memory_order_acquire);
    if (aos == AOS_DoneOk && g_st.async_open_pending_sync) {
        // Worker handed us a Sync we never installed. Adopt it so the
        // standard teardown can disconnect it cleanly.
        g_st.sync = g_st.async_open_pending_sync;
        g_st.async_open_pending_sync = nullptr;
        g_st.sink_active = true;
    }
    g_st.async_open_state.store(AOS_Idle, std::memory_order_release);

    if (!g_st.sync) return;
    if (g_st.sink_active) {
        bounded_disconnect(g_st.sync, /*budget_ms*/ 0, "shutdown");
    }
    g_st.sync->close();
    delete g_st.sync;
    g_st.sync = nullptr;
    g_st.sink_active = false;
    g_st.bytes_per_frame = 0;
    g_st.src_bytes_per_frame = 0;
    g_st.partial_frame_len = 0;
}


static std::atomic<int> g_inflight_cleanups{0};

static void cleanup_sync_async(scream_diretta::ScreamDirettaSync* old,
                               const char* reason_in) {
    if (!old) return;
    std::string reason = reason_in ? reason_in : "";
    g_inflight_cleanups.fetch_add(1, std::memory_order_acq_rel);
    DLOG(1, "cleanup thread started (%s); inflight=%d",
         reason.c_str(),
         g_inflight_cleanups.load(std::memory_order_acquire));
    dbg_event("format_change_cleanup_begin",
              "reason=%s inflight=%d", reason.c_str(),
              g_inflight_cleanups.load(std::memory_order_acquire));

    // Run the deterministic disconnect + close + delete on a worker
    // thread so the audio thread is never blocked. bounded_disconnect()
    // uses stop() + disconnect_flgset() + 50ms wait, which is bounded
    // and never hangs on a stale target session. close() and delete are
    // then safe to call directly because no SDK thread is mid-pull.
    std::thread t([old, reason]() {
#if defined(__linux__)
        // Same rationale as the async-open worker: a thread spawned from a
        // SCHED_FIFO context inherits real-time policy. bounded_disconnect() +
        // close() + delete are blocking cleanup work that must not preempt the
        // audio threads, so demote to SCHED_OTHER before nice(-10) (which is
        // otherwise ignored under SCHED_FIFO).
        {
            struct sched_param sp; sp.sched_priority = 0;
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
        }
        ::nice(-10);
#endif
        const auto t0 = std::chrono::steady_clock::now();
        try {
            bounded_disconnect(old, 0, reason.c_str());
            old->close();
            delete old;
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[diretta] cleanup thread exception (%s): %s\n",
                reason.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr,
                "[diretta] cleanup thread unknown exception (%s)\n",
                reason.c_str());
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr,
            "[diretta] cleanup thread completed in %lldms (%s)\n",
            (long long)elapsed, reason.c_str());
        dbg_event("format_change_cleanup_end",
                  "elapsed_ms=%lld reason=%s",
                  (long long)elapsed, reason.c_str());
        g_inflight_cleanups.fetch_sub(1, std::memory_order_acq_rel);
    });
    t.detach();
}

static bool teardown_sync_for_runtime(const char* reason) {
    if (!g_st.sync) {
        g_st.sink_active = false;
        g_st.bytes_per_frame = 0;
        g_st.src_bytes_per_frame = 0;
        g_st.partial_frame_len = 0;
        g_st.have_sink_open_at = false;
        g_st.ever_connected = false;
        g_st.stream_started = false;
        g_st.paused = false;
        return true;
    }
    scream_diretta::ScreamDirettaSync* old = g_st.sync;
    if (old) {
        old->deactivate(); // stop getNewStream() from touching the ring
        old->stop();       // stop the SDK send thread
    }
    g_st.sync = nullptr;
    g_st.sink_active = false;
    g_st.bytes_per_frame = 0;
    g_st.src_bytes_per_frame = 0;
    g_st.partial_frame_len = 0;
    g_st.have_sink_open_at = false;
    g_st.ever_connected = false;
    g_st.stream_started = false;
    g_st.paused = false;   // any teardown invalidates the paused-Sync state
    cleanup_sync_async(old, reason);
    return true;
}

static bool validate_format(const receiver_format_t& rf, FormatConfigure& out_fc,
                            uint32_t* out_rate, uint32_t* out_channels,
                            uint32_t* out_bits, uint32_t* out_bpf,
                            DerivedFormat* out_df) {
    if (rf.sample_rate == 0) return false;
    if (rf.channels == 0) return false;
    // DSD sentinel (sample_size == 1) is valid; normal PCM needs sample_size > 0.
    if (rf.sample_size == 0) return false;
    if (!build_format(rf, out_fc, out_rate, out_channels, out_bits, out_bpf, out_df)) return false;
    if (*out_bpf == 0 || *out_bpf > PARTIAL_FRAME_MAX) return false;
    return true;
}

// Validate and stage a real format change. Tears down the old Sync via
// async cleanup, builds a fresh unified queue for the new format, and
// arms the cooldown timer. PCM that arrives during cooldown lands in the
// new queue directly — no staging, no batch drain.
static bool reconfigure(const receiver_format_t& rf) {
    FormatConfigure fc;
    uint32_t rate = 0, ch = 0, bits = 0, bpf = 0;
    DerivedFormat df;
    if (!validate_format(rf, fc, &rate, &ch, &bits, &bpf, &df)) {
        DLOG(0, "unsupported scream format: rate=%u Hz size=%u ch=%u",
             rf.sample_rate, rf.sample_size, rf.channels);
        return false;
    }
    // SOURCE bytes per frame from the Scream header (e.g. 8 for stereo S24_LE).
    const uint32_t src_bpf = bpf;
    // DESTINATION bytes per frame for the queue / SDK. Diretta SDK only has
    // packed S24_3LE, so S24_LE is converted on ingress and the queue stores
    // 3 bytes per sample regardless of wire_layout.
    const uint32_t dst_bpf = (df.pcm_needs_pack_24) ? (3 * ch) : src_bpf;

    // First open in this session has no previous stream to release.
    // For first-open we skip the format-change cooldown entirely (cooldown=0)
    // so the unified queue does not have to absorb 1.2 s of PCM before the
    // SDK begins pulling -- this is what causes the startup drop the user
    // reported by queue_fill near capacity at sync_open_end and a real
    // drop before first_real_pcm). The configured cooldown still applies on
    // any subsequent format change because the target needs time to release
    // the previous stream.
    const bool is_first_open = !g_st.first_open_done;
    const int cooldown_ms = is_first_open ? 0 : effective_cooldown_ms();
    g_st.last_cooldown_ms = cooldown_ms;

    if (df.is_dsd) {
        DLOG(1, "format change accepted: DSD (real_rate=%u Hz, mult=%u) container=%u Hz, %u-bit, %u ch (bpf=%u) "
             "[first_open=%d cooldown_ms=%d]",
             df.dsd_real_rate, df.dsd_multiplier,
             rate, bits, ch, dst_bpf, is_first_open ? 1 : 0, cooldown_ms);
    } else {
        DLOG(1, "format change accepted: %u Hz, %u-bit, %u ch (src_bpf=%u dst_bpf=%u pack24=%d) "
             "[first_open=%d cooldown_ms=%d]",
             rate, bits, ch, src_bpf, dst_bpf, df.pcm_needs_pack_24 ? 1 : 0,
             is_first_open ? 1 : 0, cooldown_ms);
    }
    // Reset the global phase-trace anchor so every event after this point
    // is timestamped relative to "format change accepted".
    dbg_set_anchor();
    dbg_event("format_change_accepted",
              "rate_hz=%u bits=%u channels=%u src_bpf=%u dst_bpf=%u pack24=%d first_open=%d cooldown_ms=%d "
              "open_gate_max_wait_ms=%d dsd=%d dsd_mult=%u dsd_real_rate=%u",
              rate, bits, ch, src_bpf, dst_bpf, df.pcm_needs_pack_24 ? 1 : 0,
              is_first_open ? 1 : 0, cooldown_ms, OPEN_GATE_MAX_WAIT_MS,
              df.is_dsd ? 1 : 0, df.dsd_multiplier, df.dsd_real_rate);

    teardown_sync_for_runtime("format change");

    // If a previous async open is still in flight, wait for it and
    // then discard whatever it produced — its FC / queue / format scalars
    // belong to the OLD format and cannot be used. The worker is
    // guaranteed to terminate in bounded time because all SDK calls in
    // open_sync_worker_blocking() have their own internal timeouts.
    if (g_st.async_open_thread.joinable()) {
        DLOG(1, "format change: awaiting in-flight async sync open before re-arming");
        g_st.async_open_thread.join();
        const int prev = g_st.async_open_state.load(std::memory_order_acquire);
        if (prev == AOS_DoneOk && g_st.async_open_pending_sync) {
            DLOG(1, "format change: discarding stale async-opened Sync from previous format");
            scream_diretta::ScreamDirettaSync* stale = g_st.async_open_pending_sync;
            g_st.async_open_pending_sync = nullptr;
            // The worker already activated this Sync (play() + activate())
            // before returning. Stop the SDK send thread and close the gate
            // so no in-flight getNewStream() is still inside the ring before
            // configure_unified_queue() resizes it below. cleanup_sync_async()
            // then finishes the asynchronous disconnect/close/delete.
            stale->deactivate();
            stale->stop();
            cleanup_sync_async(stale, "stale-async-open-on-format-change");
        }
        g_st.async_open_state.store(AOS_Idle, std::memory_order_release);
    }

    // Commit the derived format flags only after any in-flight async open
    // worker has joined. The worker reads these scalars during negotiation;
    // committing them before the join was a data race (and could drive a
    // stale worker to send a bogus setSinkConfigure when a format-change
    // packet arrived mid-handshake).
    g_st.is_dsd = df.is_dsd;
    g_st.dsd_multiplier = df.dsd_multiplier;
    g_st.dsd_real_rate = df.dsd_real_rate;
    g_st.pcm_needs_pack_24 = df.pcm_needs_pack_24;
    g_st.dsd_needs_bit_reverse.store(false, std::memory_order_release);
    g_st.dsd_needs_byte_swap.store(false, std::memory_order_release);
    g_st.async_open_dsd_needs_bit_reverse.store(false, std::memory_order_release);
    g_st.async_open_dsd_needs_byte_swap.store(false, std::memory_order_release);

    g_st.sample_rate = rate;
    g_st.channels = ch;
    g_st.bits_per_sample = bits;
    g_st.bytes_per_frame = dst_bpf;
    g_st.src_bytes_per_frame = src_bpf;
    g_st.last_fc = fc;
    g_st.have_last_fc = true;
    g_st.format_changes.fetch_add(1, std::memory_order_relaxed);
    g_st.reconfigure_pending = true;
    g_st.first_open_done = true;
    g_st.startup_overflow_risk_logged = false;
    const auto now_ts = std::chrono::steady_clock::now();
    g_st.reconfigure_ready_at = now_ts +
        std::chrono::milliseconds(cooldown_ms);
    g_st.open_gate_deadline_at = g_st.reconfigure_ready_at +
        std::chrono::milliseconds(OPEN_GATE_MAX_WAIT_MS);
    g_st.last_open_gate_log_ms = 0;
    g_st.reconnect_backoff_ms = 500;
    g_st.next_reconnect_at = g_st.reconfigure_ready_at;
    g_st.conn_lost_logged = false;

    // DSD state was committed from the DerivedFormat at the top of
    // reconfigure(); for PCM g_st.is_dsd is false. Set silence byte accordingly.
    const bool is_dsd = g_st.is_dsd;
    const uint8_t silence_byte = is_dsd ? DSD_SILENCE_BYTE : 0x00;

    // Build a fresh unified queue for the new format. PCM that
    // arrives during cooldown / open-gate / handshake will be written
    // into this queue directly; the SDK Sync will read from the same
    // queue once it's open. configure_unified_queue() also computes
    // the open-gate fill threshold for the new format.
    configure_unified_queue(rate, ch, bits / 8, dst_bpf, silence_byte);

    // Re-arm the ingress startup analyser for the new format. The
    // egress analyser and fader are armed inside configureFormat() of the
    // Sync at open time so they fire on each new Sync.
    pcm_startup_analyzer_configure(&g_st.ingress_analyzer,
                                   rate, ch, bits);

    // Re-arm the ingress-tap comparator for the new format. The
    // comparator captures the first compare_ingress_taps_ms ms of bytes
    // on each side; format change resets both buffers so the user gets
    // one summary line per format (matches the ingress analyser).
    // The raw-entry side observes the source wire format, so use src_bpf
    // (S24_LE stays 4 bytes per sample there; ingress side sees packed 3-byte).
    cmp_rearm_for_format(rate, ch, bits, src_bpf);

    const int active_ring_ms = g_st.is_dsd
        ? (g_st.cfg.dsd_buffer_ms > 0 ? g_st.cfg.dsd_buffer_ms : DSD_BUFFER_MS_DEFAULT)
        : (g_st.cfg.ring_buffer_ms > 0 ? g_st.cfg.ring_buffer_ms : 1000);
    const int active_prefill_ms = g_st.is_dsd
        ? (g_st.cfg.dsd_prefill_ms > 0 ? g_st.cfg.dsd_prefill_ms : DSD_PREFILL_MS_DEFAULT)
        : (g_st.cfg.prefill_ms > 0 ? g_st.cfg.prefill_ms : 500);
    DLOG(1, "queue sizing: rate=%u ch=%u bytes_per_sample=%u bytes_per_frame=%u "
         "ring_ms=%d prefill_ms=%d startup_queue_ms=%d",
         rate, ch, bits / 8, bpf,
         active_ring_ms,
         active_prefill_ms,
         g_st.cfg.startup_queue_ms);
    DLOG(1, "open sequencing: cooldown=%d ms (first_open=%d), then wait for queue fill "
         ">= %zu B before opening Sync (fallback open after %d ms more)",
         cooldown_ms, is_first_open ? 1 : 0,
         g_st.open_gate_threshold_bytes,
         OPEN_GATE_MAX_WAIT_MS);
    return true;
}

// Kick the non-blocking open. Tears down any old Sync (already
// non-blocking) and starts the async worker. Returns true if an open was
// scheduled (or one is already in flight). Returns false only when the
// preconditions are not met (no last_fc). The caller should then continue
// queue ingestion until the receive thread observes AOS_DoneOk and
// finalises the open via finalize_sync_open_on_receiver().
static bool try_reconnect_same_format(const char* reason) {
    if (!g_st.have_last_fc) return false;
    if (g_st.async_open_state.load(std::memory_order_acquire) == AOS_InProgress) {
        // An open is already in flight; don't tear anything down or
        // start a second one. The receive thread will pick up the
        // result on a subsequent call.
        DLOG(2, "try_reconnect_same_format: open already in flight (%s)",
             reason ? reason : "");
        return true;
    }
    if (g_st.sync) {
        teardown_sync_for_runtime("pre-reconnect cleanup");
    }
    const int inflight = g_inflight_cleanups.load(std::memory_order_acquire);
    if (inflight > 0) {
        DLOG(1, "reconnect proceeding with %d inflight cleanup(s) (%s)",
             inflight, reason ? reason : "sink lost");
    }
    g_st.reconnect_attempts.fetch_add(1, std::memory_order_relaxed);
    DLOG(0, "reconnecting to sink (attempt %llu, reason=%s, format=%u Hz, %u-bit, %u ch) "
         "[non-blocking — receive thread continues ingestion]",
         (unsigned long long)g_st.reconnect_attempts.load(std::memory_order_relaxed),
         reason ? reason : "sink lost",
         g_st.sample_rate, g_st.bits_per_sample, g_st.channels);
    if (!start_async_sync_open(g_st.last_fc, reason)) {
        return false;
    }
    g_st.conn_lost_logged = false;
    g_st.reconfigure_pending = false;
    return true;
}

// The receive thread polls async open completion and either installs the new
// Sync or arms reconnect backoff. Returns true if a transition happened.
static bool poll_async_sync_open() {
    const int st = g_st.async_open_state.load(std::memory_order_acquire);
    if (st == AOS_Idle || st == AOS_InProgress) return false;
    if (st == AOS_DoneOk) {
        std::atomic_thread_fence(std::memory_order_acquire);
        scream_diretta::ScreamDirettaSync* sync = g_st.async_open_pending_sync;
        g_st.async_open_pending_sync = nullptr;
        if (g_st.async_open_thread.joinable()) {
            g_st.async_open_thread.join();
        }
        if (!sync) {
            // Defensive: shouldn't happen, but treat as fail.
            g_st.async_open_state.store(AOS_Idle, std::memory_order_release);
            return true;
        }
        uint32_t accepted_bits = g_st.async_open_accepted_bits.load(std::memory_order_acquire);
        finalize_sync_open_on_receiver(sync, accepted_bits);
        g_st.async_open_state.store(AOS_Idle, std::memory_order_release);
        return true;
    }
    if (st == AOS_DoneFail) {
        if (g_st.async_open_thread.joinable()) {
            g_st.async_open_thread.join();
        }
        g_st.async_open_pending_sync = nullptr;
        DLOG(0, "async sync open failed (%s); will retry with backoff",
             g_st.async_open_reason.c_str());
        if (g_st.reconnect_backoff_ms < 1000) {
            g_st.reconnect_backoff_ms = std::min(1000u, g_st.reconnect_backoff_ms * 2);
        }
        g_st.next_reconnect_at = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(g_st.reconnect_backoff_ms);
        g_st.async_open_state.store(AOS_Idle, std::memory_order_release);
        // Also leave reconnect_pending armed so the next ingest cycle
        // retries naturally.
        g_st.reconnect_pending = true;
        return true;
    }
    return false;
}

// Poll the Sync's underrun event counter and rebuffer flag from the
// receive thread. When new underrun episodes are observed we emit a
// [diretta] underrun_begin line; when the rebuffer hold lifts we emit a
// matching underrun_recover line. Designed to run cheaply on every
// receive-thread call (one atomic load + one branch in the common case).
static void poll_underrun_events() {
    if (!g_st.sync) return;
    const uint64_t cur_events = g_st.sync->underrunEvents();
    const bool cur_rebuffering = g_st.sync->rebuffering();

    // Fast path: no state change → skip chrono::now() and bps math
    // entirely. In steady state this is hit on every receiver-thread
    // packet (~hundreds/sec) so deferring the vDSO clock_gettime call
    // until something actually changed measurably trims the per-packet
    // overhead.
    const bool any_change =
        (cur_events > g_st.last_observed_underrun_events) ||
        (g_st.rebuffering_logged && !cur_rebuffering);
    if (__builtin_expect(!any_change, 1)) return;

    const auto now = std::chrono::steady_clock::now();
    const uint64_t bps = static_cast<uint64_t>(g_st.sample_rate) *
                         static_cast<uint64_t>(g_st.bytes_per_frame);

    // underrun_begin: distinct new episode observed.
    if (cur_events > g_st.last_observed_underrun_events) {
        g_st.last_observed_underrun_events = cur_events;
        g_st.rebuffering_logged = true;
        g_st.underrun_begin_at = now;
        g_st.underrun_begin_silent_cycles = g_st.sync->silentCycles();
        g_st.underrun_begin_real_cycles   = g_st.sync->realCycles();

        const size_t fill = g_st.queue_ready ? g_st.queue.available() : 0;
        const size_t cap  = g_st.queue_ready ? g_st.queue.capacity()  : 0;
        uint64_t fill_ms = 0, cap_ms = 0;
        if (bps > 0) {
            fill_ms = (static_cast<uint64_t>(fill) * 1000ULL) / bps;
            cap_ms  = (static_cast<uint64_t>(cap)  * 1000ULL) / bps;
        }
        long long source_gap_ms = -1;
        if (g_st.have_last_pcm_packet_at) {
            source_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_st.last_pcm_packet_at).count();
        }
        // nic_gap_ms: time since the kernel timestamped the last PCM
        // packet at NIC arrival (SO_TIMESTAMPNS). Compared against
        // source_gap_ms it isolates upstream sender silence from local
        // wakeup latency: large nic_gap with similar source_gap means
        // the source went quiet; large source_gap with small nic_gap
        // means the kernel got the packet but our wakeup was late.
        // -1 = NIC timestamping disabled or no packet seen yet.
        long long nic_gap_ms = -1;
        if (g_st.last_pcm_nic_ts_ns != 0) {
            struct timespec now_rt;
            if (clock_gettime(CLOCK_REALTIME, &now_rt) == 0) {
                uint64_t now_ns = (uint64_t)now_rt.tv_sec * 1000000000ULL +
                                  (uint64_t)now_rt.tv_nsec;
                if (now_ns >= g_st.last_pcm_nic_ts_ns) {
                    nic_gap_ms = (long long)((now_ns - g_st.last_pcm_nic_ts_ns) / 1000000ULL);
                }
            }
        }
        const size_t rebuf_target = g_st.sync->rebufferTargetBytes();
        uint64_t rebuf_target_ms = 0;
        if (bps > 0) rebuf_target_ms = (static_cast<uint64_t>(rebuf_target) * 1000ULL) / bps;
        const bool producer_active = g_st.was_active_last_period;

        std::fprintf(stderr,
            "[diretta] underrun_begin: episode=%llu fill=%zu/%zu B "
            "(~%llu/%llu ms) source_gap_ms=%lld nic_gap_ms=%lld producer=%s "
            "stream_count=%llu real_cycles=%llu silent_cycles=%llu "
            "rebuffer_target=%zu B (~%llu ms)\n",
            (unsigned long long)cur_events,
            fill, cap,
            (unsigned long long)fill_ms,
            (unsigned long long)cap_ms,
            source_gap_ms,
            nic_gap_ms,
            producer_active ? "active" : "idle",
            (unsigned long long)g_st.sync->getStreamCount(),
            (unsigned long long)g_st.underrun_begin_real_cycles,
            (unsigned long long)g_st.underrun_begin_silent_cycles,
            rebuf_target, (unsigned long long)rebuf_target_ms);
        phase_event("underrun_begin",
                    "episode=%llu fill_ms=%llu source_gap_ms=%lld "
                    "nic_gap_ms=%lld producer=%s rebuffer_target_ms=%llu",
                    (unsigned long long)cur_events,
                    (unsigned long long)fill_ms,
                    source_gap_ms,
                    nic_gap_ms,
                    producer_active ? "active" : "idle",
                    (unsigned long long)rebuf_target_ms);
    }

    // underrun_recover: rebuffer flag went from set to clear.
    if (g_st.rebuffering_logged && !cur_rebuffering) {
        g_st.rebuffering_logged = false;
        const size_t fill = g_st.queue_ready ? g_st.queue.available() : 0;
        uint64_t fill_ms = 0;
        if (bps > 0) fill_ms = (static_cast<uint64_t>(fill) * 1000ULL) / bps;
        long long silent_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_st.underrun_begin_at).count();
        long long source_gap_ms = -1;
        if (g_st.have_last_pcm_packet_at) {
            source_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_st.last_pcm_packet_at).count();
        }
        long long nic_gap_ms = -1;
        if (g_st.last_pcm_nic_ts_ns != 0) {
            struct timespec now_rt;
            if (clock_gettime(CLOCK_REALTIME, &now_rt) == 0) {
                uint64_t now_ns = (uint64_t)now_rt.tv_sec * 1000000000ULL +
                                  (uint64_t)now_rt.tv_nsec;
                if (now_ns >= g_st.last_pcm_nic_ts_ns) {
                    nic_gap_ms = (long long)((now_ns - g_st.last_pcm_nic_ts_ns) / 1000000ULL);
                }
            }
        }
        const uint64_t silent_delta = g_st.sync->silentCycles() -
                                       g_st.underrun_begin_silent_cycles;
        const uint64_t real_delta   = g_st.sync->realCycles() -
                                       g_st.underrun_begin_real_cycles;
        std::fprintf(stderr,
            "[diretta] underrun_recover: episode=%llu fill=%zu B "
            "(~%llu ms) silent_ms=%lld source_gap_ms=%lld nic_gap_ms=%lld "
            "producer=%s silent_cycles_delta=%llu real_cycles_delta=%llu\n",
            (unsigned long long)cur_events,
            fill, (unsigned long long)fill_ms,
            silent_ms, source_gap_ms, nic_gap_ms,
            g_st.was_active_last_period ? "active" : "idle",
            (unsigned long long)silent_delta,
            (unsigned long long)real_delta);
        phase_event("underrun_recover",
                    "episode=%llu fill_ms=%llu silent_ms=%lld "
                    "source_gap_ms=%lld nic_gap_ms=%lld",
                    (unsigned long long)cur_events,
                    (unsigned long long)fill_ms,
                    silent_ms, source_gap_ms, nic_gap_ms);
    }
}

static void format_stats_line(const char* tag, char* out, size_t outlen) {
    diretta_stats_t s{};
    diretta_get_stats(&s);
    // Per-interval delta accounting. push_delta_ms is how much real PCM
    // we pushed since the last stats line; drain_delta_ms is how much real
    // PCM the SDK send thread popped (silence cycles do not count toward
    // drain). net_fill_delta_ms is push - drain in ms and should track the
    // change in ring_fill_ms; if it diverges meaningfully from
    // (cur_fill - prev_fill) the ring went through underflow (popOrSilence
    // padded with silence) or overflow (push dropped).
    const uint64_t bps = static_cast<uint64_t>(g_st.sample_rate) *
                         static_cast<uint64_t>(g_st.bytes_per_frame);
    long long push_delta_ms = -1;
    long long drain_delta_ms = -1;
    long long net_fill_delta_ms = 0;
    long long fill_delta_ms = 0;
    if (g_st.have_last_stats_snapshot && bps > 0) {
        const uint64_t push_delta_b = (s.pushed_bytes >= g_st.last_stats_pushed_bytes)
            ? (s.pushed_bytes - g_st.last_stats_pushed_bytes) : 0;
        const uint64_t drain_delta_b = (s.popped_bytes >= g_st.last_stats_popped_bytes)
            ? (s.popped_bytes - g_st.last_stats_popped_bytes) : 0;
        push_delta_ms  = (long long)((push_delta_b  * 1000ULL) / bps);
        drain_delta_ms = (long long)((drain_delta_b * 1000ULL) / bps);
        net_fill_delta_ms = push_delta_ms - drain_delta_ms;
        const long long cur_fill_b  = (long long)s.ring_fill_bytes;
        const long long prev_fill_b = (long long)g_st.last_stats_ring_fill_bytes;
        fill_delta_ms = (long long)(((cur_fill_b - prev_fill_b) * 1000) / (long long)bps);
    }
    std::snprintf(out, outlen,
        "[diretta] %s: pushed=%llu B / %llu fr | dropped=%llu B / %llu fr / %llu ms"
        " | partial_carry=%llu | fmt_changes=%llu | underruns=%llu (events=%llu)"
        " | fill=%llu/%llu B (~%llu ms) | cycles real=%llu silent=%llu"
        " | push_delta_ms=%lld drain_delta_ms=%lld net_fill_delta_ms=%lld "
        "fill_delta_ms=%lld%s",
        tag,
        (unsigned long long)s.pushed_bytes,
        (unsigned long long)s.pushed_frames,
        (unsigned long long)s.dropped_bytes,
        (unsigned long long)s.dropped_frames,
        (unsigned long long)s.dropped_ms,
        (unsigned long long)s.partial_carry_count,
        (unsigned long long)s.format_changes,
        (unsigned long long)s.underruns,
        (unsigned long long)s.underrun_events,
        (unsigned long long)s.ring_fill_bytes,
        (unsigned long long)s.ring_capacity_bytes,
        (unsigned long long)s.ring_fill_ms,
        (unsigned long long)s.real_cycles,
        (unsigned long long)s.silent_cycles,
        push_delta_ms, drain_delta_ms, net_fill_delta_ms, fill_delta_ms,
        s.dsd_active ? " | dsd" : "");

    g_st.last_stats_pushed_bytes     = s.pushed_bytes;
    g_st.last_stats_popped_bytes     = s.popped_bytes;
    g_st.last_stats_ring_fill_bytes  = s.ring_fill_bytes;
    g_st.have_last_stats_snapshot    = true;
}

static void maybe_print_periodic_stats() {
    if (!g_st.stats_print_armed) return;
    auto now = std::chrono::steady_clock::now();
    if (now < g_st.next_stats_print) return;

    diretta_stats_t s_now{};
    diretta_get_stats(&s_now);
    const bool active = (s_now.pushed_frames > g_st.last_stats_pushed_frames);

    const char* tag;
    if (g_st.reconfigure_pending) {
        tag = "stats[reconfiguring]";
    } else if (!g_st.sync || !g_st.sink_active) {
        tag = "stats[reconnecting]";
    } else if (active && !g_st.stream_started) {
        tag = "stats[reconnecting]";
    } else if (active) {
        tag = "stats[producer-active]";
    } else if (g_st.stream_started) {
        tag = "stats[producer-idle]";
    } else {
        tag = "stats[idle]";
    }

    if (verbosity >= 1) {
        if (active && !g_st.was_active_last_period) {
            DLOG(1, "producer idle -> producer active (pushed_frames=%llu)",
                 (unsigned long long)s_now.pushed_frames);
        } else if (!active && g_st.was_active_last_period) {
            DLOG(1, "producer active -> producer idle (pushed_frames=%llu, "
                 "stream_started=%d -- SDK pipeline state unchanged)",
                 (unsigned long long)s_now.pushed_frames,
                 g_st.stream_started ? 1 : 0);
        }
    }

    char line[512];
    format_stats_line(tag, line, sizeof(line));
    std::fprintf(stderr, "%s\n", line);

    g_st.last_stats_pushed_frames = s_now.pushed_frames;
    g_st.was_active_last_period   = active;
    g_st.next_stats_print = now + std::chrono::seconds(g_st.cfg.stats_interval_sec);
}

// -vv: emit prefill-gate progress lines while the gate is still closed.
// Throttled to 200ms intervals so it doesn't flood the log during a long
// priming window (slow format change, sparse PCM). Also emits a single
// "transition to playing" line when the gate first opens.
static void maybe_log_prefill_progress() {
    if (verbosity < 2 || !g_st.sync) return;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Log the mute-gate completion exactly once. After it completes,
    // the prefill gate is allowed to open (it will the next time the SDK
    // observes ring fill >= threshold).
    if (!g_st.mute_logged_complete && g_st.sync->muteDone()) {
        g_st.mute_logged_complete = true;
        DLOG(2, "startup mute complete; real PCM gate may open once "
             "fill >= %zu B (current fill=%zu B / %llu ms, "
             "silent cycles so far=%llu)",
             g_st.sync->prefillBytes(), g_st.sync->ringFill(),
             (unsigned long long)g_st.sync->ringFillMs(),
             (unsigned long long)g_st.sync->silentCycles());
    }
    const bool gate_open = g_st.sync->prefillDone();
    if (gate_open) {
        if (!g_st.prefill_logged_open) {
            g_st.prefill_logged_open = true;
            DLOG(2, "prefill gate: OPENED — getNewStream now plays real PCM "
                 "(fill=%zu/%zu B ~%llu ms, threshold=%zu B)",
                 g_st.sync->ringFill(), g_st.sync->ringBytes(),
                 (unsigned long long)g_st.sync->ringFillMs(),
                 g_st.sync->prefillBytes());
        }
        return;
    }
    if (now_ms - g_st.last_prefill_log_ms < 200) return;
    g_st.last_prefill_log_ms = now_ms;
    DLOG(2, "prefill gate: closed (priming) — fill=%zu/%zu B (~%llu ms) "
         "threshold=%zu B; silent cycles so far=%llu",
         g_st.sync->ringFill(), g_st.sync->ringBytes(),
         (unsigned long long)g_st.sync->ringFillMs(),
         g_st.sync->prefillBytes(),
         (unsigned long long)g_st.sync->silentCycles());
}

} // namespace

extern "C" int diretta_apply_cpu_affinity(int core) {
    if (core < 0) return 0;
#if defined(__linux__)
    if (core >= CPU_SETSIZE) {
        std::cerr << "[diretta] WARNING: CPU core index " << core
                  << " exceeds CPU_SETSIZE (" << CPU_SETSIZE << ")" << std::endl;
        return -1;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        std::cerr << "[diretta] WARNING: Failed to pin thread to CPU core "
                  << core << " (errno=" << errno << ")" << std::endl;
        return -1;
    }
    std::cout << "[diretta] Thread pinned to CPU core " << core << std::endl;
    return 0;
#else
    (void)core;
    return 0;
#endif
}

extern "C" int diretta_apply_rt_priority(int priority) {
    if (priority < 1) return 0;
#if defined(__linux__)
    struct sched_param param;
    param.sched_priority = priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::cerr << "[diretta] WARNING: Failed to set SCHED_FIFO priority "
                  << priority << " (errno=" << errno << ")" << std::endl;
        return -1;
    }
    std::cout << "[diretta] Thread set to SCHED_FIFO priority " << priority << std::endl;
    return 0;
#else
    (void)priority;
    return 0;
#endif
}

extern "C" void diretta_config_init(diretta_config_t *cfg) {
    if (!cfg) return;
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->target_index = 0;
    cfg->target_buffer_ms = 0;  // 0 = let SDK use default sink buffer time
    cfg->thread_mode = 1u; // CRITICAL
    cfg->cycle_us = 0;
    cfg->cycle_min_us = 0;
    cfg->info_cycle_us = 100000;
    cfg->transfer_mode = DIRETTA_TM_AUTO;
    cfg->target_profile_limit_us = 0;
    cfg->mtu_override = 0;
    cfg->ring_buffer_ms = 1000;
    cfg->prefill_ms = 500;
    cfg->rebuffer_percent = 0.50f;
    // 0 = fall back to rebuffer_percent.
    // User can opt in to a smaller, faster recovery with
    // --underrun-rebuffer-ms 100..200.
    cfg->underrun_rebuffer_ms = 0;
    // startup_queue_ms defaults to 0. The format-specific prefill controls
    // the open-fill gate by default; --startup-queue-ms is an explicit
    // advanced override that can require a deeper queue at fresh Sync open
    // without changing the PCM/DSD prefill defaults.
    cfg->startup_queue_ms = 0;
    // startup_mute_ms is a compatibility diagnostic and DEFAULT OFF.
    // The current open gate avoids muting real PCM after Sync open.
    // the click by delaying the open itself. The CLI flag is still
    // parsed for back-compat but is not recommended.
    cfg->startup_mute_ms = 0;
    // Post-play "real delay" silent window (ms). Default 0 = disabled
    // Unlike --startup-mute-ms, the
    // unified queue is NOT consumed during this window, so the head of the
    // track is preserved. Diagnostic option to absorb target/DAC
    // stabilization artifacts into silence after play.
    cfg->startup_real_delay_ms = 0;
    // Default format-change cooldown.
    // 200 ms has proven sufficient for tested Diretta targets while keeping
    // DSD256/DSD512 format-change accumulation within the DSD ring budget.
    cfg->format_change_cooldown_ms = FORMAT_CHANGE_COOLDOWN_MS_DEFAULT;
    // Upstream-idle release: tear down the Sync after this many seconds with
    // no real PCM so the Target stops receiving a long-term silence stream.
    // 0 = disabled. Default 120 s (2 min).
    cfg->upstream_idle_timeout_sec = 120;
    cfg->upstream_pause_timeout_sec = 5;
    // DSD defaults aligned with DRUP / slim2Diretta conventions.
    cfg->dsd_buffer_ms = DSD_BUFFER_MS_DEFAULT;
    cfg->dsd_prefill_ms = DSD_PREFILL_MS_DEFAULT;
    cfg->dsd_startup_warmup_ms = 50;
    // CPU affinity: -1 = disabled (default). Applied when >= 0.
    cfg->cpu_scream = -1;
    cfg->cpu_audio  = -1;
    cfg->cpu_other  = -1;
    // Real-time priority: -1 = disabled (default). SCHED_FIFO when 1..99.
    cfg->rt_priority = -1;
    // Compatibility knobs. Kept so the CLI parses old scripts
    // without error; ignored by the unified-queue path.
    cfg->stats_interval_sec = 5;
    cfg->stats_enabled = 0;
    cfg->log_level = DIRETTA_LOG_DEFAULT;
    // Compatibility packet capacity. The current hot path uses PcmRing.
    // packets at ~1152 bytes payload covers ~50..150 ms depending on the
    // active format and the wire packet rate; deep enough to ride out a
    // single scheduler stall, small enough to fit in L1/L2 footprint.
    // UDP socket SO_RCVBUF. Stored here for the stats banner only;
    // the actual socket option is applied in init_network(). Default is
    // 4 MiB (set in main() when the backend is Diretta).
    cfg->udp_rcvbuf_bytes = 0;
    cfg->diretta_debug = 0;
    // PCM dump diagnostics. Both prefixes default to NULL (disabled);
    // the CLI sets them when --dump-ingress-wav / --dump-egress-wav is
    // passed. dump_ms is the per-file cap; default 0 = uncapped (the CLI
    // raises this to 3000 ms when any dump option is enabled and the user
    // did not pass --dump-ms explicitly).
    cfg->dump_ingress_prefix = nullptr;
    cfg->dump_egress_prefix  = nullptr;
    cfg->dump_ms = 0;
    // Raw-entry tap + ingress-tap byte comparator. Both default to
    // disabled. The raw-entry
    // tap captures data->audio raw (the exact bytes `-o stdout` would
    // write) at the entry of diretta_output_send so the user can prove
    // whether the click is present in the wire bytes or introduced by
    // the Diretta queue/SDK path.
    cfg->dump_raw_entry_prefix = nullptr;
    cfg->compare_ingress_taps_ms   = 0;
    // Startup analysis + optional egress fade. Disabled by default so
    // normal audio path is unchanged. The CLI may auto-enable the
    // analyser at 100 ms when verbosity >= 2 OR any --dump-*-wav is set
    // (still configurable via --startup-analyze-ms 0).
    cfg->startup_analyze_ms = 0;
    cfg->startup_fade_ms    = 0;
    cfg->startup_fade_shape = 0; /* linear */
}

extern "C" int diretta_list_targets(const diretta_config_t *cfg, const char *progname) {
    diretta_config_t local;
    if (cfg) local = *cfg; else diretta_config_init(&local);

    init_syslog(local);

    Find::Setting fset;
    fset.Loopback = false;
    fset.ProductID = 0;

    Find finder(fset);
    if (!finder.open()) {
        std::fprintf(stderr, "[diretta] Find::open() failed\n");
        return 1;
    }

    std::vector<DiscoveredTarget> targets;
    if (!enumerate_targets(finder, targets)) {
        std::fprintf(stderr, "[diretta] target enumeration failed\n");
        finder.close();
        return 1;
    }

    if (targets.empty()) {
        std::printf("No Diretta targets discovered.\n");
        finder.close();
        return 0;
    }

    std::printf("\n════════════════════════════════════════════════════════\n");
    std::printf("  Scanning for Diretta Targets...\n");
    std::printf("════════════════════════════════════════════════════════\n\n");
    std::printf("Available Diretta Targets (%zu found):\n\n", targets.size());

    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        const std::string addr = ip_to_str(t.portAddr);
        const std::string targetName = t.info.targetName.empty()
            ? (t.info.outputName.empty() ? "<unnamed>" : t.info.outputName)
            : t.info.targetName;
        const std::string& outputName = t.info.outputName;

        uint32_t mtu = 0;
        finder.measSendMTU(t.portAddr, mtu);

        std::printf("[%zu] %s\n", i + 1, targetName.c_str());
        if (!outputName.empty() && outputName != targetName) {
            std::printf("    Output: %s\n", outputName.c_str());
        }
        std::printf("    Address: %s", addr.c_str());
        if (mtu > 0) {
            std::printf("  MTU: %u", mtu);
        }
        std::printf("\n");
        std::printf("    Port: IN=%u OUT=%u",
                    static_cast<unsigned>(t.info.PI),
                    static_cast<unsigned>(t.info.PO));
        if (t.info.multiport) {
            std::printf(" (multiport)");
        }
        std::printf("\n");
        std::printf("    Version: %u\n",
                    static_cast<unsigned>(t.info.version));
        std::printf("    ProductID: 0x%016llX\n",
                    static_cast<unsigned long long>(t.info.productID));
        if (i + 1 < targets.size()) {
            std::printf("\n");
        }
    }

    const char *bin = progname && progname[0] ? progname : "scream2diretta";
    std::printf("\nUsage:\n");
    for (size_t i = 0; i < targets.size(); ++i) {
        std::printf("   Target #%zu: sudo %s --target %zu\n",
                    i + 1, bin, i + 1);
    }
    std::printf("\n");

    finder.close();
    return 0;
}

extern "C" int diretta_output_init(const diretta_config_t *cfg) {
    try {
    if (g_st.initialized) return 0;
    if (cfg) g_st.cfg = *cfg; else diretta_config_init(&g_st.cfg);

    init_syslog(g_st.cfg);

    if (g_st.cfg.diretta_debug) {
        // Announce the debug subsystem up front. SDK debug output still
        // depends on whether a -nolog SDK archive was linked.
        std::fprintf(stderr,
            "[diretta-debug] enabled; sdk_debug=yes; phase_trace=yes; "
            "anchor=format_change_accepted (and sync_open_begin); "
            "events on stderr with [diretta-debug] prefix\n");
    }

    Find::Setting fset;
    fset.Loopback = false;
    fset.ProductID = 0;
    g_st.finder = new Find(fset);
    if (!g_st.finder->open()) {
        std::fprintf(stderr, "[diretta] Find::open() failed\n");
        cleanup_finder();
        return 1;
    }

    std::vector<DiscoveredTarget> targets;
    if (!enumerate_targets(*g_st.finder, targets) || targets.empty()) {
        std::fprintf(stderr, "[diretta] no Diretta target discovered\n");
        cleanup_finder();
        return 1;
    }

    size_t pick = 0;
    if (g_st.cfg.target_index > 0) {
        if ((size_t)g_st.cfg.target_index > targets.size()) {
            std::fprintf(stderr,
                "[diretta] --target %d out of range (only %zu target(s) discovered).\n"
                "          Run with --list-targets to see available targets.\n",
                g_st.cfg.target_index, targets.size());
            cleanup_finder();
            return 1;
        }
        pick = (size_t)g_st.cfg.target_index - 1;
    }

    g_st.sink_addr = targets[pick].portAddr;

    // Load cached inferred overhead for this target, if available.
    g_st.inferred_overhead = load_inferred_overhead(g_st.sink_addr);
    if (g_st.inferred_overhead > 0) {
        DLOG(1, "loaded cached overhead: %d for %s",
             g_st.inferred_overhead, ip_to_str(g_st.sink_addr).c_str());
    }

    if (g_st.cfg.mtu_override > 0) {
        g_st.mtu = (uint32_t)g_st.cfg.mtu_override;
    } else {
        if (!g_st.finder->measSendMTU(g_st.sink_addr, g_st.mtu)) {
            std::fprintf(stderr, "[diretta] MTU measurement failed\n");
            cleanup_finder();
            return 1;
        }
    }
    DLOG(1, "selected target #%zu, MTU=%u", pick + 1, g_st.mtu);
    if (g_st.cfg.diretta_debug) {
        const Find::TargetConnectInfo& ti = targets[pick].info;
        std::string tname = ti.targetName.empty() ? std::string("<unnamed>") : ti.targetName;
        std::string oname = ti.outputName.empty() ? std::string("<unnamed>") : ti.outputName;
        std::fprintf(stderr,
            "[diretta-debug] target_selected: index=%zu addr=%s name=%s output=%s "
            "productID=0x%llx PI=%u PO=%u version=%u multiport=%d mtu=%u\n",
            pick + 1, ip_to_str(g_st.sink_addr).c_str(),
            tname.c_str(), oname.c_str(),
            (unsigned long long)ti.productID,
            (unsigned)ti.PI, (unsigned)ti.PO, (unsigned)ti.version,
            ti.multiport ? 1 : 0, (unsigned)g_st.mtu);
    }
    std::fprintf(stderr,
        "[pipeline] SO_RCVBUF -> receiver_data_t -> diretta_output_send() "
        "-> PcmRing -> getNewStream() -> Diretta SDK. Sync open is non-blocking; "
        "the receiver keeps pushing PCM into PcmRing while SDK open / setSink / "
        "connectWait / play runs on a worker thread.\n");
    // Diretta-facing config summary. Only the knobs passed to the SDK.
    {
        const char* tmode_name = "auto";
        switch (g_st.cfg.transfer_mode) {
            case DIRETTA_TM_AUTO:    tmode_name = "auto"; break;
            case DIRETTA_TM_VARMAX:  tmode_name = "varmax"; break;
            case DIRETTA_TM_VARAUTO: tmode_name = "varauto"; break;
            case DIRETTA_TM_FIXAUTO: tmode_name = "fixauto"; break;
            case DIRETTA_TM_RANDOM:  tmode_name = "random"; break;
            case DIRETTA_TM_AUTOFIX: tmode_name = "autofix"; break;
        }
        std::fprintf(stderr,
            "[diretta] SDK config: target_buffer_ms=%d (setSink buffer) "
            "thread_mode=0x%x cycle_time_us=%d cycle_min_time_us=%d "
            "info_cycle_us=%d transfer_mode=%s target_profile_limit_us=%d "
            "mtu_override=%d\n",
            g_st.cfg.target_buffer_ms,
            g_st.cfg.thread_mode != 0 ? g_st.cfg.thread_mode : 1u,
            g_st.cfg.cycle_us, g_st.cfg.cycle_min_us,
            g_st.cfg.info_cycle_us, tmode_name,
            g_st.cfg.target_profile_limit_us,
            g_st.cfg.mtu_override);
    }
    // Pipeline / queue config. Not passed to the SDK; controls the receiver
    // side of the unified queue and open sequencing.
    std::fprintf(stderr,
        "[pipeline] config: pcm_buffer_ms=%d pcm_prefill_ms=%d "
        "rebuffer_percent=%.0f%% underrun_rebuffer_ms=%d (0=use percent) "
        "format_change_cooldown_ms=%d udp_rcvbuf_bytes=%d "
        "startup_real_delay_ms=%d (0=disabled) "
        "upstream_idle_timeout_sec=%d (0=disabled) "
        "upstream_pause_timeout_sec=%d (0=disabled)\n",
        g_st.cfg.ring_buffer_ms > 0 ? g_st.cfg.ring_buffer_ms : 1000,
        g_st.cfg.prefill_ms > 0 ? g_st.cfg.prefill_ms : 500,
        g_st.cfg.rebuffer_percent * 100.0f,
        g_st.cfg.underrun_rebuffer_ms,
        effective_cooldown_ms(),
        g_st.cfg.udp_rcvbuf_bytes,
        g_st.cfg.startup_real_delay_ms,
        g_st.cfg.upstream_idle_timeout_sec,
        g_st.cfg.upstream_pause_timeout_sec);

    // Warn if PcmRing is too small to absorb the format-change cooldown plus
    // expected SDK open latency. This gives the user a startup warning rather
    // than silent drops at the first format change.
    {
        const int expected_open_ms = 500; // conservative budget
        const int total_budget_ms = effective_cooldown_ms() + expected_open_ms;
        const int ring_ms = g_st.cfg.ring_buffer_ms > 0 ? g_st.cfg.ring_buffer_ms : 1000;
        if (total_budget_ms > ring_ms) {
            std::fprintf(stderr,
                "[diretta] WARN: PcmRing (--pcm-buffer-ms %d) is smaller "
                "than format_change_cooldown_ms(%d) + expected_open_ms(%d) = "
                "%d ms. On a format change PcmRing may overflow "
                "before the SDK begins pulling, causing audible drops. "
                "Consider --pcm-buffer-ms %d or --format-change-cooldown-ms %d.\n",
                ring_ms, effective_cooldown_ms(), expected_open_ms,
                total_budget_ms,
                total_budget_ms + 200,
                ring_ms - expected_open_ms > 0 ? ring_ms - expected_open_ms : 100);
        } else {
            DLOG(1, "PcmRing budget check: pcm_buffer_ms=%d covers "
                 "cooldown(%d)+expected_open(%d)=%d ms (headroom=%d ms)",
                 ring_ms, effective_cooldown_ms(), expected_open_ms,
                 total_budget_ms, ring_ms - total_budget_ms);
        }
    }
    DLOG(1, "tuning: ring_buffer_ms=%d prefill_ms=%d startup_queue_ms=%d "
         "startup_mute_ms=%d (compatibility diagnostic, default 0) "
         "rebuffer_percent=%.0f%% "
         "underrun_rebuffer_ms=%d (0=use rebuffer_percent) "
         "startup_real_delay_ms=%d (0=disabled; silence after play without "
         "consuming queue)",
         g_st.cfg.ring_buffer_ms, g_st.cfg.prefill_ms,
         g_st.cfg.startup_queue_ms,
         g_st.cfg.startup_mute_ms,
         g_st.cfg.rebuffer_percent * 100.0f,
         g_st.cfg.underrun_rebuffer_ms,
         g_st.cfg.startup_real_delay_ms);
    DLOG(1, "open sequencing: cooldown=%d ms (configurable via "
         "--format-change-cooldown-ms; first open skips cooldown entirely), "
         "then wait for queue fill >= max(prefill_ms, startup_queue_ms) "
         "before opening Sync. Hard fallback after %d ms. No startup mute "
         "applied post-open by default.",
         effective_cooldown_ms(), OPEN_GATE_MAX_WAIT_MS);
    if (g_st.cfg.startup_mute_ms > 0) {
        std::fprintf(stderr,
            "[diretta] WARN: --startup-mute-ms=%d is set; this is a "
            "compatibility diagnostic knob that mutes real PCM "
            "AFTER the Sync opens. the current open gate defers Sync open instead — using "
            "both together will hide the head of the audio twice. "
            "Recommend leaving --startup-mute-ms at 0.\n",
            g_st.cfg.startup_mute_ms);
    }

#ifdef SCREAM2DIRETTA_NO_DIAGNOSTICS
    // Production binary: diagnostic facilities are compile-time disabled.
    // Warn the user once if any flag was requested, then null out the
    // cfg fields so the diagnostic init calls below produce inert dumpers
    // / disabled analysers without further special-casing. Every per-packet
    // call site that touches these is gated by diretta_diag_armed(), which
    // folds to constant 0 here and is DCE'd at the call site.
    {
        const bool any_diag_requested =
            (g_st.cfg.dump_ingress_prefix && g_st.cfg.dump_ingress_prefix[0]) ||
            (g_st.cfg.dump_egress_prefix && g_st.cfg.dump_egress_prefix[0]) ||
            (g_st.cfg.dump_raw_entry_prefix && g_st.cfg.dump_raw_entry_prefix[0]) ||
            (g_st.cfg.startup_analyze_ms > 0) ||
            (g_st.cfg.startup_fade_ms > 0) ||
            (g_st.cfg.compare_ingress_taps_ms > 0);
        if (any_diag_requested) {
            std::fprintf(stderr,
                "[diretta] warning: diagnostics not compiled into this "
                "binary (SCREAM2DIRETTA_NO_DIAGNOSTICS). "
                "--dump-ingress-wav / --dump-egress-wav / "
                "--dump-raw-entry-wav / --startup-analyze-ms / "
                "--startup-fade-ms / --compare-ingress-taps-ms are inert. "
                "Rebuild as scream2diretta-debug to enable them.\n");
        }
        g_st.cfg.dump_ingress_prefix = nullptr;
        g_st.cfg.dump_egress_prefix = nullptr;
        g_st.cfg.dump_raw_entry_prefix = nullptr;
        g_st.cfg.startup_analyze_ms = 0;
        g_st.cfg.startup_fade_ms = 0;
        g_st.cfg.compare_ingress_taps_ms = 0;
    }
#endif

    // PCM dump diagnostics. Files open lazily when the first PCM with a known
    // format arrives.
    pcm_dumper_init(&g_st.ingress_dumper,
                    g_st.cfg.dump_ingress_prefix,
                    "ingress", g_st.cfg.dump_ms);
    pcm_dumper_init(&g_st.egress_dumper,
                    g_st.cfg.dump_egress_prefix,
                    "egress",  g_st.cfg.dump_ms);
    // Raw-entry tap. Captures data->audio at the entry of diretta_output_send,
    // before frame alignment, partial carry, and PcmRing ingress.
    pcm_dumper_init(&g_st.raw_entry_dumper,
                    g_st.cfg.dump_raw_entry_prefix,
                    "raw-entry", g_st.cfg.dump_ms);
    if (pcm_dumper_enabled(&g_st.raw_entry_dumper)) {
        std::fprintf(stderr,
            "[diretta] raw-entry tap enabled: prefix=%s. "
            "Captures data->audio at the entry of diretta_output_send -- the "
            "exact byte stream that `-o stdout` (raw.c) would have written. "
            "WAV bit depth matches source (16/24/32-bit follows sample_size). "
            "Use --compare-ingress-taps-ms to byte-compare against "
            "--dump-ingress-wav.\n",
            g_st.cfg.dump_raw_entry_prefix);
    }
    if (g_st.cfg.compare_ingress_taps_ms > 0) {
        if (!pcm_dumper_enabled(&g_st.raw_entry_dumper) ||
            !pcm_dumper_enabled(&g_st.ingress_dumper)) {
            std::fprintf(stderr,
                "[diretta] WARN: --compare-ingress-taps-ms requires BOTH "
                "--dump-raw-entry-wav and --dump-ingress-wav to be set; "
                "comparator will be inert.\n");
        } else {
            std::fprintf(stderr,
                "[diretta] ingress-tap comparator armed: window=%d ms. "
                "Both raw-entry tap and ingress tap will be captured into "
                "RAM and byte-compared on the first format/open. Result is a "
                "single stderr summary line.\n",
                g_st.cfg.compare_ingress_taps_ms);
        }
    }
    if (pcm_dumper_enabled(&g_st.ingress_dumper) ||
        pcm_dumper_enabled(&g_st.egress_dumper)) {
        std::fprintf(stderr,
            "[diretta] PCM dump enabled: ingress=%s egress=%s dump_ms=%d "
            "(per-file cap; 0=uncapped). Ingress captures the queue input "
            "(post frame-align/partial-carry); egress captures real-PCM "
            "popped from the queue and handed to the SDK (silence emitted "
            "by prefill/startup-real-delay/mute gates is NOT written). "
            "When --startup-fade-ms > 0, egress dump captures POST-fade "
            "PCM (the same bytes the SDK actually receives). Ingress dump "
            "is NEVER modified by the fade.\n",
            g_st.cfg.dump_ingress_prefix ? g_st.cfg.dump_ingress_prefix : "(off)",
            g_st.cfg.dump_egress_prefix  ? g_st.cfg.dump_egress_prefix  : "(off)",
            g_st.cfg.dump_ms);
    }

    // Startup analysers and optional egress fader. The analysers learn the
    // format lazily from configure calls at format-change / Sync open.
    pcm_startup_analyzer_init(&g_st.ingress_analyzer,
                              "ingress", g_st.cfg.startup_analyze_ms,
                              verbosity);
    pcm_startup_analyzer_init(&g_st.egress_analyzer,
                              "egress",  g_st.cfg.startup_analyze_ms,
                              verbosity);
    pcm_startup_fader_init(&g_st.egress_fader,
                           "egress",
                           g_st.cfg.startup_fade_ms,
                           (pcm_fade_shape_t)g_st.cfg.startup_fade_shape);
    if (g_st.cfg.startup_analyze_ms > 0 || g_st.cfg.startup_fade_ms > 0) {
        std::fprintf(stderr,
            "[diretta] startup analysis/fade: analyze_ms=%d fade_ms=%d "
            "fade_shape=%s. Analysis runs over the first <analyze_ms> of "
            "real PCM at ingress AND egress after each format/open and "
            "emits a single summary line per side. Fade applies a 0..1 "
            "ramp IN PLACE to the first <fade_ms> of real PCM at egress "
            "after each format/open. Ingress dump is never modified by "
            "the fade; egress dump reflects post-fade PCM.\n",
            g_st.cfg.startup_analyze_ms, g_st.cfg.startup_fade_ms,
            (g_st.cfg.startup_fade_shape == 1) ? "cosine" : "linear");
    }

    // Compute the single-point Diretta-diag armed gate. All inputs are
    // finalised at this point. The gate is consulted by queue_push_frames(),
    // the raw-entry tap in diretta_output_send(), and the egress-side feeds
    // in ScreamDirettaSync::getNewStream(). In SCREAM2DIRETTA_NO_DIAGNOSTICS
    // builds the accessor folds to constant 0 and every guarded block is
    // dead-code-eliminated, so the per-packet hot path costs zero
    // instructions for these diagnostics. (The flag symbol itself is also
    // not defined in that build; see diretta_diag.h.)
#ifndef SCREAM2DIRETTA_NO_DIAGNOSTICS
    g_diretta_diag_armed_flag =
        (pcm_dumper_enabled(&g_st.ingress_dumper) ||
         pcm_dumper_enabled(&g_st.egress_dumper) ||
         pcm_dumper_enabled(&g_st.raw_entry_dumper) ||
         g_st.cfg.startup_analyze_ms > 0 ||
         g_st.cfg.startup_fade_ms > 0 ||
         g_st.cfg.compare_ingress_taps_ms > 0) ? 1 : 0;
    DLOG(1, "diretta-diag gate: armed=%d (ingress_dump=%d egress_dump=%d "
         "raw_entry_dump=%d analyze_ms=%d fade_ms=%d compare_ms=%d)",
         g_diretta_diag_armed_flag,
         pcm_dumper_enabled(&g_st.ingress_dumper),
         pcm_dumper_enabled(&g_st.egress_dumper),
         pcm_dumper_enabled(&g_st.raw_entry_dumper),
         g_st.cfg.startup_analyze_ms, g_st.cfg.startup_fade_ms,
         g_st.cfg.compare_ingress_taps_ms);
#endif

    // Preallocate format-conversion scratch buffers once. 24-bit packing
    // (S24_LE -> S24_3LE) and DSD bit-reverse/byte-swap are the hot paths
    // for non-32-bit content; we avoid std::vector::resize() per packet by
    // sizing them to a generous fixed upper bound here.
    g_st.dsd_transform_buffer.assign(SCREAM_MAX_PACKET_BYTES, 0);
    g_st.pcm_pack_buffer.assign(SCREAM_MAX_PACKET_BYTES, 0);

    g_st.sdk_open = true;
    g_st.initialized = true;
    g_st.stats_print_armed = true;
    return 0;
    } catch (...) {
        std::fprintf(stderr,
            "[diretta] FATAL: unhandled exception in diretta_output_init; "
            "aborting Diretta backend\n");
        return 1;
    }
}

extern "C" void diretta_output_tick(void) {
    try {
    if (!g_st.initialized) return;
    const int release_sec = g_st.cfg.upstream_idle_timeout_sec;
    const int pause_sec   = g_st.cfg.upstream_pause_timeout_sec;
    if (release_sec <= 0 && pause_sec <= 0) return;  // both features disabled
    if (!g_st.sync) return;                  // no active Sync -> nothing to do
    if (g_st.reconfigure_pending) return;    // a format change is already in flight
    if (!g_st.stream_started) return;        // never reached steady playback; let open-grace handle it
    if (!g_st.have_last_pcm_packet_at) return;

    const auto now_ts = std::chrono::steady_clock::now();
    const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now_ts - g_st.last_pcm_packet_at).count();

    // Stage 1: pause (#1). Keep the connection but stop the SDK send thread
    // so the Target stops being driven. Only fires once per idle episode
    // (guarded by g_st.paused) and only when we have NOT already crossed
    // the release threshold this tick. The release stage below upgrades a
    // paused Sync to a full teardown if the idle persists.
    if (pause_sec > 0 && !g_st.paused &&
        idle_ms >= (long long)pause_sec * 1000LL &&
        (release_sec <= 0 || idle_ms < (long long)release_sec * 1000LL)) {
        DLOG(0, "upstream idle for %llds (>= %ds); pausing Target "
             "(SDK stop, connection kept). Will resume on next real PCM.",
             (long long)(idle_ms / 1000), pause_sec);
        g_st.sync->stop();         // SDK "stop playback(pause)" -- halts send thread
        g_st.sync->deactivate();   // getNewStream() no longer touches the ring
        g_st.paused = true;
        return;
    }

    // Stage 2: release (#2). Tear the Sync down completely after the longer
    // idle horizon. Works whether or not we are currently paused.
    if (release_sec <= 0) return;            // release disabled
    if (idle_ms < (long long)release_sec * 1000LL) return;

    DLOG(0, "upstream idle for %llds (>= %ds); releasing Target "
         "(stop + disconnect). Will reconnect when audio resumes.",
         (long long)(idle_ms / 1000), release_sec);

    // Mirror the sink-lost teardown: save the format scalars so the
    // reconnect path can rebuild the same Sync, tear the Sync down, and
    // arm reconnect_pending. The unified queue is kept so PCM that arrives
    // on resume accumulates for the next open.
    const uint32_t saved_rate = g_st.sample_rate;
    const uint32_t saved_ch   = g_st.channels;
    const uint32_t saved_bits = g_st.bits_per_sample;
    const uint32_t saved_bpf  = g_st.bytes_per_frame;
    const uint32_t saved_src_bpf = g_st.src_bytes_per_frame;
    teardown_sync_for_runtime("upstream idle release");
    g_st.reconnect_pending = true;
    g_st.sample_rate     = saved_rate;
    g_st.channels        = saved_ch;
    g_st.bits_per_sample = saved_bits;
    g_st.bytes_per_frame = saved_bpf;
    g_st.src_bytes_per_frame = saved_src_bpf;
    g_st.reconnect_backoff_ms = 500;
    g_st.next_reconnect_at = now_ts;  // resume as soon as audio returns
    } catch (...) {
        std::fprintf(stderr,
            "[diretta] FATAL: unhandled exception in diretta_output_tick\n");
    }
}

extern "C" int diretta_output_send(receiver_data_t *data) {
    try {
    if (!g_st.initialized) return 1;

    // Stamp the most recent PCM packet timestamp before any other
    // work so source_gap_ms accounting at underrun_begin reflects how
    // long the source has been silent, not how long we have been blocked
    // by SDK opens or stat formatting.
    if (data->audio != nullptr && data->audio_size > 0) {
        g_st.last_pcm_packet_at = std::chrono::steady_clock::now();
        g_st.have_last_pcm_packet_at = true;
        // NIC-arrival timestamp populated by network.c when SO_TIMESTAMPNS
        // is enabled. 0 means "unavailable" -- preserve that semantics so
        // underrun lines can omit nic_gap_ms cleanly.
        if (data->nic_timestamp_ns != 0) {
            g_st.last_pcm_nic_ts_ns = data->nic_timestamp_ns;
        }
    }

    // Raw-entry tap. Captures data->audio raw, BEFORE any
    // frame-align / partial-carry / queue work. This is the exact byte
    // stream `-o stdout` (raw.c) would have written: see raw_output_send
    // in raw.c -- it just fwrite(data->audio, 1, data->audio_size, stdout)
    // for every packet whose declared format has a non-zero rate. The
    // tap therefore preserves the EXACT byte sequence and arrival order
    // that the stdout path would have produced, including the very first
    // packet at the start of a song (whereas the Diretta ingress dump
    // sees bytes only after reconfigure() succeeds AND after the partial-
    // frame carry buffer has been combined with any tail from the
    // previous packet, so the byte sequence at the head of a track may
    // appear shifted by up to (bytes_per_frame - 1) bytes vs stdout).
    //
    // Fast-path: skip even the wire-format decode when neither the
    // Diretta-internal diagnostic suite nor the backend-independent
    // receiver-tap is armed. In SCREAM2DIRETTA_NO_DIAGNOSTICS builds
    // both gates fold to constant 0 and the entire block is DCE'd.
    if (data->audio != nullptr && data->audio_size > 0 &&
        (diretta_diag_armed() || receiver_tap_any_armed())) {
        // Resolve the active source format from the wire bytes. This is
        // independent of whether reconfigure() has run for this packet
        // -- the raw-entry tap MUST mirror stdout, which writes regardless
        // of the Diretta path's reconfiguration state. Only valid
        // bit-depth values open a file (matches raw.c's accept set).
        const receiver_format_t& rf_now = data->format;
        const uint32_t rate_hz = rf_now.sample_rate;
        const uint32_t bits    = rf_now.sample_size;
        const uint32_t chans   = rf_now.channels;
        if (rate_hz > 0 && (bits == 16 || bits == 24 || bits == 32) &&
            chans > 0) {
            if (pcm_dumper_enabled(&g_st.raw_entry_dumper)) {
                if (pcm_dumper_open_or_rotate(&g_st.raw_entry_dumper,
                                              rate_hz, chans, bits)) {
                    pcm_dumper_write(&g_st.raw_entry_dumper,
                                     data->audio, data->audio_size);
                    // Comparator: capture the same bytes for the raw-entry
                    // side. The ingress side is captured from queue_push_frames
                    // so it sees the post-frame-align byte stream.
                    cmp_capture(g_st.cmp_raw_entry_buf, g_st.cmp_raw_entry_filled,
                                data->audio, data->audio_size);
                    cmp_maybe_emit_summary();
                }
            }
            // Feed the backend-independent receiver-tap comparator.
            // No-op unless --compare-receiver-tap-ms is in effect and the
            // active backend is Diretta. This is a tap-only call -- it
            // does NOT write a WAV (the raw-entry WAV above is the
            // file dump; this feeds only the in-RAM comparator buffer).
            receiver_tap_diretta_raw_entry_feed(data->audio, data->audio_size,
                                             rate_hz, bits, chans);
        }
    }

    // Receiver-side ingress diagnostics. These run BEFORE any
    // potentially blocking work so that the times they record reflect
    // when the receive thread actually got control with a packet in
    // hand — not when we eventually returned from an SDK open.
    if (data->audio != nullptr && data->audio_size > 0) {
        // First Scream packet observed after the most recent
        // sync_open_begin (the worker emits sync_open_begin from
        // start_async_sync_open()). If this fires within ~tens of ms of
        // sync_open_begin, multicast ingestion is NOT blocked by the
        // open sequence.
        if (g_st.dbg_open_anchor_valid &&
            !g_st.dbg_logged_first_packet_after_open_begin) {
            g_st.dbg_logged_first_packet_after_open_begin = true;
            phase_event("receiver_first_packet_after_open_begin",
                        "audio_size=%u async_open_state=%d sync_present=%d",
                        (unsigned)data->audio_size,
                        g_st.async_open_state.load(std::memory_order_acquire),
                        g_st.sync ? 1 : 0);
        }
    }

    // Drain any completed async open result before doing anything
    // else. If the worker finished, this installs the new Sync into
    // g_st.sync and emits open_grace_begin / open_grace_nonblocking.
    poll_async_sync_open();

    const receiver_format_t& rf = data->format;

    // wire_layout only distinguishes a real format for 24-bit PCM
    // (S24_3LE vs S24_LE). For all other sample sizes the receiver sets it
    // to 0 and it must be ignored, so only fold it into the comparison when
    // sample_size == 24.
    const bool wire_layout_differs =
        (rf.sample_size == 24) && (rf.wire_layout != g_st.last_wire_layout);

    const bool effective_fmt_changed =
        !g_st.have_last_fmt ||
        rf.sample_rate != g_st.last_sample_rate ||
        rf.sample_size != g_st.last_sample_size ||
        rf.channels    != g_st.last_channels ||
        wire_layout_differs;

    if (effective_fmt_changed) {
        FormatConfigure probe_fc;
        uint32_t r=0, c=0, b=0, bpf_probe=0;
        DerivedFormat probe_df;  // discarded; reconfigure() recomputes + commits
        if (!validate_format(rf, probe_fc, &r, &c, &b, &bpf_probe, &probe_df)) {
            // Spurious / sentinel packet on stop/resume. Drop without
            // touching the SDK.
            DLOG(2, "ignoring spurious format packet: rate=%u Hz size=%u ch=%u",
                 rf.sample_rate, rf.sample_size, rf.channels);
            maybe_print_periodic_stats();
            return 0;
        }
        if (g_st.have_last_fmt) {
            if (g_st.is_dsd) {
                uint32_t new_base = 0, new_mult = 0;
                map_rate(rf.sample_rate, &new_base, &new_mult);
                DLOG(1, "format change: DSD (real_rate=%u Hz, mult=%u) -> "
                     "DSD (real_rate=%u Hz, mult=%u)",
                     g_st.dsd_real_rate, g_st.dsd_multiplier,
                     rf.sample_rate * 64u, new_mult);
            } else {
                DLOG(1, "format change: %u Hz / %u-bit / %u ch -> "
                     "%u Hz / %u-bit / %u ch",
                     g_st.last_sample_rate, g_st.last_sample_size, g_st.last_channels,
                     rf.sample_rate, rf.sample_size, rf.channels);
            }
        } else {
            if (rf.sample_size == 1) {
                uint32_t dsd_base = 0, dsd_mult = 0;
                map_rate(rf.sample_rate, &dsd_base, &dsd_mult);
                uint32_t dsd_real = dsd_base * dsd_mult * 64u;
                DLOG(1, "initial format: DSD (real_rate=%u Hz, mult=%u)",
                     dsd_real, dsd_mult);
            } else {
                DLOG(1, "initial format: %u Hz / %u-bit / %u ch",
                     rf.sample_rate, rf.sample_size, rf.channels);
            }
        }
        g_st.last_sample_rate = rf.sample_rate;
        g_st.last_sample_size = rf.sample_size;
        g_st.last_channels = rf.channels;
        // Store wire_layout normalised: it only carries meaning for 24-bit
        // PCM, so for any other sample size we record 0 to match how the
        // receiver fills it and keep the next comparison stable.
        g_st.last_wire_layout = (rf.sample_size == 24) ? rf.wire_layout : 0;
        g_st.have_last_fmt = true;
        if (!reconfigure(rf)) {
            g_st.partial_frame_len = 0;
            maybe_print_periodic_stats();
            return 0;
        }
        g_st.partial_frame_len = 0;
    }

    // --- Resume from idle-pause (#1) ---
    // If we are paused (SDK stopped, connection kept) and a real PCM packet
    // has arrived, replay the existing Sync mirroring a fresh open: clear
    // any stale queue content, re-arm the startup gates so getNewStream()
    // emits a short silence run before real PCM, re-activate ring access,
    // and play(). A format change above would already have torn the Sync
    // down (clearing g_st.paused), so this only runs for same-format resume.
    if (g_st.paused && g_st.sync &&
        data->audio != nullptr && data->audio_size > 0) {
        DLOG(0, "upstream resumed; replaying paused Sync "
             "(queue clear + prefill + play).");
        g_st.queue.clear();          // drop stale PCM; require fresh prefill
        g_st.sync->resetGate();      // re-arm mute/prefill/real-delay gates
        g_st.sync->activate();       // allow getNewStream() to touch the ring
        g_st.sync->play();           // SDK resume sending
        g_st.paused = false;
    }

    // --- Unified-queue ingress ---
    // From here on, we always write into g_st.queue (when armed). The Sync
    // either:
    //   (a) doesn't exist yet (cooldown / handshake / reconnect backoff),
    //       in which case the queue accumulates and the next Sync open
    //       will read from it directly; OR
    //   (b) exists and is open, in which case the SDK send thread is
    //       already pulling from the same queue.
    // No drain step in either case.

    const auto now_ts = std::chrono::steady_clock::now();

    // Format-change cooldown + open gate — we may need to wait before
    // opening the new Sync. The queue is already armed for the new format;
    // PCM lands in the queue throughout the wait and the Sync will read
    // from it once it opens.
    //
    // Sequencing:
    //   1. Cooldown (reconfigure_ready_at): protects the target from being
    //      hit with a new FormatConfigure while it's still releasing the
    //      old stream.
    //   2. Open-gate fill threshold: once cooldown elapses, also wait for
    //      queue fill >= open_gate_threshold_bytes. Instead of muting after
    //      open, we open later so the SDK pulls real PCM from the first cycle.
    //   3. Hard fallback (open_gate_deadline_at): if queue fill never
    //      reaches the threshold (silent / sparse stream), open anyway
    //      so we don't refuse to play forever. The Sync's own prefill
    //      gate then drives the playback-start decision.
    if (g_st.reconfigure_pending) {
        if (now_ts < g_st.reconfigure_ready_at) {
            // Cooldown active. Continue queue ingress below; bail before
            // any Sync work.
            goto ingress_only;
        }
        // Cooldown elapsed. Open only when we have something to do.
        if (data->audio_size == 0 && !(g_st.queue_ready && g_st.queue.available() > 0)) {
            maybe_print_periodic_stats();
            return 0;
        }

        // Open-fill gate. Hold the Sync closed until the queue has
        // accumulated open_gate_threshold_bytes of real PCM, or the
        // fallback deadline fires.
        if (g_st.queue_ready && g_st.open_gate_threshold_bytes > 0) {
            const size_t fill = g_st.queue.available();
            if (fill < g_st.open_gate_threshold_bytes &&
                now_ts < g_st.open_gate_deadline_at) {
                if (verbosity >= 2) {
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now_ts.time_since_epoch()).count();
                    if (now_ms - g_st.last_open_gate_log_ms >= 200) {
                        g_st.last_open_gate_log_ms = now_ms;
                        const uint64_t bps = static_cast<uint64_t>(g_st.sample_rate) *
                                             static_cast<uint64_t>(g_st.bytes_per_frame);
                        uint64_t fill_ms = 0;
                        uint64_t thr_ms = 0;
                        if (bps > 0) {
                            fill_ms = (static_cast<uint64_t>(fill) * 1000ULL) / bps;
                            thr_ms  = (static_cast<uint64_t>(g_st.open_gate_threshold_bytes) * 1000ULL) / bps;
                        }
                        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                            g_st.open_gate_deadline_at - now_ts).count();
                        DLOG(2, "open gate: waiting for queue prebuffer — "
                             "fill=%zu/%zu B (~%llu/%llu ms), fallback open in %lld ms",
                             fill, g_st.open_gate_threshold_bytes,
                             (unsigned long long)fill_ms,
                             (unsigned long long)thr_ms,
                             (long long)rem);
                    }
                }
                goto ingress_only;
            }
            if (verbosity >= 1) {
                const uint64_t bps = static_cast<uint64_t>(g_st.sample_rate) *
                                     static_cast<uint64_t>(g_st.bytes_per_frame);
                uint64_t fill_ms = 0;
                if (bps > 0) {
                    fill_ms = (static_cast<uint64_t>(fill) * 1000ULL) / bps;
                }
                const bool by_fallback = (fill < g_st.open_gate_threshold_bytes);
                DLOG(1, "open gate %s: queue fill=%zu B (~%llu ms), threshold=%zu B; "
                     "opening Sync now",
                     by_fallback ? "fallback fired" : "satisfied",
                     fill, (unsigned long long)fill_ms,
                     g_st.open_gate_threshold_bytes);
            }
        }

        if (g_inflight_cleanups.load(std::memory_order_acquire) > 0) {
            const auto cleanup_extra_deadline = g_st.reconfigure_ready_at +
                std::chrono::milliseconds(RUNTIME_CLEANUP_BUDGET_MS);
            if (now_ts < cleanup_extra_deadline) {
                g_st.reconfigure_ready_at = now_ts + std::chrono::milliseconds(100);
                DLOG(2, "deferring reconnect 100ms; %d cleanup(s) still in flight",
                     g_inflight_cleanups.load(std::memory_order_acquire));
                goto ingress_only;
            }
            DLOG(0, "opening new Sync despite %d cleanup(s) still in flight (cleanup-extra deadline reached)",
                 g_inflight_cleanups.load(std::memory_order_acquire));
        }
        if (!try_reconnect_same_format("format change open")) {
            if (g_st.reconnect_backoff_ms < 1000) {
                g_st.reconnect_backoff_ms = std::min(1000u, g_st.reconnect_backoff_ms * 2);
            }
            g_st.reconfigure_ready_at = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(g_st.reconnect_backoff_ms);
            // Extend the open-gate fallback to match the new retry time.
            g_st.open_gate_deadline_at = g_st.reconfigure_ready_at +
                std::chrono::milliseconds(OPEN_GATE_MAX_WAIT_MS);
            goto ingress_only;
        }
        // Successful open. The queue already has real PCM buffered (the
        // open gate ensured it), so the SDK pulls real audio from cycle 1.
        // The Sync's prefill gate may still close briefly during the very
        // first cycles if cycle_size happens to exceed available fill,
        // but with default tunings that does not happen in practice.
    }

    // No Sync at all? If reconnect_pending is armed (after a sink-loss
    // teardown) and the backoff has elapsed and we have audio, rebuild
    // the Sync. Otherwise just push into the queue and wait.
    if (!g_st.sync) {
        if (g_st.reconnect_pending && g_st.have_last_fc && data->audio_size > 0) {
            if (now_ts >= g_st.next_reconnect_at) {
                if (!try_reconnect_same_format("reconnect-pending resume")) {
                    if (g_st.reconnect_backoff_ms < 1000) {
                        g_st.reconnect_backoff_ms = std::min(1000u, g_st.reconnect_backoff_ms * 2);
                    }
                    g_st.next_reconnect_at = now_ts +
                        std::chrono::milliseconds(g_st.reconnect_backoff_ms);
                    goto ingress_only;
                }
                g_st.reconnect_backoff_ms = 750;
                // The async open was just kicked; g_st.sync is still null
                // until poll_async_sync_open() installs it on a later call.
                // Clear reconnect_pending so subsequent ingest cycles do NOT
                // re-enter this branch and re-call try_reconnect_same_format()
                // on every packet (which only logs "open already in flight"
                // until the open lands). The AOS_DoneFail path in
                // poll_async_sync_open() re-arms reconnect_pending if the
                // open fails, so dropping it here is safe.
                g_st.reconnect_pending = false;
                // Continue queue ingress this cycle instead of falling
                // through to the g_st.sync->is_connect() deref below.
                goto ingress_only;
            } else {
                goto ingress_only;
            }
        } else {
            goto ingress_only;
        }
    }

    // Sync exists. Promote ever_connected / stream_started.
    if (g_st.sync->is_connect()) {
        if (!g_st.ever_connected) {
            g_st.ever_connected = true;
            DLOG(1, "sink connected (is_connect()==true)");
        }
        if (!g_st.dbg_logged_is_connect_true) {
            g_st.dbg_logged_is_connect_true = true;
            phase_event("is_connect_true",
                        "is_online=%d is_active=%d",
                        g_st.sync->is_online() ? 1 : 0,
                        g_st.sync->is_active() ? 1 : 0);
            dbg_event("is_connect_true",
                      "is_online=%d is_active=%d",
                      g_st.sync->is_online() ? 1 : 0,
                      g_st.sync->is_active() ? 1 : 0);
        }
    }
    {
        uint64_t cur_streams = g_st.sync->getStreamCount();
        if (!g_st.stream_started && cur_streams > g_st.stream_count_at_open) {
            g_st.stream_started = true;
            DLOG(2, "stream started (getNewStream cycles=%llu)",
                 (unsigned long long)cur_streams);
        }
        if (!g_st.dbg_logged_first_getNewStream && cur_streams > 0) {
            g_st.dbg_logged_first_getNewStream = true;
            phase_event("first_getNewStream",
                        "stream_count=%llu queue_fill=%zu/%zu B (~%llu ms) "
                        "prefillDone=%d muteDone=%d",
                        (unsigned long long)cur_streams,
                        g_st.sync->ringFill(), g_st.sync->ringBytes(),
                        (unsigned long long)g_st.sync->ringFillMs(),
                        g_st.sync->prefillDone() ? 1 : 0,
                        g_st.sync->muteDone() ? 1 : 0);
            dbg_event("first_getNewStream",
                      "stream_count=%llu silent_cycles=%llu real_cycles=%llu "
                      "queue_fill=%zu/%zu B (~%llu ms) prefillDone=%d muteDone=%d",
                      (unsigned long long)cur_streams,
                      (unsigned long long)g_st.sync->silentCycles(),
                      (unsigned long long)g_st.sync->realCycles(),
                      g_st.sync->ringFill(), g_st.sync->ringBytes(),
                      (unsigned long long)g_st.sync->ringFillMs(),
                      g_st.sync->prefillDone() ? 1 : 0,
                      g_st.sync->muteDone() ? 1 : 0);
        }
        uint64_t cur_real = g_st.sync->realCycles();
        if (!g_st.dbg_logged_first_real_pcm && cur_real > 0) {
            g_st.dbg_logged_first_real_pcm = true;
            // SDK is now pulling real PCM; startup-overflow warning is
            // no longer applicable for this open. Reset so subsequent
            // format-change reopens get their own one-shot check.
            g_st.startup_overflow_risk_logged = false;
            DLOG(1, "first real PCM output (queue_fill=%zu B, ~%llu ms)",
                 g_st.sync->ringFill(),
                 (unsigned long long)g_st.sync->ringFillMs());
            phase_event("first_real_pcm",
                        "real_cycles=%llu silent_cycles=%llu stream_count=%llu "
                        "queue_fill=%zu/%zu B (~%llu ms)",
                        (unsigned long long)cur_real,
                        (unsigned long long)g_st.sync->silentCycles(),
                        (unsigned long long)cur_streams,
                        g_st.sync->ringFill(), g_st.sync->ringBytes(),
                        (unsigned long long)g_st.sync->ringFillMs());
            dbg_event("first_real_pcm",
                      "real_cycles=%llu silent_cycles=%llu stream_count=%llu "
                      "queue_fill=%zu/%zu B (~%llu ms)",
                      (unsigned long long)cur_real,
                      (unsigned long long)g_st.sync->silentCycles(),
                      (unsigned long long)cur_streams,
                      g_st.sync->ringFill(), g_st.sync->ringBytes(),
                      (unsigned long long)g_st.sync->ringFillMs());
        }
        g_st.dbg_last_stream_count = cur_streams;
        g_st.dbg_last_real_cycles  = cur_real;
    }

    // startup_real_delay observer: emit one-shot begin/end phase events
    // by polling the Sync's gate state from the receive thread. The Sync
    // itself does not log; it just runs the gate. Begin fires the first
    // time we observe the gate consuming silence cycles (target > 0 and
    // some bytes already emitted) so the begin event reports queue fill
    // BEFORE the delay window has progressed far. End fires when the
    // realDelayDone flag flips true. Both events are no-ops if
    // --startup-real-delay-ms is 0.
    if (g_st.cfg.startup_real_delay_ms > 0) {
        const size_t rd_target  = g_st.sync->realDelayBytes();
        const size_t rd_emitted = g_st.sync->realDelayBytesEmitted();
        const bool   rd_done    = g_st.sync->realDelayDone();
        const uint64_t rd_cycles = g_st.sync->realDelayCycles();

        if (!g_st.phase_logged_startup_real_delay_begin &&
            rd_target > 0 && (rd_emitted > 0 || rd_cycles > 0 || rd_done)) {
            g_st.phase_logged_startup_real_delay_begin = true;
            g_st.startup_real_delay_queue_fill_at_begin =
                g_st.queue_ready ? g_st.queue.available() : 0;
            g_st.startup_real_delay_pushed_at_begin = g_st.sync->pushedBytes();
            g_st.startup_real_delay_popped_at_begin = g_st.sync->poppedBytes();
            phase_event("startup_real_delay_begin",
                        "delay_ms=%d target_bytes=%llu queue_fill=%llu/%llu B "
                        "(~%llu ms) popped_so_far=%llu (queue NOT consumed during delay)",
                        g_st.cfg.startup_real_delay_ms,
                        (unsigned long long)rd_target,
                        (unsigned long long)g_st.startup_real_delay_queue_fill_at_begin,
                        (unsigned long long)(g_st.queue_ready ? g_st.queue.capacity() : 0),
                        (unsigned long long)g_st.sync->ringFillMs(),
                        (unsigned long long)g_st.startup_real_delay_popped_at_begin);
            dbg_event("startup_real_delay_begin",
                      "delay_ms=%d target_bytes=%llu queue_fill=%llu/%llu B "
                      "(~%llu ms) popped_so_far=%llu",
                      g_st.cfg.startup_real_delay_ms,
                      (unsigned long long)rd_target,
                      (unsigned long long)g_st.startup_real_delay_queue_fill_at_begin,
                      (unsigned long long)(g_st.queue_ready ? g_st.queue.capacity() : 0),
                      (unsigned long long)g_st.sync->ringFillMs(),
                      (unsigned long long)g_st.startup_real_delay_popped_at_begin);
        }
        if (g_st.phase_logged_startup_real_delay_begin &&
            !g_st.phase_logged_startup_real_delay_end && rd_done) {
            g_st.phase_logged_startup_real_delay_end = true;
            const uint64_t popped_now = g_st.sync->poppedBytes();
            const uint64_t consumed = popped_now - g_st.startup_real_delay_popped_at_begin;
            phase_event("startup_real_delay_end",
                        "delay_ms=%d emitted_bytes=%zu/%zu delay_silent_cycles=%llu "
                        "queue_fill=%zu/%zu B (~%llu ms) queue_consumed_during_delay=%llu B "
                        "(must be 0)",
                        g_st.cfg.startup_real_delay_ms,
                        rd_emitted, rd_target,
                        (unsigned long long)rd_cycles,
                        g_st.queue_ready ? g_st.queue.available() : 0,
                        g_st.queue_ready ? g_st.queue.capacity() : 0,
                        (unsigned long long)g_st.sync->ringFillMs(),
                        (unsigned long long)consumed);
            dbg_event("startup_real_delay_end",
                      "delay_ms=%d emitted_bytes=%zu/%zu delay_silent_cycles=%llu "
                      "queue_fill=%zu/%zu B (~%llu ms) queue_consumed_during_delay=%llu B",
                      g_st.cfg.startup_real_delay_ms,
                      rd_emitted, rd_target,
                      (unsigned long long)rd_cycles,
                      g_st.queue_ready ? g_st.queue.available() : 0,
                      g_st.queue_ready ? g_st.queue.capacity() : 0,
                      (unsigned long long)g_st.sync->ringFillMs(),
                      (unsigned long long)consumed);
        }
    }

    {
        const bool past_open_grace = !g_st.have_sink_open_at ||
            (now_ts >= g_st.sink_open_at + std::chrono::milliseconds(SINK_OPEN_GRACE_MS));
        if (past_open_grace && g_st.have_sink_open_at &&
            g_st.phase_logged_open_grace_begin &&
            !g_st.phase_logged_open_grace_end) {
            g_st.phase_logged_open_grace_end = true;
            DLOG(1, "open grace ended, entering steady state "
                 "(ever_connected=%d stream_started=%d)",
                 g_st.ever_connected ? 1 : 0,
                 g_st.stream_started ? 1 : 0);
            phase_event("open_grace_end",
                        "ever_connected=%d stream_started=%d "
                        "is_connect=%d is_online=%d "
                        "queue_fill=%zu/%zu B receiver_first_packet_logged=%d "
                        "receiver_first_push_during_grace_logged=%d "
                        "first_real_pcm_logged=%d",
                        g_st.ever_connected ? 1 : 0,
                        g_st.stream_started ? 1 : 0,
                        g_st.sync->is_connect() ? 1 : 0,
                        g_st.sync->is_online() ? 1 : 0,
                        g_st.queue_ready ? g_st.queue.available() : 0,
                        g_st.queue_ready ? g_st.queue.capacity() : 0,
                        g_st.dbg_logged_first_packet_after_open_begin ? 1 : 0,
                        g_st.dbg_logged_first_push_during_open_grace ? 1 : 0,
                        g_st.dbg_logged_first_real_pcm ? 1 : 0);
        }
        if (verbosity >= 2 && g_st.have_sink_open_at && !past_open_grace) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                (g_st.sink_open_at + std::chrono::milliseconds(SINK_OPEN_GRACE_MS)) - now_ts).count();
            static thread_local long long s_last_log_ms = 0;
            static thread_local int s_last_ec = -1;
            static thread_local int s_last_ss = -1;
            long long now_ms_thr = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_ts.time_since_epoch()).count();
            const int ec = g_st.ever_connected ? 1 : 0;
            const int ss = g_st.stream_started ? 1 : 0;
            const bool state_changed = (ec != s_last_ec) || (ss != s_last_ss);
            if (state_changed || now_ms_thr - s_last_log_ms >= 500) {
                DLOG(2, "open_grace remaining=%lldms ever_connected=%d stream_started=%d",
                     (long long)remaining, ec, ss);
                s_last_log_ms = now_ms_thr;
                s_last_ec = ec;
                s_last_ss = ss;
            }
        }

        const bool sink_lost = (!g_st.sync->is_connect() || !g_st.sink_active);
        if (sink_lost && !past_open_grace) {
            // Open grace: ignore.
        } else if (sink_lost && past_open_grace && !g_st.ever_connected && !g_st.stream_started) {
            const auto total_waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_ts - g_st.sink_open_origin).count();
            if (total_waited < SINK_OPEN_GRACE_TOTAL_MS) {
                g_st.sink_open_at = now_ts -
                    std::chrono::milliseconds(SINK_OPEN_GRACE_MS) +
                    std::chrono::milliseconds(SINK_OPEN_GRACE_EXTEND_MS);
                DLOG(2, "extending open-grace by %dms (still in handshake; total_waited=%lldms)",
                     SINK_OPEN_GRACE_EXTEND_MS, (long long)total_waited);
            } else {
                DLOG(0, "open-grace exhausted after %lldms without connection; tearing down",
                     (long long)total_waited);
                const uint32_t saved_rate = g_st.sample_rate;
                const uint32_t saved_ch   = g_st.channels;
                const uint32_t saved_bits = g_st.bits_per_sample;
                const uint32_t saved_bpf  = g_st.bytes_per_frame;
                const uint32_t saved_src_bpf = g_st.src_bytes_per_frame;
                teardown_sync_for_runtime("open-grace exhausted");
                g_st.reconnect_pending = true;
                g_st.sample_rate = saved_rate;
                g_st.channels = saved_ch;
                g_st.bits_per_sample = saved_bits;
                g_st.bytes_per_frame = saved_bpf;
                g_st.src_bytes_per_frame = saved_src_bpf;
                // The queue is kept — PCM continues to accumulate for the
                // next Sync open.
                g_st.next_reconnect_at = now_ts +
                    std::chrono::milliseconds(g_st.reconnect_backoff_ms);
                goto ingress_only;
            }
        } else if (sink_lost) {
            if (!g_st.conn_lost_logged) {
                DLOG(0, "sink connection lost (idle timeout / sink dropped stream); "
                        "will reconnect when audio resumes "
                        "(ever_connected=%d stream_started=%d)",
                        g_st.ever_connected ? 1 : 0,
                        g_st.stream_started ? 1 : 0);
                g_st.conn_lost_logged = true;
                g_st.next_reconnect_at = now_ts +
                    std::chrono::milliseconds(g_st.reconnect_backoff_ms);
            }
            const uint32_t saved_rate = g_st.sample_rate;
            const uint32_t saved_ch   = g_st.channels;
            const uint32_t saved_bits = g_st.bits_per_sample;
            const uint32_t saved_bpf  = g_st.bytes_per_frame;
            const uint32_t saved_src_bpf = g_st.src_bytes_per_frame;
            teardown_sync_for_runtime("sink lost");
            g_st.reconnect_pending = true;
            g_st.sample_rate = saved_rate;
            g_st.channels = saved_ch;
            g_st.bits_per_sample = saved_bits;
            g_st.bytes_per_frame = saved_bpf;
            g_st.src_bytes_per_frame = saved_src_bpf;
            // Keep the queue alive across the sink-lost: subsequent PCM
            // accumulates for the next open.
            const bool backoff_elapsed = (now_ts >= g_st.next_reconnect_at);
            const bool have_audio = (data->audio_size > 0);
            if (backoff_elapsed && have_audio) {
                if (try_reconnect_same_format("sink lost / resume")) {
                    g_st.reconnect_backoff_ms = 750;
                } else {
                    if (g_st.reconnect_backoff_ms < 1000) {
                        g_st.reconnect_backoff_ms = std::min(1000u, g_st.reconnect_backoff_ms * 2);
                    }
                    g_st.next_reconnect_at = now_ts +
                        std::chrono::milliseconds(g_st.reconnect_backoff_ms);
                    goto ingress_only;
                }
            } else {
                goto ingress_only;
            }
        }
    }

ingress_only:
    // Startup-overflow risk warning. If the unified queue crosses 90%
    // of capacity BEFORE the SDK has begun pulling real PCM (i.e. before
    // first_real_pcm fires), the next push will start dropping frames at
    // the head of the track. Emit a one-shot warning so the operator sees
    // the impending drop in logs rather than discovering a silent drop
    // after the fact. The warning is one-shot per Sync open and is reset
    // by reconfigure() so format-change retries get a fresh warning.
    if (!g_st.startup_overflow_risk_logged && g_st.queue_ready &&
        !g_st.dbg_logged_first_real_pcm && g_st.queue.capacity() > 0) {
        const size_t cap = g_st.queue.capacity();
        const size_t fill = g_st.queue.available();
        if (fill * 10 >= cap * 9) { // >= 90% full
            g_st.startup_overflow_risk_logged = true;
            const uint64_t bps = static_cast<uint64_t>(g_st.sample_rate) *
                                 static_cast<uint64_t>(g_st.bytes_per_frame);
            uint64_t fill_ms = 0, cap_ms = 0;
            if (bps > 0) {
                fill_ms = (static_cast<uint64_t>(fill) * 1000ULL) / bps;
                cap_ms  = (static_cast<uint64_t>(cap)  * 1000ULL) / bps;
            }
            phase_event("startup_overflow_risk",
                        "queue_fill=%zu/%zu B (~%llu/%llu ms) reconfigure_pending=%d "
                        "async_open_state=%d sync_present=%d first_real_pcm=0",
                        fill, cap,
                        (unsigned long long)fill_ms,
                        (unsigned long long)cap_ms,
                        g_st.reconfigure_pending ? 1 : 0,
                        g_st.async_open_state.load(std::memory_order_acquire),
                        g_st.sync ? 1 : 0);
            std::fprintf(stderr,
                "[diretta] WARN: startup_overflow_risk -- unified queue "
                "filled to %zu/%zu B (~%llu/%llu ms) before the SDK began "
                "pulling real PCM. Imminent head-of-track drop unless the "
                "SDK opens and pulls within the next ~%llu ms. Consider "
                "--ring-buffer-ms %d --format-change-cooldown-ms %d.\n",
                fill, cap,
                (unsigned long long)fill_ms,
                (unsigned long long)cap_ms,
                (unsigned long long)(cap_ms > fill_ms ? cap_ms - fill_ms : 0),
                (int)((cap_ms * 2) > 2000 ? (cap_ms * 2) : 2000),
                effective_cooldown_ms() > 200 ? 200 : effective_cooldown_ms());
        }
    }

    // --- Single unified queue ingress path ---
    // Always write into g_st.queue when armed. The producer never needs
    // to know whether a Sync is open or not — the queue is the only
    // destination.
    if (g_st.queue_ready) {
        const uint32_t dst_bpf = g_st.queue_bpf;
        const uint32_t src_bpf = g_st.src_bytes_per_frame;
        // src_bpf == dst_bpf for ordinary PCM. They differ when the source
        // is S24_LE (4-byte container) and we must pack to S24_3LE (3-byte)
        // for the Diretta SDK.
        if (src_bpf == 0 || dst_bpf == 0 || dst_bpf > PARTIAL_FRAME_MAX) {
            // Format scalars and queue bpf must agree — defensive.
        } else {
            const uint8_t* src = data->audio;
            size_t len = data->audio_size;

            if (g_st.partial_frame_len > 0 && len > 0) {
                size_t needed = src_bpf - g_st.partial_frame_len;
                if (len < needed) {
                    std::memcpy(g_st.partial_frame + g_st.partial_frame_len, src, len);
                    g_st.partial_frame_len += static_cast<uint32_t>(len);
                    g_st.partial_carry_count.fetch_add(1, std::memory_order_relaxed);
                    maybe_log_prefill_progress();
                    maybe_print_periodic_stats();
                    return 0;
                }
                std::memcpy(g_st.partial_frame + g_st.partial_frame_len, src, needed);
                queue_push_frames_converted(g_st.partial_frame, src_bpf, src_bpf, dst_bpf);
                g_st.partial_frame_len = 0;
                src += needed;
                len -= needed;
            }

            size_t whole = (len / src_bpf) * src_bpf;
            if (whole > 0) {
                queue_push_frames_converted(src, whole, src_bpf, dst_bpf);
                // First successful queue push observed while the
                // sink_open grace window is still active. Together with
                // receiver_first_packet_after_open_begin, this answers
                // the question "did the receive thread keep pushing
                // PCM into the unified queue during open_grace?".
                if (!g_st.dbg_logged_first_push_during_open_grace &&
                    g_st.phase_logged_open_grace_begin &&
                    !g_st.phase_logged_open_grace_end) {
                    g_st.dbg_logged_first_push_during_open_grace = true;
                    phase_event("receiver_first_push_during_open_grace",
                                "bytes=%zu queue_fill=%zu/%zu B async_open_state=%d",
                                whole,
                                g_st.queue.available(), g_st.queue.capacity(),
                                g_st.async_open_state.load(std::memory_order_acquire));
                }
            }
            if (whole < len) {
                size_t tail = len - whole;
                if (tail <= PARTIAL_FRAME_MAX) {
                    std::memcpy(g_st.partial_frame, src + whole, tail);
                    g_st.partial_frame_len = static_cast<uint32_t>(tail);
                    g_st.partial_carry_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_st.partial_frame_len = 0;
                }
            }
        }
    }

    maybe_log_prefill_progress();
    poll_underrun_events();
    maybe_print_periodic_stats();
    return 0;
    } catch (...) {
        std::fprintf(stderr,
            "[diretta] FATAL: unhandled exception in diretta_output_send; "
            "aborting packet processing for this cycle\n");
        return 1;
    }
}

extern "C" int diretta_get_stats(diretta_stats_t *out) {
    if (!out) return 1;
    std::memset(out, 0, sizeof(*out));
    if (!g_st.initialized) return 0;
    out->partial_carry_count = g_st.partial_carry_count.load(std::memory_order_relaxed);
    out->format_changes      = g_st.format_changes.load(std::memory_order_relaxed);
    // Queue stats — pulled from the unified queue itself so they remain
    // valid even across Sync open/close.
    if (g_st.queue_ready) {
        out->pushed_bytes        = g_st.queue.pushedBytes();
        out->pushed_frames       = g_st.queue.pushedFrames();
        out->dropped_bytes       = g_st.queue.dropBytes();
        out->dropped_frames      = g_st.queue.dropFrames();
        out->underruns           = g_st.queue.underrunCount();
        out->ring_fill_bytes     = g_st.queue.available();
        out->ring_capacity_bytes = g_st.queue.capacity();
        // ms requires bytes_per_second from the current format scalars.
        const uint64_t bps = static_cast<uint64_t>(g_st.sample_rate) *
                             static_cast<uint64_t>(g_st.bytes_per_frame);
        if (bps > 0) {
            out->dropped_ms    = (out->dropped_bytes * 1000ULL) / bps;
            out->ring_fill_ms  = (out->ring_fill_bytes * 1000ULL) / bps;
        }
    }
    if (g_st.sync) {
        out->silent_cycles = g_st.sync->silentCycles();
        out->real_cycles   = g_st.sync->realCycles();
        out->underrun_events = g_st.sync->underrunEvents();
        out->popped_bytes    = g_st.sync->poppedBytes();
    }
    out->target_cycle_us = g_st.target_cycle_us;
    out->sdk_cycle_us    = g_st.sdk_cycle_us;
    out->dsd_active     = g_st.is_dsd ? 1 : 0;
    out->dsd_multiplier = g_st.is_dsd ? g_st.dsd_multiplier : 0;
    out->dsd_real_rate  = g_st.is_dsd ? g_st.dsd_real_rate : 0;
    return 0;
}

extern "C" void diretta_print_stats(const char *tag) {
    char line[512];
    format_stats_line(tag ? tag : "stats", line, sizeof(line));
    std::fprintf(stderr, "%s\n", line);
}

extern "C" void diretta_output_shutdown(void) {
    if (g_st.initialized && stats_should_print()) {
        diretta_print_stats("final");
    } else if (g_st.queue_ready && verbosity >= 1) {
        DLOG(1, "queue stats: underruns=%llu drops=%llu fill=%zu/%zu",
             (unsigned long long)g_st.queue.underrunCount(),
             (unsigned long long)g_st.queue.dropBytes(),
             g_st.queue.available(), g_st.queue.capacity());
    }
    teardown_sync_for_shutdown();
    cleanup_finder();

    // Detached cleanup threads from format-change / idle-release teardowns
    // may still be inside bounded_disconnect() / close() / delete(). Wait for
    // them to finish before returning so the process does not exit underneath
    // an active SDK thread, and so the Target gets the full disconnect notice.
    const int cleanup_wait_deadline_ms = 2000;
    const auto cleanup_wait_start = std::chrono::steady_clock::now();
    while (g_inflight_cleanups.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cleanup_wait_start).count();
        if (elapsed > cleanup_wait_deadline_ms) {
            std::fprintf(stderr,
                "[diretta] WARNING: shutdown timed out waiting for %d cleanup thread(s); "
                "proceeding with exit\n",
                g_inflight_cleanups.load(std::memory_order_acquire));
            break;
        }
    }

    // Close open PCM dump files and patch WAV header sizes so captures remain
    // playable even if shutdown was abrupt.
    // teardown_sync_for_shutdown above has already stopped the SDK send
    // thread, so the egress dumper has no further writers when we close it.
    if (pcm_dumper_enabled(&g_st.ingress_dumper)) {
        pcm_dumper_close(&g_st.ingress_dumper);
        std::fprintf(stderr,
            "[pcm-dump:ingress] shutdown: files=%llu total_bytes=%llu\n",
            (unsigned long long)g_st.ingress_dumper.total_files,
            (unsigned long long)g_st.ingress_dumper.total_bytes_written);
    }
    if (pcm_dumper_enabled(&g_st.egress_dumper)) {
        pcm_dumper_close(&g_st.egress_dumper);
        std::fprintf(stderr,
            "[pcm-dump:egress] shutdown: files=%llu total_bytes=%llu\n",
            (unsigned long long)g_st.egress_dumper.total_files,
            (unsigned long long)g_st.egress_dumper.total_bytes_written);
    }
    if (pcm_dumper_enabled(&g_st.raw_entry_dumper)) {
        pcm_dumper_close(&g_st.raw_entry_dumper);
        std::fprintf(stderr,
            "[pcm-dump:raw-entry] shutdown: files=%llu total_bytes=%llu\n",
            (unsigned long long)g_st.raw_entry_dumper.total_files,
            (unsigned long long)g_st.raw_entry_dumper.total_bytes_written);
    }
    g_st.sdk_open = false;
    g_st.initialized = false;
    g_st.stats_print_armed = false;
}
