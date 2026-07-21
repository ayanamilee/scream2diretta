#ifndef SCREAM_H
#define SCREAM_H

#include <stdint.h>
#include <signal.h>

enum receiver_type {
  Unicast, Multicast
};

enum output_type {
  Raw, Diretta
};

#define SCREAM_SUPPORTED_CHANNELS 2

/* wire_layout byte (header byte[5]). Meaningful only for 24-bit PCM;
 * receivers must ignore it for all other sample sizes.
 */
enum scream_wire_layout {
  SCREAM_WIRE_PACKED = 0,   /* S24_3LE: 3 bytes per sample on the wire */
  SCREAM_WIRE_S24_LE = 1,   /* S24_LE: 24-bit samples in 32-bit LE containers */
};

typedef struct receiver_format {
  uint32_t sample_rate;        /* Decoded rate value (Hz). For DSD this is the halved container rate. */
  unsigned char sample_size;   /* 1=DSD, 16/24/32=PCM */
  unsigned char channels;
  uint16_t channel_map;
  unsigned char wire_layout;   /* byte[5]: see enum scream_wire_layout; 0 for legacy */
} receiver_format_t;

typedef struct receiver_data {
  receiver_format_t format;
  unsigned int audio_size;
  unsigned char* audio;
  /* NIC arrival timestamp in CLOCK_REALTIME nanoseconds, captured by
   * the kernel via SO_TIMESTAMPNS when --enable-nic-timestamp is set.
   * 0 means "unavailable" (option off, or kernel did not provide one). */
  uint64_t nic_timestamp_ns;
} receiver_data_t;

extern int verbosity;

/* Global shutdown flag set from a signal handler (SIGINT/SIGTERM).
 * Receiver implementations must check this on every blocking-syscall
 * EINTR or short-poll wakeup so that Ctrl-C exits within ~1s. */
extern volatile sig_atomic_t g_shutdown_pending;

/* Decode rate from extended 6-byte ScreamALSA header bytes.
 * b0 = byte[0], b4 = byte[4]
 */
static inline uint32_t scream_decode_rate(unsigned char b0, unsigned char b4)
{
  uint16_t mult = (uint16_t)b0 | ((uint16_t)(b4 & 0x0F) << 8);
  unsigned int base = (b4 & 0x10) ? 44100U : 48000U;
  return (uint32_t)base * mult;
}

/* Decode rate from original 5-byte Scream header byte[0].
 * Original encoding: values >= 128 mean 44100 * (value - 128),
 * otherwise 48000 * value.
 */
static inline uint32_t scream_decode_rate_legacy(unsigned char b0)
{
  if (b0 >= 128) {
    return 44100U * (b0 - 128);
  }
  return 48000U * b0;
}

static inline int scream_is_end_of_track(unsigned char b4)
{
  return (b4 & 0x80) != 0;
}

/* Return the number of bytes occupied by one sample on the wire.
 * For PCM this follows wire_layout when sample_size == 24.
 * For DSD (sample_size == 1) it is always 4 bytes (DSD_U32_BE).
 */
static inline unsigned int scream_bytes_per_sample(const receiver_format_t *rf)
{
  switch (rf->sample_size) {
  case 1:  return 4;   /* DSD_U32_BE; wire_layout ignored */
  case 16: return 2;
  case 24: return (rf->wire_layout == SCREAM_WIRE_S24_LE) ? 4 : 3;
  case 32: return 4;
  default: return 0;
  }
}

#endif
