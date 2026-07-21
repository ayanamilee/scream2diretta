#ifndef SCREAM_DIRETTA_H
#define SCREAM_DIRETTA_H

#include "scream.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diretta output backend for the Scream Unix receiver.
 *
 * Current architecture (unified PCM queue):
 *
 *   Scream UDP receiver
 *     -> receiver_data_t {format, audio, audio_size}
 *     -> diretta_output_send()
 *     -> PcmRing (ordered, frame-aligned)                <- the unified queue
 *     -> ScreamDirettaSync::getNewStream() (SDK pull)
 *     -> Diretta SDK send thread -> target
 *
 * The unified queue is owned by the backend and persists across the Sync
 * lifecycle. The receiver thread writes into it during cooldown / open /
 * handshake / steady state. The SDK send thread pulls from it cycle-by-
 * cycle. There is no intermediate batch-drain step; PcmRing is the live
 * queue used by both endpoints.
 *
 * Lifecycle:
 *   diretta_output_init() once at startup (after CLI parse).
 *   diretta_output_send() per Scream PCM chunk.
 *
 * Behaviour:
 *   - Opens the Diretta Host SDK once, runs Find to discover sinks.
 *   - Target selection is by index (1-based) into the Find::findOutput() list.
 *   - On format changes, tears down the ScreamDirettaSync asynchronously without
 *     blocking the audio thread, creates a fresh unified queue for the new
 *     format, and reopens with the mapped FormatID. PCM during cooldown
 *     flows into the new queue immediately so the head of the track is
 *     preserved.
 *   - Underrun on the queue produces silence + (optional) rebuffer.
 */

/* Transfer mode selector. Mirrors DIRETTA::Sync::configTransferAuto/Var/Fix/...
 * but kept as a plain enum so the C front-end can build it from CLI flags. */
typedef enum diretta_transfer_mode_e {
    DIRETTA_TM_AUTO = 0,    /* configTransferVarAuto (default) */
    DIRETTA_TM_VARMAX,      /* configTransferVarMax */
    DIRETTA_TM_VARAUTO,     /* configTransferVarAuto */
    DIRETTA_TM_FIXAUTO,     /* configTransferFixAuto */
    DIRETTA_TM_RANDOM,      /* configTransferRandom */
    DIRETTA_TM_AUTOFIX,     /* configTransferFixAuto when cycletime set, else auto B-branch */
} diretta_transfer_mode_t;

/* Logging verbosity mapped onto SDK SysLog level. */
typedef enum diretta_log_level_e {
    DIRETTA_LOG_DEFAULT = 0,    /* Notice */
    DIRETTA_LOG_DEBUG,          /* Debug  */
    DIRETTA_LOG_WARN,           /* Warning */
} diretta_log_level_t;

