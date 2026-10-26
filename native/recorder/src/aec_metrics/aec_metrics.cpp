// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

// Numerics follow the reference implementation operation by operation: the
// same accumulation order, double precision everywhere, fused multiply-add
// exactly where the reference fuses (spelled out with std::fma) and nowhere
// else, and AArch64 conversion semantics (round-half-away for sizes,
// saturating float->integer casts). Automatic FP contraction must therefore
// stay off in this translation unit, whatever the build flags say.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#include "aec_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace plaud_rec {
namespace aec_metrics {

const IntField kIntFields[9] = {
    {"sr", &Result::sr},
    {"delayMicSamples", &Result::delayMicSamples},
    {"delayEnhSamples", &Result::delayEnhSamples},
    {"totalFrames", &Result::totalFrames},
    {"farendActiveFrames", &Result::farendActiveFrames},
    {"echoDominantFrames", &Result::echoDominantFrames},
    {"nearendOnlyFrames", &Result::nearendOnlyFrames},
    {"ansNoiseFrames", &Result::ansNoiseFrames},
    {"agcSpeechFrames", &Result::agcSpeechFrames},
};

const OptionalField kOptionalFields[40] = {
    {"farendActiveRatio", &Result::farendActiveRatio},
    {"echoDominantRatio", &Result::echoDominantRatio},
    {"nearendOnlyRatio", &Result::nearendOnlyRatio},
    {"coherentEchoSuppressionDbFarendActive", &Result::coherentEchoSuppressionDbFarendActive},
    {"coherentEchoSuppressionDbFarendActiveGainNormalized",
     &Result::coherentEchoSuppressionDbFarendActiveGainNormalized},
    {"mixtureEnergyReductionDbFarendActive", &Result::mixtureEnergyReductionDbFarendActive},
    {"meanAbsCorrMicFarendActive", &Result::meanAbsCorrMicFarendActive},
    {"meanAbsCorrEnhFarendActive", &Result::meanAbsCorrEnhFarendActive},
    {"corrSuppressionDbFarendActive", &Result::corrSuppressionDbFarendActive},
    {"residualEchoRatioFarendActive", &Result::residualEchoRatioFarendActive},
    {"erleDbEchoDominant", &Result::erleDbEchoDominant},
    {"coherentEchoSuppressionDbEchoDominant", &Result::coherentEchoSuppressionDbEchoDominant},
    {"coherentEchoSuppressionDbEchoDominantGainNormalized",
     &Result::coherentEchoSuppressionDbEchoDominantGainNormalized},
    {"meanAbsCorrMicEchoDominant", &Result::meanAbsCorrMicEchoDominant},
    {"meanAbsCorrEnhEchoDominant", &Result::meanAbsCorrEnhEchoDominant},
    {"corrSuppressionDbEchoDominant", &Result::corrSuppressionDbEchoDominant},
    {"residualEchoRatioEchoDominant", &Result::residualEchoRatioEchoDominant},
    {"ansNoiseRatio", &Result::ansNoiseRatio},
    {"agcSpeechRatio", &Result::agcSpeechRatio},
    {"ansSuppressedNoiseRatio3db", &Result::ansSuppressedNoiseRatio3db},
    {"ansSuppressedNoiseRatio6db", &Result::ansSuppressedNoiseRatio6db},
    {"ansSuppressedTotalRatio3db", &Result::ansSuppressedTotalRatio3db},
    {"ansSuppressedTotalRatio6db", &Result::ansSuppressedTotalRatio6db},
    {"agcSpeechLevelInRangeRatio", &Result::agcSpeechLevelInRangeRatio},
    {"agcSpeechLevelInRangeTotalRatio", &Result::agcSpeechLevelInRangeTotalRatio},
    {"noiseFloorDbfsMic", &Result::noiseFloorDbfsMic},
    {"noiseFloorDbfsEnh", &Result::noiseFloorDbfsEnh},
    {"noiseReductionDb", &Result::noiseReductionDb},
    {"noiseEnergyReductionDb", &Result::noiseEnergyReductionDb},
    {"speechLevelDbfsMicMean", &Result::speechLevelDbfsMicMean},
    {"speechLevelDbfsEnhMean", &Result::speechLevelDbfsEnhMean},
    {"speechLevelDbfsMicStd", &Result::speechLevelDbfsMicStd},
    {"speechLevelDbfsEnhStd", &Result::speechLevelDbfsEnhStd},
    {"speechLevelStdReductionDb", &Result::speechLevelStdReductionDb},
    {"agcGainDbMean", &Result::agcGainDbMean},
    {"agcGainDbStd", &Result::agcGainDbStd},
    {"agcGainChangeDbPerSec", &Result::agcGainChangeDbPerSec},
    {"snrImprovementDb", &Result::snrImprovementDb},
    {"enhClipFraction", &Result::enhClipFraction},
    {"enhPeakDbfs", &Result::enhPeakDbfs},
};

namespace {

constexpr double kEps = 1e-12;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kClipLevel = 0.999;
constexpr double kAgcSmoothingSec = 0.2;

// AArch64 FCVTAS (double -> int64, round half away from zero, saturating, NaN -> 0).
int64_t RoundToInt64(double x) {
  if (std::isnan(x)) return 0;
  const double r = std::round(x);
  if (r >= 9223372036854775808.0) return std::numeric_limits<int64_t>::max();
  if (r < -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
  return static_cast<int64_t>(r);
}

// AArch64 FCVTMU (double -> uint64, round toward -inf, saturating, NaN -> 0).
uint64_t FloorToUint64(double x) {
  if (!(x > 0.0)) return 0;
  const double f = std::floor(x);
  if (f >= 18446744073709551616.0) return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(f);
}

// AArch64 FCVTZU (double -> uint64, round toward zero, saturating, NaN -> 0).
uint64_t TruncToUint64(double x) {
  if (!(x > 0.0)) return 0;
  if (x >= 18446744073709551616.0) return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(x);
}

double Exp10(double x) {
#if defined(__GLIBC__) || defined(__APPLE__)
  return ::exp10(x);
#else
  return std::pow(10.0, x);
#endif
}

// Linear-interpolated quantile of an ascending vector (q clamped to [0, 1]).
double Quantile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0.0;
  double c = q > 1.0 ? 1.0 : q;
  c = q < 0.0 ? 0.0 : c;
  const uint64_t last = sorted.size() - 1;
  const double pos = c * static_cast<double>(last);
  const uint64_t lo = FloorToUint64(pos);
  const uint64_t hi = last < lo + 1 ? last : lo + 1;
  const double frac = pos - static_cast<double>(TruncToUint64(std::floor(pos)));
  return std::fma(sorted[lo], 1.0 - frac, frac * sorted[hi]);
}

struct MeanStd {
  std::optional<double> mean;
  std::optional<double> stddev;
};

// Welford mean and sample standard deviation; needs at least two values.
MeanStd MeanAndStd(const std::vector<double>& v) {
  MeanStd out;
  if (v.size() <= 1) return out;
  double mean = 0.0;
  double m2 = 0.0;
  uint32_t k = 1;
  for (const double x : v) {
    const double delta = x - mean;
    mean = mean + delta / static_cast<double>(k);
    m2 = std::fma(delta, x - mean, m2);
    ++k;
  }
  double var = m2 / static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(v.size() - 1)));
  var = std::fmax(var, 0.0);
  out.mean = mean;
  out.stddev = std::sqrt(var);
  return out;
}

