"""subprocess shim — routes through the v86:posix/process capability.

Drop-in replacement for the stdlib `subprocess` module that uses the
`_v86_posix` capability extension instead of the missing-on-wasi
`_posixsubprocess` / `os.fork` / `os.execve` primitives. Installed into
`deps/cpython/Lib/subprocess.py` by `make install-python-shims`,
shadowing the stdlib copy.

Without this shim every process-spawning code path fails with
`OSError: [Errno 58] wasi does not support processes.` — `Popen`,
`run`, `os.popen`, every variant. With it those paths reach
`_v86_posix.spawn`, which calls into the composed `v86-posix-stub`
(or, when it lands, the real `v86-component` impl). The stub answers
`SpawnError::GuestNotReady` for every call; this shim re-raises it as
`OSError(ENOTSUP, …)` so callers see the familiar errno-style shape.

API parity:

  Module-level
    subprocess.run(args, *, check=False, timeout=None, **kw)         ✓
    subprocess.call(args, *, timeout=None, **kw)                     ✓
    subprocess.check_call(args, **kw)                                ✓
    subprocess.Popen(args, *, stdin, stdout, stderr, cwd, env, …)    ✓
    subprocess.CompletedProcess                                      ✓
    subprocess.CalledProcessError                                    ✓
    subprocess.SubprocessError                                       ✓
    subprocess.TimeoutExpired                                        ✓
    subprocess.PIPE / DEVNULL / STDOUT                               ✓ (constants)
    subprocess.list2cmdline(seq)                                     ✓ (joins; UNIX style)

  Popen instance API
    .pid, .returncode                                                ✓
    .wait(timeout=None)                                              ✓ (timeout not honored — see below)
    .poll()                                                          ✓
    .terminate() / .kill() / .send_signal(sig)                       ✓
    .__enter__ / __exit__  (context manager)                         ✓

Deferred (NotImplementedError where you'd hit them):
  * .stdin / .stdout / .stderr as Popen attributes — these are
    `wasi:io/streams` handles in the WIT and need a cross-extension
    Python file-like wrapper that's TBD (see
    `cpython-ext/_v86_posix/README.md`). PIPE in spawn-options is
    accepted but the corresponding accessor raises NotImplementedError.
    INHERIT and DEVNULL work fully.
  * .communicate(input=None, timeout=None) — needs streams.
  * subprocess.check_output(args) — needs stdout capture, needs streams.
  * subprocess.getoutput / getstatusoutput — needs stdout capture.
  * timeout on wait() — the WIT has `wait` (blocking) and `try_wait`
    (non-blocking); a timeout-bounded wait would need polling in a
    loop with sleep, deferred until a real spawn impl makes timing
    meaningful.
"""

from __future__ import annotations

import errno
import os
import sys
import time
from typing import Any, Mapping, Sequence

import _v86_posix

__all__ = [
    "Popen",
    "run",
    "call",
    "check_call",
    "check_output",
    "CompletedProcess",
    "SubprocessError",
    "CalledProcessError",
    "TimeoutExpired",
    "PIPE",
    "DEVNULL",
    "STDOUT",
    "list2cmdline",
]

# ---------------------------------------------------------------------------
# Constants (match stdlib values so calling code can compare directly)
# ---------------------------------------------------------------------------

PIPE = -1
STDOUT = -2
DEVNULL = -3


def _to_stdio_spec(value: Any) -> int:
    """Translate the public `Popen` stdio sentinel into the WIT enum value."""
    if value is None:
        return _v86_posix.STDIO_INHERIT
    if value == PIPE:
        return _v86_posix.STDIO_PIPED
    if value == DEVNULL:
        return _v86_posix.STDIO_NULL
    if value == STDOUT:
        # STDOUT means "merge stderr into stdout". The current WIT doesn't
        # model fd-redirection; surface it explicitly until it does.
        raise NotImplementedError(
            "STDOUT redirection (stderr -> stdout) is not yet supported by "
            "v86:posix/process; pass DEVNULL or PIPE for stderr instead"
        )
    # Filedescriptor ints or file-like objects: not yet supported.
    raise NotImplementedError(
        f"unsupported stdio value {value!r}; expected None / PIPE / DEVNULL"
    )


