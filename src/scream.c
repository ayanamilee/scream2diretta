#define _GNU_SOURCE

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <net/if.h>

#include "scream.h"
#include "network.h"

#include "raw.h"
#include "receiver_tap.h"
#include <errno.h>

#if defined(__linux__)
#include <sched.h>
#endif

#ifndef SCREAM2DIRETTA_VERSION
#define SCREAM2DIRETTA_VERSION "0.7"
#endif

#if DIRETTA_ENABLE
#include "diretta.h"
#endif

int verbosity = 0;

volatile sig_atomic_t g_shutdown_pending = 0;
static void on_term_signal(int sig) {
  (void)sig;
  g_shutdown_pending = 1;
}
static void install_term_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_term_signal;
  sigemptyset(&sa.sa_mask);
  // Block SIGINT/SIGTERM while the handler runs so the same signal cannot
  // reenter and corrupt shutdown state.
  sigaddset(&sa.sa_mask, SIGINT);
  sigaddset(&sa.sa_mask, SIGTERM);
  // Deliberately do NOT set SA_RESTART: blocking syscalls (recvfrom, select,
  // usleep) must return EINTR so the receivers can observe the shutdown flag
  // and unwind. Each receiver checks g_shutdown_pending both before blocking
  // and after EINTR.
  sigaction(SIGINT,  &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static void show_usage(const char *arg0)
{
  fprintf(stderr, "\n");
  fprintf(stderr, "scream2diretta %s -- Scream UDP receiver with native Diretta Host output.\n", SCREAM2DIRETTA_VERSION);
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage: %s [-u] [-p <port>] [-i <iface>] [-g <group>] [-o raw] [options...]\n", arg0);
  fprintf(stderr, "\n");
  fprintf(stderr, "Default output is Diretta. Run with no -o option to feed the first\n");
  fprintf(stderr, "discovered Diretta target. Use '-o raw' to dump the decoded PCM stream\n");
  fprintf(stderr, "to stdout for local capture / pipeline use.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Pipeline:\n");
  fprintf(stderr, "  Scream UDP source\n");
  fprintf(stderr, "    -> Kernel UDP receive buffer  (--udp-rcvbuf-bytes, default 4 MiB)\n");
  fprintf(stderr, "    -> receiver_data_t            (Scream header + PCM payload)\n");
  fprintf(stderr, "    -> diretta_output_send()      (format check, frame alignment, partial carry)\n");
  fprintf(stderr, "    -> PcmRing                    (--pcm-buffer-ms / --pcm-prefill-ms)\n");
  fprintf(stderr, "    -> getNewStream()             (Diretta SDK pull callback)\n");
  fprintf(stderr, "    -> Diretta SDK -> Diretta Target\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Receiver options:\n");
  fprintf(stderr, "  -u                           Use unicast instead of multicast.\n");
  fprintf(stderr, "  -p <port>                    Use <port> instead of default port 4010.\n");
  fprintf(stderr, "  -i <iface>                   Use local interface <iface> (IP or name).\n");
  fprintf(stderr, "  -g <group>                   Multicast group address (multicast mode only).\n");
  fprintf(stderr, "  -L                           Legacy mode: parse original 5-byte Scream header\n");
  fprintf(stderr, "                               (for original screamalsa driver / ap2renderer).\n");
  fprintf(stderr, "  --udp-rcvbuf-bytes <bytes>   Kernel SO_RCVBUF size on the UDP socket\n");
  fprintf(stderr, "                               (default 4194304 = 4 MiB for Diretta;\n");
  fprintf(stderr, "                               0 = leave kernel default).\n");
  fprintf(stderr, "  --udp-busy-poll-us <us>      SO_BUSY_POLL window in microseconds. The receiver\n");
  fprintf(stderr, "                               thread polls the NIC RX queue for up to <us> before\n");
  fprintf(stderr, "                               giving up and sleeping. Trades CPU for wakeup-jitter\n");
  fprintf(stderr, "                               removal; safe on isolated cores. 0 = off (default).\n");
  fprintf(stderr, "                               Recommended: 50..100 on isolated RT systems.\n");
  fprintf(stderr, "  --enable-nic-timestamp       Request SO_TIMESTAMPNS on the UDP socket. Captures\n");
  fprintf(stderr, "                               the NIC arrival time of each packet so stats can\n");
  fprintf(stderr, "                               distinguish upstream gaps from local wakeup jitter\n");
  fprintf(stderr, "                               (logged as nic_gap_ms alongside source_gap_ms).\n");
  fprintf(stderr, "  --allowed-source-ip <ip>     Only accept Scream packets from this source\n");
  fprintf(stderr, "                               IPv4 address. Drops all other senders.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Output backend:\n");
  fprintf(stderr, "  -o raw                       Write raw PCM to stdout (apps-to-apps with the\n");
  fprintf(stderr, "                               Scream stdout backend; pipe into a file\n");
  fprintf(stderr, "                               or another player).\n");
  fprintf(stderr, "  -o diretta                   Native Diretta Host output (default if -o is\n");
  fprintf(stderr, "                               omitted). Uses PcmRing as the in-process PCM queue.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Diretta target selection:\n");
  fprintf(stderr, "  --target, -t <index>         Select Diretta target by index (1, 2, 3...).\n");
  fprintf(stderr, "  --list-targets               List available Diretta targets and exit.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Diretta buffering:\n");
  fprintf(stderr, "  --pcm-buffer-ms <ms>         PcmRing length in ms (default 1000, range 50..5000).\n");
  fprintf(stderr, "                               Main PCM queue before getNewStream(); absorbs receiver /\n");
  fprintf(stderr, "                               SDK pull timing differences.\n");
  fprintf(stderr, "  --pcm-prefill-ms <ms>        PcmRing open-gate threshold (default 500, range 0..5000).\n");
  fprintf(stderr, "                               The Sync is held closed until PcmRing has enough audio;\n");
  fprintf(stderr, "                               the SDK then pulls real PCM from cycle one.\n");
  fprintf(stderr, "  --rebuffer-percent <pct>    Resume real PCM after underrun when PcmRing reaches <pct>%%\n");
  fprintf(stderr, "                               full (default 50, range 0..95; 0 disables hold).\n");
  fprintf(stderr, "  --upstream-idle-timeout-sec <s> Release the Diretta Target after the upstream stops\n");
  fprintf(stderr, "                               sending PCM for <s> seconds, so it stops receiving a\n");
  fprintf(stderr, "                               long-term silence stream (default 120; 0 disables,\n");
  fprintf(stderr, "                               range 10..3600). Reopens automatically when audio resumes.\n");
  fprintf(stderr, "  --upstream-pause-timeout-sec <s> Pause the Diretta Target (SDK stop, connection kept)\n");
  fprintf(stderr, "                               after the upstream stops sending PCM for <s> seconds, so\n");
  fprintf(stderr, "                               the Target stops being driven but can resume instantly\n");
  fprintf(stderr, "                               (default 5; 0 disables, range 1..600). Upgrades to a\n");
  fprintf(stderr, "                               full release at --upstream-idle-timeout-sec.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "DSD buffering (used when Scream sender signals DSD via sample_size==1):\n");
  fprintf(stderr, "  --dsd-buffer-ms <ms>         DSD ring length in ms (default 1500, range 50..5000).\n");
  fprintf(stderr, "  --dsd-prefill-ms <ms>        DSD open-gate threshold (default 200, range 0..5000).\n");
  fprintf(stderr, "  --dsd-startup-warmup-ms <ms> Base silent warmup after DSD open, scaled by DSD\n");
  fprintf(stderr, "                               multiplier at runtime (default 50, range 0..2000).\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Diretta SDK advanced knobs:\n");
  fprintf(stderr, "  --thread-mode <mode>         SDK thread mode bitmask (default: 1=CRITICAL).\n");
  fprintf(stderr, "  --cycle-time <us>            Max cycle time in microseconds (333-10000).\n");
  fprintf(stderr, "                               Default: auto-calculated from format and MTU.\n");
  fprintf(stderr, "  --cycle-min-time <us>        Min cycle time in microseconds (random mode only).\n");
  fprintf(stderr, "  --info-cycle <us>            Info packet cycle in microseconds (default: 100000).\n");
  fprintf(stderr, "  --transfer-mode <mode>       Transfer mode: auto, varmax, varauto, fixauto, autofix, random.\n");
  fprintf(stderr, "  --target-profile-limit <us>  Target profile limit cycle (0=SelfProfile, default: 0,\n");
  fprintf(stderr, "                               >0=TargetProfile via ProfileMaker(limit=us)).\n");
  fprintf(stderr, "  --mtu <bytes>                MTU override (default: auto-detect).\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "CPU affinity (Linux only):\n");
  fprintf(stderr, "  --cpu-scream <core>          Pin the Scream receiver thread (main thread) to\n");
  fprintf(stderr, "                               <core> via pthread_setaffinity_np.\n");
  fprintf(stderr, "  --cpu-audio <core>           Pin the Diretta SDK main audio thread to <core>.\n");
  fprintf(stderr, "                               Automatically adds OCCUPIED to --thread-mode.\n");
  fprintf(stderr, "  --cpu-other <core>           Pin Diretta SDK helper threads to <core>.\n");
  fprintf(stderr, "  --rt-priority <1-99>         SCHED_FIFO real-time priority for the receiver\n");
  fprintf(stderr, "                               and Diretta SDK worker threads. -1=disabled (default).\n");
  fprintf(stderr, "  --no-mlock                   Disable mlockall(MCL_CURRENT|MCL_FUTURE). By default\n");
  fprintf(stderr, "                               the receiver pins all current and future pages in\n");
  fprintf(stderr, "                               RAM so the kernel cannot page out the PCM ring or\n");
  fprintf(stderr, "                               SDK buffers under memory pressure. Requires\n");
  fprintf(stderr, "                               sufficient RLIMIT_MEMLOCK (root or systemd\n");
  fprintf(stderr, "                               LimitMEMLOCK=infinity); failure is non-fatal.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Stats:\n");
  fprintf(stderr, "  --stats-interval <sec>       Periodic producer-side stats every <sec> seconds (default 0=off).\n");
  fprintf(stderr, "                               Active when --verbose or --stats is set; rate-limited and run\n");
  fprintf(stderr, "                               from the receiver thread (never on the audio hot path).\n");
  fprintf(stderr, "  --stats                      Force stats printing even without --verbose.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Logging:\n");
  fprintf(stderr, "  --verbose, -v                Verbose debug output (Diretta SDK log level: DEBUG).\n");
  fprintf(stderr, "  --quiet,   -q                Quiet -- only errors and warnings (log level: WARN).\n");
  fprintf(stderr, "  --diretta-debug              Advanced SDK phase trace; default off.\n");
  fprintf(stderr, "  --help,    -h                Show this help and exit.\n");
  fprintf(stderr, "  --version                   Show version and exit.\n");
  fprintf(stderr, "\n");
  exit(1);
}


static in_addr_t get_interface(const char *name)
{
  int sockfd = -1;
  struct ifreq ifr;
  in_addr_t addr = inet_addr(name);
  struct if_nameindex *ni;
  int i;

  if (addr != INADDR_NONE) {
    return addr;
  }

  memset(&ifr, 0, sizeof(ifr));

  if (strlen(name) >= sizeof(ifr.ifr_name)) {
    fprintf(stderr, "Too long interface name: %s\n\n", name);
    goto error_exit;
  }
  strcpy(ifr.ifr_name, name);

  sockfd = socket(AF_INET,SOCK_DGRAM,0);
  if (sockfd < 0) {
    fprintf(stderr, "Failed to create socket to resolve interface: %s\n\n", name);
    goto error_exit;
  }
  if (ioctl(sockfd, SIOCGIFADDR, &ifr) != 0) {
    fprintf(stderr, "Invalid interface: %s\n\n", name);
    goto error_exit;
  }
  close(sockfd);
  return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;

error_exit:
  ni = if_nameindex();
  if (ni) {
    fprintf(stderr, "Available interfaces:\n");
    for (i = 0; ni[i].if_name != NULL; i++) {
      strcpy(ifr.ifr_name, ni[i].if_name);
      if (sockfd >= 0 && ioctl(sockfd, SIOCGIFADDR, &ifr) == 0) {
        fprintf(stderr, "  %-10s (%s)\n", ni[i].if_name, inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
      }
    }
    if_freenameindex(ni);
  }
  if (sockfd >= 0) {
    close(sockfd);
  }
  exit(1);
}

#if DIRETTA_ENABLE
static int parse_transfer_mode(const char *s, diretta_transfer_mode_t *out) {
  if (strcmp(s, "auto") == 0)    { *out = DIRETTA_TM_AUTO; return 0; }
  if (strcmp(s, "varmax") == 0)  { *out = DIRETTA_TM_VARMAX; return 0; }
  if (strcmp(s, "varauto") == 0) { *out = DIRETTA_TM_VARAUTO; return 0; }
  if (strcmp(s, "fixauto") == 0) { *out = DIRETTA_TM_FIXAUTO; return 0; }
  if (strcmp(s, "random") == 0)  { *out = DIRETTA_TM_RANDOM; return 0; }
  if (strcmp(s, "autofix") == 0) { *out = DIRETTA_TM_AUTOFIX; return 0; }
  return 1;
}
#endif

enum {
  OPT_LIST_TARGETS = 1000,
  OPT_THREAD_MODE,
  OPT_CYCLE_TIME,
  OPT_CYCLE_MIN_TIME,
  OPT_INFO_CYCLE,
  OPT_TRANSFER_MODE,
  OPT_TARGET_PROFILE_LIMIT,
  OPT_MTU,
  OPT_RING_BUFFER_MS,
  OPT_PREFILL_MS,
  OPT_REBUFFER_PERCENT,
  OPT_STARTUP_QUEUE_MS,
  OPT_STARTUP_MUTE_MS,
  OPT_STATS_INTERVAL,
  OPT_STATS,
  OPT_DIRETTA_DEBUG,
  OPT_FORMAT_CHANGE_COOLDOWN_MS,
  OPT_UPSTREAM_IDLE_TIMEOUT_SEC,
  OPT_UPSTREAM_PAUSE_TIMEOUT_SEC,
  OPT_UNDERRUN_REBUFFER_PERCENT,
  OPT_UNDERRUN_REBUFFER_MS,
  OPT_STARTUP_REAL_DELAY_MS,
  OPT_DUMP_INGRESS_WAV,
  OPT_DUMP_EGRESS_WAV,
  OPT_DUMP_MS,
  OPT_STARTUP_ANALYZE_MS,
  OPT_STARTUP_FADE_MS,
  OPT_STARTUP_FADE_SHAPE,
  OPT_DUMP_RAW_ENTRY_WAV,
  OPT_COMPARE_INGRESS_TAPS_MS,
  /* Backend-independent receiver-payload tap + raw-stdout tap. */
  OPT_DUMP_RECEIVER_PAYLOAD_WAV,
  OPT_DUMP_RAW_STDOUT_WAV,
  OPT_COMPARE_RECEIVER_TAP_MS,
  /* Current buffering and UDP receive knobs. */
  OPT_PCM_BUFFER_MS,
  OPT_PCM_PREFILL_MS,
  OPT_DSD_BUFFER_MS,
  OPT_DSD_PREFILL_MS,
  OPT_DSD_STARTUP_WARMUP_MS,
  OPT_UDP_RCVBUF_BYTES,
  OPT_UDP_BUSY_POLL_US,
  OPT_ENABLE_NIC_TIMESTAMP,
  OPT_ALLOWED_SOURCE_IP,
  OPT_CPU_SCREAM,
  OPT_CPU_AUDIO,
  OPT_CPU_OTHER,
  OPT_RT_PRIORITY,
  OPT_NO_MLOCK,
  OPT_VERSION,
};

static const struct option long_options[] = {
  { "target",               required_argument, 0, 't' },
  { "list-targets",         no_argument,       0, OPT_LIST_TARGETS },
  { "verbose",              no_argument,       0, 'v' },
  { "quiet",                no_argument,       0, 'q' },
  { "help",                 no_argument,       0, 'h' },
  { "version",              no_argument,       0, OPT_VERSION },
  { "thread-mode",          required_argument, 0, OPT_THREAD_MODE },
  { "cycle-time",           required_argument, 0, OPT_CYCLE_TIME },
  { "cycle-min-time",       required_argument, 0, OPT_CYCLE_MIN_TIME },
  { "info-cycle",           required_argument, 0, OPT_INFO_CYCLE },
  { "transfer-mode",        required_argument, 0, OPT_TRANSFER_MODE },
  { "target-profile-limit", required_argument, 0, OPT_TARGET_PROFILE_LIMIT },
  { "mtu",                  required_argument, 0, OPT_MTU },
  { "ring-buffer-ms",       required_argument, 0, OPT_RING_BUFFER_MS },
  { "prefill-ms",           required_argument, 0, OPT_PREFILL_MS },
  { "rebuffer-percent",     required_argument, 0, OPT_REBUFFER_PERCENT },
  { "startup-queue-ms",     required_argument, 0, OPT_STARTUP_QUEUE_MS },
  { "startup-mute-ms",      required_argument, 0, OPT_STARTUP_MUTE_MS },
  { "stats-interval",       required_argument, 0, OPT_STATS_INTERVAL },
  { "stats",                no_argument,       0, OPT_STATS },
  { "diretta-debug",        no_argument,       0, OPT_DIRETTA_DEBUG },
  { "format-change-cooldown-ms", required_argument, 0, OPT_FORMAT_CHANGE_COOLDOWN_MS },
  { "upstream-idle-timeout-sec", required_argument, 0, OPT_UPSTREAM_IDLE_TIMEOUT_SEC },
  { "upstream-pause-timeout-sec", required_argument, 0, OPT_UPSTREAM_PAUSE_TIMEOUT_SEC },
  { "underrun-rebuffer-percent", required_argument, 0, OPT_UNDERRUN_REBUFFER_PERCENT },
  { "underrun-rebuffer-ms",      required_argument, 0, OPT_UNDERRUN_REBUFFER_MS },
  { "startup-real-delay-ms",     required_argument, 0, OPT_STARTUP_REAL_DELAY_MS },
  { "dump-ingress-wav",          required_argument, 0, OPT_DUMP_INGRESS_WAV },
  { "dump-egress-wav",           required_argument, 0, OPT_DUMP_EGRESS_WAV },
  { "dump-ms",                   required_argument, 0, OPT_DUMP_MS },
  { "startup-analyze-ms",        required_argument, 0, OPT_STARTUP_ANALYZE_MS },
  { "startup-fade-ms",           required_argument, 0, OPT_STARTUP_FADE_MS },
  { "startup-fade-shape",        required_argument, 0, OPT_STARTUP_FADE_SHAPE },
  { "dump-raw-entry-wav",        required_argument, 0, OPT_DUMP_RAW_ENTRY_WAV },
  { "compare-ingress-taps-ms",   required_argument, 0, OPT_COMPARE_INGRESS_TAPS_MS },
  /* Receiver-side diagnostics. Available in every backend mode. */
  { "dump-receiver-payload-wav", required_argument, 0, OPT_DUMP_RECEIVER_PAYLOAD_WAV },
  { "dump-raw-stdout-wav",       required_argument, 0, OPT_DUMP_RAW_STDOUT_WAV },
  { "compare-receiver-tap-ms",   required_argument, 0, OPT_COMPARE_RECEIVER_TAP_MS },
  /* Current buffer option names. */
  { "pcm-buffer-ms",             required_argument, 0, OPT_PCM_BUFFER_MS },
  { "pcm-prefill-ms",            required_argument, 0, OPT_PCM_PREFILL_MS },
  { "dsd-buffer-ms",             required_argument, 0, OPT_DSD_BUFFER_MS },
  { "dsd-prefill-ms",            required_argument, 0, OPT_DSD_PREFILL_MS },
  { "dsd-startup-warmup-ms",     required_argument, 0, OPT_DSD_STARTUP_WARMUP_MS },
  { "udp-rcvbuf-bytes",          required_argument, 0, OPT_UDP_RCVBUF_BYTES },
  { "udp-busy-poll-us",          required_argument, 0, OPT_UDP_BUSY_POLL_US },
  { "enable-nic-timestamp",      no_argument,       0, OPT_ENABLE_NIC_TIMESTAMP },
  { "allowed-source-ip",         required_argument, 0, OPT_ALLOWED_SOURCE_IP },
  { "cpu-scream",                required_argument, 0, OPT_CPU_SCREAM },
  { "cpu-audio",                 required_argument, 0, OPT_CPU_AUDIO },
  { "cpu-other",                 required_argument, 0, OPT_CPU_OTHER },
  { "rt-priority",               required_argument, 0, OPT_RT_PRIORITY },
  { "no-mlock",                  no_argument,       0, OPT_NO_MLOCK },
  { 0, 0, 0, 0 }
};

int main(int argc, char*argv[]) {
  /* systemd multi-line ExecStart expands undefined env vars to empty
   * strings, e.g. ${VERBOSE} when VERBOSE is commented out.  Strip
   * them so getopt does not receive stray "" tokens. */
  int j = 1;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && argv[i][0] != '\0') {
      argv[j++] = argv[i];
    }
  }
  argc = j;

  void (*receiver_rcv_fn)(receiver_data_t* receiver_data);
  receiver_data_t receiver_data;

  int (*output_send_fn)(receiver_data_t* receiver_data);

  enum receiver_type receiver_mode = Multicast;

  /* Default output is Diretta. Running scream2diretta with no -o
   * option is equivalent to '-o diretta' (which is the program's reason
   * to exist). Fall back to Raw only when Diretta is not compiled in. */
#if DIRETTA_ENABLE
  enum output_type output_mode = Diretta;
#else
  enum output_type output_mode = Raw;
#endif
  int output_mode_user_set = 0;

  char *multicast_group      = NULL;
  char *output               = NULL;
  const char* interface_name = NULL;
  in_addr_t interface        = INADDR_ANY;
  uint16_t port              = DEFAULT_PORT;
  int quiet                  = 0;
  int legacy_mode            = 0;
  int do_list_targets        = 0;
  /* PCM dump tracking. Persisted across the option loop because we
   * may need to retroactively set the default --dump-ms once we know any
   * --dump-*-wav option was passed. The pointers stored in dcfg below
   * reference these (strdup'd) strings, which live for the whole run. */
  int dump_ms_user_set       = 0;
  char *dump_ingress_prefix  = NULL;
  char *dump_egress_prefix   = NULL;
  int dump_ms_value          = 0;
  /* Startup analyse/fade. Tracked separately because
   * --startup-analyze-ms has an auto-on default (100 ms) when any dump is
   * enabled or verbosity >= 2; the user can still force it off with
   * --startup-analyze-ms 0. The other knobs default to 0 = disabled. */
  int startup_analyze_ms_user_set = 0;
  int startup_analyze_ms_value    = 0;
  int startup_fade_ms_value       = 0;
  int startup_fade_shape_value    = 0; /* 0=linear, 1=cosine */
  /* Raw-entry tap + ingress-tap comparator. */
  char *dump_raw_entry_prefix          = NULL;
  int   compare_ingress_taps_ms_value = 0;
  int   compare_ingress_taps_ms_user_set = 0;
  /* Receiver-payload tap + raw-stdout tap + receiver-tap comparator. */
  char *dump_receiver_payload_prefix = NULL;
  char *dump_raw_stdout_prefix       = NULL;
  int   compare_receiver_tap_ms_value = 0;
  int   compare_receiver_tap_ms_user_set = 0;
  /* Transport knobs. udp_rcvbuf defaults to 4 MiB when the active
   * output is Diretta; the user can override or disable (0) explicitly. */
  int   udp_rcvbuf_bytes      = 0;
  int   udp_rcvbuf_user_set   = 0;
  int   udp_busy_poll_us      = 0;   /* SO_BUSY_POLL window (us). 0 = off. */
  int   enable_nic_timestamp  = 0;   /* SO_TIMESTAMPNS on the UDP socket. */
  int   memlock_enabled       = 1;   /* mlockall(MCL_CURRENT|MCL_FUTURE); --no-mlock to disable. */
  char *allowed_source_ip     = NULL;

#if DIRETTA_ENABLE
  diretta_config_t dcfg;
  diretta_config_init(&dcfg);
#endif

  int opt;
  int longindex = 0;

  while ((opt = getopt_long(argc, argv,
                            "i:g:p:x:o:t:uLvqh",
                            long_options, &longindex)) != -1) {
    switch (opt) {
    case 'i':
      interface_name = strdup(optarg);
      break;
    case 'p':
      port = atoi(optarg);
      if (!port) show_usage(argv[0]);
      break;
    case 'u':
      receiver_mode = Unicast;
      break;
    case 'g':
      multicast_group = strdup(optarg);
      break;
    case 'L':
      legacy_mode = 1;
      break;
    case 'o':
      output = strdup(optarg);
      /* Only 'raw' and 'diretta' are supported. The legacy backends
       * (pulse/alsa/jack/sndio) have been removed. The 'stdout' spelling
       * is kept as a hidden alias for 'raw' so old shell scripts keep
       * working. */
      if (strcmp(output,"raw") == 0 || strcmp(output,"stdout") == 0) output_mode = Raw;
      else if (strcmp(output,"diretta") == 0) output_mode = Diretta;
      else {
        fprintf(stderr, "output backend '%s' has been removed; only 'raw' and 'diretta' are supported\n", output);
        return 1;
      }
      output_mode_user_set = 1;
      break;
    case 't':
#if DIRETTA_ENABLE
      dcfg.target_index = atoi(optarg);
      if (dcfg.target_index < 1) {
        fprintf(stderr, "--target/-t must be >= 1 (1-based index)\n");
        return 1;
      }
#else
      fprintf(stderr, "-t requires Diretta support; not compiled in\n");
      return 1;
#endif
      break;
    case 'v':
      verbosity += 1;
      break;
    case 'q':
      quiet = 1;
      break;
    case 'h':
      show_usage(argv[0]);
      break;
    case OPT_LIST_TARGETS:
      do_list_targets = 1;
      break;
    case OPT_VERSION:
      printf("scream2diretta %s\n", SCREAM2DIRETTA_VERSION);
      return 0;
#if DIRETTA_ENABLE
    case OPT_THREAD_MODE:
      dcfg.thread_mode = (unsigned)strtoul(optarg, NULL, 0);
      break;
    case OPT_CYCLE_TIME:
      dcfg.cycle_us = atoi(optarg);
      if (dcfg.cycle_us != 0 && (dcfg.cycle_us < 333 || dcfg.cycle_us > 10000)) {
        fprintf(stderr, "--cycle-time must be 0 (auto) or 333..10000 us\n");
        return 1;
      }
      break;
    case OPT_CYCLE_MIN_TIME:
      dcfg.cycle_min_us = atoi(optarg);
      if (dcfg.cycle_min_us < 0) show_usage(argv[0]);
      break;
    case OPT_INFO_CYCLE:
      dcfg.info_cycle_us = atoi(optarg);
      if (dcfg.info_cycle_us <= 0) show_usage(argv[0]);
      break;
    case OPT_TRANSFER_MODE:
      if (parse_transfer_mode(optarg, &dcfg.transfer_mode) != 0) {
        fprintf(stderr, "--transfer-mode must be one of: auto, varmax, varauto, fixauto, autofix, random\n");
        return 1;
      }
      break;
    case OPT_TARGET_PROFILE_LIMIT:
      dcfg.target_profile_limit_us = atoi(optarg);
      if (dcfg.target_profile_limit_us < 0) show_usage(argv[0]);
      break;
    case OPT_MTU:
      dcfg.mtu_override = atoi(optarg);
      if (dcfg.mtu_override < 0) show_usage(argv[0]);
      break;
    case OPT_RING_BUFFER_MS:
      /* Compatibility alias for --pcm-buffer-ms. */
      dcfg.ring_buffer_ms = atoi(optarg);
      if (dcfg.ring_buffer_ms < 50 || dcfg.ring_buffer_ms > 5000) {
        fprintf(stderr, "--ring-buffer-ms must be 50..5000\n");
        return 1;
      }
      fprintf(stderr, "[scream2diretta] NOTE: --ring-buffer-ms is a compatibility alias; "
              "use --pcm-buffer-ms instead.\n");
      break;
    case OPT_PCM_BUFFER_MS:
      dcfg.ring_buffer_ms = atoi(optarg);
      if (dcfg.ring_buffer_ms < 50 || dcfg.ring_buffer_ms > 5000) {
        fprintf(stderr, "--pcm-buffer-ms must be 50..5000\n");
        return 1;
      }
      break;
    case OPT_PREFILL_MS:
      /* Compatibility alias for --pcm-prefill-ms. */
      dcfg.prefill_ms = atoi(optarg);
      if (dcfg.prefill_ms < 0) show_usage(argv[0]);
      fprintf(stderr, "[scream2diretta] NOTE: --prefill-ms is a compatibility alias; "
              "use --pcm-prefill-ms instead.\n");
      break;
    case OPT_PCM_PREFILL_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--pcm-prefill-ms must be 0..5000\n");
        return 1;
      }
      dcfg.prefill_ms = v;
      break;
    }
    case OPT_DSD_BUFFER_MS: {
      int v = atoi(optarg);
      if (v < 50 || v > 5000) {
        fprintf(stderr, "--dsd-buffer-ms must be 50..5000\n");
        return 1;
      }
      dcfg.dsd_buffer_ms = v;
      break;
    }
    case OPT_DSD_PREFILL_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--dsd-prefill-ms must be 0..5000\n");
        return 1;
      }
      dcfg.dsd_prefill_ms = v;
      break;
    }
    case OPT_DSD_STARTUP_WARMUP_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 2000) {
        fprintf(stderr, "--dsd-startup-warmup-ms must be 0..2000\n");
        return 1;
      }
      dcfg.dsd_startup_warmup_ms = v;
      break;
    }
    case OPT_UDP_RCVBUF_BYTES: {
      int v = atoi(optarg);
      if (v < 0 || v > (1 << 30)) {
        fprintf(stderr, "--udp-rcvbuf-bytes must be 0..1073741824\n");
        return 1;
      }
      udp_rcvbuf_bytes    = v;
      udp_rcvbuf_user_set = 1;
      break;
    }
    case OPT_UDP_BUSY_POLL_US: {
      int v = atoi(optarg);
      if (v < 0 || v > 1000000) {
        fprintf(stderr, "--udp-busy-poll-us must be 0..1000000\n");
        return 1;
      }
      udp_busy_poll_us = v;
      break;
    }
    case OPT_ENABLE_NIC_TIMESTAMP:
      enable_nic_timestamp = 1;
      break;
    case OPT_ALLOWED_SOURCE_IP:
      allowed_source_ip = strdup(optarg);
      break;
    case OPT_CPU_SCREAM: {
      int v = atoi(optarg);
      if (v < 0) {
        fprintf(stderr, "--cpu-scream must be >= 0\n");
        return 1;
      }
#if defined(__linux__)
      if (v >= CPU_SETSIZE) {
        fprintf(stderr, "--cpu-scream must be < %d\n", CPU_SETSIZE);
        return 1;
      }
#endif
      dcfg.cpu_scream = v;
      break;
    }
    case OPT_CPU_AUDIO: {
      int v = atoi(optarg);
      if (v < 0) {
        fprintf(stderr, "--cpu-audio must be >= 0\n");
        return 1;
      }
#if defined(__linux__)
      if (v >= CPU_SETSIZE) {
        fprintf(stderr, "--cpu-audio must be < %d\n", CPU_SETSIZE);
        return 1;
      }
#endif
      dcfg.cpu_audio = v;
      break;
    }
    case OPT_CPU_OTHER: {
      int v = atoi(optarg);
      if (v < 0) {
        fprintf(stderr, "--cpu-other must be >= 0\n");
        return 1;
      }
#if defined(__linux__)
      if (v >= CPU_SETSIZE) {
        fprintf(stderr, "--cpu-other must be < %d\n", CPU_SETSIZE);
        return 1;
      }
#endif
      dcfg.cpu_other = v;
      break;
    }
    case OPT_RT_PRIORITY: {
      int v = atoi(optarg);
      if (v != -1 && (v < 1 || v > 99)) {
        fprintf(stderr, "--rt-priority must be -1 (disabled) or 1..99\n");
        return 1;
      }
      dcfg.rt_priority = v;
      break;
    }
    case OPT_NO_MLOCK:
      memlock_enabled = 0;
      break;
    case OPT_REBUFFER_PERCENT: {
      double pct = atof(optarg);
      if (pct < 0.0 || pct > 95.0) {
        fprintf(stderr, "--rebuffer-percent must be 0..95\n");
        return 1;
      }
      dcfg.rebuffer_percent = (float)(pct / 100.0);
      break;
    }
    case OPT_STARTUP_QUEUE_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--startup-queue-ms must be 0..5000\n");
        return 1;
      }
      dcfg.startup_queue_ms = v;
      break;
    }
    case OPT_STARTUP_MUTE_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 2000) {
        fprintf(stderr, "--startup-mute-ms must be 0..2000\n");
        return 1;
      }
      dcfg.startup_mute_ms = v;
      break;
    }
    case OPT_STATS_INTERVAL: {
      int s = atoi(optarg);
      if (s < 0 || s > 86400) {
        fprintf(stderr, "--stats-interval must be 0..86400 seconds\n");
        return 1;
      }
      dcfg.stats_interval_sec = s;
      break;
    }
    case OPT_STATS:
      dcfg.stats_enabled = 1;
      break;
    case OPT_DIRETTA_DEBUG:
      dcfg.diretta_debug = 1;
      break;
    case OPT_FORMAT_CHANGE_COOLDOWN_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--format-change-cooldown-ms must be 0..5000\n");
        return 1;
      }
      dcfg.format_change_cooldown_ms = v;
      break;
    }
    case OPT_UPSTREAM_IDLE_TIMEOUT_SEC: {
      int v = atoi(optarg);
      if (v != 0 && (v < 10 || v > 3600)) {
        fprintf(stderr, "--upstream-idle-timeout-sec must be 0 (disabled) or 10..3600\n");
        return 1;
      }
      dcfg.upstream_idle_timeout_sec = v;
      break;
    }
    case OPT_UPSTREAM_PAUSE_TIMEOUT_SEC: {
      int v = atoi(optarg);
      if (v != 0 && (v < 1 || v > 600)) {
        fprintf(stderr, "--upstream-pause-timeout-sec must be 0 (disabled) or 1..600\n");
        return 1;
      }
      dcfg.upstream_pause_timeout_sec = v;
      break;
    }
    case OPT_UNDERRUN_REBUFFER_PERCENT: {
      double pct = atof(optarg);
      if (pct < 0.0 || pct > 95.0) {
        fprintf(stderr, "--underrun-rebuffer-percent must be 0..95\n");
        return 1;
      }
      dcfg.rebuffer_percent = (float)(pct / 100.0);
      break;
    }
    case OPT_UNDERRUN_REBUFFER_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--underrun-rebuffer-ms must be 0..5000 (0 = use --rebuffer-percent)\n");
        return 1;
      }
      dcfg.underrun_rebuffer_ms = v;
      break;
    }
    case OPT_STARTUP_REAL_DELAY_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--startup-real-delay-ms must be 0..5000 (0 = disabled)\n");
        return 1;
      }
      dcfg.startup_real_delay_ms = v;
      break;
    }
    case OPT_DUMP_INGRESS_WAV:
      if (!optarg || !optarg[0]) {
        fprintf(stderr, "--dump-ingress-wav requires a non-empty path prefix\n");
        return 1;
      }
      dump_ingress_prefix = strdup(optarg);
      break;
    case OPT_DUMP_EGRESS_WAV:
      if (!optarg || !optarg[0]) {
        fprintf(stderr, "--dump-egress-wav requires a non-empty path prefix\n");
        return 1;
      }
      dump_egress_prefix = strdup(optarg);
      break;
    case OPT_DUMP_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 600000) {
        fprintf(stderr, "--dump-ms must be 0..600000 (0 = uncapped)\n");
        return 1;
      }
      dump_ms_value = v;
      dump_ms_user_set = 1;
      break;
    }
    case OPT_STARTUP_ANALYZE_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--startup-analyze-ms must be 0..5000 (0 = disabled)\n");
        return 1;
      }
      startup_analyze_ms_value    = v;
      startup_analyze_ms_user_set = 1;
      break;
    }
    case OPT_STARTUP_FADE_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 1000) {
        fprintf(stderr, "--startup-fade-ms must be 0..1000 (0 = disabled, preserves bit purity)\n");
        return 1;
      }
      startup_fade_ms_value = v;
      break;
    }
    case OPT_STARTUP_FADE_SHAPE: {
      if (strcmp(optarg, "linear") == 0) startup_fade_shape_value = 0;
      else if (strcmp(optarg, "cosine") == 0) startup_fade_shape_value = 1;
      else {
        fprintf(stderr, "--startup-fade-shape must be 'linear' or 'cosine'\n");
        return 1;
      }
      break;
    }
    case OPT_DUMP_RAW_ENTRY_WAV:
      if (!optarg || !optarg[0]) {
        fprintf(stderr, "--dump-raw-entry-wav requires a non-empty path prefix\n");
        return 1;
      }
      dump_raw_entry_prefix = strdup(optarg);
      break;
    case OPT_COMPARE_INGRESS_TAPS_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--compare-ingress-taps-ms must be 0..5000 (0 = disabled)\n");
        return 1;
      }
      compare_ingress_taps_ms_value    = v;
      compare_ingress_taps_ms_user_set = 1;
      break;
    }
    case OPT_DUMP_RECEIVER_PAYLOAD_WAV:
      if (!optarg || !optarg[0]) {
        fprintf(stderr, "--dump-receiver-payload-wav requires a non-empty path prefix\n");
        return 1;
      }
      dump_receiver_payload_prefix = strdup(optarg);
      break;
    case OPT_DUMP_RAW_STDOUT_WAV:
      if (!optarg || !optarg[0]) {
        fprintf(stderr, "--dump-raw-stdout-wav requires a non-empty path prefix\n");
        return 1;
      }
      dump_raw_stdout_prefix = strdup(optarg);
      break;
    case OPT_COMPARE_RECEIVER_TAP_MS: {
      int v = atoi(optarg);
      if (v < 0 || v > 5000) {
        fprintf(stderr, "--compare-receiver-tap-ms must be 0..5000 (0 = disabled)\n");
        return 1;
      }
      compare_receiver_tap_ms_value    = v;
      compare_receiver_tap_ms_user_set = 1;
      break;
    }
