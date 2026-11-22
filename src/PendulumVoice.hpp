#pragma once

#include <cmath>
#include <dsp/resampler.hpp>

#include "supercell/dsp/adsr_envelope.h"

class PendulumVoice {
 public:
  void reset() {
    envelope.Reset();
    decimator.reset();
    phase = drivePhase = angle = velocity = dc = 0.f;
  }

  void configure(bool gate, float pitch, float attack, float decay, float sustain,
                 float release, float amount, float shape, float fold, float animate) {
    envelope.Configure(gate, attack, decay, sustain, release);
    const float frequency = 261.625565f * std::exp2(pitch / 12.f);
    phaseStep = kTau * frequency / (2.f * kSampleRate);
    // A bipolar quadratic taper gives fine detune control around noon.
    const float centeredAnimate = 2.f * animate - 1.f;
    const float detuneCents = 100.f * centeredAnimate * std::abs(centeredAnimate);
    drivePhaseStep = phaseStep * std::exp2(detuneCents / 1200.f);
    chaosAmount = amount;
    force = 0.75f + 1.15f * shapeToDrive(shape);
    secondHarmonicMix = 0.65f * shape;
    drive = 1.f + 4.f * fold;
  }

  float next() {
    const float gain = envelope.Next();
    float oversampled[2];
    for (float& sample : oversampled) {
      const float carrier = std::sin(phase);

      // Integrate a damped, driven pendulum with a midpoint step. Time is
      // measured in radians of its own drive so Animate detunes this system
      // without changing the keyed sine or the behavior selected by Shape.
      const float halfStep = 0.5f * drivePhaseStep;
      const float midAngle = angle + halfStep * velocity;
      const float midVelocity = velocity + halfStep * acceleration(angle, velocity, std::sin(drivePhase));
      velocity += drivePhaseStep * acceleration(midAngle, midVelocity, std::sin(drivePhase + halfStep));
      angle = std::remainder(angle + drivePhaseStep * midVelocity, kTau);
      phase = advancePhase(phase, phaseStep);
      drivePhase = advancePhase(drivePhase, drivePhaseStep);

      // Shape also changes the pendulum's harmonic balance in regions where
      // its motion locks to the carrier and becomes periodic again.
      const float fundamental = std::sin(angle);
      const float secondHarmonic = 2.f * fundamental * std::cos(angle);
      const float pendulum = fundamental + secondHarmonicMix * (secondHarmonic - fundamental);
      const float signal = carrier + chaosAmount * (pendulum - carrier);
      // Reflect successive peaks back through the range [-1, 1].
      sample = 1.f - std::abs(std::remainder(signal * drive - 1.f, 4.f));
    }
    const float signal = decimator.process(oversampled);
    dc += kDcCoefficient * (signal - dc);
    // Leave headroom for overshoot from the decimation filter.
    return 0.7f * gain * (signal - dc);
  }

 private:
  static constexpr float kTau = 6.28318530718f;
  static constexpr float kSampleRate = 32000.f;
  static constexpr float kDcCoefficient = kTau * 8.f / kSampleRate;

  static float shapeToDrive(float shape) {
    // Local taper centered between the repeating and irregular regimes.
    // The slope is 0.2 at the center and rejoins the linear curve smoothly.
    constexpr float center = 0.725f;
    constexpr float radius = 0.22f;
    const float distance = shape - center;
    const float x = distance / radius;
    if (std::abs(x) >= 1.f) {
      return shape;
    }
    const float window = 1.f - x * x;
    return shape - 0.8f * distance * window * window;
  }

  static float advancePhase(float phase, float step) {
    phase += step;
    return phase >= kTau ? phase - kTau : phase;
  }

  float acceleration(float position, float speed, float excitation) const {
    return force * excitation - 0.28f * speed - 1.65f * std::sin(position);
  }

  clouds::AdsrEnvelope envelope;
  rack::dsp::Decimator<2, 12> decimator;
  float phase = 0.f;
  float drivePhase = 0.f;
  float angle = 0.f;
  float velocity = 0.f;
  float dc = 0.f;
  float phaseStep = 0.f;
  float drivePhaseStep = 0.f;
  float chaosAmount = 0.f;
  float force = 0.f;
  float secondHarmonicMix = 0.f;
  float drive = 1.f;
};
