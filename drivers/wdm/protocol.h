#pragma once
#define OPENAOIP_WDM_VERSION 2
#define OPENAOIP_WDM_PAIRS 16
#define OPENAOIP_WDM_RATE 48000
#define OPENAOIP_WDM_BLOCK_FRAMES 240
#define OPENAOIP_IOCTL_STATUS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
#define OPENAOIP_IOCTL_CONFIGURE                                                                   \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define OPENAOIP_IOCTL_READ_RENDER                                                                 \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA)
#define OPENAOIP_IOCTL_WRITE_CAPTURE                                                               \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_WRITE_DATA)
typedef struct OPENAOIP_WDM_CONFIG {
  unsigned long version, tx_channels, rx_channels, sample_rate;
} OPENAOIP_WDM_CONFIG;
typedef struct OPENAOIP_WDM_STATUS {
  OPENAOIP_WDM_CONFIG config;
  unsigned long render_mask, capture_mask, owner_pid, pair_count;
  unsigned long long render_frames, capture_frames, render_dropped, capture_underruns;
} OPENAOIP_WDM_STATUS;
struct OPENAOIP_WDM_BLOCK {
  unsigned long version, pair, frames, running;
  short samples[OPENAOIP_WDM_BLOCK_FRAMES * 2];
};
static_assert(sizeof(OPENAOIP_WDM_CONFIG) == 16);
static_assert(sizeof(OPENAOIP_WDM_STATUS) == 64);
static_assert(sizeof(OPENAOIP_WDM_BLOCK) == 976);
