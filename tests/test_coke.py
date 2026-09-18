"""Behavioral tests; power calls are fake, process/terminal/file APIs are real."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
PYTHON = sys.executable


class CokeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="coke-test-")
        self.root = Path(self.temp.name)
        self.trace = self.root / "trace"
        self.env = dict(os.environ, COKE_TEST_CONTROL=str(self.root / "control"),
                        COKE_TEST_SESSIONS=str(self.root / "sessions"),
                        COKE_TEST_TRACE=str(self.trace))
        self.children = []

    def tearDown(self):
        for process in self.children:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)
        self.temp.cleanup()

    def run_coke(self, *args, options=None, **kwargs):
        env = dict(self.env, **(options or {}))
        return subprocess.run([BINARY, *args], env=env, capture_output=True,
                              text=True, timeout=10, **kwargs)

    def spawn(self, *args, options=None):
        process = subprocess.Popen([BINARY, *args],
            env=dict(self.env, **(options or {})), stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        self.children.append(process)
        return process

    def events(self):
        if not self.trace.exists():
            return []
        return [line.split()[1] for line in self.trace.read_text().splitlines()]

    def ready(self, process):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.trace.exists() and f"{process.pid} SET\n" in self.trace.read_text():
                return
            if process.poll() is not None:
                self.fail(process.communicate())
            time.sleep(0.01)
        self.fail("Coke did not start")

    def test_command_arguments_streams_and_environment(self):
        code = "import os,sys; print(repr(sys.argv[1:])); print(os.environ['EXAMPLE']); print('err',file=sys.stderr)"
        result = self.run_coke("--", PYTHON, "-c", code, "a b", "", "$(false)", "--",
                               options={"EXAMPLE": "inherited"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "['a b', '', '$(false)', '--']\ninherited\n")
        self.assertIn("err\n", result.stderr)
        self.assertEqual(self.events(), ["IDLE_ON", "SET", "CLEAR", "IDLE_OFF"])

    def test_stdin_and_working_directory(self):
        result = self.run_coke("--", PYTHON, "-c",
            "import os,sys; print(os.getcwd()); print(sys.stdin.read())",
            input="hello", cwd=self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"{self.root.resolve()}\nhello\n")

    def test_child_exit_code(self):
        result = self.run_coke("--", "/bin/sh", "-c", "exit 37")
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertIn("CLEAR", self.events())

    def test_child_signal_exit_code(self):
        result = self.run_coke("--", "/bin/sh", "-c", "kill -TERM $$")
        self.assertEqual(result.returncode, 143, result.stderr)
        self.assertIn("CLEAR", self.events())

    def test_killed_command_still_cleans_up(self):
        result = self.run_coke("--", "/bin/sh", "-c", "kill -KILL $$")
        self.assertEqual(result.returncode, 137, result.stderr)
        self.assertIn("CLEAR", self.events())

    def test_nested_sessions_clear_only_after_outer_command(self):
        result = self.run_coke("--", BINARY, "--", "/usr/bin/true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events().count("SET"), 2)
        self.assertEqual(self.events().count("CLEAR"), 1)
        self.assertEqual(self.events().count("IDLE_OFF"), 2)

    def test_exec_errors(self):
        result = self.run_coke("--", "/this-command-does-not-exist")
        self.assertEqual(result.returncode, 127, result.stderr)
        self.assertIn("CLEAR", self.events())
        script = self.root / "not-executable"
        script.write_text("#!/bin/sh\nexit 0\n")
        result = self.run_coke("--", str(script))
        self.assertEqual(result.returncode, 126, result.stderr)

    def test_no_command_or_extra_arguments(self):
        for args in [("--",), ("off", "extra"), ("status", "extra")]:
            result = self.run_coke(*args)
            self.assertEqual(result.returncode, 2)
        self.assertEqual(self.events(), [])

    def test_startup_failures_do_not_run_command(self):
        marker = self.root / "ran"
        for option in ["COKE_TEST_FAIL_IDLE", "COKE_TEST_FAIL_SET"]:
            result = self.run_coke("--", "/usr/bin/touch", str(marker),
                                   options={option: "1"})
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("override requested", result.stderr)
            self.assertFalse(marker.exists())
        self.assertNotIn("CLEAR", self.events())
        self.assertEqual(self.events().count("IDLE_OFF"), 1)

    def test_failed_cleanup_is_not_success(self):
        result = self.run_coke("--", "/usr/bin/true",
                               options={"COKE_TEST_FAIL_CLEAR": "1"})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("retry coke off", result.stderr)
        self.assertNotIn("override cleared", result.stderr)
        result = self.run_coke("--", "/bin/sh", "-c", "exit 37",
                               options={"COKE_TEST_FAIL_CLEAR": "1"})
        self.assertEqual(result.returncode, 37, result.stderr)

    def test_failed_idle_release(self):
        result = self.run_coke("--", "/usr/bin/true",
                               options={"COKE_TEST_FAIL_IDLE_RELEASE": "1"})
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_failed_retry_is_reported_when_command_finishes(self):
        result = self.run_coke("--", "/bin/sleep", "1.3",
                               options={"COKE_TEST_FAIL_RETRY": "1"})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("continuing to retry", result.stderr)
        self.assertIn("CLEAR", self.events())

    def test_standalone_retry_failure_exits_and_restores(self):
        result = self.run_coke(options={"COKE_TEST_FAIL_RETRY": "1"})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("protection failed; stopping", result.stderr)
        self.assertEqual(self.events().count("CLEAR"), 1)

    def test_off_and_exit_preserve_native_policy(self):
        for options in [{"COKE_TEST_EXTERNAL": "1", "COKE_TEST_AC": "1"},
                        {"COKE_TEST_LID_ASSERTION": "1"},
                        {"COKE_TEST_HOTPLUG_ASSERTION": "1"}]:
            for args in [("off",), ("--", "/usr/bin/true")]:
                result = self.run_coke(*args, options=options)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("override left intact", result.stderr)
        self.assertNotIn("CLEAR", self.events())

    def test_inactive_lid_assertion_does_not_preserve_override(self):
        result = self.run_coke("off", options={"COKE_TEST_LID_ASSERTION": "1",
                                             "COKE_TEST_INACTIVE_ASSERTION": "1"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CLEAR", self.events())

    def test_unknown_policy_is_not_cleared(self):
        for option in ["COKE_TEST_UNKNOWN_DISPLAY", "COKE_TEST_UNKNOWN_POWER",
                       "COKE_TEST_UNKNOWN_ASSERTIONS"]:
            result = self.run_coke("off", options={option: "1"})
            self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("CLEAR", self.events())

    def test_status_missing_property_is_an_error(self):
        result = self.run_coke("status", options={"COKE_TEST_MISSING_PROPERTY": "1"})
        self.assertEqual(result.returncode, 1)

    def test_off_refuses_active_sessions_and_status_finds_them(self):
        process = self.spawn()
        self.ready(process)
        self.assertEqual(self.run_coke("off").returncode, 1)
        self.assertIn("yes (all users)", self.run_coke("status").stdout)
        self.assertNotIn("CLEAR", self.events())
        process.terminate()
        process.communicate(timeout=5)
        self.assertEqual(process.returncode, 143)
        self.assertIn("no (all users)", self.run_coke("status").stdout)
        self.assertEqual(self.events().count("CLEAR"), 1)

    def test_last_instance_clears_once_after_simultaneous_exits(self):
        processes = [self.spawn() for _ in range(12)]
        for process in processes:
            self.ready(process)
        # Hold control while signals queue to force shutdown contention.
        fd = os.open(self.env["COKE_TEST_CONTROL"], os.O_RDONLY)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            for process in processes:
                process.terminate()
            time.sleep(0.05)
        finally:
            os.close(fd)
        for process in processes:
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 143)
        self.assertEqual(self.events().count("CLEAR"), 1)

    def test_child_does_not_inherit_coordination_descriptors(self):
        code = """