typedef struct diretta_config_s {
    /* Target selection -- 1-based index into the Find result list.
     * 0 = "use first sink" (back-compat default). */
    int target_index;

    /* Steady-state runtime tunables. */
    int target_buffer_ms;       /* default 0 (SDK default) */
    unsigned thread_mode;       /* SDK Sync::THRED_MODE bitmask, default 1=CRITICAL */
    int cycle_us;               /* target/max cycle in us. 0 = auto. range 333..10000 enforced */
    int cycle_min_us;           /* min cycle for random mode, 0 = auto */
    int info_cycle_us;          /* info packet cycle, default 100000 */
    diretta_transfer_mode_t transfer_mode;
    int target_profile_limit_us; /* default 0 (SelfProfile). >0 = TargetProfile via ProfileMaker */
    int mtu_override;           /* 0 = auto-detect via measSendMTU */

    /* Unified PCM queue between the Scream receiver thread and the SDK
     * send thread. The backend owns this queue at the backend level (it persists
     * across Sync open/close; there is no intermediate batch-drain step).
     *   ring_buffer_ms   total queue length in ms of audio (default 1000)
     *   prefill_ms       audio buffered before getNewStream() outputs real
     *                    PCM (default 500). Below threshold the SDK gets
     *                    silence; the head of the track waits in the queue.
     *   rebuffer_percent if >0, after an underrun hold silence until the
     *                    queue refills to this fraction (default 0.50)
     *   startup_queue_ms startup gate threshold (ms). Effective open-
     *                    time gate = max(prefill_ms, startup_queue_ms).
     *                    Default 0 (use prefill_ms only). Raise this to
     *                    require a deeper queue at fresh-Sync open without
     *                    affecting steady-state prefill behaviour. */
    int ring_buffer_ms;
    int prefill_ms;
    float rebuffer_percent;
    /*  absolute rebuffer target (ms) used specifically after an
     * underrun. When > 0 this overrides rebuffer_percent during the
     * underrun recovery hold, so a single transient hiccup recovers after
     * accumulating ~underrun_rebuffer_ms of audio instead of refilling to
     * 50% of the ring (~500 ms at default ring_buffer_ms=1000). 0 (default
     * behaviour) falls back to rebuffer_percent. Range 0..5000. */
    int underrun_rebuffer_ms;
    int startup_queue_ms;
    /*  minimum number of getNewStream cycles, expressed in ms of audio,
     * during which the Sync MUST output zero PCM after a fresh open. Acts as
     * a forced silent warmup that runs through real Diretta pull cycles --
     * letting the target / DAC settle on silence before any real PCM lands.
     * 0 disables it (= default behaviour and the default). Range 0..2000. */
    int startup_mute_ms;

    /*  optional post-play startup "real delay" window (ms). When >0,
     * after the Sync has been play()'d and the prefill gate would normally
     * release, the SDK pull (getNewStream) emits silence for this many ms
     * of real pull cycles BEFORE the first real PCM byte is popped from
     * the unified queue. Unlike --startup-mute-ms, the queue is
     * NOT consumed during this window -- the head of the track waits
     * intact while the target/DAC settles on silence. Adds latency
     * deterministically (no audio is lost). Diagnostic option to test
     * whether the residual subtle artifact is caused by target/DAC
     * stabilization after play. 0 (default) = disabled. Range 0..5000. */
    int startup_real_delay_ms;

    /*  format-change cooldown in milliseconds. Replaces the hardcoded
     * 1200 ms used in earlier builds. After tearing down an old Sync we wait this
     * long before opening a fresh one against the same target, to let the
     * target release the previous stream. Default 200 ms (down from 1200)
     * to keep startup/format-change short enough that the unified queue
     * does not overflow before the SDK begins pulling.
     *
     * First open has no previous Sync, nothing to
     * release) from "format-change open" (must wait for the target to
     * release the previous stream). The first open does NOT apply this
     * cooldown -- only the queue prefill gate. Range 0..5000. */
    int format_change_cooldown_ms;

    /* Upstream-idle release. When the upstream stops sending PCM for this
     * many seconds, the backend tears down the active Sync (stop +
     * disconnect + close) so the Diretta Target / DAC stops receiving a
     * long-term silence stream and can let its USB/ALSA endpoint go idle.
     * The receiver keeps running; when audio resumes the Sync is reopened
     * via the existing reconnect path. 0 = disabled (stream silence
     * forever, the legacy behaviour). Default 120 s. */
    int upstream_idle_timeout_sec;

    /* Upstream-idle pause. Shorter-horizon companion to
     * upstream_idle_timeout_sec. When the upstream stops sending PCM for
     * this many seconds (but less than the release timeout), the backend
     * calls the SDK stop() (documented as "stop playback(pause)") and
     * deactivates getNewStream() WITHOUT tearing the connection down, so
     * the Diretta send thread stops driving the Target while the
     * connection is kept warm for an instant resume. When real PCM
     * returns, the same Sync is replayed (queue cleared, startup gates
     * re-armed, play()) so a short silence precedes real PCM, mirroring a
     * fresh open. If the idle persists to upstream_idle_timeout_sec the
     * pause is upgraded to a full release. 0 = disabled. Default 5 s. */
    int upstream_pause_timeout_sec;

    /* DSD-specific buffer tuning. Scream signals DSD via sample_size==1.
     * dsd_buffer_ms         : ring size for DSD (default 1500).
     * dsd_prefill_ms        : prefill gate for DSD (default 200).
     * dsd_startup_warmup_ms : base silent warmup after DSD open, scaled by
     *                         dsd_multiplier at runtime (default 50). */
    int dsd_buffer_ms;
    int dsd_prefill_ms;
    int dsd_startup_warmup_ms;

    /* Periodic producer-side stats reporting in seconds. 0 = off (only the
     * one-shot shutdown summary at -v / explicit stats is printed). When >0
     * and either verbose or stats are enabled, a single line with pushed /
     * dropped / underrun / fill counters is printed every N seconds. The
     * print is rate-limited and runs from the receiver thread, so it is
     * never on the audio hot path. */
    int stats_interval_sec;
    /* Force stats printing even without --verbose. */
    int stats_enabled;

    diretta_log_level_t log_level;

    /* PCM dump diagnostics. When --dump-ingress-wav / --dump-egress-wav
     * is set, the backend writes the corresponding PCM stream into one or
     * more WAV files (auto-rotated on format change). dump_ms caps each
     * individual file in ms of audio (0 = uncapped; default 3000 ms when
     * any dump option is enabled). Both prefixes default to NULL (disabled).
     *
     * Ingress = PCM as written into the unified queue (post frame-align,
     *           post partial-carry). Captures what came off the wire.
     * Egress  = PCM popped from the unified queue and handed to the SDK
     *           (real-PCM cycles only). Silence emitted by the prefill /
     *           startup-real-delay / mute gates is NOT written. */
    const char* dump_ingress_prefix; /* NULL = disabled */
    const char* dump_egress_prefix;  /* NULL = disabled */
    int dump_ms;                     /* per-file capture cap in ms; 0 = uncapped */

    /* Raw-entry tap. Captures the exact byte stream that
     * `-o stdout` (raw.c) would have written: data->audio for
     * data->audio_size bytes, wrapped as a WAV with the active source
     * format. Tapped at the very entry of diretta_output_send before any
     * frame-alignment / partial-carry / queue work runs. This is the
     * apples-to-apples comparison point against --dump-ingress-wav.
     * NULL = disabled. */
    const char* dump_raw_entry_prefix;

    /* In-process ingress-tap byte comparator. When both
     * dump_raw_entry_prefix AND dump_ingress_prefix are enabled, the
     * backend records the first <compare_ingress_taps_ms> ms of REAL PCM
     * at each tap (raw-entry and ingress) into RAM, then once both buffers
     * are full emits a single summary line on stderr noting whether the
     * bytes are identical, the first mismatching byte/frame offset, and
     * the largest per-sample delta. 0 = disabled. Range 0..5000. */
    int compare_ingress_taps_ms;

    /* Startup PCM diagnostics + optional egress fade.
     *
     * startup_analyze_ms : analysis window in ms over the first N ms of
     *                      real PCM at ingress AND egress, run independently
     *                      per format/open. Emits a single summary stderr
     *                      line per window. 0 = disabled. When any
     *                      --dump-*-wav is enabled OR verbosity >= 2 the
     *                      CLI defaults this to 100 ms; pass 0 to silence.
     * startup_fade_ms    : optional linear / cosine ramp 0..1 applied
     *                      IN PLACE to the first N ms of REAL PCM at
     *                      egress (after each format/open). 0 = disabled
     *                      (default). When >0 the egress dump captures
     *                      POST-fade PCM. Ingress dump is never modified.
     * startup_fade_shape : 0 = linear (default), 1 = cosine (raised). */
    int startup_analyze_ms;
    int startup_fade_ms;
    int startup_fade_shape;

    /* CPU affinity for the Scream receiver thread (producer). -1 = disabled.
     * Pin the main/receive thread to this core via pthread_setaffinity_np.
     * Linux only; silently ignored on other platforms. */
    int cpu_scream;

    /* CPU affinity for the Diretta SDK main audio thread (consumer).
     * Passed to Sync::open(cpuMain, ...) and connect(cpuMain).
     * When >= 0, THRED_MODE::OCCUPIED (bit 4) is automatically OR'd into
     * thread_mode so the SDK pins its worker thread. -1 = disabled. */
    int cpu_audio;

    /* CPU affinity for the Diretta SDK helper threads.
     * Passed to Sync::open(..., cpuOther, rngOther). -1 = disabled. */
    int cpu_other;

    /* Real-time priority for the Scream receiver thread and the Diretta
     * SDK worker thread. SCHED_FIFO is applied when >= 1 (range 1..99).
     * -1 = disabled (default). Other / control threads are never elevated. */
    int rt_priority;

    /*  SO_RCVBUF size requested on the UDP receive socket, in bytes.
     * 0 = leave kernel default. main() defaults to 4 MiB whenever
     * the output backend is Diretta. Reported here for the stats banner
     * only; the socket itself is configured by init_network(). */
    int udp_rcvbuf_bytes;

    /*  detailed Diretta SDK phase tracing. When non-zero, the backend
     * emits dense [diretta-debug] lines covering every Sync/Target lifecycle
     * step (sync_open_begin/_end, connect_begin/_return, is_connect_true,
     * first_getNewStream, first_real_pcm, format_change_cleanup_begin/_end,
     * setSink/setSinkConfigure/inquirySupportFormat/connectPrepare/play,
     * MTU + sink Info, target inquiry/profile params) with monotonic
     * timestamps relative to the last format-change-accepted instant. It
     * also forces the SDK's own SysLog level to Debug so the underlying
     * library log lines are visible alongside ours.
     *
     * Independent of -v / -vv. Default 0 (off). Output goes to stderr
     * (same stream as other [diretta] messages). */
    int diretta_debug;
} diretta_config_t;

