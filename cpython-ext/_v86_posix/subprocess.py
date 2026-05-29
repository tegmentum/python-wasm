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
        # STDOUT means "merge stderr into stdout". The current WIT models
        # stdout and stderr as independent pipes — we can't ask the child
        # to dup2(stdout, 2). Accept it by piping stderr separately;
        # communicate()/wait() will concatenate the two streams in the
        # legacy order (stdout first, stderr appended). Live readline()
        # loops (pip's call_subprocess) see only stdout, not the merged
        # stream — acceptable degradation for an install workflow where
        # the merged log only matters when something fails.
        return _v86_posix.STDIO_PIPED
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


class _StdinBuffer:
    """Phase 3c: deferred-spawn stdin pseudo-stream.

    `Popen(stdin=PIPE)` returns immediately without spawning; the
    caller writes bytes here, and `close()` triggers the parent's
    `_ensure_spawned()` which routes through the Phase 3b v2 shell-
    redirect wrapper with the buffered bytes as the file content.

    Looks file-like enough for the `subprocess.Popen.stdin.write(b'…')
    + p.stdin.close()` shape callers expect, but it's NOT a real
    `wasi:io/streams::output_stream` — closing it is what wires it
    through to the child.
    """

    def __init__(self, popen: "Popen") -> None:
        self._popen = popen
        self._buf = bytearray()
        self._closed = False

    def write(self, data: bytes) -> int:
        if self._closed:
            raise ValueError("I/O operation on closed file")
        # Accept bytes-like inputs uniformly.
        view = bytes(data)
        self._buf.extend(view)
        return len(view)

    def flush(self) -> None:
        pass  # we're a buffer; nothing to flush until close

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        # close() is the spawn trigger — the buffered bytes become the
        # child's stdin via the Phase 3b v2 shell-redirect wrapper.
        self._popen._ensure_spawned()

    @property
    def closed(self) -> bool:
        return self._closed

    def writable(self) -> bool:
        return True

    def readable(self) -> bool:
        return False

    def fileno(self) -> int:
        raise OSError("buffer has no host file descriptor")

    # Context-manager protocol — callers may `with p.stdin: …`
    def __enter__(self) -> "_StdinBuffer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


import io as _io


class _InputStreamReader(_io.RawIOBase):
    """RawIOBase adapter over ``_v86_posix.InputStream``.

    The underlying stream has ``.read(size)`` that returns up to size
    bytes (blocking until at least 1 byte or EOF). ``BufferedReader``
    expects ``.readinto(buf)``. Translate one to the other so callers
    get a full file-like surface (readline, iteration, etc.)."""

    def __init__(self, stream):
        super().__init__()
        self._stream = stream

    def readable(self) -> bool:
        return True

    def close(self) -> None:
        super().close()
        try:
            self._stream.close()
        except Exception:
            pass

    def readinto(self, buf) -> int:
        n = len(buf)
        if n == 0:
            return 0
        data = self._stream.read(n)
        m = len(data)
        buf[:m] = data
        return m


def _wrap_input(stream):
    """Wrap a raw ``_v86_posix.InputStream`` in ``io.BufferedReader``
    so callers get ``.readline()`` and iteration."""
    return _io.BufferedReader(_InputStreamReader(stream))


class _DeferredPipe:
    """Truthy placeholder for ``self.stdout``/``stderr`` during deferred
    spawn (``stdin=PIPE`` path). Callers that hold ``proc.stdout`` and do
    ``assert proc.stdout`` before ``proc.stdin.close()`` need a non-None
    value here; once the deferred spawn runs, ``Popen._do_spawn`` rebinds
    ``self.stdout`` (or ``self.stderr``) to the real pipe and subsequent
    attribute lookups see it. This class exists only to satisfy the
    early truthy check — its methods either delegate (if the real pipe
    is already in place by some race) or raise, since no real read can
    happen against a not-yet-spawned process."""

    __slots__ = ("_popen", "_name")

    def __init__(self, popen: "Popen", name: str) -> None:
        self._popen = popen
        self._name = name

    def _real(self):
        # If anyone reads via this instance after the spawn (e.g., a
        # local that held the pre-spawn reference), forward to the
        # actual pipe object the spawn parked on Popen.
        real = getattr(self._popen, self._name, None)
        if real is None or real is self:
            raise RuntimeError(
                f"_DeferredPipe.{self._name}: process not yet spawned")
        return real

    def __bool__(self) -> bool:
        return True

    def read(self, *a, **kw):     return self._real().read(*a, **kw)
    def readline(self, *a, **kw): return self._real().readline(*a, **kw)
    def readinto(self, *a, **kw): return self._real().readinto(*a, **kw)
    def close(self):              return self._real().close()
    def fileno(self):             return self._real().fileno()


