"""Synthetic preview control tests; no aircraft, SSH connection or real media."""
import shlex
import unittest
from unittest.mock import Mock, patch
import threading
import time
from l3_preview import PreviewApp, PreviewState, UNSUPPORTED, arguments, parse_event, ssh_command


class PreviewControlTests(unittest.TestCase):
    def test_recovery_waits_for_clean_sdk_exit_and_keeps_original_deadline(self):
        args = arguments(["--host", "127.0.0.1", "--seconds", "60", "--no-display"])
        with patch("l3_preview.PreviewSession") as factory:
            old = Mock()
            old.snapshot.return_value = PreviewState(current=14)
            old.ready = threading.Event()
            old.finished = threading.Event()
            old.lock = threading.Lock()
            old.state = PreviewState(current=14)
            factory.return_value = old
            app = PreviewApp(args)
            receiver = Mock()
            receiver.stats.return_value = {"video_frames": 12}
            app.receiver = receiver
            app.started = time.monotonic() - 10
            app.request_recovery(time.monotonic(), source=7)
            receiver.stop.assert_called_once()
            old.send.assert_called_once_with("stop")
            self.assertEqual(app.received_frames, 12)
            app.tick()
            self.assertEqual(factory.call_count, 1)  # No overlapping SDK processes.
            new = Mock()
            new.finished = threading.Event()
            factory.return_value = new
            old.returncode = 0
            old.finished.set()
            app.tick()
            self.assertIs(app.session, new)
            self.assertEqual(app.restarts, 1)
            self.assertLess(factory.call_args.args[0].seconds, 60)
            self.assertEqual(factory.call_args.args[0].expected_source, 7)
            self.assertEqual(args.seconds, 60)
            new.start.assert_called_once()
            app.session = old
            app.recovering = True
            old.returncode = 1
            app.close = Mock()
            app.tick()
            self.assertEqual(factory.call_count, 2)  # SDK failure never retries.
            app.close.assert_called_once()

    def test_first_source_observation_does_not_restart_decoder(self):
        state = PreviewState()
        state.apply(parse_event("event=video_control action=status current=14 success=yes"))
        self.assertEqual(state.current, 14)
        self.assertEqual(state.generation, 0)
        state.apply(parse_event("event=video_control action=status current=-1 success=no"))
        state.apply(parse_event("event=video_control action=status current=14 success=yes"))
        self.assertEqual(state.generation, 2)

    def test_rejected_switch_keeps_actual_source_and_message(self):
        state = PreviewState(current=14, pending=True)
        state.apply(parse_event("event=video_control action=switch requested=7 current=14 "
                                f"success=no code={UNSUPPORTED}"))
        self.assertEqual(state.current, 14)
        self.assertFalse(state.pending)
        self.assertIn("未开放", state.message)
        message = state.message
        state.apply(parse_event("event=video_control action=status current=14 success=yes"))
        self.assertEqual(state.message, message)
        self.assertEqual(state.generation, 0)

    def test_ack_is_not_enough_and_unknown_readback_clears_source(self):
        state = PreviewState(current=14, pending=True)
        state.apply(parse_event("event=video_control action=switch requested=7 current=14 success=yes code=0"))
        self.assertIn("未确认", state.message)
        state.apply(parse_event("event=video_control action=switch requested=7 current=-1 success=no code=1"))
        self.assertIsNone(state.current)
        self.assertEqual(state.generation, 1)
        state.apply(parse_event("event=video_control action=switch requested=7 current=7 success=yes code=0"))
        self.assertEqual(state.current, 7)
        self.assertEqual(state.generation, 2)
        self.assertIn("已确认", state.message)

    def test_failed_ack_reports_observed_change_without_claiming_success(self):
        state = PreviewState(current=14)
        state.apply(parse_event("event=video_control action=switch requested=7 current=7 success=no code=1"))
        self.assertEqual(state.current, 7)
        self.assertEqual(state.generation, 1)
        self.assertIn("未确认", state.message)

    def test_ssh_argument_quoting_and_no_encoder_or_rviz(self):
        args = arguments(["--host", "127.0.0.1", "--remote-root", "/tmp/a b'$(touch nope)`x`",
                          "--remote-config", "/tmp/private space/config", "--seconds", "5"])
        command = ssh_command(args)
        remote = shlex.split(command[-1])
        self.assertEqual(remote[1], args.remote_root + "/scripts/run_l3_preview_onboard.sh")
        self.assertEqual(remote[remote.index("--config") + 1], args.remote_config)
        self.assertNotIn("rviz", command[-1])
        self.assertNotIn("bitrate", command[-1])
        with self.assertRaises(SystemExit):
            arguments(["--host", "127.0.0.1", "--ssh-host=-oProxyCommand=invalid"])



if __name__ == "__main__":
    unittest.main()