/* Snapshot of producer-side counters; safe to read from any thread. */
typedef struct diretta_stats_s {
    uint64_t pushed_bytes;
    uint64_t pushed_frames;
    uint64_t dropped_bytes;
    uint64_t dropped_frames;
    uint64_t dropped_ms;
    uint64_t partial_carry_count;  /* number of times a partial frame was carried */
    uint64_t format_changes;
    uint64_t underruns;
    uint64_t ring_fill_bytes;
    uint64_t ring_capacity_bytes;
    uint64_t ring_fill_ms;
    /* SDK pull-side: SDK pull-side cycle accounting. */
    uint64_t silent_cycles;        /* getNewStream cycles that emitted silence */
    uint64_t real_cycles;          /* getNewStream cycles that popped real PCM */
    /* Additional: distinct underrun episodes (begin transitions) and
     * cumulative drain counter so the stats line can compute per-interval
     * push / drain / net-fill deltas. */
    uint64_t underrun_events;
    uint64_t popped_bytes;
    uint64_t target_cycle_us;   /* configured target cycle at last open */
    uint64_t sdk_cycle_us;      /* SDK generated transmission interval at last open */
    /* DSD diagnostics */
    uint64_t dsd_active;        /* 1 if current format is DSD */
    uint64_t dsd_multiplier;    /* 1=DSD64, 2=DSD128, 4=DSD256, 8=DSD512 */
    uint64_t dsd_real_rate;     /* e.g. 2822400 for DSD64 */
} diretta_stats_t;