// Copy of `lpb` shifted by `delay` samples (zero filled), so that a positive
// delay lines the loopback up with a later-arriving echo.
void AlignByDelay(const std::vector<float>& lpb, int delay, std::vector<float>& out) {
  out.assign(lpb.size(), 0.0f);
  if (delay == 0 || lpb.empty()) {
    if (delay == 0) out.assign(lpb.begin(), lpb.end());
    return;
  }
  const size_t size = lpb.size();
  if (delay < 1) {
    const uint64_t off = static_cast<uint32_t>(-static_cast<uint32_t>(delay));
    if (size > off) std::memmove(out.data(), lpb.data() + off, (size - off) * sizeof(float));
  } else {
    const uint64_t off = static_cast<uint32_t>(delay);
    if (size > off) std::memmove(out.data() + off, lpb.data(), (size - off) * sizeof(float));
  }
}

// Mean of each `factor`-sample block of src[0, n); the last block may be short.
void BlockAverage(const float* src, uint64_t n, uint32_t factor, uint64_t blocks, std::vector<float>& out) {
  out.assign(blocks, 0.0f);
  uint64_t base = 0;
  for (uint64_t i = 0; i < blocks; ++i, base += factor) {
    double sum = 0.0;
    uint64_t k = 0;
    for (; k < factor; ++k) {
      if (base + k >= n) break;
      sum += static_cast<double>(src[base + k]);
    }
    const uint32_t count = static_cast<uint32_t>(k);
    out[i] = count == 0 ? 0.0f : static_cast<float>(sum / static_cast<double>(count));
  }
}

// |normalized cross-correlation| of a[0, count) and b[0, count).
double AbsNormalizedCorrelation(const float* a, const float* b, uint64_t count) {
  double dot = 0.0;
  double ea = 0.0;
  double eb = 0.0;
  for (uint64_t j = 0; j < count; ++j) {
    const double x = a[j];
    const double y = b[j];
    dot = std::fma(x, y, dot);
    ea = std::fma(x, x, ea);
    eb = std::fma(y, y, eb);
  }
  return std::fabs(dot / (std::sqrt(ea * eb) + kEps));
}

}  // namespace