# ---------------------------------------------------------------------------
# Exceptions (mirror stdlib hierarchy)
# ---------------------------------------------------------------------------


class SubprocessError(Exception):
    """Base class for all subprocess errors."""


class CalledProcessError(SubprocessError):
    """run() / check_call exit-status != 0 (with check=True)."""

    def __init__(self, returncode: int, cmd: Any,
                 output: bytes | None = None, stderr: bytes | None = None) -> None:
        super().__init__()
        self.returncode = returncode
        self.cmd = cmd
        self.output = output
        self.stderr = stderr

    @property
    def stdout(self) -> bytes | None:
        return self.output

    @stdout.setter
    def stdout(self, value: bytes | None) -> None:
        self.output = value

    def __str__(self) -> str:
        if self.returncode and self.returncode < 0:
            return (f"Command {self.cmd!r} died with signal {-self.returncode}.")
        return (f"Command {self.cmd!r} returned non-zero exit status "
                f"{self.returncode}.")


class TimeoutExpired(SubprocessError):
    """wait()/run() exceeded the timeout."""

    def __init__(self, cmd: Any, timeout: float,
                 output: bytes | None = None, stderr: bytes | None = None) -> None:
        super().__init__()
        self.cmd = cmd
        self.timeout = timeout
        self.output = output
        self.stderr = stderr

    @property
    def stdout(self) -> bytes | None:
        return self.output

    @stdout.setter
    def stdout(self, value: bytes | None) -> None:
        self.output = value

    def __str__(self) -> str:
        return f"Command {self.cmd!r} timed out after {self.timeout} seconds"


# ---------------------------------------------------------------------------
# CompletedProcess (pure data class, drop-in stdlib parity)
# ---------------------------------------------------------------------------


class CompletedProcess:
    """run() return value: args/returncode plus optional stdout/stderr."""

    def __init__(self, args: Any, returncode: int,
                 stdout: bytes | None = None,
                 stderr: bytes | None = None) -> None:
        self.args = args
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr

    def __repr__(self) -> str:
        parts = [f"args={self.args!r}", f"returncode={self.returncode!r}"]
        if self.stdout is not None:
            parts.append(f"stdout={self.stdout!r}")
        if self.stderr is not None:
            parts.append(f"stderr={self.stderr!r}")
        return f"CompletedProcess({', '.join(parts)})"

    def check_returncode(self) -> None:
        if self.returncode:
            raise CalledProcessError(
                self.returncode, self.args, self.stdout, self.stderr)


# ---------------------------------------------------------------------------
# Popen
# ---------------------------------------------------------------------------


def _normalise_args(args: Sequence[str] | str, shell: bool) -> tuple[str, list[str]]:
    """Return (program, argv) per subprocess's rules.

    * shell=True with a string: ['/bin/sh', '-c', the_string].
    * args is a list: program = args[0], argv = list(args).
    * args is a string with shell=False: per stdlib, argv = [args]. Match.
    """
    if shell:
        if not isinstance(args, str):
            raise TypeError("shell=True requires a string command")
        return "/bin/sh", ["/bin/sh", "-c", args]
    if isinstance(args, str):
        return args, [args]
    if not args:
        raise ValueError("args must not be empty")
    argv = list(args)
    return argv[0], argv


