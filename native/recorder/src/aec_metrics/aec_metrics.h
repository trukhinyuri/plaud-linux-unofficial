// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

// Offline "light" AEC quality metrics over aligned loopback (lpb), raw mic and
// enhanced (post-APM) mono float buffers: echo delay (GCC-PHAT), far-end /
// echo-dominant frame classification, echo suppression, noise suppression and
// AGC statistics. Behavior-compatible reimplementation of the
// Recorder.computeAecLightMetrics static of Plaud Desktop's recorder addon;
// see docs/native-api/recorder.md for the contract and evidence.
//
// Pure C++ (no N-API) so it can be unit-tested; every function may throw
// std::exception subclasses, which the N-API layer reports as
// "computeAecLightMetrics failed: <what()>".

#ifndef PLAUD_REC_AEC_METRICS_H
#define PLAUD_REC_AEC_METRICS_H

#include <cstddef>
#include <optional>
#include <vector>

namespace plaud_rec {
namespace aec_metrics {

struct Options {
  double frameMs = 20.0;
  double hopMs = 10.0;
  double maxDelayMs = 500.0;
  double corrThresh = 0.6;
  double lpbRelThreshDb = -40.0;
  int downsampleFactorForDelay = 4;
  double delayEstimationMaxSec = 10.0;
  int targetSampleRate = 0;  // <= 0: use the loopback rate
  double residualEchoCorrThresh = 0.3;
  double noiseMaxDbAboveFloor = 3.0;
  double speechMinDbAboveNoise = 10.0;
  int minNoiseFrames = 20;
  int minSpeechFrames = 20;
  double ansSuppressionDbThreshold1 = 3.0;
  double ansSuppressionDbThreshold2 = 6.0;
  double agcSpeechTargetDbfs = -18.0;
  double agcSpeechToleranceDb = 6.0;
};

struct Result {
  int sr = 0;
  int delayMicSamples = 0;
  int delayEnhSamples = 0;
  int totalFrames = 0;
  int farendActiveFrames = 0;
  int echoDominantFrames = 0;
  int nearendOnlyFrames = 0;
  int ansNoiseFrames = 0;
  int agcSpeechFrames = 0;
  std::optional<double> farendActiveRatio;
  std::optional<double> echoDominantRatio;
  std::optional<double> nearendOnlyRatio;
  std::optional<double> coherentEchoSuppressionDbFarendActive;
  std::optional<double> coherentEchoSuppressionDbFarendActiveGainNormalized;
  std::optional<double> mixtureEnergyReductionDbFarendActive;
  std::optional<double> meanAbsCorrMicFarendActive;
  std::optional<double> meanAbsCorrEnhFarendActive;
  std::optional<double> corrSuppressionDbFarendActive;
  std::optional<double> residualEchoRatioFarendActive;
  std::optional<double> erleDbEchoDominant;
  std::optional<double> coherentEchoSuppressionDbEchoDominant;
  std::optional<double> coherentEchoSuppressionDbEchoDominantGainNormalized;
  std::optional<double> meanAbsCorrMicEchoDominant;
  std::optional<double> meanAbsCorrEnhEchoDominant;
  std::optional<double> corrSuppressionDbEchoDominant;
  std::optional<double> residualEchoRatioEchoDominant;
  std::optional<double> ansNoiseRatio;
  std::optional<double> agcSpeechRatio;
  std::optional<double> ansSuppressedNoiseRatio3db;
  std::optional<double> ansSuppressedNoiseRatio6db;
  std::optional<double> ansSuppressedTotalRatio3db;
  std::optional<double> ansSuppressedTotalRatio6db;
  std::optional<double> agcSpeechLevelInRangeRatio;
  std::optional<double> agcSpeechLevelInRangeTotalRatio;
  std::optional<double> noiseFloorDbfsMic;
  std::optional<double> noiseFloorDbfsEnh;
  std::optional<double> noiseReductionDb;
  std::optional<double> noiseEnergyReductionDb;
  std::optional<double> speechLevelDbfsMicMean;
  std::optional<double> speechLevelDbfsEnhMean;
  std::optional<double> speechLevelDbfsMicStd;
  std::optional<double> speechLevelDbfsEnhStd;
  std::optional<double> speechLevelStdReductionDb;
  std::optional<double> agcGainDbMean;
  std::optional<double> agcGainDbStd;
  std::optional<double> agcGainChangeDbPerSec;
  std::optional<double> snrImprovementDb;
  std::optional<double> enhClipFraction;
  std::optional<double> enhPeakDbfs;
};

// JS property names in the order the official addon defines them: the nine
// integer fields first, then the optional fields (null when absent).
struct IntField {
  const char* name;
  int Result::*member;
};
struct OptionalField {
  const char* name;
  std::optional<double> Result::*member;
};
extern const IntField kIntFields[9];
extern const OptionalField kOptionalFields[40];

// All three inputs are resampled (linear interpolation) to
// opt.targetSampleRate, or to lpbSr when that is <= 0, and truncated to the
// shortest. Null/empty inputs or a non-positive working rate return a
// default Result (sr 0). Throws std::runtime_error on non-positive input rates.
Result ComputeAecLightMetrics(const float* lpb, std::size_t lpbLen, int lpbSr, const float* mic,
                              std::size_t micLen, int micSr, const float* enh, std::size_t enhLen,
                              int enhSr, const Options& opt);

// Lag (in samples at `sampleRate`) that maximizes the correlation of
// ref[i + lag] with sig[i]: positive when `ref` lags `sig`. Coarse GCC-PHAT on
// block-averaged signals, refined by normalized time-domain correlation.
int EstimateDelaySamplesGccPhat(const float* ref, std::size_t refLen, const float* sig,
                                std::size_t sigLen, int sampleRate, double maxDelayMs,
                                int downsampleFactor, double maxSeconds);

void ResampleLinear(const float* in, std::size_t len, int srcRate, int dstRate,
                    std::vector<float>& out);

// Iterative radix-2 FFT; inverse scales by 1/n.
void FftInPlace(std::vector<double>& re, std::vector<double>& im, bool inverse);

}  // namespace aec_metrics
}  // namespace plaud_rec

#endif  // PLAUD_REC_AEC_METRICS_H