import os,fcntl
for key in ['COKE_TEST_CONTROL','COKE_TEST_SESSIONS']:
    path = os.environ[key]
    target = os.stat(path)
    for fd in range(3, 256):
        try: st = os.fstat(fd)
        except OSError: continue
        assert (st.st_dev, st.st_ino) != (target.st_dev, target.st_ino)
"""
        result = self.run_coke("--", PYTHON, "-c", code)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_parent_signal_reaches_command_group_and_waits_for_cleanup(self):
        marker = self.root / "child-ready"
        code = """
import os,signal,sys,time
def stop(sig, frame):
    print('handled', flush=True)
    time.sleep(0.1)
    sys.exit(23)
signal.signal(signal.SIGTERM, stop)
open(sys.argv[1], 'w').close()
while True: time.sleep(1)
"""
        process = self.spawn("--", PYTHON, "-c", code, str(marker))
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(marker.exists())
        process.terminate()
        out, err = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 23, err)
        self.assertIn("handled", out)
        self.assertEqual(self.events().count("CLEAR"), 1)

    def test_signal_reaches_grandchild(self):
        marker = self.root / "grandchild-ready"
        code = """
import signal,subprocess,sys,time
signal.signal(signal.SIGTERM, lambda *_: None)
child = subprocess.Popen([sys.executable, '-c',
    "import pathlib,sys,time; pathlib.Path(sys.argv[1]).touch(); time.sleep(30)", sys.argv[1]])