void ResampleLinear(const float* in, std::size_t len, int srcRate, int dstRate, std::vector<float>& out) {
  out.clear();
  if (in == nullptr || len == 0) return;
  if (srcRate < 1 || dstRate <= 0) throw std::runtime_error("ResampleLinear: invalid sample rates");
  if (srcRate == dstRate) {
    out.assign(in, in + len);
    return;
  }
  const double dst = static_cast<double>(static_cast<uint32_t>(dstRate));
  const double src = static_cast<double>(static_cast<uint32_t>(srcRate));
  const auto count = static_cast<uint64_t>(RoundToInt64((dst / src) * static_cast<double>(static_cast<uint64_t>(len))));
  if (count == 0) return;
  out.resize(count);
  if (count == 1) {
    out[0] = 0.0f;
    return;
  }
  const double step = src / dst;
  const uint64_t lastIndex = len - 1;
  for (uint64_t i = 0; i < count; ++i) {
    const double pos = step * static_cast<double>(i);
    const uint64_t idx = FloorToUint64(pos);
    if (idx < lastIndex) {
      const double frac = pos - static_cast<double>(idx);
      const double a = in[idx];
      const double b = in[idx + 1];
      out[i] = static_cast<float>(std::fma(b - a, frac, a));
    } else {
      out[i] = in[lastIndex];
    }
  }
}

void FftInPlace(std::vector<double>& re, std::vector<double>& im, bool inverse) {
  const auto n = static_cast<uint32_t>(re.size());
  if (n == 0) return;
  if (static_cast<uint32_t>(im.size()) != n) throw std::runtime_error("FFT: real/imag length mismatch");
  if ((n & (n - 1)) != 0) throw std::runtime_error("FFT: length must be a power of two");
  const uint64_t n31 = re.size() & 0x7fffffffu;
  if (static_cast<int32_t>(n) > 1) {
    uint32_t j = 0;
    for (uint64_t i = 1; i != n31; ++i) {
      uint32_t bit = n;
      bool carry;
      do {
        bit >>= 1;
        carry = (j & bit) != 0;
        j ^= bit;
      } while (carry);
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int32_t len = 2; len <= static_cast<int32_t>(n); len <<= 1) {
      const double step = kTwoPi / static_cast<double>(len);
      const double angle = inverse ? step : -step;
      const double s = std::sin(angle);
      const double c = std::cos(angle);
      const int32_t half = len >> 1;
      if (half < 1) continue;
      uint64_t base = 0;
      do {
        double wi = 0.0;
        double wr = 1.0;
        for (uint32_t k = 0; k < static_cast<uint32_t>(half); ++k) {
          const uint64_t a = base + k;
          const uint64_t b = a + static_cast<uint32_t>(half);
          const double ur = re[a];
          const double ui = im[a];
          const double xr = re[b];
          const double xi = im[b];
          const double vr = std::fma(xr, wr, -(xi * wi));
          re[a] = ur + vr;
          const double vi = std::fma(xr, wi, wr * xi);
          im[a] = ui + vi;
          re[b] = ur - vr;
          im[b] = ui - vi;
          const double nextWiTerm = c * wi;
          const double nextWrTerm = -(wi * s);
          wi = std::fma(wr, s, nextWiTerm);
          wr = std::fma(wr, c, nextWrTerm);
        }
        base += static_cast<uint32_t>(len);
      } while (base < n31);
    }
  }
  if (static_cast<int32_t>(n) >= 1 && inverse) {
    const double scale = static_cast<double>(n);
    for (uint64_t i = 0; i < n31; ++i) {
      re[i] = re[i] / scale;
      im[i] = im[i] / scale;
    }
  }
}