#else
    case OPT_THREAD_MODE:
    case OPT_CYCLE_TIME:
    case OPT_CYCLE_MIN_TIME:
    case OPT_INFO_CYCLE:
    case OPT_TRANSFER_MODE:
    case OPT_TARGET_PROFILE_LIMIT:
    case OPT_MTU:
    case OPT_RING_BUFFER_MS:
    case OPT_PREFILL_MS:
    case OPT_REBUFFER_PERCENT:
    case OPT_STARTUP_QUEUE_MS:
    case OPT_STARTUP_MUTE_MS:
    case OPT_STATS_INTERVAL:
    case OPT_STATS:
    case OPT_DIRETTA_DEBUG:
    case OPT_FORMAT_CHANGE_COOLDOWN_MS:
    case OPT_UPSTREAM_IDLE_TIMEOUT_SEC:
    case OPT_UPSTREAM_PAUSE_TIMEOUT_SEC:
    case OPT_UNDERRUN_REBUFFER_PERCENT:
    case OPT_UNDERRUN_REBUFFER_MS:
    case OPT_STARTUP_REAL_DELAY_MS:
    case OPT_DUMP_INGRESS_WAV:
    case OPT_DUMP_EGRESS_WAV:
    case OPT_DUMP_MS:
    case OPT_STARTUP_ANALYZE_MS:
    case OPT_STARTUP_FADE_MS:
    case OPT_STARTUP_FADE_SHAPE:
    case OPT_DUMP_RAW_ENTRY_WAV:
    case OPT_COMPARE_INGRESS_TAPS_MS:
    case OPT_DUMP_RECEIVER_PAYLOAD_WAV:
    case OPT_DUMP_RAW_STDOUT_WAV:
    case OPT_COMPARE_RECEIVER_TAP_MS:
    case OPT_PCM_BUFFER_MS:
    case OPT_PCM_PREFILL_MS:
    case OPT_UDP_RCVBUF_BYTES:
    case OPT_ALLOWED_SOURCE_IP:
    case OPT_CPU_SCREAM:
    case OPT_CPU_AUDIO:
    case OPT_CPU_OTHER:
      fprintf(stderr, "Diretta options require Diretta support; not compiled in\n");
      return 1;
