// dsp.h - The signal-processing core: sample conversion, channel mapping, and a
// FIFO that resamples on the way out so a device with its own clock can be kept
// in step with the source. Header-only so the tests can use it directly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sas {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Sample conversion

enum class SampleType { Unsupported, Float32, Int16, Int24, Int32 };

inline int bytesPerSample(SampleType t) {
  switch (t) {
    case SampleType::Float32: return 4;
    case SampleType::Int16: return 2;
    case SampleType::Int24: return 3;
    case SampleType::Int32: return 4;
    default: return 0;
  }
}

// Converts `count` device samples to floats in [-1, 1].
inline void toFloat(SampleType t, const uint8_t* src, float* dst, size_t count) {
  switch (t) {
    case SampleType::Float32:
      std::memcpy(dst, src, count * sizeof(float));
      break;
    case SampleType::Int16:
      for (size_t i = 0; i < count; ++i) {
        int16_t v;
        std::memcpy(&v, src + 2 * i, 2);
        dst[i] = v * (1.0f / 32768.0f);
      }
      break;
    case SampleType::Int24:
      for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = src + 3 * i;
        const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 |
                                               static_cast<uint32_t>(p[1]) << 16 |
                                               static_cast<uint32_t>(p[2]) << 24) >> 8;
        dst[i] = v * (1.0f / 8388608.0f);
      }
      break;
    case SampleType::Int32:
      for (size_t i = 0; i < count; ++i) {
        int32_t v;
        std::memcpy(&v, src + 4 * i, 4);
        dst[i] = static_cast<float>(v * (1.0 / 2147483648.0));
      }
      break;
    default:
      std::memset(dst, 0, count * sizeof(float));
  }
}