int EstimateDelaySamplesGccPhat(const float* ref, std::size_t refLen, const float* sig, std::size_t sigLen,
                                int sampleRate, double maxDelayMs, int downsampleFactor, double maxSeconds) {
  if (sampleRate < 1 || sigLen == 0 || refLen == 0 || ref == nullptr || sig == nullptr) return 0;
  const auto rate = static_cast<uint32_t>(sampleRate);
  uint64_t limit = static_cast<uint64_t>(RoundToInt64(static_cast<double>(rate) * maxSeconds));
  if (!(maxSeconds > 0.0)) limit = std::numeric_limits<uint64_t>::max();
  const uint64_t shorter = sigLen < refLen ? sigLen : refLen;
  const uint64_t n = limit < shorter ? limit : shorter;
  if (n < 2) return 0;

  uint32_t factor = downsampleFactor > 1 ? static_cast<uint32_t>(downsampleFactor) : 1u;
  if (rate / factor < 1001) factor = 1;

  std::vector<float> a;
  std::vector<float> b;
  if (factor <= 1) {
    a.assign(ref, ref + n);
    b.assign(sig, sig + n);
  } else if (n - 1 <= std::numeric_limits<uint64_t>::max() - factor) {
    const uint64_t blocks = (n - 1 + factor) / factor;
    BlockAverage(ref, n, factor, blocks, a);
    BlockAverage(sig, n, factor, blocks, b);
  }

  const uint32_t decimatedRate = rate / factor;
  int32_t coarse = 0;
  if (static_cast<int32_t>(decimatedRate) >= 1 && !b.empty() && !a.empty()) {
    const uint64_t len = b.size() < a.size() ? b.size() : a.size();
    if (len >= 2) {
      const auto doubled = static_cast<int32_t>(static_cast<uint32_t>(len) << 1);
      uint32_t fftLen = doubled > 1 ? static_cast<uint32_t>(doubled) : 1u;
      if ((fftLen & (fftLen - 1)) != 0) {
        uint32_t v = fftLen - 1;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        fftLen = v + 1;
      }
      std::vector<double> xr(fftLen, 0.0);
      std::vector<double> xi(fftLen, 0.0);
      std::vector<double> yr(fftLen, 0.0);
      std::vector<double> yi(fftLen, 0.0);
      for (uint64_t i = 0; i < len; ++i) {
        xr[i] = a[i];
        yr[i] = b[i];
      }
      FftInPlace(xr, xi, false);
      FftInPlace(yr, yi, false);
      // PHAT-weighted cross spectrum X * conj(Y).
      for (uint32_t k = 0; k < fftLen; ++k) {
        const double re = std::fma(xr[k], yr[k], xi[k] * yi[k]);
        const double imv = std::fma(xi[k], yr[k], -(xr[k] * yi[k]));
        const double mag = std::fmax(std::hypot(re, imv), kEps);
        xr[k] = re / mag;
        xi[k] = imv / mag;
      }
      FftInPlace(xr, xi, true);
      const auto maxLag = static_cast<uint32_t>(RoundToInt64(maxDelayMs / 1000.0 * static_cast<double>(decimatedRate)));
      const uint32_t halfLen = fftLen >> 1;
      uint32_t lim = halfLen < maxLag ? halfLen : maxLag;
      if (static_cast<int32_t>(maxLag) < 0) lim = 0;
      double best = -std::numeric_limits<double>::infinity();
      int32_t lag = -static_cast<int32_t>(lim);
      for (uint32_t remaining = (lim << 1) | 1u; remaining != 0; --remaining, ++lag) {
        const double v = std::fabs(xr[(fftLen + static_cast<uint32_t>(lag)) & (fftLen - 1)]);
        if (v > best) {
          best = v;
          coarse = lag;
        }
      }
    }
  }

  // Refine around the coarse peak at the full rate.
  const int32_t center = static_cast<int32_t>(static_cast<uint32_t>(coarse) * factor);
  uint32_t window = factor << 1;
  if (window <= 2) window = 2;
  const int32_t first = static_cast<int32_t>(static_cast<uint32_t>(center) - window);
  const int32_t last = static_cast<int32_t>(static_cast<uint32_t>(center) + window);
  int32_t bestLag = center;
  if (first <= last) {
    double best = -1.0;
    for (int32_t lag = first;; ++lag) {
      double v = 0.0;
      if (lag >= 0) {
        const uint64_t off = static_cast<uint32_t>(lag);
        if (n > off) v = AbsNormalizedCorrelation(ref + off, sig, n - off);
      } else {
        const uint64_t off = static_cast<uint32_t>(-static_cast<uint32_t>(lag));
        if (n > off) v = AbsNormalizedCorrelation(ref, sig + off, n - off);
      }
      if (v > best) {
        best = v;
        bestLag = lag;
      }
      if (lag == last) break;
    }
  }
  return bestLag;
}