#endif
    default:
      show_usage(argv[0]);
    }
  }

#if DIRETTA_ENABLE
  // Map verbosity / quiet to Diretta SDK log level.
  if (quiet) dcfg.log_level = DIRETTA_LOG_WARN;
  else if (verbosity > 0) dcfg.log_level = DIRETTA_LOG_DEBUG;
  else dcfg.log_level = DIRETTA_LOG_DEFAULT;

  if (do_list_targets) {
    return diretta_list_targets(&dcfg, argv[0]);
  }
#else
  (void)quiet;
  if (do_list_targets) {
    fprintf(stderr, "--list-targets requires Diretta support; not compiled in\n");
    return 1;
  }
#endif

  // Carry per-Diretta knobs that the existing CLI also sets.
  /* Default UDP SO_RCVBUF = 4 MiB when output is Diretta. The user can
   * override with --udp-rcvbuf-bytes <n> (0 = leave kernel default). For all
   * other backends the kernel default is fine and we leave the socket alone
   * unless the user asked for something specific. */
  if (!udp_rcvbuf_user_set) {
    udp_rcvbuf_bytes = (output_mode == Diretta) ? (4 * 1024 * 1024) : 0;
  }

#if DIRETTA_ENABLE
  dcfg.udp_rcvbuf_bytes = udp_rcvbuf_bytes;
  /* PCM dump wiring. If the user enabled any dump but did NOT pass
   * --dump-ms, default to 3 s per file (long enough to inspect the head
   * of the track / the artifact window, short enough to avoid filling
   * disks on the Pi). 0 stays uncapped if the user asked for that. */
  dcfg.dump_ingress_prefix = dump_ingress_prefix;
  dcfg.dump_egress_prefix  = dump_egress_prefix;
  /* Raw-entry tap. Treated as a third dump option so the
   * --dump-ms default behaviour also covers it (3 s per file when any
   * dump is enabled and the user did not pass --dump-ms). */
  dcfg.dump_raw_entry_prefix = dump_raw_entry_prefix;
  if (dump_ingress_prefix || dump_egress_prefix || dump_raw_entry_prefix) {
    if (dump_ms_user_set) {
      dcfg.dump_ms = dump_ms_value;
    } else {
      dcfg.dump_ms = 3000;
    }
  } else {
    dcfg.dump_ms = dump_ms_value;
  }
  /* Ingress-tap comparator. Auto-default 1000 ms when BOTH the
   * raw-entry dump and the ingress dump are enabled and the user
   * did not pass --compare-ingress-taps-ms explicitly. Otherwise 0
   * (disabled). The comparator is a no-op unless both taps are armed. */
  if (compare_ingress_taps_ms_user_set) {
    dcfg.compare_ingress_taps_ms = compare_ingress_taps_ms_value;
  } else if (dump_raw_entry_prefix && dump_ingress_prefix) {
    dcfg.compare_ingress_taps_ms = 1000;
  } else {
    dcfg.compare_ingress_taps_ms = 0;
  }
  /* Startup analysis/fade. The analyser auto-enables at 100 ms when
   * verbosity >= 2 or any --dump-*-wav is enabled (so the diagnostic data
   * lands in the log alongside the dump files). The user can disable it
   * explicitly with --startup-analyze-ms 0. The fade and shape default to
   * 0/linear (no audio change).
   *
   * In SCREAM2DIRETTA_NO_DIAGNOSTICS builds the auto-enable is suppressed
   * so a bare `-vv` does not silently arm a facility the binary cannot
   * service (which would then trigger the [diretta] "diagnostics not
   * compiled into this binary" warning at init). Explicit user flags
   * still flow through and still trigger the warning, which is intended.
   */
  if (startup_analyze_ms_user_set) {
    dcfg.startup_analyze_ms = startup_analyze_ms_value;
#ifndef SCREAM2DIRETTA_NO_DIAGNOSTICS
  } else if (verbosity >= 2 || dump_ingress_prefix || dump_egress_prefix) {
    dcfg.startup_analyze_ms = 100;
#endif
  } else {
    dcfg.startup_analyze_ms = 0;
  }
  dcfg.startup_fade_ms    = startup_fade_ms_value;
  dcfg.startup_fade_shape = startup_fade_shape_value;