// Converts floats back to the device format (same 2^n scaling as toFloat, so a
// round trip is exact); integer formats are clipped.
inline void fromFloat(SampleType t, const float* src, uint8_t* dst, size_t count) {
  switch (t) {
    case SampleType::Float32:
      std::memcpy(dst, src, count * sizeof(float));
      break;
    case SampleType::Int16:
      for (size_t i = 0; i < count; ++i) {
        const int16_t v = static_cast<int16_t>(std::clamp(std::lrint(src[i] * 32768.0f), -32768L, 32767L));
        std::memcpy(dst + 2 * i, &v, 2);
      }
      break;
    case SampleType::Int24:
      for (size_t i = 0; i < count; ++i) {
        const int32_t v = static_cast<int32_t>(std::clamp(std::lrint(src[i] * 8388608.0f), -8388608L, 8388607L));
        uint8_t* p = dst + 3 * i;
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
      }
      break;
    case SampleType::Int32:
      for (size_t i = 0; i < count; ++i) {
        const int32_t v = static_cast<int32_t>(
            std::clamp(std::llrint(src[i] * 2147483648.0), -2147483648LL, 2147483647LL));
        std::memcpy(dst + 4 * i, &v, 4);
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Channel mapping

// Speaker position bits, identical to SPEAKER_* in ksmedia.h.
enum Speaker : uint32_t {
  kFL = 0x1, kFR = 0x2, kFC = 0x4, kLFE = 0x8, kBL = 0x10, kBR = 0x20,
  kFLC = 0x40, kFRC = 0x80, kBC = 0x100, kSL = 0x200, kSR = 0x400,
  kTC = 0x800, kTFL = 0x1000, kTFC = 0x2000, kTFR = 0x4000,
  kTBL = 0x8000, kTBC = 0x10000, kTBR = 0x20000,
};

// The layout Windows assumes when a format carries no channel mask.
inline uint32_t defaultChannelMask(int channels) {
  switch (channels) {
    case 1: return kFC;
    case 2: return kFL | kFR;
    case 3: return kFL | kFR | kFC;
    case 4: return kFL | kFR | kBL | kBR;
    case 5: return kFL | kFR | kFC | kBL | kBR;
    case 6: return kFL | kFR | kFC | kLFE | kBL | kBR;
    case 7: return kFL | kFR | kFC | kLFE | kBL | kBR | kBC;
    case 8: return kFL | kFR | kFC | kLFE | kBL | kBR | kSL | kSR;
    default: return 0;
  }
}

// Speaker position of each channel, in order (0 when unknown).
inline std::vector<uint32_t> channelPositions(int channels, uint32_t mask) {
  if (mask == 0) mask = defaultChannelMask(channels);
  std::vector<uint32_t> pos(static_cast<size_t>(channels), 0);
  int ch = 0;
  for (uint32_t bit = 1; bit != 0 && ch < channels; bit <<= 1)
    if (mask & bit) pos[static_cast<size_t>(ch++)] = bit;
  return pos;
}

namespace detail {

// Adds one source speaker's gain to the destination speakers, substituting the
// nearest neighbours for speakers the destination does not have.
struct Router {
  const std::vector<uint32_t>& dst;
  uint32_t srcMask;  // every speaker present in the source
  float* column;     // gain per destination channel for the current source channel

  int find(uint32_t pos) const {
    for (size_t d = 0; d < dst.size(); ++d)
      if (dst[d] == pos) return static_cast<int>(d);
    return -1;
  }
  bool has(uint32_t pos) const { return find(pos) >= 0; }

  void route(uint32_t pos, float g, int depth = 0) {
    if (depth > 3) return;
    if (const int d = find(pos); d >= 0) {
      column[d] += g;
      return;
    }
    constexpr float h = 0.70710678f;  // -3 dB
    const int next = depth + 1;
    switch (pos) {
      case kFL:
      case kFR:
        route(kFC, g * h, next);
        break;
      case kFC:
      case kTFC:
      case kTC:
        if (has(kFC)) {
          route(kFC, g * h, next);
        } else {
          route(kFL, g * h, next);
          route(kFR, g * h, next);
        }
        break;
      case kLFE:  // no subwoofer channel: dropped, as standard downmixes do
        break;
      case kFLC:
      case kTFL:
        route(kFL, g, next);
        break;
      case kFRC:
      case kTFR:
        route(kFR, g, next);
        break;
      case kSL:
      case kTBL:
        if (has(kBL)) route(kBL, (srcMask & kBL) ? g * h : g, next);
        else route(kFL, g * h, next);
        break;
      case kSR:
      case kTBR:
        if (has(kBR)) route(kBR, (srcMask & kBR) ? g * h : g, next);
        else route(kFR, g * h, next);
        break;
      case kBL:
        if (has(kSL)) route(kSL, (srcMask & kSL) ? g * h : g, next);
        else route(kFL, g * h, next);
        break;
      case kBR:
        if (has(kSR)) route(kSR, (srcMask & kSR) ? g * h : g, next);
        else route(kFR, g * h, next);
        break;
      case kBC:
      case kTBC:
        if (has(kBL) && has(kBR)) {
          route(kBL, g * h, next);
          route(kBR, g * h, next);
        } else if (has(kSL) && has(kSR)) {
          route(kSL, g * h, next);
          route(kSR, g * h, next);
        } else {
          route(kFL, g * 0.5f, next);
          route(kFR, g * 0.5f, next);
        }
        break;
      default:
        break;
    }
  }
};

}  // namespace detail

// Builds a dst x src gain matrix (row-major: m[d * srcCh + s]). Matching speakers
// pass straight through; missing ones are folded into their neighbours with an
// ITU-style downmix. Nothing is normalised, so stereo content inside a 7.1 mix
// keeps its level.
inline std::vector<float> buildChannelMatrix(int srcCh, uint32_t srcMask, int dstCh, uint32_t dstMask) {
  std::vector<float> m(static_cast<size_t>(std::max(0, dstCh)) * std::max(0, srcCh), 0.0f);
  if (srcCh <= 0 || dstCh <= 0) return m;
  auto at = [&](int d, int s) -> float& { return m[static_cast<size_t>(d) * srcCh + s]; };

  if (dstCh == 1) {  // mono output: average of a stereo downmix
    if (srcCh == 1) {
      at(0, 0) = 1.0f;
      return m;
    }
    const auto st = buildChannelMatrix(srcCh, srcMask, 2, kFL | kFR);
    for (int s = 0; s < srcCh; ++s) at(0, s) = 0.5f * (st[s] + st[static_cast<size_t>(srcCh) + s]);
    return m;
  }

  const auto dp = channelPositions(dstCh, dstMask);
  auto dstIndex = [&](uint32_t pos) {
    for (int d = 0; d < dstCh; ++d)
      if (dp[d] == pos) return d;
    return -1;
  };

  if (srcCh == 1) {  // mono input: full level on both front speakers
    const int l = dstIndex(kFL), r = dstIndex(kFR), c = dstIndex(kFC);
    if (l >= 0 && r >= 0) {
      at(l, 0) = 1.0f;
      at(r, 0) = 1.0f;
    } else {
      at(c >= 0 ? c : 0, 0) = 1.0f;
    }
    return m;
  }

  const auto sp = channelPositions(srcCh, srcMask);
  uint32_t srcAll = 0;
  for (uint32_t p : sp) srcAll |= p;
  std::vector<float> column(static_cast<size_t>(dstCh));
  for (int s = 0; s < srcCh; ++s) {
    std::fill(column.begin(), column.end(), 0.0f);
    if (sp[s] == 0) {
      if (s < dstCh) column[s] = 1.0f;  // unknown position: keep the channel index
    } else {
      detail::Router router{dp, srcAll, column.data()};
      router.route(sp[s], 1.0f);
    }
    for (int d = 0; d < dstCh; ++d) at(d, s) = column[d];
  }
  return m;
}

inline bool isIdentityMatrix(const std::vector<float>& m, int srcCh, int dstCh) {
  if (srcCh != dstCh) return false;
  for (int d = 0; d < dstCh; ++d)
    for (int s = 0; s < srcCh; ++s)
      if (m[static_cast<size_t>(d) * srcCh + s] != (d == s ? 1.0f : 0.0f)) return false;
  return true;
}

inline void applyChannelMatrix(const std::vector<float>& m, int srcCh, int dstCh, const float* in,
                               float* out, size_t frames) {
  for (size_t f = 0; f < frames; ++f) {
    const float* x = in + f * srcCh;
    float* y = out + f * dstCh;
    for (int d = 0; d < dstCh; ++d) {
      const float* row = &m[static_cast<size_t>(d) * srcCh];
      float acc = 0.0f;
      for (int s = 0; s < srcCh; ++s) acc += row[s] * x[s];
      y[d] = acc;
    }
  }
}

// ---------------------------------------------------------------------------
// Resampling FIFO

// Modified Bessel function of the first kind, order 0 (for the Kaiser window).
inline double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 100; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

// Interleaved frames go in at the source rate and come out at the destination rate
// through a windowed-sinc interpolator. The read ratio can be nudged on every call,
// which is how the clock drift between two devices is absorbed without clicks.
class ResamplingFifo {
 public:
  void configure(int channels, double srcRate, double dstRate, double capacitySeconds = 0.5) {
    channels_ = std::max(1, channels);
    step_ = srcRate / dstRate;
    // Downsampling needs a lower cutoff, and so a proportionally longer kernel.
    const double scale = std::min(1.0, dstRate / srcRate);
    half_ = static_cast<int>(std::ceil(kBaseHalfTaps / scale));
    const int taps = 2 * half_;
    const double cutoff = kCutoff * scale;
    const double i0beta = besselI0(kBeta);
    phases_.assign(static_cast<size_t>(kPhases + 1) * taps, 0.0f);
    for (int p = 0; p <= kPhases; ++p) {
      const double frac = static_cast<double>(p) / kPhases;
      float* row = &phases_[static_cast<size_t>(p) * taps];
      double sum = 0.0;
      std::vector<double> h(static_cast<size_t>(taps));
      for (int k = 0; k < taps; ++k) {
        const double t = (k - half_ + 1) - frac;  // distance of tap k from the read position
        const double x = t / half_;
        if (std::fabs(x) >= 1.0) continue;
        const double a = kPi * cutoff * t;
        const double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
        h[k] = sinc * besselI0(kBeta * std::sqrt(1.0 - x * x)) / i0beta;
        sum += h[k];
      }
      for (int k = 0; k < taps; ++k) row[k] = static_cast<float>(h[k] / sum);  // unity DC gain per phase
    }
    coeffs_.assign(static_cast<size_t>(taps), 0.0f);

    size_t cap = 1;
    const size_t want = static_cast<size_t>(srcRate * capacitySeconds) + 4 * static_cast<size_t>(half_);
    while (cap < want) cap <<= 1;
    capacity_ = cap;
    data_.assign(2 * cap * channels_, 0.0f);  // every frame is stored twice, see write()
    clear();
  }

  void clear() {
    std::fill(data_.begin(), data_.end(), 0.0f);
    wpos_ = 0;
    rpos_ = 0;
    rfrac_ = 0.0;
  }

  int channels() const { return channels_; }
  int halfWidth() const { return half_; }  // input frames the interpolator looks ahead
  size_t capacity() const { return capacity_; }
  double nominalStep() const { return step_; }

  // Input frames buffered ahead of the read position.
  double fill() const { return static_cast<double>(wpos_ - rpos_) - rfrac_; }

  // Appends frames (nullptr appends silence).
  void write(const float* in, size_t frames) {
    const size_t ch = static_cast<size_t>(channels_);
    while (frames > 0) {
      const size_t slot = static_cast<size_t>(wpos_) & (capacity_ - 1);
      const size_t n = std::min(frames, capacity_ - slot);
      // Mirror each frame into the upper half too, so any window of frames the
      // interpolator reads is contiguous in memory.
      float* a = &data_[slot * ch];
      float* b = &data_[(slot + capacity_) * ch];
      if (in) {
        std::memcpy(a, in, n * ch * sizeof(float));
        std::memcpy(b, in, n * ch * sizeof(float));
        in += n * ch;
      } else {
        std::memset(a, 0, n * ch * sizeof(float));
        std::memset(b, 0, n * ch * sizeof(float));
      }
      wpos_ += static_cast<int64_t>(n);
      frames -= n;
    }
  }

  // Produces `frames` output frames, consuming input at nominalStep * (1 + ratioAdjust)
  // input frames per output frame. Returns how many frames came from buffered input;
  // if the FIFO runs dry the rest are zero-filled and the read position holds.
  size_t read(float* out, size_t frames, double ratioAdjust) {
    const double step = step_ * (1.0 + ratioAdjust);
    // The channel count is fixed per stream; specialising the inner loop for the
    // common ones lets the compiler keep the sums in registers and vectorise.
    size_t n;
    switch (channels_) {
      case 1: n = readFrames<1>(out, frames, step); break;
      case 2: n = readFrames<2>(out, frames, step); break;
      case 6: n = readFrames<6>(out, frames, step); break;
      case 8: n = readFrames<8>(out, frames, step); break;
      default: n = readFrames<0>(out, frames, step); break;
    }
    const size_t ch = static_cast<size_t>(channels_);
    if (n < frames) std::memset(out + n * ch, 0, (frames - n) * ch * sizeof(float));
    return n;
  }

  // Moves the read position so that exactly `fillFrames` input frames are buffered.
  void seekToFill(double fillFrames) {
    const double maxFill = static_cast<double>(capacity_) - 2.0 * half_ - 1.0;
    const double pos = static_cast<double>(wpos_) - std::clamp(fillFrames, 0.0, maxFill);
    rpos_ = static_cast<int64_t>(std::floor(pos));
    rfrac_ = pos - static_cast<double>(rpos_);
  }

 private:
  // CH > 0: compile-time channel count; CH == 0: channels_ at run time.
  template <int CH>
  size_t readFrames(float* out, size_t frames, double step) {
    const size_t ch = CH > 0 ? static_cast<size_t>(CH) : static_cast<size_t>(channels_);
    const int taps = 2 * half_;
    float* __restrict coeffs = coeffs_.data();
    size_t n = 0;
    for (; n < frames; ++n) {
      if (rpos_ + half_ >= wpos_) break;  // need input up to rpos_ + half_
      const double pf = rfrac_ * kPhases;
      const int p = static_cast<int>(pf);
      const float a = static_cast<float>(pf - p);
      const float* __restrict r0 = &phases_[static_cast<size_t>(p) * taps];
      const float* __restrict r1 = r0 + taps;
      for (int k = 0; k < taps; ++k) coeffs[k] = r0[k] + a * (r1[k] - r0[k]);

      const int64_t first = rpos_ - half_ + 1;
      const float* __restrict x = &data_[(static_cast<size_t>(first) & (capacity_ - 1)) * ch];
      float* __restrict y = out + n * ch;
      if constexpr (CH > 0) {
        float acc[CH] = {};
        for (int k = 0; k < taps; ++k) {
          const float w = coeffs[k];
          const float* __restrict xk = x + static_cast<size_t>(k) * CH;
          for (int c = 0; c < CH; ++c) acc[c] += w * xk[c];
        }
        for (int c = 0; c < CH; ++c) y[c] = acc[c];
      } else {
        for (size_t c = 0; c < ch; ++c) y[c] = 0.0f;
        for (int k = 0; k < taps; ++k) {
          const float w = coeffs[k];
          const float* __restrict xk = x + static_cast<size_t>(k) * ch;
          for (size_t c = 0; c < ch; ++c) y[c] += w * xk[c];
        }
      }

      rfrac_ += step;
      const double whole = std::floor(rfrac_);
      rpos_ += static_cast<int64_t>(whole);
      rfrac_ -= whole;
    }
    return n;
  }

  static constexpr int kBaseHalfTaps = 24;  // 48 taps when not downsampling
  static constexpr int kPhases = 256;       // kernel table resolution (linearly interpolated)
  static constexpr double kCutoff = 0.95;   // fraction of Nyquist: flat to ~20 kHz at 48 kHz
  static constexpr double kBeta = 8.0;      // Kaiser window: ~80 dB stopband

  int channels_ = 1;
  int half_ = kBaseHalfTaps;
  double step_ = 1.0;
  size_t capacity_ = 0;  // frames, power of two
  std::vector<float> data_;
  std::vector<float> phases_;
  std::vector<float> coeffs_;
  int64_t wpos_ = 0;   // frames written so far
  int64_t rpos_ = 0;   // integer part of the read position
  double rfrac_ = 0;   // fractional part of the read position, [0, 1)
};

// ---------------------------------------------------------------------------
// Drift control

// Holds a FIFO's fill level at a target by nudging the resampling ratio: a critically
// damped PI loop on a smoothed level. Two sound devices never run at exactly the same
// speed (typically 10-300 ppm apart); this absorbs it. For the first seconds the loop
// runs fast to learn the clock difference before the buffer level wanders far, then
// it settles into slow tracking. The ratio moves by a few hundred ppm at most, far
// below what the ear can detect as pitch.
class DriftController {
 public:
  void reset(double targetSec) {
    target_ = targetSec;
    integral_ = 0.0;
    adjust_ = 0.0;
    elapsed_ = 0.0;
    primed_ = false;
  }
  void setTarget(double targetSec) { target_ = targetSec; }

  // Re-anchors the smoothed level after the FIFO was repositioned. Keeps the learned
  // drift so the loop does not have to converge again.
  void resync(double fillSec) {
    smoothed_ = fillSec;
    primed_ = true;
  }

  // fillSec: buffered input in seconds; dt: seconds since the previous update.
  // Returns the relative ratio adjustment to pass to ResamplingFifo::read.
  double update(double fillSec, double dt) {
    if (!primed_) resync(fillSec);
    dt = std::clamp(dt, 0.0, 0.1);
    smoothed_ += (1.0 - std::exp(-dt / kSmoothingSec)) * (fillSec - smoothed_);
    const bool acquiring = elapsed_ < kAcquireSec;
    elapsed_ += dt;
    const double kp = acquiring ? kAcquireP : kTrackP;
    const double ki = acquiring ? kAcquireI : kTrackI;
    const double err = smoothed_ - target_;
    integral_ = std::clamp(integral_ + ki * err * dt, -kMaxAdjust, kMaxAdjust);
    adjust_ = std::clamp(kp * err + integral_, -kMaxAdjust, kMaxAdjust);
    return adjust_;
  }

  double adjust() const { return adjust_; }
  double drift() const { return integral_; }  // learned long-term clock ratio offset
  double smoothedFill() const { return smoothed_; }
  double target() const { return target_; }

 private:
  // Gains are per second of level error; each pair gives a damping ratio of ~1.
  static constexpr double kSmoothingSec = 1.5;
  static constexpr double kAcquireSec = 20.0;
  static constexpr double kAcquireP = 0.3;
  static constexpr double kAcquireI = 0.0225;
  static constexpr double kTrackP = 0.05;
  static constexpr double kTrackI = 6e-4;
  static constexpr double kMaxAdjust = 0.005;  // 5000 ppm

  double target_ = 0.0;
  double smoothed_ = 0.0;
  double integral_ = 0.0;
  double adjust_ = 0.0;
  double elapsed_ = 0.0;
  bool primed_ = false;
};

// ---------------------------------------------------------------------------
// One source -> one output device

// Everything between the captured source and one output device: channel mapping,
// buffering, drift-compensated resampling and recovery from dropouts. The engine
// feeds it from the WASAPI threads; the tests drive it with simulated clocks.
//
// Audio arrives in packets (10 ms each on most systems) and leaves in chunks of the
// output device's period. Sampling the raw FIFO level at chunk time would show a
// slow sawtooth as the two clocks slide past each other, and the controller would
// chase it. Instead the level is measured as if the source delivered continuously:
// the FIFO contents plus the part of the next packet already "produced" since the
// last one arrived.
class MirrorPipeline {
 public:
  struct Format {
    int channels = 2;
    uint32_t mask = 0;
    double rate = 48000;
  };

  // chunkSec: how much the output device takes per refill (its period).
  // marginSec: extra safety buffer on top of the structural minimum.
  void configure(const Format& src, const Format& dst, double chunkSec, double marginSec) {
    src_ = src;
    dst_ = dst;
    chunkSec_ = chunkSec;
    marginSec_ = baseMarginSec_ = std::min(marginSec, kMaxMarginSec);
    calmSec_ = 0.0;
    matrix_ = buildChannelMatrix(src.channels, src.mask, dst.channels, dst.mask);
    passthrough_ = isIdentityMatrix(matrix_, src.channels, dst.channels);
    fifo_.configure(dst.channels, src.rate, dst.rate);
    packetSec_ = 0.010;
    lastArrival_ = 0.0;
    lastUpdate_ = 0.0;
    haveInput_ = false;
    priming_ = true;
    fadeLeft_ = 0;
    underruns_ = 0;
    resyncs_ = 0;
    drift_.reset(targetSec());
  }

  // Buffered audio the controller aims for, in seconds: one packet in flight, one
  // output chunk, the interpolator's look-ahead, plus the safety margin.
  double targetSec() const {
    return packetSec_ + chunkSec_ + marginSec_ + fifo_.halfWidth() / src_.rate;
  }

  // Source side: `frames` interleaved source frames (nullptr = silence) that arrived at `now`.
  void push(const float* in, size_t frames, double now) {
    if (frames == 0) return;
    packetSec_ = std::max(packetSec_, frames / src_.rate);
    if (passthrough_ || !in) {
      fifo_.write(in, frames);
    } else {
      mapped_.resize(frames * static_cast<size_t>(dst_.channels));
      applyChannelMatrix(matrix_, src_.channels, dst_.channels, in, mapped_.data(), frames);
      fifo_.write(mapped_.data(), frames);
    }
    lastArrival_ = now;
    haveInput_ = true;
    // The output side stalled (device busy, PC suspended...): drop the backlog
    // instead of letting the delay pile up.
    if (!priming_ && fifo_.fill() > (targetSec() + kMaxExcessSec) * src_.rate) {
      fifo_.seekToFill(targetSec() * src_.rate);
      drift_.resync(targetSec());
      ++resyncs_;
    }
  }

  // Output side: writes `frames` interleaved output frames. Returns false when the
  // output is silence because the pipeline is (re)buffering.
  bool pull(float* out, size_t frames, double now) {
    const size_t samples = frames * static_cast<size_t>(dst_.channels);
    const double target = targetSec();
    if (priming_) {
      if (!haveInput_ || virtualFillSec(now) < target) {
        std::memset(out, 0, samples * sizeof(float));
        return false;
      }
      // Start from exactly the target level.
      fifo_.seekToFill((target - inflightSec(now)) * src_.rate);
      drift_.setTarget(target);
      drift_.resync(target);
      lastUpdate_ = now;
      priming_ = false;
      fadeLeft_ = fadeFrames();
    }

    const double dt = now - lastUpdate_;
    lastUpdate_ = now;
    // After a long stretch without dropouts, give back one step of extra margin; the
    // controller then slews the level down inaudibly.
    calmSec_ += dt;
    if (calmSec_ > kCalmSec && marginSec_ > baseMarginSec_) {
      marginSec_ = std::max(baseMarginSec_, marginSec_ - kMarginGrowSec);
      calmSec_ = 0.0;
    }
    drift_.setTarget(targetSec());
    const double adjust = drift_.update(virtualFillSec(now), dt);
    const size_t got = fifo_.read(out, frames, adjust);
    if (got < frames) {
      // Ran dry: the source stalled or the buffer is too tight for this PC.
      // Grow the margin a little and rebuffer.
      ++underruns_;
      marginSec_ = std::min(marginSec_ + kMarginGrowSec, kMaxMarginSec);
      calmSec_ = 0.0;
      priming_ = true;
      return false;
    }
    if (std::fabs(drift_.smoothedFill() - targetSec()) > kResyncErrorSec) {
      // Far off target (e.g. after a long system stall): jump back rather than
      // slewing for minutes.
      fifo_.seekToFill((targetSec() - inflightSec(now)) * src_.rate);
      drift_.resync(targetSec());
      ++resyncs_;
    }
    applyFadeIn(out, frames);
    return true;
  }

  // Stats.
  bool priming() const { return priming_; }
  double latencySec() const { return drift_.smoothedFill(); }
  double bufferedSec() const { return fifo_.fill() / src_.rate; }  // actually in the FIFO right now
  double driftPpm() const { return drift_.drift() * 1e6; }
  double adjustPpm() const { return drift_.adjust() * 1e6; }
  double marginSec() const { return marginSec_; }
  uint32_t underruns() const { return underruns_; }
  uint32_t resyncs() const { return resyncs_; }
  const DriftController& controller() const { return drift_; }

 private:
  static constexpr double kMaxExcessSec = 0.1;
  static constexpr double kResyncErrorSec = 0.05;
  static constexpr double kMarginGrowSec = 0.005;
  static constexpr double kMaxMarginSec = 0.1;
  static constexpr double kCalmSec = 600.0;  // dropout-free time before the margin shrinks again
  static constexpr double kFadeSec = 0.005;

  double inflightSec(double now) const { return std::clamp(now - lastArrival_, 0.0, 2.0 * packetSec_); }
  double virtualFillSec(double now) const { return fifo_.fill() / src_.rate + inflightSec(now); }
  size_t fadeFrames() const { return static_cast<size_t>(kFadeSec * dst_.rate); }

  // Ramps the first few milliseconds after (re)buffering so playback does not start with a click.
  void applyFadeIn(float* out, size_t frames) {
    const size_t total = fadeFrames();
    const size_t ch = static_cast<size_t>(dst_.channels);
    for (size_t f = 0; f < frames && fadeLeft_ > 0; ++f, --fadeLeft_) {
      const float g = static_cast<float>(total - fadeLeft_) / static_cast<float>(total);
      for (size_t c = 0; c < ch; ++c) out[f * ch + c] *= g;
    }
  }

  Format src_, dst_;
  double chunkSec_ = 0.01;
  double marginSec_ = 0.005;
  double baseMarginSec_ = 0.005;  // as configured
  double calmSec_ = 0.0;          // time since the last dropout
  double packetSec_ = 0.01;  // largest packet seen from the source
  std::vector<float> matrix_;
  bool passthrough_ = true;
  std::vector<float> mapped_;
  ResamplingFifo fifo_;
  DriftController drift_;
  double lastArrival_ = 0.0;
  double lastUpdate_ = 0.0;
  bool haveInput_ = false;
  bool priming_ = true;
  size_t fadeLeft_ = 0;
  uint32_t underruns_ = 0;
  uint32_t resyncs_ = 0;
};

}  // namespace sas
