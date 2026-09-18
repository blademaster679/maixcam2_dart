"""Causal IMU bias compensation. All gyro units are degrees/second.

Gravity direction is not assumed. Bias adaptation requires external confirmation
of rest: a six-axis IMU alone cannot reliably distinguish slow yaw from bias.
"""
from dataclasses import dataclass
import math
import numpy as np


@dataclass(frozen=True)
class Settings:
    cutoff_hz: float = 10.0
    bias_time_constant_s: float = 60.0
    rest_dwell_s: float = 3.0
    max_accel_std_g: float = 0.02
    max_gyro_std_dps: float = 1.0
    max_gravity_change_g: float = 0.01
    max_gyro_innovation_dps: float = 0.20
    max_axis_update_innovation_dps: float = 0.05


class RestBias:
    """Correct with current bias; update only after a completed rest window.

    Call correct() before finish_window(). The resulting estimate can affect
    only subsequent samples, never the window used to estimate it.
    """
    def __init__(self, initial_bias, settings=Settings()):
        self.bias = np.array(initial_bias, dtype=float, copy=True)
        self.settings = settings
        self.rest_s = 0.0
        self.previous_accel = None

    def correct(self, gyro):
        return np.asarray(gyro) - self.bias

    def finish_window(self, gyro, accel, duration_s, *, confirmed_static=False):
        gyro, accel = np.asarray(gyro), np.asarray(accel)
        c = self.settings
        if duration_s <= 0 or not len(gyro) or len(gyro) != len(accel):
            raise ValueError('window duration and sample counts must be valid')
        gm, am = gyro.mean(axis=0), accel.mean(axis=0)
        stable_direction = (self.previous_accel is not None and
                            np.linalg.norm(am-self.previous_accel) < c.max_gravity_change_g)
        eligible = bool(confirmed_static and np.isfinite(gyro).all() and
                        np.isfinite(accel).all() and 0.85 < np.linalg.norm(am) < 1.15 and
                        np.max(accel.std(axis=0)) < c.max_accel_std_g and
                        np.max(gyro.std(axis=0)) < c.max_gyro_std_dps and stable_direction and
                        np.linalg.norm(gm-self.bias) < c.max_gyro_innovation_dps)
        self.previous_accel = am
        self.rest_s = self.rest_s+duration_s if eligible else 0.0
        updated = eligible and self.rest_s >= c.rest_dwell_s
        if updated:
            alpha = -math.expm1(-duration_s/c.bias_time_constant_s)
            self.bias += alpha*np.clip(gm-self.bias, -c.max_axis_update_innovation_dps,
                                      c.max_axis_update_innovation_dps)
        return dict(eligible=eligible, updated=bool(updated), bias=self.bias.tolist())


def lowpass(values, dt_s, cutoff_hz=10.0):
    """First-order causal low-pass, vectorized in bounded blocks, no filtfilt.

    Recurrence y[k]=(1-alpha)*y[k-1]+alpha*x[k], alpha=1-exp(-2*pi*fc*dt).
    The bounded block length avoids exponential underflow in the vectorization.
    """
    x = np.asarray(values, dtype=float)
    if not len(x) or dt_s <= 0 or not 0 < cutoff_hz < 0.5/dt_s:
        raise ValueError('invalid samples or cutoff')
    exponent = 2*math.pi*cutoff_hz*dt_s
    alpha = -math.expm1(-exponent)
    block = max(1, min(256, int(20/exponent)))
    y = np.empty_like(x)
    state = x[0].copy()
    for start in range(0, len(x), block):
        z = x[start:start+block]
        powers = np.exp(-exponent*np.arange(1, len(z)+1))[:, None]
        y[start:start+len(z)] = powers*(state + np.cumsum(alpha*z/powers, axis=0))
        state = y[start+len(z)-1]
    return y
