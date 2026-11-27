"""Synthetic preview control tests; no aircraft, SSH connection or real media."""
from io import StringIO
import shlex
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import MagicMock, Mock, patch
import threading
import time
from types import SimpleNamespace
from l3_preview import PreviewApp, PreviewSession, PreviewState, UNSUPPORTED, arguments, parse_event, ssh_command


class PreviewControlTests(unittest.TestCase):
    def test_retries_banner_timeout_before_remote_preview_starts(self):
        args = arguments(["--host", "127.0.0.1", "--no-display"])
        timeout = "Connection timed out during banner exchange\n"
        connected = "event=stream phase=video_listen\n"
        for log, retry in ((timeout, True), ("event=preview_begin\n" + timeout, False)):
            with self.subTest(retry=retry), TemporaryDirectory() as directory, \
                    patch("l3_preview.tempfile.mkdtemp", return_value=directory), \
                    patch("l3_preview.subprocess.Popen") as launch:
                first, second = MagicMock(), MagicMock()
                for process, output, code in ((first, log, 255), (second, connected, 0)):
                    process.__enter__.return_value = process
                    process.stdout = StringIO(output)
                    process.wait.return_value = code
                launch.side_effect = [first, second]
                session = PreviewSession(args)
                with patch.object(session.stopping, "wait", return_value=False):
                    session._run()
                self.assertEqual(launch.call_count, 2 if retry else 1)
                self.assertEqual(session.returncode, 0 if retry else 255)
                self.assertEqual(session.ready.is_set(), retry)

    def test_startup_failure_keeps_the_error_visible(self):
        args = arguments(["--host", "127.0.0.1", "--no-display"])
        with patch("l3_preview.PreviewSession") as factory:
            session = factory.return_value
            session.ready, session.finished = threading.Event(), threading.Event()
            session.finished.set()
            session.returncode = 255
            state = PreviewState(message="4G SSH 握手超时")
            session.snapshot.return_value = state
            app = PreviewApp(args)
            app.root = Mock()
            app.toggle = Mock()
            app.message_text = Mock()
            app.source_text = Mock()
            app.tick()
            self.assertTrue(app.startup_failed)
            self.assertFalse(app.closed)
            app.message_text.set.assert_called_with(state.message)
            app.root.destroy.assert_not_called()

    def test_auto_yolo_uses_first_fresh_visible_frame_once(self):
        args = arguments(["--host", "127.0.0.1", "--no-display", "--yolo-engine",
                          "/tmp/YOLOX.engine", "--start-yolo"])
        with patch("l3_preview.PreviewSession") as factory:
            session = factory.return_value
            session.ready, session.finished = threading.Event(), threading.Event()
            session.ready.set()
            state = PreviewState(current=14, yolo_available=True)
            session.snapshot.return_value = state
            session.send.return_value = True
            app = PreviewApp(args)
            now = time.monotonic()
            frame = SimpleNamespace(received_monotonic_ns=time.monotonic_ns() - 3_000_000_000)
            app.start_yolo_on_first_frame(state, frame, now)
            session.send.assert_not_called()
            frame.received_monotonic_ns = time.monotonic_ns()
            app.start_yolo_on_first_frame(state, frame, now)
            app.start_yolo_on_first_frame(state, frame, now)
            session.send.assert_called_once_with("visible")
            state.current = 1
            app.start_yolo_on_first_frame(state, frame, now)
            app.start_yolo_on_first_frame(state, frame, now)
            self.assertEqual([call.args[0] for call in session.send.call_args_list], ["visible", "yolo on"])

    def test_button_toggles_actual_l3_sources_and_blocks_during_transition(self):
        args = arguments(["--host", "127.0.0.1", "--seconds", "60", "--no-display"])
        with patch("l3_preview.PreviewSession") as factory:
            session = factory.return_value
            session.ready = threading.Event()
            session.finished = threading.Event()
            state = PreviewState(current=14)
            session.snapshot.return_value = state
            app = PreviewApp(args)
            app.switch()
            session.send.assert_not_called()
            session.ready.set()
            app.switch()
            session.send.assert_called_once_with("visible")
            state.pending = True
            app.switch()
            self.assertEqual(session.send.call_count, 1)
            state.pending = False
            state.current = 1
            app.recovering = True
            app.switch()
            self.assertEqual(session.send.call_count, 1)
            app.recovering = False
            app.switch()
            self.assertEqual(session.send.call_args.args, ("point",))
            self.assertEqual(session.send.call_count, 2)
            state.current = 7  # Old VISIBLE_CAM is not an L3 source.
            app.switch()
            self.assertEqual(session.send.call_count, 2)
            state.current = 14
            app.started = time.monotonic() - 55
            app.switch()  # No source change when too little time remains to resubscribe.
            self.assertEqual(session.send.call_count, 2)

    def test_recovery_waits_for_clean_sdk_exit_and_keeps_original_deadline(self):
        args = arguments(["--host", "127.0.0.1", "--seconds", "60", "--no-display"])
        with patch("l3_preview.PreviewSession") as factory:
            old = Mock()
            old.snapshot.return_value = PreviewState(current=1)
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
            app.request_recovery(time.monotonic(), source=1)
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
            self.assertEqual(factory.call_args.args[0].expected_source, 1)
            self.assertEqual(args.seconds, 60)
            new.start.assert_called_once()
            app.session = old
            app.recovering = True
            old.returncode = 1
            app.close = Mock()
            app.tick()
            self.assertEqual(factory.call_count, 2)  # SDK failure never retries.
            app.close.assert_called_once()

    def test_switch_readback_while_stopping_overrides_old_restart_source(self):
        args = arguments(["--host", "127.0.0.1", "--seconds", "60", "--no-display"])
        with patch("l3_preview.PreviewSession") as factory:
            old = Mock()
            old.ready = threading.Event()
            old.finished = threading.Event()
            old.lock = threading.Lock()
            old.state = PreviewState(current=1, pending=True)
            old.snapshot.side_effect = lambda: PreviewState(**vars(old.state))
            factory.return_value = old
            app = PreviewApp(args)
            receiver = Mock()
            receiver.latest_video.return_value = None
            receiver.stats.return_value = {"video_frames": 12, "video_disconnects": 1}
            app.receiver = receiver
            app.active_source = 1
            app.tick()
            old.send.assert_not_called()  # Wait for the switch ACK after video EOF.
            self.assertFalse(app.recovering)
            # Also cover the ACK arriving after a recovery has already started.
            app.request_recovery(time.monotonic())
            self.assertEqual(app.restart_source, 1)
            old.state = PreviewState(current=14)
            old.returncode = 0
            old.finished.set()
            new = Mock()
            new.finished = threading.Event()
            factory.return_value = new
            app.tick()
            self.assertEqual(factory.call_args.args[0].expected_source, 14)
            new.start.assert_called_once()

    def test_first_source_observation_does_not_restart_decoder(self):
        state = PreviewState()
        state.apply(parse_event("event=video_control action=status current=14 success=yes"))
        self.assertEqual(state.current, 14)
        self.assertEqual(state.generation, 0)
        state.apply(parse_event("event=video_control action=status current=-1 success=no"))
        state.apply(parse_event("event=video_control action=status current=14 success=yes"))
        self.assertEqual(state.generation, 2)

    def test_fast_switch_waits_for_new_tcp_and_decodes_without_reopening_ssh(self):
        args = arguments(["--host", "127.0.0.1", "--no-display", "--fast-switch", "--seconds", "60"])
        with patch("l3_preview.PreviewSession") as factory, patch("l3_preview.StreamReceiver") as receiver_factory:
            session = factory.return_value
            session.ready = threading.Event()
            session.ready.set()
            session.finished = threading.Event()
            session.lock = threading.Lock()
            session.send.return_value = True
            state = PreviewState(current=1, pending=True)
            session.state = state
            session.snapshot.side_effect = lambda: PreviewState(**vars(state))
            old = Mock()
            old.latest_video.return_value = None
            old.stats.return_value = {"video_frames": 12, "video_disconnects": 1}
            app = PreviewApp(args)
            app.receiver = old
            app.active_source = 1
            app.switch_started = time.monotonic()
            app.tick()
            session.send.assert_not_called()  # EOF may precede the source readback.

            state.apply(parse_event("event=video_control action=switch requested=14 current=14 success=yes resubscribe=yes"))
            app.tick()
            old.stop.assert_called_once()
            session.send.assert_called_once_with("resume")
            receiver_factory.assert_not_called()
            self.assertFalse(app.can_switch(state))

            state.apply(parse_event("event=fast_switch phase=video_relisten"))
            new = receiver_factory.return_value.start.return_value
            new.latest_video.return_value = None
            new.stats.return_value = {"video_disconnects": 0}
            app.tick()
            self.assertFalse(app.can_switch(state))
            new.latest_video.return_value = SimpleNamespace(sequence=1, received_monotonic_ns=time.monotonic_ns(),
                                                            image=SimpleNamespace(shape=(1080, 1920, 3)))
            app.tick()
            self.assertTrue(app.can_switch(state))
            self.assertIs(app.session, session)
            self.assertEqual(app.active_source, 14)
            self.assertEqual(app.received_frames, 12)
            factory.assert_called_once()

    def test_fast_switch_timeout_waits_for_clean_exit_and_disables_fast_retry(self):
        args = arguments(["--host", "127.0.0.1", "--no-display", "--fast-switch", "--seconds", "60"])
        with patch("l3_preview.PreviewSession") as factory:
            old = factory.return_value
            old.ready, old.finished = threading.Event(), threading.Event()
            old.lock = threading.Lock()
            old.state = PreviewState(current=14, resubscribes=1)
            old.snapshot.return_value = old.state
            app = PreviewApp(args)
            app.resubscribes = 1
            app.fast_deadline = time.monotonic() - 1
            app.tick()
            old.send.assert_called_once_with("stop")
            factory.assert_called_once()
            self.assertTrue(app.recovering)
            old.returncode = 0
            old.finished.set()
            new = Mock()
            new.finished = threading.Event()
            factory.return_value = new
            app.tick()
            self.assertEqual(factory.call_count, 2)
            self.assertFalse(factory.call_args.args[0].fast_switch)
            self.assertEqual(factory.call_args.args[0].expected_source, 14)
            new.start.assert_called_once()

    def test_yolo_modes_wait_for_fresh_frames_and_keep_the_same_source_and_port(self):
        args = arguments(["--host", "127.0.0.1", "--no-display", "--seconds", "60",
                          "--yolo-engine", "/tmp/YOLOX s.engine"])
        remote = shlex.split(ssh_command(args)[-1])
        self.assertIn("--fast-switch", remote)
        self.assertEqual(remote[remote.index("--yolo-engine") + 1], args.yolo_engine)
        with patch("l3_preview.PreviewSession") as factory, patch("l3_preview.StreamReceiver") as receivers:
            session = factory.return_value
            session.ready, session.finished = threading.Event(), threading.Event()
            session.ready.set()
            session.lock = threading.Lock()
            state = session.state = PreviewState(current=14)
            state.apply(parse_event("event=yolo phase=available"))
            session.send.return_value = True
            session.snapshot.side_effect = lambda: PreviewState(**vars(state))
            app = PreviewApp(args)
            app.toggle_yolo()
            session.send.assert_not_called()
            state.current = app.active_source = 1
            first = Mock()
            first.stats.return_value = {"video_frames": 10}
            app.receiver = first

            for target in (True, False):
                app.toggle_yolo()
                session.send.assert_called_with("yolo on" if target else "yolo off")
                state.yolo_pending = True
                mode = "on" if target else "off"
                state.apply(parse_event(f"event=yolo phase=switch target={mode}"))
                app.tick()
                session.send.assert_called_with("resume")
                self.assertEqual(app.yolo_active, not target)
                self.assertIsNone(app.receiver)
                state.apply(parse_event("event=fast_switch phase=video_relisten"))
                receiver = Mock()
                receivers.return_value.start.return_value = receiver
                receiver.latest_video.return_value = None
                receiver.stats.return_value = {"video_frames": 1, "video_disconnects": 0}
                app.tick()
                self.assertEqual(app.yolo_active, not target)
                receiver.latest_video.return_value = SimpleNamespace(sequence=1, received_monotonic_ns=time.monotonic_ns(),
                                                                    image=SimpleNamespace(shape=(1080, 1920, 3)))
                app.tick()
                self.assertEqual(app.yolo_active, target)
                self.assertFalse(state.yolo_pending)
                self.assertEqual(state.current, 1)
                self.assertEqual(receivers.call_args.kwargs["video_port"], 9102)
                self.assertEqual(app.can_switch(state), not target)
            first.stop.assert_called_once()
            factory.assert_called_once()

    def test_rejected_switch_keeps_actual_source_and_message(self):
        state = PreviewState(current=14, pending=True)
        state.apply(parse_event("event=video_control action=switch requested=1 current=14 "
                                f"success=no code={UNSUPPORTED}"))
        self.assertEqual(state.current, 14)
        self.assertFalse(state.pending)
        self.assertIn("拒绝", state.message)
        message = state.message
        state.apply(parse_event("event=video_control action=status current=14 success=yes"))
        self.assertEqual(state.message, message)
        self.assertEqual(state.generation, 0)

    def test_ack_is_not_enough_and_unknown_readback_clears_source(self):
        state = PreviewState(current=14, pending=True)
        state.apply(parse_event("event=video_control action=switch requested=1 current=14 success=yes code=0"))
        self.assertIn("未确认", state.message)
        state.apply(parse_event("event=video_control action=switch requested=1 current=-1 success=no code=1"))
        self.assertIsNone(state.current)
        self.assertEqual(state.generation, 1)
        state.apply(parse_event("event=video_control action=switch requested=1 current=1 success=yes code=0"))
        self.assertEqual(state.current, 1)
        self.assertEqual(state.generation, 2)
        self.assertIn("已确认", state.message)

    def test_failed_ack_reports_observed_change_without_claiming_success(self):
        state = PreviewState(current=14)
        state.apply(parse_event("event=video_control action=switch requested=1 current=1 success=no code=1"))
        self.assertEqual(state.current, 1)
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
