import unittest
import numpy as np
from drift_filter import RestBias, lowpass
from evaluate_drift import one_second_angles


class DriftFilterTests(unittest.TestCase):
    def test_fractional_sample_boundaries_preserve_constant_rate(self):
        rate=np.tile([1.,-2.,0.005],(11000,1))
        increments=one_second_angles(rate,1/1008.47,10)
        np.testing.assert_allclose(increments,np.tile([1.,-2.,.005],(10,1)),atol=1e-10)

    def test_lowpass_is_causal_and_does_not_remove_dc_bias(self):
        x=np.random.default_rng(42).normal(size=(2000,3))
        np.testing.assert_allclose(lowpass(x,1/1000)[:800],lowpass(x[:800],1/1000),atol=1e-12)
        constant=np.tile([1.2,1.4,-.7],(2000,1))
        np.testing.assert_allclose(lowpass(constant,1/1000),constant,atol=1e-12)

    def test_unconfirmed_slow_rotation_cannot_update_bias(self):
        tracker=RestBias([1.2,1.4,-.7])
        gyro=np.tile([1.2,1.4,-.69],(100,1))
        accel=np.tile([.4,.3,np.sqrt(.75)],(100,1))
        for _ in range(120):
            self.assertFalse(tracker.finish_window(gyro,accel,1,confirmed_static=False)['updated'])
        np.testing.assert_allclose(tracker.correct(gyro),np.tile([0,0,.01],(100,1)),atol=1e-12)

    def test_tilted_rest_updates_only_future_samples(self):
        tracker=RestBias([1.,2.,3.])
        gyro=np.tile([1.03,2.,3.],(100,1))
        accel=np.tile([.4,.3,np.sqrt(.75)],(100,1))
        first=tracker.correct(gyro).copy()
        for _ in range(60):tracker.finish_window(gyro,accel,1,confirmed_static=True)
        self.assertTrue(1<tracker.bias[0]<1.03)
        np.testing.assert_allclose(first[:,0],.03)
        before=tracker.bias.copy()
        self.assertFalse(tracker.finish_window(gyro+[0,0,10],accel,1,confirmed_static=True)['updated'])
        np.testing.assert_array_equal(tracker.bias,before)


if __name__=='__main__':
    unittest.main()