#else
  (void)dump_ingress_prefix;
  (void)dump_egress_prefix;
  (void)dump_ms_value;
  (void)dump_ms_user_set;
  (void)startup_analyze_ms_user_set;
  (void)startup_analyze_ms_value;
  (void)startup_fade_ms_value;
  (void)startup_fade_shape_value;
  (void)dump_raw_entry_prefix;
  (void)compare_ingress_taps_ms_value;
  (void)compare_ingress_taps_ms_user_set;
  (void)dump_receiver_payload_prefix;
  (void)dump_raw_stdout_prefix;
  (void)compare_receiver_tap_ms_value;
  (void)compare_receiver_tap_ms_user_set;
  #endif
  (void)output_mode_user_set;

  if (interface_name) {
      interface = get_interface(interface_name);
  }

  if (optind < argc) {
    fprintf(stderr, "Expected argument after options\n");
    show_usage(argv[0]);
  }

  // Opportunistic call to renice us, so we can keep up under
  // higher load conditions. This may fail when run as non-root.
  setpriority(PRIO_PROCESS, 0, -11);

  // initialize output
  switch (output_mode) {
    case Raw:
      if (verbosity) fprintf(stderr, "Using raw output\n");
      if (raw_output_init() != 0) {
        return 1;
      }
      output_send_fn = raw_output_send;
      break;
    case Diretta:
#if DIRETTA_ENABLE
      if (verbosity) fprintf(stderr, "Using Diretta output\n");
      if (diretta_output_init(&dcfg) != 0) {
        return 1;
      }
      output_send_fn = diretta_output_send;
#else
      fprintf(stderr, "%s compiled without Diretta support. Aborting\n", argv[0]);
      return 1;
#endif
      break;
    default:
      break;
  }

  /* CPU affinity for the Scream receiver thread (main thread).
   * Applied here before entering the receive loop. */
