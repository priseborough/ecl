/****************************************************************************
 *
 *   Copyright (c) 2015 Estimation and Control Library (ECL). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name ECL nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mag_calibration.cpp
 * Magnetometer calibration methods.
 *
 * @author Paul Riseborough <p_riseborough@live.com.au>
 *
 */

#include "ekf.h"
#include <ecl.h>
#include <mathlib/mathlib.h>
#include <systemlib/mavlink_log.h>

orb_advert_t _mavlink_log_pub;

void Ekf::fuseMagCal()
{
	// apply imu bias corrections to sensor data
	Vector3f corrected_delta_ang = _imu_sample_delayed.delta_ang - _state.gyro_bias;

	// check if yaw rate and tilt is sufficient to perform calibration
	if (_imu_sample_delayed.delta_ang_dt > 0.0001f) {
		float yaw_rate = _R_to_earth(2,0) * corrected_delta_ang(0)
				+ _R_to_earth(2,1) * corrected_delta_ang(1)
				+ _R_to_earth(2,2) * corrected_delta_ang(2);
		yaw_rate = yaw_rate / _imu_sample_delayed.delta_ang_dt;

		bool tilt_ok = _R_to_earth(2,2) > cosf(math::radians(45.0f));

		if (!_mag_bias_ekf_active && fabsf(yaw_rate) > math::radians(10.0f) && tilt_ok) {
			_mag_bias_ekf_active = true;
		} else if (_mag_bias_ekf_active && (fabsf(yaw_rate) < math::radians(5.0f) || !tilt_ok)) {
			_mag_bias_ekf_active = false;
		}

	} else {
		// invalid dt so can't proceed
		return;

	}

	// don't run if main filter is using the magnetomer or if excessively tilted of if not rotating quickly enough
	if (!_mag_use_inhibit || !_mag_bias_ekf_active) {
		return;
	}

	// limit to run once per 10 degrees of yaw rotation
	Eulerf euler321(_state.quat_nominal);
	float yaw_delta = euler321(2) - _mag_bias_ekf_yaw_last;
	if (yaw_delta > M_PI_F) {
		yaw_delta -= M_TWOPI_F;
	} else if (yaw_delta < -M_PI_F) {
		yaw_delta += M_TWOPI_F;
	}
	if (fabsf(yaw_delta) < math::radians(10.0f)) {
		return;
	}
	_mag_bias_ekf_yaw_last = euler321(2);

	// reset the covariance matrix and states first time of if data hasn't been fused in the last 20 seconds
	float time_delta_sec =  1E-6f * (float)(_imu_sample_delayed.time_us - _mag_bias_ekf_time_us);
	if (_mag_bias_ekf_time_us == 0 || time_delta_sec > 20 ) {
		memset(_mag_cov_mat, 0, sizeof(_mag_cov_mat));
		_mag_cov_mat[0][0] = 0.25f;
		_mag_cov_mat[1][1] = 0.25f;
		_mag_cov_mat[2][2] = 0.25f;
		_mag_cov_mat[3][3] = 1.0f;
		_mag_cal_states.mag_bias(0) = 0.0f;
		_mag_cal_states.mag_bias(1) = 0.0f;
		_mag_cal_states.mag_bias(2) = 0.0f;
		_mag_cal_states.yaw_offset = 0.0f;
		_mag_bias_ekf_time_us = _imu_sample_delayed.time_us;

		return;

	}

	// Apply process noise of 0.5 deg/sec to yaw state variance
	float yaw_process_noise_variance = time_delta_sec * math::radians(0.5f);
	yaw_process_noise_variance *= yaw_process_noise_variance;
	_mag_cov_mat[3][3] += yaw_process_noise_variance;

	// predicted earth field vector
	Vector3f mag_EF = getGeoMagNED();

	// rotate the quaternions by the iniital yaw offset
	Quatf quat_relative;
	quat_relative(0) = cosf(_mag_cal_states.yaw_offset);
	quat_relative(1) = 0.0f;
	quat_relative(2) = 0.0f;
	quat_relative(3) = sinf(_mag_cal_states.yaw_offset);
	quat_relative = _state.quat_nominal * quat_relative;

	// get equivalent rotation matrix
	Matrix3f Teb = quat_to_invrotmat(quat_relative);

	// rotate earth field into body frame and add bias states to get predicted measurement
	Vector3f mag_obs_predicted = Teb * mag_EF + _mag_cal_states.mag_bias;

	// XYZ Measurement noise.
	float R_MAG = fmaxf(_params.mag_noise, 0.0f);
	R_MAG = R_MAG * R_MAG;

	// copy to variable names used by autocode
	// TODO remove
	float q0 = quat_relative(0);
	float q1 = quat_relative(1);
	float q2 = quat_relative(2);
	float mn = mag_EF(0);
	float me = mag_EF(1);
	float md = mag_EF(2);

	// intermediate variables from algebraic optimisation
	float t2 = cosf(_mag_cal_states.yaw_offset);
	float t3 = sinf(_mag_cal_states.yaw_offset);
	float t4 = q1*t2;
	float t5 = q0*t3;
	float t6 = t4+t5;
	float t7 = q2*t2;
	float t8 = q1*t3;
	float t9 = t7+t8;
	float t10 = q0*t2;
	float t15 = q2*t3;
	float t11 = t4-t15;
	float t12 = q0*t2*t9*2.0f;
	float t13 = t8-t10;
	float t14 = q0*t3*t13*2.0f;
	float t16 = q0*t3*t11*2.0f;
	float t17 = q0*q0;
	float t18 = t9*t11*2.0f;
	float t19 = q0*t2*t6*2.0f;
	float t20 = t6*t11*2.0f;
	float t21 = t2*t3*t17*4.0f;
	float t22 = t6*t13*2.0f;
	float t23 = t18+t22;
	float t24 = t12-t14+t16+t19;

	// Observation jacobian and Kalman gain vectors
	float H_MAG[4];
	float Kfusion[4];
	float innovation[3];

	// update the states and covariance using sequential fusion of the magnetometer components
	for (uint8_t index = 0; index <= 2; index++) {

		// Calculate observation jacobians and innovation
		if (index == 0) {
			// Calculate X axis observation jacobians
			memset(H_MAG, 0, sizeof(H_MAG));
			H_MAG[0] = 1.0f;
			H_MAG[1] = 0.0f;
			H_MAG[2] = 0.0f;
			H_MAG[3] = -md*(t12+t14+t16-q0*t2*t6*2.0f)-me*t24+mn*(t18+t6*(t10-q1*t3)*2.0f-t2*t3*t17*4.0f);

		} else if (index == 1) {
			// Calculate Y axis observation jacobians
			memset(H_MAG, 0, sizeof(H_MAG));
			H_MAG[0] = 0.0f;
			H_MAG[1] = 1.0f;
			H_MAG[2] = 0.0f;
			H_MAG[3] = me*t23-md*(t20+t21-t9*t13*2.0f)+mn*(t12+t14+t16-t19);

		} else if (index == 2) {
			// calculate Z axis observation jacobians
			memset(H_MAG, 0, sizeof(H_MAG));
			H_MAG[0] = 0.0f;
			H_MAG[1] = 0.0f;
			H_MAG[2] = 1.0f;
			H_MAG[3] = -md*t23-mn*t24+me*(-t20+t21+t9*t13*2.0f);

		}
		innovation[index] = mag_obs_predicted(index) - _mag_sample_delayed.mag(index);

		// calculate the innovation variance
		float PH[4];
		float mag_innov_var = R_MAG;
		for (unsigned row = 0; row < 4; row++) {
			PH[row] = 0.0f;

			for (uint8_t col = 0; col < 4; col++) {
				PH[row] += _mag_cov_mat[row][col] * H_MAG[col];
			}

			mag_innov_var += H_MAG[row] * PH[row];
		}

		float mag_innov_var_inv;

		// check if the innovation variance calculation is badly conditioned
		if (mag_innov_var >= R_MAG) {
			// the innovation variance contribution from the state covariances is not negative, no fault
			mag_innov_var_inv = 1.0f / mag_innov_var;

		} else {
			// we reinitialise the covariance matrix and abort this fusion step
			memset(_mag_cov_mat, 0, sizeof(_mag_cov_mat));
			_mag_cov_mat[0][0] = 0.25f;
			_mag_cov_mat[1][1] = 0.25f;
			_mag_cov_mat[2][2] = 0.25f;
			_mag_cov_mat[3][3] = 1.0f;

			ECL_ERR("EKF mag bias cal fusion numerical error - covariance reset");

			return;

		}

		// calculate the Kalman gains
		for (uint8_t row = 0; row < 4; row++) {
			Kfusion[row] = 0.0f;

			for (uint8_t col = 0; col < 4; col++) {
				Kfusion[row] += _mag_cov_mat[row][col] * H_MAG[col];
			}

			Kfusion[row] *= mag_innov_var_inv;

		}

		// apply covariance correction via P_new = (I -K*H)*P
		// first calculate expression for KHP
		// then calculate P - KHP
		float KHP[4][4];
		float KH[4];

		for (unsigned row = 0; row < 4; row++) {

			KH[0] = Kfusion[row] * H_MAG[0];
			KH[1] = Kfusion[row] * H_MAG[1];
			KH[2] = Kfusion[row] * H_MAG[2];
			KH[3] = Kfusion[row] * H_MAG[3];

			for (unsigned col = 0; col < 4; col++) {
				float tmp = KH[0] * _mag_cov_mat[0][col];
				tmp += KH[1] * _mag_cov_mat[1][col];
				tmp += KH[2] * _mag_cov_mat[2][col];
				tmp += KH[3] * _mag_cov_mat[3][col];
				KHP[row][col] = tmp;
			}
		}

		// apply the covariance corrections
		for (unsigned row = 0; row < 4; row++) {
			for (unsigned col = 0; col < 4; col++) {
				_mag_cov_mat[row][col] = _mag_cov_mat[row][col] - KHP[row][col];
			}
		}

		// correct the covariance matrix for gross errors
		// force symmetry
		for (unsigned row = 0; row < 4; row++) {
			for (unsigned col = 0; col < row; col++) {
				float tmp = (_mag_cov_mat[row][col] + _mag_cov_mat[col][row]) / 2;
				_mag_cov_mat[row][col] = tmp;
				_mag_cov_mat[col][row] = tmp;
			}
		}
		// force positive variances
		for (unsigned col = 0; col < 4; col++) {
			_mag_cov_mat[col][col] = fmaxf(_mag_cov_mat[col][col], 1e-12f);
		}

		// apply the state corrections
		const float innov_limit = 0.5f;
		innovation[index] = math::constrain(innovation[index], -innov_limit, innov_limit);
		_mag_cal_states.mag_bias(0) -= Kfusion[0] * innovation[index];
		_mag_cal_states.mag_bias(1) -= Kfusion[1] * innovation[index];
		_mag_cal_states.mag_bias(2) -= Kfusion[2] * innovation[index];
		_mag_cal_states.yaw_offset -= Kfusion[3] * innovation[index];

		// Constrain state estimates
		const float bias_limit = 0.5f;
		_mag_cal_states.mag_bias(0) = math::constrain(_mag_cal_states.mag_bias(0), -bias_limit, bias_limit);
		_mag_cal_states.mag_bias(1) = math::constrain(_mag_cal_states.mag_bias(1), -bias_limit, bias_limit);
		_mag_cal_states.mag_bias(2) = math::constrain(_mag_cal_states.mag_bias(2), -bias_limit, bias_limit);
		_mag_cal_states.yaw_offset = math::constrain(_mag_cal_states.yaw_offset, -math::radians(180.0f), math::radians(180.0f));
	}

	_mag_bias_ekf_time_us = _imu_sample_delayed.time_us;

	// replay debug code
	printf("states = %5.3f,%5.3f,%5.3f , %5.3f,\n",
	(double)_mag_cal_states.mag_bias(0), (double)_mag_cal_states.mag_bias(1), (double)_mag_cal_states.mag_bias(2),
	(double)_mag_cal_states.yaw_offset);
	printf("innov = %5.3f,%5.3f,%5.3f\n\n",(double)innovation[0],(double)innovation[1],(double)innovation[2]);

/*
	// debug code
	static uint64_t print_time_us = 0;
	if (_imu_sample_delayed.time_us - print_time_us > 1000000) {
		mavlink_and_console_log_info(&_mavlink_log_pub, "states = %5.3f,%5.3f,%5.3f , %5.3f,\n",
		(double)_mag_cal_states.mag_bias(0), (double)_mag_cal_states.mag_bias(1), (double)_mag_cal_states.mag_bias(2),
		(double)_mag_cal_states.yaw_offset);
		mag_obs_predicted = Teb * mag_EF;
		mavlink_and_console_log_info(&_mavlink_log_pub, "magEF,BF = %5.3f,%5.3f,%5.3f, %5.3f,%5.3f,%5.3f\n",
		(double)mag_EF(0), (double)mag_EF(1), (double)mag_EF(2),
		(double)mag_obs_predicted(0), (double)mag_obs_predicted(1), (double)mag_obs_predicted(2));
		print_time_us = _imu_sample_delayed.time_us;

	}
*/
}