status = child.wait()
print('grandchild='+str(status))
"""
        process = self.spawn("--", PYTHON, "-c", code, str(marker))
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(marker.exists())
        process.terminate()
        out, err = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, err)
        self.assertIn("grandchild=-15", out)

    def test_hup_ignored_by_nohup_is_preserved(self):
        result = self.run_coke("--", "/usr/bin/nohup", BINARY, "--", PYTHON, "-c",
            "import os,signal; os.kill(os.getpid(), signal.SIGHUP); print('survived')")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("survived", result.stdout)

    def test_symlink_and_bad_permissions_fail_without_power_changes(self):
        path = Path(self.env["COKE_TEST_CONTROL"])
        target = self.root / "target"
        target.write_text("untouched")
        path.symlink_to(target)
        self.assertEqual(self.run_coke("off").returncode, 1)
        self.assertEqual(target.read_text(), "untouched")
        path.unlink()
        path.write_text("")
        path.chmod(0o666)
        self.assertEqual(self.run_coke("off").returncode, 1)
        self.assertEqual(self.events(), [])

    def test_lock_mode_ignores_restrictive_umask(self):
        result = self.run_coke("off", umask=0o077)
        self.assertEqual(result.returncode, 0, result.stderr)
        for key in ["COKE_TEST_CONTROL", "COKE_TEST_SESSIONS"]:
            self.assertEqual(os.stat(self.env[key]).st_mode & 0o777, 0o444)

    def test_terminal_input_and_ctrl_c(self):
        pid, master = pty.fork()
        if pid == 0:
            os.execve(BINARY, [BINARY, "--", PYTHON, "-u", "-c",
                "print('READY'); print('INPUT:'+input()); import time; time.sleep(30)"], self.env)
        output = b""
        reaped = False
        try:
            def wait_text(text):
                nonlocal output
                deadline = time.monotonic() + 5
                while text not in output and time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        output += os.read(master, 4096)
                self.assertIn(text, output)
            wait_text(b"READY")
            os.write(master, b"hello\n")
            wait_text(b"INPUT:hello")
            os.write(master, b"\x03")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.01)[0]:
                    try:
                        output += os.read(master, 4096)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                child, status = os.waitpid(pid, os.WNOHANG)
                if child:
                    reaped = True
                    self.assertEqual(os.waitstatus_to_exitcode(status), 130)
                    break
                time.sleep(0.01)
            self.assertTrue(reaped, output)
        finally:
            os.close(master)
            if not reaped:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)

    def test_terminal_stop_and_foreground_resume(self):
        # An interactive shell supplies actual foreground job-control behavior.
        pid, master = pty.fork()
        if pid == 0:
            os.execve("/bin/sh", ["/bin/sh", "-i"], self.env)
        output = b""
        try:
            def wait_text(text):
                nonlocal output
                deadline = time.monotonic() + 5
                while text not in output and time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        output += os.read(master, 4096)
                self.assertIn(text, output)
            os.write(master, (BINARY + " -- /bin/cat\n").encode())
            wait_text(b"override requested")
            os.write(master, b"\x1a")
            wait_text(b"Stopped")
            output = b""
            os.write(master, b"fg\n")
            time.sleep(0.05)
            os.write(master, b"resumed-input\n")
            wait_text(b"resumed-input\r\nresumed-input")
            os.write(master, b"\x03")
            wait_text(b"override cleared")
        finally:
            os.close(master)
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)


if __name__ == "__main__":
    unittest.main()
