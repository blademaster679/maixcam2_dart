"""Host-only tests. Synthetic timing fixture is NOT a sensor mode or deployable table."""
import unittest
import tempfile
from pathlib import Path
from profile import ProfileError, validate, header
from analyze import analyze
from reverse_engineer import final_state, candidate, BASE


def synthetic_profile():
    # Arithmetic fixture only: intentionally has no analog/binning sequence.
    p = dict(status='vendor_sequence_supplied', source='SYNTHETIC COMPILE TEST ONLY', compile_only=True,
             sensor='OS04A10', width=640, height=360, fps=360, raw_bits=10,
             xclk_hz=24000000, lanes=4, readout='2x2-binning-crop', bayer='RGGB',
             hts=750, vts=400, min_vts=400, exposure_margin_lines=8,
             sclk_hz=108000000, lane_mbps=720)
    regs = {0x0100:0, 0x0322:0, 0x0323:0, 0x0324:0, 0x0325:9,
            0x0328:1, 0x032A:0, 0x032F:0, 0x376C:0x74}
    for addr, value in [(0x3808,640),(0x380A,360),(0x380C,750),(0x380E,400),(0x3501,100),(0x384C,750)]:
        regs[addr], regs[addr+1] = value >> 8, value & 255
    p['registers'] = [[a,v,0] for a,v in regs.items()]
    return p


class ProfileTests(unittest.TestCase):
    def test_candidate_only_changes_documented_geometry_timing(self):
        source='static camera_i2c_reg_array '+BASE+'[] = {\n'+''.join(
            f'{{0x{a:04x}, 0x{v:02x}}},\n' for a,v in
            [(0x0103,1),(0x0322,1),(0x0323,2),(0x0324,0),(0x0325,0x90),
             (0x3600,0xab),(0x0305,0x3c),(0x3800,0),(0x3801,0),(0x3802,0),(0x3803,0),
             (0x3804,10),(0x3805,143),(0x3806,5),(0x3807,255),
             (0x3810,0),(0x3811,9),(0x3812,0),(0x3813,9),(0x3820,2)])+'};\n'
        source+='static camera_i2c_reg_array OS04A10_2lane_2688x1520_10bit_Linear_60fps[] = {\n{0x0325, 0xd8},\n};\n'
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'settings.h'; path.write_text(source)
            p,state=candidate(path,500,400,full_width=True)
            self.assertEqual(p['status'],'reverse_engineered_candidate')
            self.assertEqual(p['fps'],360)
            self.assertEqual(p['bayer'],'BGGR')
            self.assertEqual(state[0x3600],0xab)
            self.assertEqual(state[0x0305],0x3c)
            self.assertEqual(sum(a==0x0325 for a,_,_ in p['registers']),1)
            self.assertFalse(any(a==0x0328 for a,_,_ in p['registers']))
            self.assertEqual(state[0x380e]*256+state[0x380f],400)
            with self.assertRaises(ValueError): candidate(path,499,400)
            q,state108=candidate(path,750,400,full_width=True,sclk_mhz=108)
            self.assertEqual(state108[0x0325],0xd8)
            self.assertEqual(state108[0x0305],0x3c)
            self.assertEqual(q['fps'],360)
            with self.assertRaises(ValueError): candidate(path,732,400,sclk_mhz=108)
    def test_reverse_final_state_respects_reset(self):
        self.assertEqual(final_state([[0x3808,2,0],[0x0103,1,10000],[0x3808,3,0]]),
                         {0x0103:1,0x3808:3})
    def test_missing_profile_stays_disabled(self):
        self.assertIn('OS04A10_HFR_AVAILABLE 0', header())
        with self.assertRaises(ProfileError): validate({'status':'missing_vendor_sequence'})

    def test_final_state_and_reset(self):
        p = synthetic_profile()
        p['registers'].insert(0, [0x3808,0xff,0])
        self.assertEqual(validate(p)[1][0x3808],2)
        p['registers'].append([0x0103,1,10000])
        with self.assertRaises(ProfileError): validate(p)

    def test_invalid_timing_and_exposure(self):
        for key, value in [('sclk_hz',float('nan')),('vts',401),('lane_mbps',100),
                           ('exposure_margin_lines',9),('fps',361)]:
            p = synthetic_profile(); p[key] = value
            with self.subTest(key=key), self.assertRaises(ProfileError): validate(p)
        p = synthetic_profile(); p['registers'].append([0x3501,255,0])
        with self.assertRaises(ProfileError): validate(p)

    def test_stream_on_and_pll_rejected(self):
        for reg in [[0x0100,1,0],[0x0325,10,0]]:
            p = synthetic_profile(); p['registers'].append(reg)
            with self.assertRaises(ProfileError): validate(p)
        p = synthetic_profile(); p['registers'] = [r for r in p['registers'] if r[0]!=0x032F]
        with self.assertRaises(ProfileError): validate(p)

    def test_enabled_header_separates_driver_tables(self):
        text = header(synthetic_profile())
        self.assertIn('#ifdef OS04A10_HFR_DRIVER_TABLES', text)

    def test_frame_analyzer_detects_gaps_and_duplicates(self):
        rows = [dict(sequence=s,pts_raw=1000+i*100,monotonic_us=2000+i*100,
                     width=640,height=360) for i,s in enumerate([1,2,4,4,3])]
        result = analyze(rows)
        self.assertEqual(result['sequence_gaps'],1)
        self.assertEqual(result['sequence_duplicates'],1)
        self.assertEqual(result['sequence_backwards'],1)
        self.assertEqual(result['monotonic_fps'],10000)

    def test_complete_windows_exclude_partial_and_detect_under_rate(self):
        rows=[dict(sequence=i,pts_raw=i*10000,monotonic_us=i*10000,width=640,height=360)
              for i in range(2501)]
        result=analyze(rows)
        self.assertEqual(result['complete_10s_windows'],2)
        self.assertEqual(result['window_fps_10s'],[100,100])
        self.assertEqual(result['windows_below_359fps'],[0,1])

    def test_nonmonotonic_host_clock_is_rejected(self):
        rows=[dict(sequence=i,pts_raw=i,monotonic_us=10,width=640,height=360) for i in range(2)]
        with self.assertRaises(ValueError): analyze(rows)


if __name__ == '__main__': unittest.main()
