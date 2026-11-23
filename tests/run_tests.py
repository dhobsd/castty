#!/usr/bin/env python3
import fcntl
import json
import os
import pty
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASTTY = os.path.join(ROOT_DIR, "src", "castty")
UPGRADE_TOOL = os.path.join(ROOT_DIR, "tools", "castty-upgrade")
TEST_UNIT = os.path.join(ROOT_DIR, "tests", "test_unit")


def run_in_pty(args, send_seq=None, timeout=8.0, cwd=None, rows=40, cols=120,
               wait_for=None):
    master_fd, slave_fd = pty.openpty()
    fcntl.ioctl(
        slave_fd,
        termios.TIOCSWINSZ,
        struct.pack("HHHH", rows, cols, 0, 0),
    )
    # Keep slave_fd open in parent so stdin stays open after child exits.
    proc = subprocess.Popen(
        [CASTTY] + args,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        cwd=cwd,
        close_fds=True,
    )
    os.set_blocking(master_fd, False)
    start = time.monotonic()
    ready_at = None if wait_for is not None else start
    out_chunks = []
    sent_idx = 0
    send_seq = send_seq or []

    try:
        while True:
            now = time.monotonic()
            elapsed = now - start
            if ready_at is not None:
                since_ready = now - ready_at
                while sent_idx < len(send_seq) and since_ready >= send_seq[sent_idx][0]:
                    os.write(master_fd, send_seq[sent_idx][1])
                    sent_idx += 1

            rlist, _, _ = select.select([master_fd], [], [], 0.02)
            if master_fd in rlist:
                try:
                    data = os.read(master_fd, 4096)
                    if data:
                        out_chunks.append(data)
                        if ready_at is None and wait_for in b"".join(out_chunks):
                            ready_at = time.monotonic()
                except OSError:
                    pass

            if proc.poll() is not None:
                break
            if elapsed > timeout:
                proc.kill()
                proc.wait()
                stderr_data = proc.stderr.read().decode("utf-8", "replace")
                raise TimeoutError(
                    f"castty {args} hung for >{timeout}s; stderr={stderr_data!r}"
                )
    finally:
        # Drain any buffered PTY output before closing.
        for _ in range(5):
            rlist, _, _ = select.select([master_fd], [], [], 0.01)
            if master_fd not in rlist:
                break
            try:
                data = os.read(master_fd, 4096)
                if not data:
                    break
                out_chunks.append(data)
            except OSError:
                break
        stderr_data = proc.stderr.read().decode("utf-8", "replace")
        proc.stderr.close()
        os.close(master_fd)
        os.close(slave_fd)

    return proc.returncode, time.monotonic() - start, b"".join(out_chunks), stderr_data


def ffmpeg_fmt_from_soundio(fmt_desc):
    mapping = {
        "float 32-bit LE": "f32le",
        "float 32-bit BE": "f32be",
        "float 64-bit LE": "f64le",
        "float 64-bit BE": "f64be",
        "signed 32-bit LE": "s32le",
        "signed 32-bit BE": "s32be",
        "unsigned 32-bit LE": "u32le",
        "unsigned 32-bit BE": "u32be",
        "signed 24-bit LE": "s24le",
        "signed 24-bit BE": "s24be",
        "signed 16-bit LE": "s16le",
        "signed 16-bit BE": "s16be",
        "unsigned 16-bit LE": "u16le",
        "unsigned 16-bit BE": "u16be",
        "signed 8-bit": "s8",
        "unsigned 8-bit": "u8",
    }
    return mapping.get(fmt_desc, "f32le")


def find_device_from_castty(desc_substr):
    out = subprocess.check_output([CASTTY, "record", "-l"], text=True)
    fmt = None
    rate = None
    devid = None
    for line in out.splitlines():
        if desc_substr in line and "-d '" in line:
            m = re.search(r"-d '([^']+)'", line)
            if m:
                devid = m.group(1)
        elif desc_substr in line:
            m_rate = re.search(r"(\d+)Hz", line)
            m_fmt = re.search(r"format: '([^']+)'", line)
            if m_rate:
                rate = int(m_rate.group(1))
            if m_fmt:
                fmt = m_fmt.group(1)
    if not devid:
        raise RuntimeError(f"Device matching {desc_substr!r} not found in:\n{out}")
    return devid, rate, fmt


