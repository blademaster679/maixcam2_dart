import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ctl', Path(__file__).with_name('recorderctl.py'))
ctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ctl)

class ControlTests(unittest.TestCase):
    def test_multiplier_conversion_and_no_display_arguments(self):
        with patch('sys.argv', ['ctl', '_run', '--gain', '2', '--seconds', '600']), patch.object(ctl, 'supervise', return_value=0) as run:
            self.assertEqual(0, ctl.main())
            args = run.call_args.args[0]
            self.assertEqual('2048', args[args.index('--gain')+1])
            self.assertEqual('600', args[args.index('--seconds')+1])

    def test_invalid_input_never_starts_service(self):
        for options in (['--gain', 'nan'], ['--seconds', '601'], ['--label', '../bad']):
            with patch('sys.argv', ['ctl', 'start', *options]), patch.object(ctl, 'command') as command:
                with self.assertRaises(ValueError): ctl.main()
                command.assert_not_called()

    def test_start_uses_transient_service(self):
        with patch('sys.argv', ['ctl', 'start']), patch.object(ctl, 'active', return_value=False), patch.object(ctl, 'command') as command:
            command.return_value.returncode = 0
            self.assertEqual(0, ctl.main())
            args = command.call_args.args
            self.assertEqual('systemd-run', args[0])
            self.assertIn('_run', args)
            self.assertIn('--collect', args)

    def test_stop_signals_supervisor_only(self):
        with patch('sys.argv', ['ctl', 'stop']), patch.object(ctl, 'command') as command:
            command.return_value.returncode = 0
            ctl.main()
            command.assert_called_once_with('systemctl', 'kill', '--kill-who=main', '--signal=SIGUSR1', ctl.UNIT)

if __name__ == '__main__': unittest.main()