/* Initialise sane defaults. */
void diretta_config_init(diretta_config_t *cfg);

/* Apply CPU affinity to the calling thread. core >= 0 pins to that core;
 * core < 0 is a no-op. Returns 0 on success, -1 on failure (logged to stderr).
 * Linux only; silently returns 0 on other platforms. */
int diretta_apply_cpu_affinity(int core);

/* Apply SCHED_FIFO real-time priority to the calling thread.
 * priority must be in the range 1..99 (higher = more urgent).
 * Linux only; silently returns 0 on other platforms.
 * Returns 0 on success, -1 on failure (logged to stderr). */
int diretta_apply_rt_priority(int priority);

/* Discover targets and print a numbered list to stdout, then return. Returns
 * 0 on success (even if no sinks are found -- it just prints "no targets"),
 * non-zero on SDK init failure. */
int diretta_list_targets(const diretta_config_t *cfg, const char *progname);

/* Open the SDK, pick the target by cfg->target_index, measure MTU, and
 * leave the backend ready for diretta_output_send() calls. */
int diretta_output_init(const diretta_config_t *cfg);

int diretta_output_send(receiver_data_t *data);

/* Heartbeat tick called by the receiver loop when no PCM packet was
 * received this iteration (upstream silent / select() timeout). Runs the
 * upstream-idle escalation in two stages:
 *   Stage 1 (pause): after cfg.upstream_pause_timeout_sec of idle, stop the
 *     SDK send thread but keep the connection, so the Target stops being
 *     driven; resumes on the next real PCM.
 *   Stage 2 (release): after the longer cfg.upstream_idle_timeout_sec, tear
 *     the Sync down completely (stop + disconnect) and arm the reconnect
 *     path so playback resumes on the next packet.
 * No-op when both idle features are disabled or no Sync is open. */
void diretta_output_tick(void);

void diretta_output_shutdown(void);

/* Snapshot current producer-side stats. Returns 0 on success. */
int diretta_get_stats(diretta_stats_t *out);

/* Format and print a one-line stats summary to stderr. tag is a short prefix
 * such as "stats" or "final". Safe to call from the receiver thread. */
void diretta_print_stats(const char *tag);

#ifdef __cplusplus
}
#endif

#endif
