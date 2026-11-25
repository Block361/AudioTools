#pragma once
// Kernel and user-mode tests execute the same bounded queue implementation.
// Callers serialize access; no allocation, CRT calls or locks inside the queue.
namespace aoip_wdm {
inline constexpr unsigned capacity = 4800;
struct Queue {
  short samples[capacity * 2]{};
  unsigned long long read = 0, write = 0, dropped = 0, underruns = 0, frames = 0;
  void clear() {
    read = write;
  }
  unsigned push(const short* source, unsigned count) {
    unsigned n = 0;
    for (; n < count && write - read < capacity; ++n, ++write) {
      const auto at = (write % capacity) * 2;
      samples[at] = source[n * 2];
      samples[at + 1] = source[n * 2 + 1];
    }
    dropped += count - n;
    frames += n;
    return n;
  }
  unsigned pop(short* target, unsigned count, bool count_underruns = true) {
    unsigned n = 0;
    for (; n < count && read < write; ++n, ++read) {
      const auto at = (read % capacity) * 2;
      target[n * 2] = samples[at];
      target[n * 2 + 1] = samples[at + 1];
    }
    for (unsigned i = n * 2; i < count * 2; ++i)
      target[i] = 0;
    if (count_underruns)
      underruns += count - n;
    return n;
  }
};
inline bool valid_config(unsigned version, unsigned tx, unsigned rx, unsigned rate) {
  return version == 2 && tx >= 1 && tx <= 32 && rx >= 1 && rx <= 32 && rate == 48000;
}
inline bool valid_block(unsigned version, unsigned pair, unsigned frames) {
  return version == 2 && pair < 16 && frames >= 1 && frames <= 240;
}
} // namespace aoip_wdm
