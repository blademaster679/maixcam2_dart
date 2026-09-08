import csv,json,tempfile,unittest
from pathlib import Path
from analyze_business import analyze
class BusinessAnalysisTest(unittest.TestCase):
    def test_rates_do_not_claim_accuracy_or_exposure_latency(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/'capture.json').write_text(json.dumps({'exit_code':10,'acquire_errors':0}))
            (p/'leases.json').write_text(json.dumps({'acquired':3,'released':3,'map_errors':0,'release_errors':0}))
            (p/'frames.csv').write_text('sequence,pts_raw,monotonic_us\n10,100,1000000\n11,5656,1005556\n13,16767,1016667\n')
            (p/'vision.csv').write_text('received_us,started_us,finished_us,green_ran,direct_green,armor_ran,search_ran,motion_ran,search_us,roi_convert_us,detect_us,motion_us\n1000000,1001000,1002000,1,0,1,1,0,20,10,100,0\n')
            (p/'health.csv').write_text('elapsed_us,temperature_c,mipi_errors,rss_kb\n0,50,0,22000\n')
            (p/'targets.jsonl').write_text(json.dumps({'timestamp_us':1003000,'source_received_us':1000000,'source_metadata_valid':True,'safe_for_control':False})+'\n')
            r=analyze(p)
            self.assertEqual(r['sequence_missing'],1)
            self.assertFalse(r['capture_continuity_pass'])
            self.assertGreater(r['green_detection_call_hz'],0)
            self.assertEqual(r['direct_green_observation_hz'],0)
            self.assertIsNone(r['exposure_to_control_latency_ms'])
            self.assertFalse(r['competition_acceptance_pass'])
            self.assertEqual(r['software_source_age_at_control_ms']['p95'],3)
if __name__=='__main__':unittest.main()