class Popen:
    """Subprocess managed by `_v86_posix.spawn`.

    Constructor accepts the subprocess.Popen signature; many of the
    knobs are not yet wired (encoding, text mode, etc.). Unsupported
    knobs raise NotImplementedError at construction time rather than
    silently dropping data.

    Phase 3c: when `stdin=PIPE`, spawn is **deferred** — the Popen
    returns without launching a child; `self.stdin` is a buffer-backed
    pseudo-stream. Closing it (or any lifecycle method like `wait`,
    `communicate`, `pid` access) triggers the real spawn via the
    Phase 3b v2 shell-redirect wrapper with the buffered bytes as
    the child's stdin file.
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
                "text mode / encoding / universal_newlines require a stdio "
                "stream wrapping refactor; use bytes-mode via PIPE+communicate")

        self.args = args
        program, argv = _normalise_args(args, shell)
        if executable is not None:
            program = executable

        # Cache everything _do_spawn will need.
        self._program = program
        self._argv = argv
        self._env_pairs: list[tuple[str, str]] = (
            list(env.items()) if env is not None else []
        )
        self._cwd = cwd
        self._stdin_kind = _to_stdio_spec(stdin)
        self._stdout_kind = _to_stdio_spec(stdout)
        self._stderr_kind = _to_stdio_spec(stderr)
        self._stdout_was_pipe = (stdout == PIPE)
        # stderr=STDOUT maps to PIPE inside _to_stdio_spec — both the
        # explicit PIPE and STDOUT cases need a take_stderr() at spawn
        # time so communicate()/wait() can drain it.
        self._stderr_was_pipe = (stderr == PIPE or stderr == STDOUT)
        self._stdin_was_pipe = (stdin == PIPE)

        self.returncode: int | None = None
        self._process = None
        self.stdin = None
        self.stdout = None
        self.stderr = None
        self._stdin_tmpfile: str | None = None

        if self._stdin_was_pipe:
            # Defer spawn. Caller writes to self.stdin; closing triggers
            # _ensure_spawned which routes through the shell-wrapper
            # path with the buffered bytes.
            self.stdin = _StdinBuffer(self)
            # Pip's call_subprocess does `assert proc.stdout` (and
            # sometimes `assert proc.stderr`) immediately after Popen,
            # BEFORE writing to stdin. With deferred spawn self.stdout
            # is still None at that point, blowing the assertion.
            # Park a truthy placeholder; _do_spawn() rebinds the
            # attribute to the real pipe before any read happens
            # (the user's expression `proc.stdout.readline()` is a
            # fresh getattr lookup, so it sees the rebind).
            if self._stdout_was_pipe:
                self.stdout = _DeferredPipe(self, "stdout")
            if self._stderr_was_pipe:
                self.stderr = _DeferredPipe(self, "stderr")
        else:
            self._do_spawn(stdin_bytes=None)

    def _do_spawn(self, stdin_bytes: bytes | None) -> None:
        """Call `_v86_posix.spawn`. If `stdin_bytes` is given, wraps argv
        in the Phase 3b v2 shell-redirect so the child sees those bytes
        as stdin via a regular file (sidesteps the wasmtime
        wasi:filesystem-on-FIFO blocker).
        """
        program = self._program
        argv = self._argv
        stdin_kind = self._stdin_kind

        if stdin_bytes is not None:
            # Write the buffered bytes to a unique sibling file in the
            # mailbox dir; the wrapper script opens it via
            # $POSIX_MAILBOX_DIR/$rel-name + execs the user's command.
            mailbox_wasm = os.environ.get(
                "V86_POSIX_MAILBOX_DIR", "/workspace/posix-mailbox")
            nonce = os.urandom(8).hex()
            rel_name = f".stdin-{nonce}.dat"
            abs_path = f"{mailbox_wasm}/{rel_name}"
            try:
                with open(abs_path, "wb") as f:
                    f.write(stdin_bytes)
            except OSError as exc:
                raise OSError(
                    errno.ENOENT,
                    f"could not write stdin tempfile {abs_path}: {exc}",
                ) from None
            self._stdin_tmpfile = abs_path
            wrapper_script = (
                'IN="$POSIX_MAILBOX_DIR/$1"; shift; exec "$@" 0<"$IN"'
            )
            argv = ["/bin/sh", "-c", wrapper_script, "--", rel_name, *argv]
            program = "/bin/sh"
            # Override stdin_kind — wrapper's <"$IN" redirect supersedes
            # whatever was inherited.
            stdin_kind = _v86_posix.STDIO_INHERIT

        try:
            self._process = _v86_posix.spawn(
                program,
                argv,
                self._env_pairs,
                self._cwd,
                stdin_kind,
                self._stdout_kind,
                self._stderr_kind,
            )
        except _v86_posix.SpawnError as exc:
            raise _v86_to_subprocess_error(exc, self.args) from None

        # Take stdout/stderr streams now that the child is running.
        # The raw InputStream from _v86_posix exposes .read(n) but not
        # readline(); pip's call_subprocess iterates line by line.
        # Wrap via io.BufferedReader on a thin RawIOBase adapter.
        if self._stdout_was_pipe:
            self.stdout = _wrap_input(self._process.take_stdout())
        if self._stderr_was_pipe:
            self.stderr = _wrap_input(self._process.take_stderr())

    def _ensure_spawned(self) -> None:
        """Trigger the deferred spawn if it hasn't happened yet.

        Called from `stdin.close()` (the natural trigger) and from any
        lifecycle method that needs the child to exist. Empty stdin
        buffer turns into an empty stdin file → child sees EOF
        immediately on read.
        """
        if self._process is not None:
            return
        if isinstance(self.stdin, _StdinBuffer):
            stdin_bytes = bytes(self.stdin._buf)
        else:
            stdin_bytes = b""
        self._do_spawn(stdin_bytes=stdin_bytes)

    @property
    def pid(self) -> int | None:
        # stdlib pid is set after Popen.__init__. With deferred spawn
        # we make pid trigger the spawn — preserves the contract for
        # the common case where the caller checks pid as a diagnostic.
        # Buffered bytes (if any) are committed at that point.
        if self._process is None:
            self._ensure_spawned()
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
        if self._process is None:
            # No child yet — caller hasn't closed stdin. Report
            # 'still running' to match the Popen contract.
            return None
        status = self._process.try_wait()
        if status is None:
            return None
        self.returncode = self._translate_status(status)
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        if self.returncode is not None:
            return self.returncode
        # If we never spawned, do it now so wait has something to wait
        # for. Empty stdin → immediate EOF for any reader.
        if self._process is None:
            self._ensure_spawned()
        if timeout is None:
            self.returncode = self._translate_status(self._process.wait())
            return self.returncode
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
        if self._process is None:
            # No child yet — silent no-op. Matches the "send_signal on
            # already-dead child is silent" stdlib pattern, and the
            # natural user expectation that `kill()` cleans up a Popen
            # they're abandoning before stdin.close().
            return
        try:
            self._process.signal(int(sig))
        except _v86_posix.NoSuchProcessError:
            pass

    def terminate(self) -> None:
        self.send_signal(15)

    def kill(self) -> None:
        self.send_signal(9)

    def communicate(self, input: bytes | None = None,
                    timeout: float | None = None) -> tuple[bytes | None, bytes | None]:
        """Send `input` to stdin (if PIPE), read stdout/stderr to EOF, wait.

        With deferred spawn, the input bytes are merged into the
        buffered stdin and committed when stdin.close() triggers the
        real spawn. From then on this is the Phase 2 capture-only path.
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
            # At this point _ensure_spawned has fired (via stdin.close).
            # If neither input nor existing stdin was provided AND we
            # haven't spawned (no stdin=PIPE case), spawn happened in
            # __init__ already; either way self.stdout/self.stderr are
            # populated for the corresponding PIPE flags.
            stdout_data = self.stdout.read() if self.stdout is not None else None
            stderr_data = self.stderr.read() if self.stderr is not None else None
        finally:
            self.wait(timeout=timeout)
        return stdout_data, stderr_data

    def __enter__(self) -> "Popen":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        # If exiting without ever spawning (e.g. exception in caller
        # before stdin.close), still spawn-then-wait so the helper
        # doesn't end up with a dangling expectation, and clean up
        # the tmpfile.
        try:
            if self.returncode is None:
                try:
                    self.wait()
                except Exception:
                    self.kill()
                    self.wait()
        finally:
            if self._stdin_tmpfile is not None:
                try:
                    os.unlink(self._stdin_tmpfile)
                except OSError:
                    pass
                self._stdin_tmpfile = None


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
    """subprocess.run analog with capture_output + input support.

    `input=…` is routed via `Popen(stdin=PIPE).communicate(input=…)`,
    which under the Phase 3c deferred-spawn shim ends up using the
    same shell-redirect wrapper trick Phase 3b v2 introduced: bytes
    written to the buffered stdin become a regular file in the
    mailbox dir, and the child is wrapped in a tiny `sh -c` that
    `0<$file` redirects from it. No FIFOs, no live streaming.
    """
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