#if DIRETTA_ENABLE
  if (dcfg.cpu_scream >= 0) {
    diretta_apply_cpu_affinity(dcfg.cpu_scream);
  }
  if (dcfg.rt_priority >= 1) {
    diretta_apply_rt_priority(dcfg.rt_priority);
  }
#endif

  /* Receiver-side diagnostic taps. Backend-agnostic; default off.
   * active_backend selects which downstream tap the receiver-payload
   * buffer is compared against:
   *   1 = raw_stdout (only meaningful with -o raw / Raw)
   *   2 = diretta_raw_entry (only meaningful with -o diretta)
   *   0 = no comparator (everything still dumps + analyses if enabled).
   *
   * The auto-default for --compare-receiver-tap-ms is 1000 ms when the
   * receiver-payload dump is enabled AND the relevant backend dump is
   * enabled in this run. Otherwise 0. The comparator is a no-op unless
   * both sides actually feed; harmless to leave armed.
   *
   * The auto-default for --dump-ms / --startup-analyze-ms follows the
   * same policy as the Diretta path: when any dump is enabled
   * and the user did not pass --dump-ms, default to 3000 ms per file.
   * The analyser auto-enables at 100 ms when any dump is enabled or
   * verbosity >= 2; the user can force-disable with 0. */
  {
    int active_backend = 0;
    int relevant_backend_dump_enabled = 0;
    if (output_mode == Raw) {
      active_backend = 1;
      relevant_backend_dump_enabled = (dump_raw_stdout_prefix != NULL);
    } else if (output_mode == Diretta) {
      active_backend = 2;
      relevant_backend_dump_enabled = (dump_raw_entry_prefix != NULL);
    }

    int rt_dump_ms;
    if (dump_receiver_payload_prefix || dump_raw_stdout_prefix ||
        dump_ingress_prefix || dump_egress_prefix || dump_raw_entry_prefix) {
      rt_dump_ms = dump_ms_user_set ? dump_ms_value : 3000;
    } else {
      rt_dump_ms = dump_ms_value;
    }

    int rt_analyze_ms;
    if (startup_analyze_ms_user_set) {
      rt_analyze_ms = startup_analyze_ms_value;
#ifndef SCREAM2DIRETTA_NO_DIAGNOSTICS
    } else if (verbosity >= 2 ||
               dump_receiver_payload_prefix ||
               dump_raw_stdout_prefix ||
               dump_ingress_prefix ||
               dump_egress_prefix ||
               dump_raw_entry_prefix) {
      rt_analyze_ms = 100;
#endif
    } else {
      rt_analyze_ms = 0;
    }

    int rt_compare_ms;
    if (compare_receiver_tap_ms_user_set) {
      rt_compare_ms = compare_receiver_tap_ms_value;
    } else if (dump_receiver_payload_prefix && relevant_backend_dump_enabled) {
      rt_compare_ms = 1000;
    } else {
      rt_compare_ms = 0;
    }

    receiver_tap_init(dump_receiver_payload_prefix,
                      dump_raw_stdout_prefix,
                      rt_dump_ms,
                      rt_analyze_ms,
                      rt_compare_ms,
                      active_backend);
  }

  // Install SIGINT/SIGTERM handlers for every backend so that Ctrl-C exits
  // the receiver loop within a single select() poll period (~500ms).
  install_term_handlers();

  /* Lock all current and future pages in RAM so the kernel cannot page
   * out PcmRing / SDK buffers / TLS / stack under memory pressure. A
   * paged-out hot page would otherwise cause a multi-ms major fault on
   * the next access, which is enough to underrun the Diretta pull cycle.
   *
   * Two-step bring-up:
   *   1. Try to raise RLIMIT_MEMLOCK to RLIM_INFINITY. Only effective
   *      when the process has CAP_SYS_RESOURCE (typically root). On
   *      systemd we expect LimitMEMLOCK=infinity in the unit instead.
   *   2. Call mlockall(MCL_CURRENT | MCL_FUTURE). MCL_FUTURE is the
   *      important half: receiver buffers / Diretta SDK allocations
   *      / pthread stacks created later are also pinned.
   *
   * Failure (EPERM, ENOMEM, EAGAIN) is non-fatal: we log a warning and
   * keep running with default paging behaviour. --no-mlock disables
   * the call entirely for systems where pinning is undesirable. */
  if (memlock_enabled) {
#if defined(__linux__)
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
      if (rl.rlim_cur != RLIM_INFINITY || rl.rlim_max != RLIM_INFINITY) {
        struct rlimit want;
        want.rlim_cur = RLIM_INFINITY;
        want.rlim_max = RLIM_INFINITY;
        /* Best effort. If we cannot raise the limit (no
         * CAP_SYS_RESOURCE), mlockall may still partially succeed up
         * to the existing limit and pin the most critical pages. */
        (void)setrlimit(RLIMIT_MEMLOCK, &want);
      }
    }