Result ComputeAecLightMetrics(const float* lpb, std::size_t lpbLen, int lpbSr, const float* mic, std::size_t micLen,
                              int micSr, const float* enh, std::size_t enhLen, int enhSr, const Options& opt) {
  Result r;
  if (lpb == nullptr || mic == nullptr || enh == nullptr || lpbLen == 0 || micLen == 0 || enhLen == 0) return r;
  const int sr = opt.targetSampleRate > 0 ? opt.targetSampleRate : lpbSr;
  if (sr < 1) return r;
  r.sr = sr;

  std::vector<float> lp;
  std::vector<float> mc;
  std::vector<float> en;
  ResampleLinear(lpb, lpbLen, lpbSr, sr, lp);
  ResampleLinear(mic, micLen, micSr, sr, mc);
  ResampleLinear(enh, enhLen, enhSr, sr, en);
  const size_t n = std::min(en.size(), std::min(mc.size(), lp.size()));
  lp.resize(n);
  mc.resize(n);
  en.resize(n);
  if (n == 0) return r;

  const int delayMic = EstimateDelaySamplesGccPhat(mc.data(), n, lp.data(), n, sr, opt.maxDelayMs,
                                                   opt.downsampleFactorForDelay, opt.delayEstimationMaxSec);
  const int delayEnh = EstimateDelaySamplesGccPhat(en.data(), n, lp.data(), n, sr, opt.maxDelayMs,
                                                   opt.downsampleFactorForDelay, opt.delayEstimationMaxSec);
  r.delayMicSamples = delayMic;
  r.delayEnhSamples = delayEnh;
  std::vector<float> lpForMic;
  std::vector<float> lpForEnh;
  AlignByDelay(lp, delayMic, lpForMic);
  AlignByDelay(lp, delayEnh, lpForEnh);

  // Framing.
  const double rate = static_cast<double>(static_cast<uint32_t>(sr));
  const int64_t frameRaw = RoundToInt64(opt.frameMs / 1000.0 * rate);
  if (static_cast<int32_t>(frameRaw) < 1) return r;
  const int64_t hopRaw = RoundToInt64(opt.hopMs / 1000.0 * rate);
  if (static_cast<int32_t>(hopRaw) < 1) return r;
  const uint64_t frameLen = static_cast<uint64_t>(frameRaw) & 0x7fffffffu;
  if (n < frameLen) return r;
  const uint64_t hop = static_cast<uint64_t>(hopRaw) & 0x7fffffffu;
  const uint64_t lastFrameWide = (n - frameLen) / hop;
  const auto lastFrame32 = static_cast<uint32_t>(lastFrameWide);
  const bool noFrames = lastFrame32 == 0xffffffffu;
  const int32_t total = static_cast<int32_t>(lastFrame32 + 1u);
  r.totalFrames = total;
  if (!noFrames && static_cast<int32_t>(lastFrame32) <= -2) throw std::length_error("vector");
  const size_t frameCount = noFrames ? 0 : static_cast<size_t>(static_cast<int64_t>(total));
  const uint64_t lastFrame = lastFrameWide & 0x7fffffffu;
  // The reference reports the ratios below only when the frame loop ran.
  const bool framesRan = static_cast<int32_t>(lastFrame32) >= 0;

  std::vector<double> lpbPow(frameCount, 0.0);
  std::vector<double> micPow(frameCount, 0.0);
  std::vector<double> enhPow(frameCount, 0.0);
  std::vector<double> corrMic(frameCount, 0.0);
  std::vector<double> corrEnh(frameCount, 0.0);
  std::vector<double> echoMic(frameCount, 0.0);
  std::vector<double> echoEnh(frameCount, 0.0);
  double maxLpbPow = 0.0;
  if (!noFrames) {
    const double frameD = static_cast<double>(static_cast<uint32_t>(frameRaw));
    for (uint64_t f = 0; f <= lastFrame; ++f) {
      const uint64_t start = f * hop;
      const float* pm = lpForMic.data() + start;
      const float* pe = lpForEnh.data() + start;
      const float* m = mc.data() + start;
      const float* e = en.data() + start;
      double eLpbM = 0.0;
      double eLpbE = 0.0;
      double eMic = 0.0;
      double eEnh = 0.0;
      double xMic = 0.0;
      double xEnh = 0.0;
      for (uint64_t k = 0; k < frameLen; ++k) {
        const double a = pm[k];
        const double b = pe[k];
        const double c = m[k];
        const double d = e[k];
        eLpbM = std::fma(a, a, eLpbM);
        eLpbE = std::fma(b, b, eLpbE);
        eMic = std::fma(c, c, eMic);
        eEnh = std::fma(d, d, eEnh);
        xMic = std::fma(c, a, xMic);
        xEnh = std::fma(d, b, xEnh);
      }
      const double lpbP = eLpbM / frameD;
      lpbPow[f] = lpbP;
      micPow[f] = eMic / frameD;
      enhPow[f] = eEnh / frameD;
      if (lpbP > maxLpbPow) maxLpbPow = lpbP;
      corrMic[f] = xMic / (std::sqrt(eMic * eLpbM) + kEps);
      corrEnh[f] = xEnh / (std::sqrt(eEnh * eLpbE) + kEps);
      // Power of the loopback projected onto mic / enh (coherent echo).
      const double gMic = xMic / (eLpbM + kEps);
      const double gEnh = xEnh / (eLpbE + kEps);
      echoMic[f] = eLpbM * (gMic * gMic) / frameD;
      echoEnh[f] = eLpbE * (gEnh * gEnh) / frameD;
    }
  }

  double farThreshold = maxLpbPow * Exp10(opt.lpbRelThreshDb / 10.0);
  farThreshold = farThreshold < kEps ? kEps : farThreshold;
  double residualThreshold = opt.residualEchoCorrThresh > 1.0 ? 1.0 : opt.residualEchoCorrThresh;
  residualThreshold = opt.residualEchoCorrThresh < 0.0 ? 0.0 : residualThreshold;
  const double corrThreshold = opt.corrThresh;

  std::vector<uint8_t> farActive(frameCount, 0);
  int32_t far = 0;
  int32_t dominant = 0;
  int32_t residualFar = 0;
  int32_t residualDominant = 0;
  double sumEchoMic = 0.0, sumEchoEnh = 0.0, sumEchoEnhNorm = 0.0, sumMicPow = 0.0, sumEnhPow = 0.0;
  double sumAbsCorrMic = 0.0, sumAbsCorrEnh = 0.0;
  double domMicPow = 0.0, domEnhPow = 0.0, domEchoMic = 0.0, domEchoEnh = 0.0, domEchoEnhNorm = 0.0;
  double domAbsCorrMic = 0.0, domAbsCorrEnh = 0.0;
  if (!noFrames) {
    for (uint64_t f = 0; f <= lastFrame; ++f) {
      const bool active = lpbPow[f] > farThreshold;
      farActive[f] = active ? 1 : 0;
      if (!active) continue;
      ++far;
      const double em = echoMic[f];
      const double ee = echoEnh[f];
      sumEchoMic += em;
      sumEchoEnh += ee;
      const double mp = micPow[f];
      const double ep = enhPow[f];
      // Enh echo power rescaled by the frame's overall mic/enh level ratio.
      const double gainNorm = (mp + kEps) / (ep + kEps);
      sumEchoEnhNorm = std::fma(ee, gainNorm, sumEchoEnhNorm);
      sumMicPow += mp;
      sumEnhPow += ep;
      const double aMic = std::fabs(corrMic[f]);
      sumAbsCorrMic += aMic;
      const double aEnh = std::fabs(corrEnh[f]);
      sumAbsCorrEnh += aEnh;
      if (aEnh >= residualThreshold) ++residualFar;
      if (!(aMic >= corrThreshold)) continue;
      ++dominant;
      domMicPow += mp;
      domEnhPow += ep;
      domEchoMic += em;
      domEchoEnh += ee;
      domEchoEnhNorm = std::fma(ee, gainNorm, domEchoEnhNorm);
      domAbsCorrMic += aMic;
      domAbsCorrEnh += aEnh;
      if (aEnh >= residualThreshold) ++residualDominant;
    }
  }
  sumEchoMic += kEps;
  sumEchoEnh += kEps;
  sumEchoEnhNorm += kEps;
  sumMicPow += kEps;
  sumEnhPow += kEps;
  domMicPow += kEps;
  domEnhPow += kEps;
  domEchoMic += kEps;
  domEchoEnh += kEps;
  domEchoEnhNorm += kEps;

  r.farendActiveFrames = far;
  r.echoDominantFrames = dominant;
  int32_t nearOnly = static_cast<int32_t>(static_cast<uint32_t>(total) - static_cast<uint32_t>(far));
  if (nearOnly < 0) nearOnly = 0;
  r.nearendOnlyFrames = nearOnly;
  const double farD = static_cast<double>(far);
  const double totalD = static_cast<double>(total);
  const double dominantD = static_cast<double>(dominant);
  if (framesRan) {
    r.farendActiveRatio = farD / totalD;
    r.nearendOnlyRatio = static_cast<double>(static_cast<uint32_t>(nearOnly)) / totalD;
  }
  if (far > 0) {
    r.echoDominantRatio = dominantD / farD;
    r.residualEchoRatioFarendActive = static_cast<double>(residualFar) / farD;
  }
  if (dominant > 0) r.residualEchoRatioEchoDominant = static_cast<double>(residualDominant) / dominantD;
  if (far >= 1) {
    r.coherentEchoSuppressionDbFarendActive = std::log10(sumEchoMic / sumEchoEnh) * 10.0;
    r.coherentEchoSuppressionDbFarendActiveGainNormalized = std::log10(sumEchoMic / sumEchoEnhNorm) * 10.0;
    r.mixtureEnergyReductionDbFarendActive = std::log10(sumMicPow / sumEnhPow) * 10.0;
    const double meanMic = sumAbsCorrMic / farD;
    const double meanEnh = sumAbsCorrEnh / farD;
    r.meanAbsCorrMicFarendActive = meanMic;
    r.meanAbsCorrEnhFarendActive = meanEnh;
    r.corrSuppressionDbFarendActive = std::log10((meanMic + kEps) / (meanEnh + kEps)) * 20.0;
  }
  if (dominant >= 1) {
    r.erleDbEchoDominant = std::log10(domMicPow / domEnhPow) * 10.0;
    r.coherentEchoSuppressionDbEchoDominant = std::log10(domEchoMic / domEchoEnh) * 10.0;
    r.coherentEchoSuppressionDbEchoDominantGainNormalized = std::log10(domEchoMic / domEchoEnhNorm) * 10.0;
    const double meanMic = domAbsCorrMic / dominantD;
    const double meanEnh = domAbsCorrEnh / dominantD;
    r.meanAbsCorrMicEchoDominant = meanMic;
    r.meanAbsCorrEnhEchoDominant = meanEnh;
    r.corrSuppressionDbEchoDominant = std::log10((meanMic + kEps) / (meanEnh + kEps)) * 20.0;
  }

  // Per-frame levels in dBFS (power).
  std::vector<double> micDb(frameCount, 0.0);
  std::vector<double> enhDb(frameCount, 0.0);
  if (!noFrames) {
    for (uint64_t f = 0; f <= lastFrame; ++f) {
      micDb[f] = std::log10(micPow[f] + kEps) * 10.0;
      enhDb[f] = std::log10(enhPow[f] + kEps) * 10.0;
    }
  }

  // Frames without far-end activity feed the noise (ANS) and speech (AGC) statistics.
  std::vector<uint64_t> candidates;
  candidates.reserve(frameCount);
  if (!noFrames) {
    for (uint64_t f = 0; f <= lastFrame; ++f) {
      if (farActive[f] == 0) candidates.push_back(f);
    }
  }
  int32_t minCandidates =
      static_cast<int32_t>(static_cast<uint32_t>(opt.minNoiseFrames) + static_cast<uint32_t>(opt.minSpeechFrames));
  if (minCandidates <= 1) minCandidates = 1;

  double noiseCountD = 0.0;
  double speechCountD = 0.0;
  if (candidates.size() < static_cast<uint32_t>(minCandidates)) {
    r.ansNoiseFrames = 0;
    r.agcSpeechFrames = 0;
  } else {
    std::vector<double> candidateMicDb;
    candidateMicDb.reserve(candidates.size());
    for (const uint64_t f : candidates) candidateMicDb.push_back(micDb[f]);
    std::vector<double> sortedMicDb(candidateMicDb);
    std::sort(sortedMicDb.begin(), sortedMicDb.end());
    const double floorRef = Quantile(sortedMicDb, 0.1);

    const double ansThreshold1 = std::fmax(opt.ansSuppressionDbThreshold1, 0.0);
    const double ansThreshold2 = std::fmax(opt.ansSuppressionDbThreshold2, 0.0);
    const double agcTarget = opt.agcSpeechTargetDbfs;
    const double agcTolerance = std::fmax(opt.agcSpeechToleranceDb, 0.0);
    const double noiseCeiling = floorRef + std::fmax(opt.noiseMaxDbAboveFloor, 0.0);
    const double speechFloor = floorRef + std::fmax(opt.speechMinDbAboveNoise, 0.0);

    std::vector<double> noiseMicDb;
    std::vector<double> noiseEnhDb;
    std::vector<double> speechMicDb;
    std::vector<double> speechEnhDb;
    std::vector<double> speechGainDb;
    std::vector<uint64_t> speechFrames;
    double noiseMicPow = 0.0;
    double noiseEnhPow = 0.0;
    int32_t suppressed1 = 0;
    int32_t suppressed2 = 0;
    int32_t inRange = 0;
    for (const uint64_t f : candidates) {
      const double m = micDb[f];
      const double e = enhDb[f];
      if (m <= noiseCeiling) {
        noiseMicDb.push_back(m);
        noiseEnhDb.push_back(e);
        noiseMicPow += micPow[f];
        noiseEnhPow += enhPow[f];
        const double reduction = m - e;
        if (reduction >= ansThreshold1) ++suppressed1;
        if (reduction >= ansThreshold2) ++suppressed2;
      }
      if (m >= speechFloor) {
        speechMicDb.push_back(m);
        speechEnhDb.push_back(e);
        speechGainDb.push_back(e - m);
        speechFrames.push_back(f);
        if (std::fabs(e - agcTarget) <= agcTolerance) ++inRange;
      }
    }
    const double suppressed1D = static_cast<double>(suppressed1);
    const double suppressed2D = static_cast<double>(suppressed2);
    const double inRangeD = static_cast<double>(inRange);
    noiseMicPow += kEps;
    noiseEnhPow += kEps;

    const auto noiseCount = static_cast<int32_t>(noiseMicDb.size());
    const auto speechCount = static_cast<int32_t>(speechMicDb.size());
    r.ansNoiseFrames = noiseCount;
    r.agcSpeechFrames = speechCount;
    const int32_t minNoise = opt.minNoiseFrames > 1 ? opt.minNoiseFrames : 1;
    const int32_t minSpeech = opt.minSpeechFrames > 1 ? opt.minSpeechFrames : 1;
    if (minNoise <= noiseCount) {
      const double count = static_cast<double>(static_cast<uint32_t>(noiseCount));
      r.ansSuppressedNoiseRatio3db = suppressed1D / count;
      r.ansSuppressedNoiseRatio6db = suppressed2D / count;
      if (framesRan) {
        r.ansSuppressedTotalRatio3db = suppressed1D / totalD;
        r.ansSuppressedTotalRatio6db = suppressed2D / totalD;
      }
    }
    if (minSpeech <= speechCount) {
      r.agcSpeechLevelInRangeRatio = inRangeD / static_cast<double>(static_cast<uint32_t>(speechCount));
      if (framesRan) r.agcSpeechLevelInRangeTotalRatio = inRangeD / totalD;
    }

    bool noiseStats = false;
    double noiseFloorMic = 0.0;
    double noiseFloorEnh = 0.0;
    if (noiseMicDb.size() >= static_cast<uint32_t>(minNoise) && noiseEnhDb.size() == noiseMicDb.size()) {
      std::sort(noiseMicDb.begin(), noiseMicDb.end());
      std::sort(noiseEnhDb.begin(), noiseEnhDb.end());
      noiseFloorMic = Quantile(noiseMicDb, 0.5);
      noiseFloorEnh = Quantile(noiseEnhDb, 0.5);
      r.noiseFloorDbfsMic = noiseFloorMic;
      r.noiseFloorDbfsEnh = noiseFloorEnh;
      r.noiseReductionDb = noiseFloorMic - noiseFloorEnh;
      r.noiseEnergyReductionDb = std::log10(noiseMicPow / noiseEnhPow) * 10.0;
      noiseStats = true;
    }

    const int32_t minSpeechStats = opt.minSpeechFrames > 2 ? opt.minSpeechFrames : 2;
    bool speechMeans = false;
    double speechMicMean = 0.0;
    double speechEnhMean = 0.0;
    if (static_cast<uint32_t>(minSpeechStats) <= speechMicDb.size() && speechEnhDb.size() == speechMicDb.size()) {
      const MeanStd micStats = MeanAndStd(speechMicDb);
      const MeanStd enhStats = MeanAndStd(speechEnhDb);
      if (micStats.mean && enhStats.mean) {
        speechMicMean = *micStats.mean;
        speechEnhMean = *enhStats.mean;
        r.speechLevelDbfsMicMean = speechMicMean;
        r.speechLevelDbfsEnhMean = speechEnhMean;
        r.agcGainDbMean = speechEnhMean - speechMicMean;
        speechMeans = true;
      }
      if (micStats.stddev && enhStats.stddev) {
        r.speechLevelDbfsMicStd = *micStats.stddev;
        r.speechLevelDbfsEnhStd = *enhStats.stddev;
        r.speechLevelStdReductionDb = *micStats.stddev - *enhStats.stddev;
      }
    }

    if (static_cast<uint32_t>(minSpeechStats) <= speechGainDb.size()) {
      const MeanStd gainStats = MeanAndStd(speechGainDb);
      if (gainStats.mean && gainStats.stddev) r.agcGainDbStd = *gainStats.stddev;
      // Mean rate of change of an exponentially smoothed (200 ms) AGC gain.
      std::sort(speechFrames.begin(), speechFrames.end());
      if (speechFrames.size() >= 2) {
        const double hopSec = std::fmax(opt.hopMs / 1000.0, 1e-6);
        uint64_t prev = speechFrames[0];
        double smoothed = enhDb[prev] - micDb[prev];
        double sum = 0.0;
        for (size_t k = 1; k < speechFrames.size(); ++k) {
          const uint64_t f = speechFrames[k];
          const double gain = enhDb[f] - micDb[f];
          double dt = hopSec * static_cast<double>(f - prev);
          dt = hopSec < dt ? dt : hopSec;
          const double alpha = std::exp(dt / -kAgcSmoothingSec);
          const double next = std::fma(alpha, smoothed, gain * (1.0 - alpha));
          sum += std::fabs(next - smoothed) / dt;
          prev = f;
          smoothed = next;
        }
        const uint32_t steps = static_cast<uint32_t>(speechFrames.size()) - 1u;
        if (steps != 0) r.agcGainChangeDbPerSec = sum / static_cast<double>(steps);
      }
    }

    if (speechMeans && noiseStats) {
      r.snrImprovementDb = (speechEnhMean - noiseFloorEnh) - (speechMicMean - noiseFloorMic);
    }
    noiseCountD = static_cast<double>(r.ansNoiseFrames);
    speechCountD = static_cast<double>(r.agcSpeechFrames);
  }
  if (framesRan) {
    r.ansNoiseRatio = noiseCountD / totalD;
    r.agcSpeechRatio = speechCountD / totalD;
  }

  // Clipping and peak of the enhanced signal at the working rate.
  if (!en.empty()) {
    uint64_t clipped = 0;
    double peak = 0.0;
    for (const float x : en) {
      const double a = static_cast<double>(std::fabs(x));
      if (peak < a) peak = a;
      if (a >= kClipLevel) ++clipped;
    }
    r.enhClipFraction = static_cast<double>(clipped) / static_cast<double>(static_cast<uint64_t>(en.size()));
    r.enhPeakDbfs = std::log10(peak + kEps) * 20.0;
  }
  return r;
}

}  // namespace aec_metrics
}  // namespace plaud_rec