def _v86_to_subprocess_error(exc: BaseException, args: Any) -> BaseException:
    """Map a `_v86_posix.SpawnError` into the closest stdlib equivalent.

    Most map to OSError with a meaningful errno; the caller sees a familiar
    `OSError: [Errno N] message` rather than a v86-specific exception type
    leaking out of the public subprocess API.
    """
    if isinstance(exc, _v86_posix.GuestNotReadyError):
        return OSError(errno.ENOTSUP, f"v86 guest not ready: {exc}")
    if isinstance(exc, _v86_posix.ProgramNotFoundError):
        # FileNotFoundError extends OSError(ENOENT).
        return FileNotFoundError(errno.ENOENT, f"executable not found: {exc}")
    if isinstance(exc, _v86_posix.ExecFailedError):
        return PermissionError(errno.EACCES, f"exec failed: {exc}")
    if isinstance(exc, _v86_posix.TooManyProcessesError):
        return OSError(errno.EAGAIN, "guest process-table limit reached")
    if isinstance(exc, _v86_posix.InvalidArgumentError):
        return OSError(errno.EINVAL, f"invalid spawn argument: {exc}")
    if isinstance(exc, _v86_posix.SpawnError):
        return OSError(errno.EIO, f"spawn failed: {exc}")
    return exc


class Popen:
    """Subprocess managed by `_v86_posix.spawn`.

    Constructor accepts the subprocess.Popen signature; many of the
    knobs are not yet wired (stdin/out/err other than INHERIT/DEVNULL,
    encoding, text mode, …). Unsupported knobs raise NotImplementedError
    at construction time rather than silently dropping data.
    """

    def __init__(self,
                 args: Sequence[str] | str,
                 bufsize: int = -1,
                 executable: str | None = None,
                 stdin: Any = None,
                 stdout: Any = None,
                 stderr: Any = None,
                 preexec_fn: Any = None,
                 close_fds: bool = True,
                 shell: bool = False,
                 cwd: str | None = None,
                 env: Mapping[str, str] | None = None,
                 universal_newlines: bool | None = None,
                 startupinfo: Any = None,
                 creationflags: int = 0,
                 restore_signals: bool = True,
                 start_new_session: bool = False,
                 pass_fds: Sequence[int] = (),
                 *,
                 text: bool | None = None,
                 encoding: str | None = None,
                 errors: str | None = None,
                 user: Any = None,
                 group: Any = None,
                 extra_groups: Any = None,
                 umask: int = -1,
                 process_group: int = -1,
                 pipesize: int = -1) -> None:
        if preexec_fn is not None:
            raise NotImplementedError("preexec_fn is not supported")
        if pass_fds:
            raise NotImplementedError("pass_fds is not supported")
        if text or encoding is not None or universal_newlines:
            raise NotImplementedError(
                "text mode / encoding / universal_newlines require stdio "
                "stream wrapping (TBD); use bytes-mode via PIPE+communicate "
                "once that lands")

        self.args = args
        program, argv = _normalise_args(args, shell)
        if executable is not None:
            program = executable

        env_pairs = list(env.items()) if env is not None else []
        stdin_kind = _to_stdio_spec(stdin)
        stdout_kind = _to_stdio_spec(stdout)
        stderr_kind = _to_stdio_spec(stderr)

        try:
            self._process = _v86_posix.spawn(
                program,
                argv,
                env_pairs,
                cwd,
                stdin_kind,
                stdout_kind,
                stderr_kind,
            )
        except _v86_posix.SpawnError as exc:
            raise _v86_to_subprocess_error(exc, args) from None

        self.returncode: int | None = None
        # Public stdin/stdout/stderr — populated when the corresponding
        # stdio was PIPE. take_std* returns None if it wasn't, so this
        # is a safe one-shot.
        self.stdin = self._process.take_stdin() if stdin == PIPE else None
        self.stdout = self._process.take_stdout() if stdout == PIPE else None
        self.stderr = self._process.take_stderr() if stderr == PIPE else None

    @property
    def pid(self) -> int:
        return self._process.pid()

    def _translate_status(self, status: tuple[str, int]) -> int:
        kind, value = status
        if kind == "exited":
            return int(value)
        if kind == "signaled":
            return -int(value)
        raise RuntimeError(f"unknown exit-status kind: {kind!r}")

    def poll(self) -> int | None:
        if self.returncode is not None:
            return self.returncode
        status = self._process.try_wait()
        if status is None:
            return None
        self.returncode = self._translate_status(status)
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        if self.returncode is not None:
            return self.returncode
        if timeout is None:
            self.returncode = self._translate_status(self._process.wait())
            return self.returncode
        # Poll-with-sleep: try_wait every `tick` until timeout. Coarse,
        # but adequate while spawn dispatches to a stub that finishes
        # instantly or never; revisit if/when the real impl wants tight
        # timing.
        deadline = time.monotonic() + timeout
        tick = 0.05
        while True:
            status = self._process.try_wait()
            if status is not None:
                self.returncode = self._translate_status(status)
                return self.returncode
            if time.monotonic() >= deadline:
                raise TimeoutExpired(self.args, timeout)
            time.sleep(min(tick, max(0.0, deadline - time.monotonic())))

    def send_signal(self, sig: int) -> None:
        try:
            self._process.signal(int(sig))
        except _v86_posix.NoSuchProcessError:
            # subprocess.send_signal silently no-ops on an already-dead
            # child to avoid races; match that.
            pass

    def terminate(self) -> None:
        # SIGTERM
        self.send_signal(15)

    def kill(self) -> None:
        # SIGKILL
        self.send_signal(9)

    def communicate(self, input: bytes | None = None,
                    timeout: float | None = None) -> tuple[bytes | None, bytes | None]:
        """Send `input` to stdin (if piped), read stdout/stderr to EOF, wait.

        Returns `(stdout_bytes, stderr_bytes)` where each is `None` if the
        corresponding stream was not piped, else `bytes`. Honours `timeout`
        on the final `wait` (the stream reads are blocking — a tight timeout
        on a stream that takes ages would be missed).
        """
        try:
            if input is not None:
                if self.stdin is None:
                    raise ValueError(
                        "communicate(input=...) requires stdin=PIPE on the Popen call")
                try:
                    self.stdin.write(input)
                finally:
                    self.stdin.close()
            elif self.stdin is not None:
                self.stdin.close()
            stdout_data = self.stdout.read() if self.stdout is not None else None
            stderr_data = self.stderr.read() if self.stderr is not None else None
        finally:
            self.wait(timeout=timeout)
        return stdout_data, stderr_data

    def __enter__(self) -> "Popen":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        if self.returncode is None:
            try:
                self.wait()
            except Exception:
                self.kill()
                self.wait()


