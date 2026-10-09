// Offline tests for the DSP core: resampler accuracy, drift tracking against a
// simulated pair of device clocks, channel mapping and sample conversion.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "../src/dsp.h"

using namespace sas;

static int g_failures = 0;

static void check(bool ok, const std::string& what) {
  std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

static double snrDb(const std::vector<double>& expected, const std::vector<float>& got) {
  double sig = 0, err = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    sig += expected[i] * expected[i];
    const double e = got[i] - expected[i];
    err += e * e;
  }
  return 10.0 * std::log10(sig / std::max(err, 1e-30));
}

// Reads a sine through the FIFO starting at a fractional input position and
// compares it to the analytic signal.
static double resampleSnr(double srcRate, double dstRate, double freq, double startPos) {
  ResamplingFifo fifo;
  fifo.configure(1, srcRate, dstRate);
  const size_t inFrames = 20000;
  std::vector<float> in(inFrames);
  for (size_t i = 0; i < inFrames; ++i) in[i] = static_cast<float>(0.5 * std::sin(2 * kPi * freq * i / srcRate));
  fifo.write(in.data(), inFrames);
  fifo.seekToFill(inFrames - startPos);

  const double step = srcRate / dstRate;
  const size_t outFrames = static_cast<size_t>((inFrames - startPos - 200) / step);
  std::vector<float> out(outFrames);
  const size_t got = fifo.read(out.data(), outFrames, 0.0);
  if (got != outFrames) return -1;
  std::vector<double> expected(outFrames);
  for (size_t n = 0; n < outFrames; ++n) expected[n] = 0.5 * std::sin(2 * kPi * freq * (startPos + n * step) / srcRate);
  return snrDb(expected, out);
}

static void testResamplerAccuracy() {
  std::printf("Resampler accuracy\n");
  // ~85 dB is the Kaiser window's passband ripple (+-0.0001 dB): far below audibility.
  struct Case { double src, dst, freq, minSnr; };
  const Case cases[] = {
      {48000, 48000, 1000, 85}, {48000, 48000, 10000, 80}, {48000, 48000, 18000, 70},
      {44100, 48000, 1000, 85}, {44100, 48000, 15000, 70},
      {48000, 44100, 1000, 85}, {48000, 44100, 15000, 70},
      {96000, 48000, 1000, 85},
  };
  for (const Case& c : cases) {
    const double snr = resampleSnr(c.src, c.dst, c.freq, 1000.37);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%5.0f -> %5.0f Hz, %5.0f Hz tone: SNR %.1f dB (need > %.0f)", c.src, c.dst, c.freq,
                  snr, c.minSnr);
    check(snr > c.minSnr, buf);
  }
}

// Two simulated device clocks drive a MirrorPipeline exactly the way the engine
// does: the source delivers packets at its own (slightly off) rate, the output pulls
// one period at a time at its rate, and both wake up with scheduling jitter.
struct DriftResult {
  double maxErrMs = 0;      // largest smoothed level error after settling
  double learnedPpm = 0;    // drift estimate at the end
  double latencyMs = 0;     // smoothed buffered audio at the end
  uint32_t underruns = 0;
  uint32_t resyncs = 0;
  double maxCurvature = 0;  // largest second difference of the output sine after start-up
};