def probe_volume(ffmpeg_input_args):
    cmd = ["ffmpeg", "-hide_banner"] + ffmpeg_input_args + [
        "-af", "volumedetect", "-f", "null", "-"
    ]
    res = subprocess.run(
        cmd, stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True, check=True
    )
    mean_m = re.search(r"mean_volume:\s*([-0-9.]+)\s*dB", res.stderr)
    max_m = re.search(r"max_volume:\s*([-0-9.]+)\s*dB", res.stderr)
    mean_db = float(mean_m.group(1)) if mean_m else -999.0
    max_db = float(max_m.group(1)) if max_m else -999.0
    return mean_db, max_db


def ffprobe_stream(path):
    out = subprocess.check_output(
        [
            "ffprobe",
            "-v", "error",
            "-show_entries", "stream=codec_name,channels,sample_rate,duration",
            "-of", "json",
            path,
        ],
        text=True,
    )
    streams = json.loads(out).get("streams", [])
    return streams[0] if streams else {}


def pipewire_loopback_available():
    needed = ("pw-loopback", "paplay", "pactl", "ffmpeg", "ffprobe")
    if not all(shutil.which(cmd) for cmd in needed):
        return False
    pactl = subprocess.run(
        ["pactl", "info"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    return pactl.returncode == 0


HAS_FFMPEG = bool(shutil.which("ffmpeg") and shutil.which("ffprobe"))
HAS_LOOPBACK = pipewire_loopback_available()


class TestRecordingAndFormats(unittest.TestCase):
    def test_argument_validation(self):
        with tempfile.TemporaryDirectory(prefix="castty_args_") as d:
            cast_path = os.path.join(d, "err.cast")
            res = subprocess.run(
                [CASTTY], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
            self.assertNotEqual(res.returncode, 0)

            res = subprocess.run(
                [CASTTY, "record", "-d", "devonly", cast_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(res.returncode, 0)

            res = subprocess.run(
                [CASTTY, "record", "-a", os.path.join(d, "out.raw"), cast_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(res.returncode, 0)

    def test_rapid_pty_exit_no_hang(self):
        with tempfile.TemporaryDirectory(prefix="castty_rapid_") as d:
            for i in range(10):
                cast_path = os.path.join(d, f"fast_{i}.cast")
                rc, dt, _, err = run_in_pty(
                    ["record", "-N", "-e", "printf hello", cast_path]
                )
                self.assertEqual(rc, 0, msg=f"iter {i} err={err}")
                self.assertLess(dt, 2.0)

    def test_interactive_shell_pty_exit(self):
        with tempfile.TemporaryDirectory(prefix="castty_sh_") as d:
            cast_path = os.path.join(d, "shell.cast")
            rc, dt, out, err = run_in_pty(
                ["record", "-N", "-e", "sh", cast_path],
                send_seq=[(0.15, b"echo hi_shell\nexit\n")],
            )
            self.assertEqual(rc, 0, msg=err)
            self.assertLess(dt, 3.0)
            self.assertIn(b"hi_shell", out)

    def test_start_paused_and_ctrl_a_passthrough(self):
        with tempfile.TemporaryDirectory(prefix="castty_ctrl_") as d:
            cast_path = os.path.join(d, "ctrl.cast")
            # Send paused text, unpause (^a p), literal ^a (^a a and ^a ^a)
            # bundled in one write, split across two writes, and a single EOF (^d).
            rc, dt, _, err = run_in_pty(
                ["record", "-N", "-p", "-e", "cat", cast_path],
                wait_for=b"\x1b[H",
                send_seq=[
                    (0.05, b"paused_hidden\n"),
                    (0.15, b"\x01p"),
                    (0.25, b"visible \x01a\x01\x01!\n"),
                    (0.35, b"split\x01"),
                    (0.42, b"a_end\n"),
                    (0.50, b"\x01"),
                    (0.57, b"\x01ok\n"),
                    (0.65, b"\x04"),
                ],
            )
            self.assertEqual(rc, 0, msg=err)
            self.assertLess(dt, 3.0)
            with open(cast_path, "r", encoding="utf-8") as f:
                lines = [json.loads(line) for line in f]
            outs = "".join(ev[2] for ev in lines[1:] if ev[1] == "o")
            self.assertNotIn("paused_hidden", outs)
            self.assertIn("visible \x01\x01!", outs)
            self.assertIn("split\x01_end", outs)
            self.assertIn("\x01ok", outs)

    def test_asciicast_v1_v2_v3_and_upgrade(self):
        with tempfile.TemporaryDirectory(prefix="castty_fmt_") as d:
            title = 'my "cast"\ntitle'
            v1 = os.path.join(d, "v1.cast")
            v2 = os.path.join(d, "v2.cast")
            v3 = os.path.join(d, "v3.cast")

            rc, _, _, err = run_in_pty([
                "record", "-N", "-1", "-c", "90", "-r", "30", "-t", title,
                "-e", "sh -c 'printf hi_v1'", v1,
            ])
            self.assertEqual(rc, 0, msg=err)
            with open(v1, "r", encoding="utf-8") as f:
                d1 = json.load(f)
            self.assertEqual(d1["version"], 1)
            self.assertEqual(d1["width"], 90)
            self.assertEqual(d1["height"], 30)
            self.assertEqual(d1["title"], title)
            self.assertIsInstance(d1["duration"], (int, float))
            self.assertIn("hi_v1", "".join(ev[1] for ev in d1["stdout"]))

            rc, _, _, err = run_in_pty([
                "record", "-N", "-2", "-c", "90", "-r", "30", "-t", title,
                "-e", "sh -c 'printf hi_v2'", v2,
            ])
            self.assertEqual(rc, 0, msg=err)
            with open(v2, "r", encoding="utf-8") as f:
                lines2 = [json.loads(line) for line in f]
            self.assertEqual(lines2[0]["version"], 2)
            self.assertEqual(lines2[0]["width"], 90)
            self.assertEqual(lines2[0]["height"], 30)
            self.assertEqual(lines2[0]["title"], title)
            self.assertIn("hi_v2", "".join(ev[2] for ev in lines2[1:] if ev[1] == "o"))

            rc, _, _, err = run_in_pty([
                "record", "-N", "-3", "-c", "90", "-r", "30", "-t", title,
                "-e", "sh -c 'printf hi_v3; exit 42'", v3,
            ])
            self.assertEqual(rc, 0, msg=err)
            with open(v3, "r", encoding="utf-8") as f:
                lines3 = [json.loads(line) for line in f]
            self.assertEqual(lines3[0]["version"], 3)
            self.assertEqual(lines3[0]["term"]["cols"], 90)
            self.assertEqual(lines3[0]["term"]["rows"], 30)
            self.assertEqual(lines3[0]["title"], title)
            outs = "".join(ev[2] for ev in lines3[1:] if ev[1] == "o")
            exits = [ev[2] for ev in lines3[1:] if ev[1] == "x"]
            self.assertIn("hi_v3", outs)
            self.assertEqual(exits, ["42"])

            for src_path in (v1, v2):
                up_path = src_path + ".v3"
                subprocess.run([UPGRADE_TOOL, src_path, "-o", up_path], check=True)
                with open(up_path, "r", encoding="utf-8") as f:
                    ulines = [json.loads(line) for line in f]
                self.assertEqual(ulines[0]["version"], 3)
                self.assertEqual(ulines[0]["term"]["cols"], 90)

    def test_append_creates_and_extends_v1_v2_v3(self):
        with tempfile.TemporaryDirectory(prefix="castty_app_") as d:
            # -A on missing file creates a fresh v3 recording cleanly.
            v3 = os.path.join(d, "app_v3.cast")
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-A", "-e", "sh -c 'printf seg1'", v3],
                rows=24, cols=80,
            )
            self.assertEqual(rc, 0, msg=err)
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-A", "-e", "sh -c 'printf seg2'", v3],
                rows=30, cols=100,
            )
            self.assertEqual(rc, 0, msg=err)
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-A", "-e", "sh -c 'printf seg3'", v3],
                rows=35, cols=120,
            )
            self.assertEqual(rc, 0, msg=err)
            with open(v3, "r", encoding="utf-8") as f:
                lines3 = [json.loads(line) for line in f]
            self.assertEqual(lines3[0]["version"], 3)
            self.assertEqual(lines3[0]["term"]["cols"], 80)
            outs3 = "".join(ev[2] for ev in lines3[1:] if ev[1] == "o")
            resizes3 = [ev[2] for ev in lines3[1:] if ev[1] == "r"]
            self.assertIn("seg1", outs3)
            self.assertIn("seg2", outs3)
            self.assertIn("seg3", outs3)
            self.assertEqual(resizes3, ["100x30", "120x35"])
            self.assertEqual(len([ev for ev in lines3[1:] if ev[1] == "x"]), 3)

            # Format mismatch or corrupt file must fail without modifying file.
            with open(v3, "rb") as f:
                before_bytes = f.read()
            res = subprocess.run(
                [CASTTY, "record", "-N", "-A", "-2", v3],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(res.returncode, 0)
            with open(v3, "rb") as f:
                self.assertEqual(f.read(), before_bytes)

            bad = os.path.join(d, "bad.cast")
            with open(bad, "wb") as f:
                f.write(b"not a cast file")
            res = subprocess.run(
                [CASTTY, "record", "-N", "-A", bad],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertNotEqual(res.returncode, 0)
            with open(bad, "rb") as f:
                self.assertEqual(f.read(), b"not a cast file")

            # v2 append preserves monotonic timestamps and emits resize.
            v2 = os.path.join(d, "app_v2.cast")
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-2", "-e", "sh -c 'printf v2a; sleep 0.08'", v2],
                rows=24, cols=80,
            )
            self.assertEqual(rc, 0, msg=err)
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-A", "-e", "sh -c 'printf v2b'", v2],
                rows=32, cols=110,
            )
            self.assertEqual(rc, 0, msg=err)
            with open(v2, "r", encoding="utf-8") as f:
                lines2 = [json.loads(line) for line in f]
            self.assertEqual(lines2[0]["version"], 2)
            ts2 = [ev[0] for ev in lines2[1:]]
            self.assertTrue(all(ts2[i] >= ts2[i - 1] for i in range(1, len(ts2))))
            self.assertIn("110x32", [ev[2] for ev in lines2[1:] if ev[1] == "r"])
            outs2 = "".join(ev[2] for ev in lines2[1:] if ev[1] == "o")
            self.assertIn("v2a", outs2)
            self.assertIn("v2b", outs2)

            # v1 append (both castty-generated and external duration-first v1).
            v1 = os.path.join(d, "app_v1.cast")
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-1", "-e", "sh -c 'printf v1a; sleep 0.1'", v1],
                rows=24, cols=80,
            )
            self.assertEqual(rc, 0, msg=err)
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-A", "-e", "sh -c 'printf v1b'", v1],
                rows=24, cols=80,
            )
            self.assertEqual(rc, 0, msg=err)
            with open(v1, "r", encoding="utf-8") as f:
                d1 = json.load(f)
            self.assertEqual(d1["version"], 1)
            outs1 = "".join(ev[1] for ev in d1["stdout"])
            self.assertIn("v1a", outs1)
            self.assertIn("v1b", outs1)
            self.assertGreaterEqual(d1["duration"], 0.08)

            v1_ext = os.path.join(d, "ext_v1.cast")
            with open(v1_ext, "w", encoding="utf-8") as f:
                f.write('{"duration": 0.4, "version": 1, "width": 80, "height": 24, "stdout": [[0.1, "ext1"]]}\n')
            rc, _, _, err = run_in_pty(
                ["record", "-N", "-A", "-e", "sh -c 'printf ext2'", v1_ext],
                rows=24, cols=80,
            )
            self.assertEqual(rc, 0, msg=err)
            with open(v1_ext, "r", encoding="utf-8") as f:
                d1e = json.load(f)
            outs1e = "".join(ev[1] for ev in d1e["stdout"])
            self.assertIn("ext1", outs1e)
            self.assertIn("ext2", outs1e)
            self.assertGreaterEqual(d1e["duration"], 0.4)


@unittest.skipUnless(HAS_FFMPEG, "requires ffmpeg/ffprobe")
class TestLameDecoderVolumes(unittest.TestCase):
    def test_all_soundio_formats_decode_audible_tone(self):
        with tempfile.TemporaryDirectory(prefix="castty_lame_fmts_") as d:
            subprocess.run([TEST_UNIT, "--dump-lame-formats", d], check=True)
            mp3s = sorted(f for f in os.listdir(d) if f.endswith(".mp3"))
            self.assertEqual(len(mp3s), 18)
            for name in mp3s:
                path = os.path.join(d, name)
                info = ffprobe_stream(path)
                self.assertEqual(info.get("codec_name"), "mp3", msg=name)
                mean_db, max_db = probe_volume(["-i", path])
                self.assertGreater(mean_db, -15.0, msg=f"{name} too quiet: {mean_db}")
                self.assertLess(mean_db, -10.0, msg=f"{name} unexpected gain: {mean_db}")
                self.assertGreater(max_db, -11.0, msg=f"{name} max too quiet: {max_db}")


@unittest.skipUnless(HAS_LOOPBACK, "requires PipeWire pw-loopback, paplay, pactl, and ffmpeg")
class TestAudioLoopback(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmpdir = tempfile.TemporaryDirectory(prefix="castty_audio_loop_")
        cls.loopbacks = [
            subprocess.Popen(
                [
                    "pw-loopback", "-c", "2",
                    "--capture-props=media.class=Audio/Sink node.name=castty_t_lsink_st node.description=castty_t_lsink_st",
                    "--playback-props=media.class=Audio/Source node.name=castty_t_lmic_st node.description=CasttyTestStereo",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ),
            subprocess.Popen(
                [
                    "pw-loopback", "-c", "1", "-m", "[ MONO ]",
                    "--capture-props=media.class=Audio/Sink node.name=castty_t_lsink_mo node.description=castty_t_lsink_mo",
                    "--playback-props=media.class=Audio/Source node.name=castty_t_lmic_mo node.description=CasttyTestMono",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ),
        ]
        time.sleep(0.8)
        cls.st_dev, cls.st_rate, cls.st_fmt = find_device_from_castty("CasttyTestStereo")
        cls.mo_dev, cls.mo_rate, cls.mo_fmt = find_device_from_castty("CasttyTestMono")

        cls.tone_wav = os.path.join(cls.tmpdir.name, "tone440.wav")
        subprocess.run(
            [
                "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                "-f", "lavfi",
                "-i", "sine=frequency=440:sample_rate=48000:duration=4,volume=4.0",
                "-ac", "2", cls.tone_wav,
            ],
            check=True,
        )
        cls.ref_mean, cls.ref_max = probe_volume(["-i", cls.tone_wav])

    @classmethod
    def tearDownClass(cls):
        for proc in getattr(cls, "loopbacks", []):
            proc.terminate()
            proc.wait()
        if hasattr(cls, "tmpdir"):
            cls.tmpdir.cleanup()

    def test_start_paused_audio_exit_no_segfault(self):
        out_audio = os.path.join(self.tmpdir.name, "paused_audio.raw")
        cast_path = os.path.join(self.tmpdir.name, "paused_audio.cast")
        rc, dt, _, err = run_in_pty([
            "record", "-N", "-p", "-d", self.st_dev, "-a", out_audio,
            "-e", "printf paused_audio", cast_path,
        ])
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")
        self.assertLess(dt, 2.0)

    def test_stereo_raw_pcm_capture(self):
        out_raw = os.path.join(self.tmpdir.name, "stereo.raw")
        cast_path = os.path.join(self.tmpdir.name, "stereo_raw.cast")
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, dt, _, err = run_in_pty([
                "record", "-N", "-d", self.st_dev, "-a", out_raw,
                "-e", "sh -c 'echo start; sleep 1.8; echo end'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")
        self.assertLess(dt, 4.0)
        self.assertGreater(os.path.getsize(out_raw), 100000)

        ff_fmt = ffmpeg_fmt_from_soundio(self.st_fmt)
        mean_db, max_db = probe_volume(
            ["-f", ff_fmt, "-ar", str(self.st_rate), "-ac", "2", "-i", out_raw]
        )
        self.assertAlmostEqual(mean_db, self.ref_mean, delta=2.0)
        self.assertAlmostEqual(max_db, self.ref_max, delta=2.0)

    def test_stereo_mp3_capture(self):
        out_mp3 = os.path.join(self.tmpdir.name, "stereo.mp3")
        cast_path = os.path.join(self.tmpdir.name, "stereo_mp3.cast")
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, dt, _, err = run_in_pty([
                "record", "-N", "-m", "-d", self.st_dev, "-a", out_mp3,
                "-e", "sh -c 'echo start; sleep 2.0; echo end'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")
        self.assertLess(dt, 4.0)

        info = ffprobe_stream(out_mp3)
        self.assertEqual(info.get("codec_name"), "mp3")
        self.assertEqual(info.get("channels"), 2)
        self.assertGreater(float(info.get("duration", 0)), 1.0)
        mean_db, max_db = probe_volume(["-i", out_mp3])
        self.assertAlmostEqual(mean_db, self.ref_mean, delta=2.0)
        self.assertAlmostEqual(max_db, self.ref_max, delta=2.0)

    def test_mono_mp3_capture(self):
        out_mp3 = os.path.join(self.tmpdir.name, "mono.mp3")
        cast_path = os.path.join(self.tmpdir.name, "mono_mp3.cast")
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_mo", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, dt, _, err = run_in_pty([
                "record", "-N", "-m", "-d", self.mo_dev, "-a", out_mp3,
                "-e", "sh -c 'echo start; sleep 2.0; echo end'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")
        self.assertLess(dt, 4.0)

        info = ffprobe_stream(out_mp3)
        self.assertEqual(info.get("codec_name"), "mp3")
        self.assertEqual(info.get("channels"), 1)
        self.assertGreater(float(info.get("duration", 0)), 1.0)
        mean_db, max_db = probe_volume(["-i", out_mp3])
        self.assertAlmostEqual(mean_db, self.ref_mean, delta=2.0)
        self.assertAlmostEqual(max_db, self.ref_max, delta=2.0)

    def test_mute_silences_audio(self):
        out_mp3 = os.path.join(self.tmpdir.name, "muted.mp3")
        cast_path = os.path.join(self.tmpdir.name, "muted.cast")
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, dt, _, err = run_in_pty(
                [
                    "record", "-N", "-m", "-d", self.st_dev, "-a", out_mp3,
                    "-e", "sh -c 'echo start; sleep 1.5; echo end'", cast_path,
                ],
                send_seq=[(0.05, b"\x01m")],
            )
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")
        self.assertLess(dt, 4.0)
        _, max_db = probe_volume(["-ss", "0.3", "-i", out_mp3])
        self.assertLess(max_db, -40.0)

    def test_pause_resume_and_mute_controls(self):
        out_mp3 = os.path.join(self.tmpdir.name, "ctrl.mp3")
        cast_path = os.path.join(self.tmpdir.name, "ctrl_audio.cast")
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, dt, _, err = run_in_pty(
                [
                    "record", "-N", "-m", "-d", self.st_dev, "-a", out_mp3,
                    "-e", "sh -c 'echo start; sleep 2.4; echo end'", cast_path,
                ],
                send_seq=[
                    (0.4, b"\x01m"),
                    (0.7, b"\x01m"),
                    (1.0, b"\x01p"),
                    (1.5, b"\x01p"),
                ],
            )
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")
        self.assertLess(dt, 5.0)
        info = ffprobe_stream(out_mp3)
        self.assertEqual(info.get("codec_name"), "mp3")
        self.assertGreater(float(info.get("duration", 0)), 1.0)
        _, max_db = probe_volume(["-i", out_mp3])
        self.assertGreater(max_db, -15.0)

    def _max_step_around(self, ffmpeg_input_args, center_sec):
        cmd = (
            ["ffmpeg", "-hide_banner", "-loglevel", "error"]
            + ffmpeg_input_args
            + ["-ss", f"{max(center_sec - 0.35, 0.0):.3f}", "-t", "0.70",
               "-f", "f32le", "-ac", "1", "-"]
        )
        raw = subprocess.check_output(cmd)
        samples = struct.unpack(f"<{len(raw) // 4}f", raw)
        self.assertGreater(len(samples), 1000)
        return max(abs(samples[i] - samples[i - 1]) for i in range(1, len(samples)))

    def test_append_raw_pcm_seamless_and_synced(self):
        out_raw = os.path.join(self.tmpdir.name, "append.raw")
        cast_path = os.path.join(self.tmpdir.name, "append_raw.cast")
        ff_fmt = ffmpeg_fmt_from_soundio(self.st_fmt)

        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, _, _, err = run_in_pty([
                "record", "-N", "-d", self.st_dev, "-a", out_raw,
                "-e", "sh -c 'echo p1; sleep 1.2; echo p1done'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)

        with open(cast_path, "r", encoding="utf-8") as f:
            dur1 = sum(max(ev[0], 0.0) for ev in (json.loads(l) for l in f.readlines()[1:]))

        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, _, _, err = run_in_pty([
                "record", "-N", "-A", "-d", self.st_dev, "-a", out_raw,
                "-e", "sh -c 'echo p2; sleep 1.2; echo p2done'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")

        # Verify both halves have strong signal, continuity (no pop), and synced cast duration.
        mean1, _ = probe_volume([
            "-f", ff_fmt, "-ar", str(self.st_rate), "-ac", "2",
            "-t", "0.9", "-i", out_raw,
        ])
        mean2, _ = probe_volume([
            "-f", ff_fmt, "-ar", str(self.st_rate), "-ac", "2",
            "-ss", f"{dur1 + 0.25:.3f}", "-t", "0.8", "-i", out_raw,
        ])
        self.assertAlmostEqual(mean1, self.ref_mean, delta=2.5)
        self.assertAlmostEqual(mean2, self.ref_mean, delta=2.5)

        max_jump = self._max_step_around(
            ["-f", ff_fmt, "-ar", str(self.st_rate), "-ac", "2", "-i", out_raw],
            dur1,
        )
        self.assertLess(max_jump, 0.08, msg=f"Pop/step jump detected at raw splice: {max_jump}")

    def test_append_mp3_validation_and_stereo_plus_mono_splice(self):
        # Invalid/non-MP3 file must fail cleanly without altering cast or audio.
        bad_mp3 = os.path.join(self.tmpdir.name, "not_mp3.mp3")
        bad_cast = os.path.join(self.tmpdir.name, "not_mp3.cast")
        with open(bad_mp3, "wb") as f:
            f.write(b"NOT_AN_MP3_FILE" * 64)
        res = subprocess.run(
            [CASTTY, "record", "-N", "-A", "-m", "-d", self.st_dev,
             "-a", bad_mp3, bad_cast],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertFalse(os.path.exists(bad_cast))

        # Session 1: stereo MP3 capture.
        out_mp3 = os.path.join(self.tmpdir.name, "append.mp3")
        cast_path = os.path.join(self.tmpdir.name, "append_mp3.cast")
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, _, _, err = run_in_pty([
                "record", "-N", "-m", "-d", self.st_dev, "-a", out_mp3,
                "-e", "sh -c 'echo s1; sleep 1.2; echo s1done'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        info1 = ffprobe_stream(out_mp3)
        dur1 = float(info1["duration"])

        # -A -p exiting without unpausing must leave existing MP3 intact.
        size_before = os.path.getsize(out_mp3)
        rc, _, _, err = run_in_pty([
            "record", "-N", "-A", "-p", "-m", "-d", self.st_dev, "-a", out_mp3,
            "-e", "printf noop", cast_path,
        ])
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(os.path.getsize(out_mp3), size_before)

        # Session 2: append using MONO mic onto existing STEREO MP3.
        play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_mo", self.tone_wav])
        try:
            time.sleep(0.2)
            rc, _, _, err = run_in_pty([
                "record", "-N", "-A", "-m", "-d", self.mo_dev, "-a", out_mp3,
                "-e", "sh -c 'echo s2; sleep 1.2; echo s2done'", cast_path,
            ])
        finally:
            play.wait()
        self.assertEqual(rc, 0, msg=err)
        self.assertEqual(err, "")

        info2 = ffprobe_stream(out_mp3)
        self.assertEqual(info2.get("codec_name"), "mp3")
        self.assertEqual(info2.get("channels"), 2)
        dur2 = float(info2["duration"])
        self.assertGreater(dur2, dur1 + 0.9)

        with open(cast_path, "r", encoding="utf-8") as f:
            lines = [json.loads(l) for l in f]
        cast_dur = sum(max(ev[0], 0.0) for ev in lines[1:])
        outs = "".join(ev[2] for ev in lines[1:] if ev[1] == "o")
        self.assertIn("s1", outs)
        self.assertIn("s2", outs)
        self.assertAlmostEqual(cast_dur, dur2, delta=0.25)

        mean1, _ = probe_volume(["-t", f"{max(dur1 - 0.15, 0.5):.3f}", "-i", out_mp3])
        mean2, _ = probe_volume(["-ss", f"{dur1 + 0.25:.3f}", "-t", "0.8", "-i", out_mp3])
        self.assertAlmostEqual(mean1, self.ref_mean, delta=2.5)
        self.assertAlmostEqual(mean2, self.ref_mean, delta=2.5)

        max_jump = self._max_step_around(["-i", out_mp3], dur1)
        self.assertLess(max_jump, 0.08, msg=f"Pop/step jump detected at MP3 splice: {max_jump}")

    def test_sndfile_formats_capture(self):
        # Verify unsupported -F fails cleanly.
        rc, _, _, err = run_in_pty([
            "record", "-N", "-F", "notasoundfmt",
            os.path.join(self.tmpdir.name, "badfmt.cast"),
        ])
        self.assertNotEqual(rc, 0)
        self.assertIn("unsupported audio format", err)

        cases = [
            ("wav", "pcm_s16le", None, []),
            ("flac", "flac", None, []),
            ("opus", "opus", "48000", []),
            ("ogg", "vorbis", None, []),
            ("m4a", "aac", None, []),
            # Explicit -F overrides filename extension cleanly.
            ("bin", "opus", "48000", ["-F", "opus"]),
            ("raw", "aac", None, ["-F", "m4a"]),
        ]
        for idx, (ext, exp_codec, exp_rate, extra_flags) in enumerate(cases):
            out_audio = os.path.join(self.tmpdir.name, f"cap_{idx}.{ext}")
            cast_path = os.path.join(self.tmpdir.name, f"cap_{idx}.cast")
            play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
            try:
                time.sleep(0.2)
                rc, _, _, err = run_in_pty([
                    "record", "-N", *extra_flags, "-d", self.st_dev,
                    "-a", out_audio,
                    "-e", "sh -c 'echo start; sleep 1.1; echo end'",
                    cast_path,
                ])
            finally:
                play.wait()
            self.assertEqual(rc, 0, msg=f"{ext}: {err}")
            info = ffprobe_stream(out_audio)
            self.assertEqual(info.get("codec_name"), exp_codec, msg=f"{ext}: {info}")
            if exp_rate is not None:
                self.assertEqual(info.get("sample_rate"), exp_rate, msg=f"{ext}: {info}")
            mean_db, _ = probe_volume(["-i", out_audio])
            self.assertAlmostEqual(mean_db, self.ref_mean, delta=2.5, msg=ext)

    def test_append_wav_opus_and_m4a_splice(self):
        bad_wav = os.path.join(self.tmpdir.name, "not_wav.wav")
        bad_cast = os.path.join(self.tmpdir.name, "not_wav.cast")
        with open(bad_wav, "wb") as f:
            f.write(b"not really a wav file\n")
        rc, _, _, err = run_in_pty([
            "record", "-N", "-A", "-d", self.st_dev, "-a", bad_wav, bad_cast,
        ])
        self.assertNotEqual(rc, 0)
        self.assertIn("not a valid wav", err)

        bad_m4a = os.path.join(self.tmpdir.name, "not_m4a.m4a")
        with open(bad_m4a, "wb") as f:
            f.write(b"not really an m4a file\n")
        rc, _, _, err = run_in_pty([
            "record", "-N", "-A", "-d", self.st_dev, "-a", bad_m4a, bad_cast,
        ])
        self.assertNotEqual(rc, 0)
        self.assertIn("not a valid m4a", err)

        for ext, exp_codec in [("wav", "pcm_s16le"), ("opus", "opus"), ("m4a", "aac")]:
            out_audio = os.path.join(self.tmpdir.name, f"append_{ext}.{ext}")
            cast_path = os.path.join(self.tmpdir.name, f"append_{ext}.cast")

            play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_st", self.tone_wav])
            try:
                time.sleep(0.2)
                rc, _, _, err = run_in_pty([
                    "record", "-N", "-d", self.st_dev, "-a", out_audio,
                    "-e", "sh -c 'echo s1; sleep 1.1'", cast_path,
                ])
            finally:
                play.wait()
            self.assertEqual(rc, 0, msg=f"{ext} s1: {err}")
            self.assertEqual(err, "", msg=f"{ext} s1 stderr: {err}")
            dur1 = float(ffprobe_stream(out_audio)["duration"])

            # Append with MONO source onto STEREO file across formats.
            play = subprocess.Popen(["paplay", "-d", "castty_t_lsink_mo", self.tone_wav])
            try:
                time.sleep(0.2)
                rc, _, _, err = run_in_pty([
                    "record", "-N", "-A", "-d", self.mo_dev, "-a", out_audio,
                    "-e", "sh -c 'echo s2; sleep 1.2'", cast_path,
                ])
            finally:
                play.wait()
            self.assertEqual(rc, 0, msg=f"{ext} s2: {err}")
            self.assertEqual(err, "", msg=f"{ext} s2 stderr: {err}")
            info2 = ffprobe_stream(out_audio)
            self.assertEqual(info2.get("codec_name"), exp_codec)
            self.assertEqual(info2.get("channels"), 2)
            dur2 = float(info2["duration"])
            self.assertGreater(dur2, dur1 + 0.9)

            with open(cast_path, "r", encoding="utf-8") as f:
                lines = [json.loads(l) for l in f]
            cast_dur = sum(max(ev[0], 0.0) for ev in lines[1:])
            self.assertAlmostEqual(cast_dur, dur2, delta=0.25, msg=ext)

            mean1, _ = probe_volume(["-t", f"{max(dur1 - 0.15, 0.5):.3f}", "-i", out_audio])
            mean2, _ = probe_volume(["-ss", f"{dur1 + 0.25:.3f}", "-t", "0.8", "-i", out_audio])
            self.assertAlmostEqual(mean1, self.ref_mean, delta=2.5, msg=ext)
            self.assertAlmostEqual(mean2, self.ref_mean, delta=2.5, msg=ext)

            max_jump = self._max_step_around(["-i", out_audio], dur1)
            self.assertLess(max_jump, 0.08, msg=f"Pop/step jump in {ext}: {max_jump}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
