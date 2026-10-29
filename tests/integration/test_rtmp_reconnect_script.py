import argparse
from contextlib import contextmanager, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "test_rtmp_reconnect.py"
SPEC = importlib.util.spec_from_file_location("reconnect_script", SCRIPT)
reconnect = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reconnect)


class ReconnectScriptTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="zlm-reconnect-test-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.source = self.work / "source.mp4"
        self.source.touch()
        self.args = argparse.Namespace(
            input=self.source, url="rtmp://127.0.0.1:1935/live/test",
            mode="live", push_seconds=2, final_seconds=None,
            disconnect_seconds=1, reconnect_count=2, finalize_seconds=0,
            progress_timeout_seconds=20, record_directory=None,
            output_directory=self.work / "report",
        )

    def run_publisher(self, is_last, seconds, return_code=0):
        commands = []
        process = Mock()
        process.poll.return_value = return_code

        @contextmanager
        def encoder(ffmpeg, arguments, prefix):
            commands.append(arguments)
            Path(str(prefix) + ".progress.log").write_text(
                f"out_time_us={int(seconds * 1_000_000)}\nprogress=end\n"
            )
            yield process

        with patch.object(reconnect, "encoder_process", encoder):
            sent = reconnect.publish_segment("ffmpeg", self.args, 6, is_last, self.work / "segment")
        return sent, commands[0]

    def run_main(self, duration=7.4, startup_delays=(0, 0, 0)):
        offsets = []
        elapsed = [0.0]

        def publish(ffmpeg, args, source_offset, is_last, prefix):
            offsets.append(source_offset)
            remaining = duration - source_offset
            seconds = min(remaining, args.final_seconds or remaining) if is_last else args.push_seconds
            elapsed[0] += seconds + startup_delays[len(offsets) - 1]
            return seconds

        def sleep(seconds):
            elapsed[0] += seconds

        with patch.object(reconnect, "parse_arguments", return_value=self.args), \
                patch.object(reconnect.shutil, "which", side_effect=lambda name: name), \
                patch.object(reconnect, "probe_duration", return_value=duration), \
                patch.object(reconnect, "publish_segment", side_effect=publish) as publisher, \
                patch.object(reconnect, "probe_recordings", return_value=[]) as recordings, \
                patch.object(reconnect.time, "monotonic", side_effect=lambda: elapsed[0]), \
                patch.object(reconnect.time, "sleep", side_effect=sleep), \
                redirect_stdout(io.StringIO()), patch("sys.stderr", new_callable=io.StringIO):
            return_code = reconnect.main()
        report = json.loads((self.args.output_directory / "report.json").read_text())
        return return_code, report, offsets, publisher, recordings

    def test_final_segment_has_no_duration_limit_and_accepts_short_tail(self):
        sent, command = self.run_publisher(True, 0.4)
        self.assertEqual(sent, 0.4)
        self.assertNotIn("-t", command)

    def test_explicit_final_segment_limit_is_retained(self):
        self.args.final_seconds = 1
        _, command = self.run_publisher(True, 1)
        self.assertEqual(command[command.index("-t") + 1], 1)

    def test_early_eof_before_forced_disconnect_is_an_error(self):
        with self.assertRaisesRegex(RuntimeError, "提前退出"):
            self.run_publisher(False, 1)

    def test_failed_publisher_is_an_error_even_with_progress(self):
        with self.assertRaisesRegex(RuntimeError, "推流失败"):
            self.run_publisher(True, 2, return_code=1)

    def test_each_live_gap_is_counted_once_and_tail_reaches_eof(self):
        return_code, report, offsets, _, recordings = self.run_main()
        self.assertEqual(return_code, 0)
        self.assertEqual(offsets, [0, 3, 6])
        self.assertEqual(report["disconnect_wait_seconds"], 2)
        self.assertEqual(report["source_consumed_seconds"], 7.4)
        self.assertEqual(report["unpublished_source_seconds"], 0)
        self.assertTrue(report["source_end_reached"])
        self.assertEqual(report["recording_status"], "not_found")
        self.assertIsNone(report["recording_minus_source_seconds"])
        self.assertIn("云上传", report["recording_note"])
        searched = [call.args[1] for call in recordings.call_args_list]
        self.assertIn(SCRIPT.parents[1] / "release/linux/Debug/www/record/live/test", searched)

    def test_resume_preserves_source_content_during_disconnect(self):
        self.args.mode = "resume"
        _, report, offsets, _, _ = self.run_main()
        self.assertEqual(offsets, [0, 2, 4])
        self.assertTrue(report["source_end_reached"])

    def test_live_clock_compensates_for_repeated_publisher_startup(self):
        return_code, report, offsets, _, _ = self.run_main(duration=10, startup_delays=(0.6, 0.7, 0.65))
        self.assertEqual(return_code, 0)
        for actual, expected in zip(offsets, (0, 3.6, 7.4)):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual([segment["estimated_startup_seconds"] for segment in report["segments"]], [0, 0.6, 0.7])
        self.assertTrue(report["source_end_reached"])
        self.assertEqual(report["unpublished_source_seconds"], 0)
        self.assertAlmostEqual(report["push_wall_seconds"] - 0.6, 10, delta=0.1)

    def test_resume_does_not_skip_content_to_compensate_startup(self):
        self.args.mode = "resume"
        _, report, offsets, _, _ = self.run_main(duration=10, startup_delays=(0.6, 0.7, 0.65))
        self.assertEqual(offsets, [0, 2, 4])
        self.assertEqual([segment["estimated_startup_seconds"] for segment in report["segments"]], [0, 0, 0])
        self.assertTrue(report["source_end_reached"])

    def test_live_clock_reports_source_exhaustion_from_startup_overhead(self):
        return_code, report, offsets, _, _ = self.run_main(duration=7, startup_delays=(2, 2, 2))
        self.assertEqual(return_code, 1)
        self.assertIn("源视频已耗尽", report["error"])
        self.assertFalse(report["source_end_reached"])
        self.assertEqual(offsets, [0, 5])

    def test_fixed_final_segment_reports_unpublished_tail(self):
        self.args.final_seconds = 1
        _, report, _, _, _ = self.run_main()
        self.assertFalse(report["source_end_reached"])
        self.assertEqual(report["unpublished_source_seconds"], 0.4)

    def test_insufficient_source_fails_before_publishing(self):
        return_code, report, _, publisher, _ = self.run_main(duration=6)
        self.assertEqual(return_code, 1)
        self.assertIn("不足", report["error"])
        publisher.assert_not_called()

    def test_zero_reconnects_and_custom_record_directory(self):
        self.args.reconnect_count = 0
        self.args.record_directory = self.work / "custom-recordings"
        _, report, offsets, _, recordings = self.run_main()
        self.assertEqual(offsets, [0])
        self.assertEqual(report["disconnect_wait_seconds"], 0)
        self.assertEqual(recordings.call_count, 1)
        self.assertEqual(recordings.call_args.args[1], self.args.record_directory)

    def test_invalid_source_duration_is_rejected(self):
        result = subprocess.CompletedProcess([], 0, stdout="nan\n", stderr="")
        with patch.object(reconnect.subprocess, "run", return_value=result), \
                self.assertRaisesRegex(RuntimeError, "时长无效"):
            reconnect.probe_duration("ffprobe", self.source)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "需要 FFmpeg")
    def test_real_ffmpeg_reaches_eof_or_explicit_limit(self):
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=25",
             "-t", "3", "-c:v", "libx264", "-preset", "ultrafast", str(self.source)],
            check=True, capture_output=True, timeout=30,
        )
        original_encoder = reconnect.encoder_process
        self.args.push_seconds = 20

        for limit, expected in ((None, 2), (1, 1)):
            with self.subTest(final_seconds=limit):
                self.args.final_seconds = limit
                destination = self.work / f"final-{limit}.flv"

                @contextmanager
                def local_encoder(ffmpeg, arguments, prefix):
                    command = list(arguments)
                    command.remove("-re")
                    command[-1] = str(destination)
                    with original_encoder(ffmpeg, command, prefix) as process:
                        yield process

                with patch.object(reconnect, "encoder_process", local_encoder):
                    reconnect.publish_segment("ffmpeg", self.args, 1, True, self.work / f"segment-{limit}")
                self.assertAlmostEqual(reconnect.probe_duration("ffprobe", destination), expected, delta=0.1)


if __name__ == "__main__":
    unittest.main()