static DriftResult simulateDrift(double srcRate, double dstRate, double ppm, double jitterMs, double seconds) {
  MirrorPipeline pipe;
  pipe.configure({1, 0, srcRate}, {1, 0, dstRate}, 0.010, 0.005);

  const double actualSrcRate = srcRate * (1.0 + ppm * 1e-6);
  const size_t srcPacket = static_cast<size_t>(srcRate / 100);
  const size_t dstChunk = static_cast<size_t>(dstRate / 100);
  const double srcPeriod = srcPacket / actualSrcRate;
  const double dstPeriod = dstChunk / dstRate;
  const double tone = 440.0;

  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> jitter(0.0, jitterMs / 1000.0);

  DriftResult r;
  std::vector<float> packet(srcPacket), out(dstChunk);
  int64_t srcIndex = 0, srcCount = 1, dstCount = 1;
  double prev1 = 0, prev2 = 0;
  int64_t outSamples = 0;
  double nextSrc = srcPeriod + jitter(rng), nextDst = dstPeriod + jitter(rng);
  for (;;) {
    const bool srcFirst = nextSrc <= nextDst;
    const double t = srcFirst ? nextSrc : nextDst;
    if (t > seconds) break;
    if (srcFirst) {
      for (size_t i = 0; i < srcPacket; ++i, ++srcIndex)
        packet[i] = static_cast<float>(0.5 * std::sin(2 * kPi * tone * srcIndex / srcRate));
      pipe.push(packet.data(), srcPacket, t);
      nextSrc = ++srcCount * srcPeriod + jitter(rng);
    } else {
      nextDst = ++dstCount * dstPeriod + jitter(rng);
      pipe.pull(out.data(), dstChunk, t);
      if (t > 120.0) r.maxErrMs = std::max(r.maxErrMs, std::fabs(pipe.latencySec() - pipe.targetSec()) * 1000.0);
      for (size_t i = 0; i < dstChunk; ++i, ++outSamples) {
        if (t > 5.0) r.maxCurvature = std::max(r.maxCurvature, static_cast<double>(std::fabs(out[i] - 2 * prev1 + prev2)));
        prev2 = prev1;
        prev1 = out[i];
      }
    }
  }
  r.learnedPpm = pipe.driftPpm();
  r.latencyMs = pipe.latencySec() * 1000.0;
  r.underruns = pipe.underruns();
  r.resyncs = pipe.resyncs();
  return r;
}

static void testDrift() {
  std::printf("Drift compensation (simulated device clocks)\n");
  struct Case { double src, dst, ppm, jitterMs, seconds; };
  const Case cases[] = {
      {48000, 48000, 100, 2, 600},
      {48000, 48000, -300, 2, 600},
      {44100, 48000, 50, 2, 300},
      {48000, 44100, -80, 2, 300},
      {48000, 48000, 0, 4, 300},
      {48000, 48000, 1000, 2, 300},
  };
  for (const Case& c : cases) {
    const DriftResult r = simulateDrift(c.src, c.dst, c.ppm, c.jitterMs, c.seconds);
    char buf[260];
    std::snprintf(buf, sizeof buf,
                  "%5.0f -> %5.0f Hz, %+5.0f ppm, jitter %.0f ms: learned %+.1f ppm, level error %.2f ms, "
                  "latency %.1f ms, underruns %u, resyncs %u, curvature %.4f",
                  c.src, c.dst, c.ppm, c.jitterMs, r.learnedPpm, r.maxErrMs, r.latencyMs, r.underruns, r.resyncs,
                  r.maxCurvature);
    const double clean = 0.5 * std::pow(2 * kPi * 440.0 / c.dst, 2) * 1.5;  // a sine's own curvature + margin
    check(r.underruns == 0 && r.resyncs == 0 && r.maxErrMs < 1.5 && std::fabs(r.learnedPpm - c.ppm) < 10.0 &&
              r.maxCurvature < clean,
          buf);
  }
}