#endif
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
      if (verbosity) {
        fprintf(stderr,
                "[scream2diretta] mlockall(MCL_CURRENT|MCL_FUTURE) ok: "
                "current+future pages pinned in RAM\n");
      }
    } else {
      int err = errno;
      fprintf(stderr,
              "[scream2diretta] WARNING: mlockall failed (errno=%d %s); "
              "running without page locking. Pass --no-mlock to silence "
              "this warning, or grant CAP_IPC_LOCK / "
              "LimitMEMLOCK=infinity to enable it.\n",
              err, strerror(err));
    }
  } else if (verbosity) {
    fprintf(stderr,
            "[scream2diretta] mlockall disabled by --no-mlock\n");
  }

  // initialize receiver
  if (verbosity) fprintf(stderr, "Starting %s receiver\n", receiver_mode == Unicast ? "unicast" : "multicast");
  if (init_network(receiver_mode, interface, port, multicast_group,
                     udp_rcvbuf_bytes, allowed_source_ip,
                     udp_busy_poll_us, enable_nic_timestamp,
                     legacy_mode) != 0) {
    return 1;
  }
  receiver_rcv_fn = rcv_network;


  int rc = 0;
  while (!g_shutdown_pending) {
    receiver_data.audio_size = 0;
    receiver_data.audio = NULL;
    receiver_data.nic_timestamp_ns = 0;
    receiver_rcv_fn(&receiver_data);
    if (g_shutdown_pending) break;
    if (receiver_data.audio_size == 0 || receiver_data.audio == NULL) {
      // Receiver returned without data (timeout, EINTR, transient producer
      // gap). Loop and re-check the shutdown flag. Also tick the Diretta
      // backend so upstream-idle release can fire even when no packets are
      // arriving at all (full upstream silence delivers no audio packets,
      // so diretta_output_send is never called on this branch).
#if DIRETTA_ENABLE
      if (output_mode == Diretta) {
        diretta_output_tick();
      }
#endif
      continue;
    }
    /* Backend-independent payload tap. Tapped AFTER the receiver
     * parsed/validated the Scream packet and BEFORE output dispatch, so
     * data->audio / data->audio_size match exactly what the backend
     * receives. Source format is recovered from data->format the same
     * way every backend does it. No-op unless --dump-receiver-payload-wav
     * or --compare-receiver-tap-ms is in effect.
     *
     * Fast-path: skip even the format decode when no diagnostic facility
     * is armed. The static inline gate reads a single int set once at
     * init, so the common fully-off case folds into a predictable branch
     * with zero function-call overhead. */
    if (receiver_tap_any_armed()) {
      const receiver_format_t* rf = &receiver_data.format;
      uint32_t rate_hz = rf->sample_rate;
      uint32_t bits = (uint32_t)rf->sample_size;
      uint32_t chans = (uint32_t)rf->channels;
      receiver_tap_payload_feed(receiver_data.audio,
                                (size_t)receiver_data.audio_size,
                                rate_hz, bits, chans);
    }
    if (output_send_fn(&receiver_data) != 0) {
      rc = 1;
      break;
    }
  }

#if DIRETTA_ENABLE
  if (output_mode == Diretta) {
    diretta_output_shutdown();
  }
#endif
  receiver_tap_shutdown();
  return rc;
};