# ---------------------------------------------------------------------------
# High-level helpers
# ---------------------------------------------------------------------------


def list2cmdline(seq: Sequence[str]) -> str:
    """Join a sequence of args for human display (UNIX-ish quoting)."""
    out: list[str] = []
    for arg in seq:
        if not arg or any(ch in arg for ch in " \t\n\"\\'$`*?|&;<>()"):
            out.append("'" + arg.replace("'", "'\"'\"'") + "'")
        else:
            out.append(arg)
    return " ".join(out)


def call(args: Sequence[str] | str, *, timeout: float | None = None,
         **kwargs: Any) -> int:
    """Run command, wait, return its exit status."""
    with Popen(args, **kwargs) as p:
        try:
            return p.wait(timeout=timeout)
        except Exception:
            p.kill()
            raise


def check_call(args: Sequence[str] | str, **kwargs: Any) -> int:
    rc = call(args, **kwargs)
    if rc:
        raise CalledProcessError(rc, kwargs.get("args", args))
    return 0


def run(args: Sequence[str] | str, *,
        input: bytes | None = None,
        check: bool = False,
        timeout: float | None = None,
        capture_output: bool = False,
        **kwargs: Any) -> CompletedProcess:
    """subprocess.run analog with capture_output + input support."""
    if capture_output:
        if "stdout" in kwargs or "stderr" in kwargs:
            raise ValueError(
                "capture_output may not be used with stdout / stderr kwargs")
        kwargs["stdout"] = PIPE
        kwargs["stderr"] = PIPE
    if input is not None and "stdin" not in kwargs:
        kwargs["stdin"] = PIPE

    with Popen(args, **kwargs) as p:
        try:
            stdout_data, stderr_data = p.communicate(input=input, timeout=timeout)
            rc = p.wait()
        except TimeoutExpired:
            p.kill()
            raise
        except Exception:
            p.kill()
            raise
    completed = CompletedProcess(args, rc, stdout_data, stderr_data)
    if check:
        completed.check_returncode()
    return completed