static void testChannelMatrix() {
  std::printf("Channel mapping\n");
  const uint32_t stereo = kFL | kFR, mono = kFC;
  const uint32_t s71 = kFL | kFR | kFC | kLFE | kBL | kBR | kSL | kSR;
  const uint32_t s51 = kFL | kFR | kFC | kLFE | kBL | kBR;
  const float h = 0.70710678f;
  auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };

  auto m = buildChannelMatrix(2, stereo, 2, stereo);
  check(isIdentityMatrix(m, 2, 2), "stereo -> stereo is a passthrough");

  m = buildChannelMatrix(8, s71, 2, stereo);  // rows: L, R; columns: FL FR FC LFE BL BR SL SR
  check(near(m[0], 1) && near(m[1], 0) && near(m[2], h) && near(m[3], 0) && near(m[4], h) && near(m[5], 0) &&
            near(m[6], h) && near(m[7], 0),
        "7.1 -> stereo left = FL + .707 (FC + BL + SL), LFE dropped");
  check(near(m[8 + 1], 1) && near(m[8 + 2], h) && near(m[8 + 5], h) && near(m[8 + 7], h),
        "7.1 -> stereo right mirrors left");

  m = buildChannelMatrix(1, mono, 2, stereo);
  check(near(m[0], 1) && near(m[1], 1), "mono -> stereo plays at full level on both sides");

  m = buildChannelMatrix(2, stereo, 1, mono);
  check(near(m[0], 0.5f) && near(m[1], 0.5f), "stereo -> mono averages");

  m = buildChannelMatrix(2, stereo, 8, s71);
  bool ok = near(m[0 * 2 + 0], 1) && near(m[1 * 2 + 1], 1);
  for (int d = 2; d < 8; ++d) ok = ok && near(m[d * 2 + 0], 0) && near(m[d * 2 + 1], 0);
  check(ok, "stereo -> 7.1 fills only the front pair");

  m = buildChannelMatrix(6, s51, 8, s71);  // 5.1 back pair stays on the back pair
  check(near(m[4 * 6 + 4], 1) && near(m[5 * 6 + 5], 1) && near(m[6 * 6 + 4], 0), "5.1 -> 7.1 keeps BL/BR in place");

  m = buildChannelMatrix(8, s71, 6, s51);  // 7.1 side pair folds into the back pair
  check(near(m[4 * 8 + 4], 1) && near(m[4 * 8 + 6], h), "7.1 -> 5.1 folds SL into BL at -3 dB");

  m = buildChannelMatrix(2, 0, 2, 0);
  check(isIdentityMatrix(m, 2, 2), "missing channel masks fall back to the default layout");
}

static void testConversion() {
  std::printf("Sample conversion\n");
  const float in[] = {0.0f, 0.5f, -0.5f, 0.999f, -1.0f, 0.123456f};
  const size_t n = sizeof in / sizeof in[0];
  struct Case { SampleType type; const char* name; float tol; };
  const Case cases[] = {{SampleType::Float32, "float32", 0}, {SampleType::Int16, "int16", 1.0f / 65536},
                        {SampleType::Int24, "int24", 1.0f / 16777216}, {SampleType::Int32, "int32", 1e-7f}};
  for (const Case& c : cases) {
    std::vector<uint8_t> bytes(n * bytesPerSample(c.type));
    std::vector<float> back(n);
    fromFloat(c.type, in, bytes.data(), n);
    toFloat(c.type, bytes.data(), back.data(), n);
    float worst = 0;
    for (size_t i = 0; i < n; ++i) worst = std::max(worst, std::fabs(back[i] - in[i]));
    check(worst <= c.tol, std::string(c.name) + " round trip");
  }
  const float loud[] = {1.5f, -1.5f};
  int16_t s16[2];
  fromFloat(SampleType::Int16, loud, reinterpret_cast<uint8_t*>(s16), 2);
  check(s16[0] == 32767 && s16[1] == -32768, "int16 output clips instead of wrapping");
}

// CPU cost of one mirrored stream, in milliseconds per second of audio.
static void benchmark() {
  std::printf("Speed (CPU time per second of audio, one stream)\n");
  struct Case { int ch; double src, dst; };
  const Case cases[] = {{2, 48000, 48000}, {2, 44100, 48000}, {8, 44100, 44100}, {8, 48000, 48000}};
  for (const Case& c : cases) {
    ResamplingFifo fifo;
    fifo.configure(c.ch, c.src, c.dst);
    const size_t packet = static_cast<size_t>(c.src / 100), chunk = static_cast<size_t>(c.dst / 100);
    std::vector<float> in(packet * c.ch, 0.25f), out(chunk * c.ch);
    const int seconds = 20;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < seconds * 100; ++i) {
      fifo.write(in.data(), packet);
      if (fifo.fill() > 2.0 * packet) fifo.read(out.data(), chunk, 1e-4);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / seconds;
    std::printf("  %d ch, %5.0f -> %5.0f Hz: %.2f ms per second (%.2f%% of one core)\n", c.ch, c.src, c.dst, ms, ms / 10);
  }
}

int main() {
  benchmark();
  testResamplerAccuracy();
  testChannelMatrix();
  testConversion();
  testDrift();
  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
