/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    ScenarioRunner.h
 * @brief   Simple class to test navigation scenarios
 * @author  Frank Dellaert
 */

#pragma once
#include <gtsam/navigation/AggregateImuReadings.h>
#include <gtsam/navigation/Scenario.h>
#include <gtsam/linear/Sampler.h>

namespace gtsam {

/*
 *  Simple class to test navigation scenarios.
 *  Takes a trajectory scenario as input, and can generate IMU measurements
 */
class ScenarioRunner {
 public:
  typedef imuBias::ConstantBias Bias;
  typedef boost::shared_ptr<AggregateImuReadings::Params> SharedParams;

 private:
  const Scenario* scenario_;
  const SharedParams p_;
  const double imuSampleTime_, sqrt_dt_;
  const Bias estimatedBias_;

  // Create two samplers for acceleration and omega noise
  mutable Sampler gyroSampler_, accSampler_;

 public:
  ScenarioRunner(const Scenario* scenario, const SharedParams& p,
                 double imuSampleTime = 1.0 / 100.0, const Bias& bias = Bias())
      : scenario_(scenario),
        p_(p),
        imuSampleTime_(imuSampleTime),
        sqrt_dt_(std::sqrt(imuSampleTime)),
        estimatedBias_(bias),
        // NOTE(duy): random seeds that work well:
        gyroSampler_(Diagonal(p->gyroscopeCovariance), 10),
        accSampler_(Diagonal(p->accelerometerCovariance), 29284) {}

  // NOTE(frank): hardcoded for now with Z up (gravity points in negative Z)
  // also, uses g=10 for easy debugging
  const Vector3& gravity_n() const { return p_->n_gravity; }

  // A gyro simply measures angular velocity in body frame
  Vector3 actual_omega_b(double t) const { return scenario_->omega_b(t); }

  // An accelerometer measures acceleration in body, but not gravity
  Vector3 actual_specific_force_b(double t) const {
    Rot3 bRn = scenario_->rotation(t).transpose();
    return scenario_->acceleration_b(t) - bRn * gravity_n();
  }

  // versions corrupted by bias and noise
  Vector3 measured_omega_b(double t) const {
    return actual_omega_b(t) + estimatedBias_.gyroscope() +
           gyroSampler_.sample() / sqrt_dt_;
  }
  Vector3 measured_specific_force_b(double t) const {
    return actual_specific_force_b(t) + estimatedBias_.accelerometer() +
           accSampler_.sample() / sqrt_dt_;
  }

  const double& imuSampleTime() const { return imuSampleTime_; }

  /// Integrate measurements for T seconds into a PIM
  AggregateImuReadings integrate(double T, const Bias& estimatedBias = Bias(),
                                 bool corrupted = false) const;

  /// Predict predict given a PIM
  NavState predict(const AggregateImuReadings& pim,
                   const Bias& estimatedBias = Bias()) const;

  /// Compute a Monte Carlo estimate of the predict covariance using N samples
  Matrix9 estimateCovariance(double T, size_t N = 1000,
                             const Bias& estimatedBias = Bias()) const;

  /// Estimate covariance of sampled noise for sanity-check
  Matrix6 estimateNoiseCovariance(size_t N = 1000) const;
};

}  // namespace gtsam