def check_output(args: Sequence[str] | str, *, input: bytes | None = None,
                 **kwargs: Any) -> bytes:
    """Run command, return stdout. Stderr by default goes to caller's stderr."""
    if "stdout" in kwargs:
        raise ValueError("stdout argument not allowed, it will be overridden")
    kwargs["stdout"] = PIPE
    completed = run(args, input=input, check=True, **kwargs)
    return completed.stdout


# Stdlib subprocess defines a few capability flags (e.g. _USE_VFORK,
# _USE_POSIX_SPAWN) that some callers introspect. Provide neutral defaults
# so accidental getattr doesn't raise.
_USE_VFORK = False
_USE_POSIX_SPAWN = False


# ---------------------------------------------------------------------------
# Private helpers that other stdlib modules import directly from `subprocess`.
# `multiprocessing.util` and friends do
#   `from subprocess import _args_from_interpreter_flags`
# so the shim must preserve both. Verbatim copies from upstream subprocess.py
# — pure introspection of sys.flags / sys.warnoptions / sys._xoptions, no
# spawn dependency.
# ---------------------------------------------------------------------------


def _optim_args_from_interpreter_flags() -> list[str]:
    """Reproduce -O / -OO from sys.flags.optimize."""
    args: list[str] = []
    value = sys.flags.optimize
    if value > 0:
        args.append("-" + "O" * value)
    return args


def _args_from_interpreter_flags() -> list[str]:
    """Reproduce the current interpreter's flags / -X / -W options."""
    flag_opt_map = {
        "debug": "d",
        "dont_write_bytecode": "B",
        "no_site": "S",
        "verbose": "v",
        "bytes_warning": "b",
        "quiet": "q",
    }
    args = _optim_args_from_interpreter_flags()
    for flag, opt in flag_opt_map.items():
        v = getattr(sys.flags, flag, 0)
        if v > 0:
            args.append("-" + opt * v)

    if getattr(sys.flags, "isolated", False):
        args.append("-I")
    else:
        if getattr(sys.flags, "ignore_environment", False):
            args.append("-E")
        if getattr(sys.flags, "no_user_site", False):
            args.append("-s")
        if getattr(sys.flags, "safe_path", False):
            args.append("-P")

    warnopts = list(getattr(sys, "warnoptions", ()))
    xoptions = getattr(sys, "_xoptions", {}) or {}
    bytes_warning = getattr(sys.flags, "bytes_warning", 0)
    dev_mode = getattr(sys.flags, "dev_mode", False)

    if bytes_warning > 1:
        if "error::BytesWarning" in warnopts:
            warnopts.remove("error::BytesWarning")
    elif bytes_warning:
        if "default::BytesWarning" in warnopts:
            warnopts.remove("default::BytesWarning")
    if dev_mode and "default" in warnopts:
        warnopts.remove("default")
    for opt in warnopts:
        args.append("-W" + opt)

    if dev_mode:
        args.extend(("-X", "dev"))
    for opt in ("faulthandler", "tracemalloc", "importtime",
                "frozen_modules", "showrefcount", "utf8", "gil"):
        if opt in xoptions:
            value = xoptions[opt]
            if value is True:
                arg = opt
            else:
                arg = f"{opt}={value}"
            args.extend(("-X", arg))

    return args
