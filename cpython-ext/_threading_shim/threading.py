"""Single-threaded threading shim for python-wasm.

Core wasm has no preemptive threads. The CPython _thread builtin compiled
for wasm32-wasip2 provides Lock/RLock/get_ident (single-thread-safe), but
_thread.start_new_thread raises `RuntimeError: can't start new thread`,
which makes the stdlib threading.py unusable for any caller that does
Thread(target=...).start().

This shim replaces stdlib Lib/threading.py with a single-threaded
implementation: Thread.start() runs target(*args, **kwargs) inline and
returns. Lock / RLock / Condition / Event / Semaphore / BoundedSemaphore /
Barrier are wrappers over _thread primitives where useful and no-ops
otherwise (there's no concurrent waiter to coordinate). Timer runs its
callback synchronously after the interval (via time.sleep).

This is *enough* for the common "I want my code to import threading and
not crash" case — pip dependencies that conditionally use Thread pools,
test code that exercises Lock as a context manager, etc. Real parallelism
is out of scope; for that, use the tegmentum:py-offload reference-worker
path (separate process) or wait for wasi-threads to land in WASI P2.
"""

import _thread
import sys
import time as _time
from _weakrefset import WeakSet

TIMEOUT_MAX = getattr(_thread, "TIMEOUT_MAX", 1e10)

_start_new_thread = _thread.start_new_thread
_allocate_lock = _thread.allocate_lock
_set_sentinel = getattr(_thread, "_set_sentinel", lambda: _allocate_lock())
get_ident = _thread.get_ident
get_native_id = getattr(_thread, "get_native_id", get_ident)


def _no_op_signature(*args, **kwargs):
    return None


class _ThreadError(RuntimeError):
    pass


_active = {}
_dangling = WeakSet()


class Lock:
    def __init__(self):
        self._lock = _allocate_lock()

    def acquire(self, blocking=True, timeout=-1):
        return self._lock.acquire(blocking, timeout)

    def release(self):
        return self._lock.release()

    def locked(self):
        return self._lock.locked()

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.release()


def allocate_lock():
    return Lock()


class RLock:
    def __init__(self):
        self._owner = None
        self._count = 0
        self._lock = _allocate_lock()

    def acquire(self, blocking=True, timeout=-1):
        ident = get_ident()
        if self._owner == ident:
            self._count += 1
            return True
        acquired = self._lock.acquire(blocking, timeout)
        if acquired:
            self._owner = ident
            self._count = 1
        return acquired

    def release(self):
        if self._owner != get_ident():
            raise RuntimeError("cannot release un-acquired lock")
        self._count -= 1
        if self._count == 0:
            self._owner = None
            self._lock.release()

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.release()

    def _is_owned(self):
        return self._owner == get_ident()


class Condition:
    def __init__(self, lock=None):
        if lock is None:
            lock = RLock()
        self._lock = lock
        self.acquire = lock.acquire
        self.release = lock.release

    def __enter__(self):
        return self._lock.__enter__()

    def __exit__(self, exc_type, exc_val, exc_tb):
        return self._lock.__exit__(exc_type, exc_val, exc_tb)

    def wait(self, timeout=None):
        # No other thread can wake us in single-threaded mode.
        # Honor timeout to avoid hanging callers; return False (timed out).
        if timeout and timeout > 0:
            _time.sleep(timeout)
        return False

    def wait_for(self, predicate, timeout=None):
        result = predicate()
        if result:
            return result
        if timeout and timeout > 0:
            _time.sleep(timeout)
        return predicate()

    def notify(self, n=1):
        return None

    def notify_all(self):
        return None

    notifyAll = notify_all


class Semaphore:
    def __init__(self, value=1):
        if value < 0:
            raise ValueError("semaphore initial value must be >= 0")
        self._value = value
        self._lock = _allocate_lock()

    def acquire(self, blocking=True, timeout=None):
        if self._value > 0:
            self._value -= 1
            return True
        if not blocking:
            return False
        if timeout and timeout > 0:
            _time.sleep(timeout)
        return False

    def release(self, n=1):
        self._value += n

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.release()


class BoundedSemaphore(Semaphore):
    def __init__(self, value=1):
        super().__init__(value)
        self._initial_value = value

    def release(self, n=1):
        if self._value + n > self._initial_value:
            raise ValueError("Semaphore released too many times")
        super().release(n)


class Event:
    def __init__(self):
        self._flag = False

    def is_set(self):
        return self._flag

    isSet = is_set

    def set(self):
        self._flag = True

    def clear(self):
        self._flag = False

    def wait(self, timeout=None):
        if self._flag:
            return True
        # Nobody else can set it; honor timeout then return current state.
        if timeout and timeout > 0:
            _time.sleep(timeout)
        return self._flag


class Barrier:
    def __init__(self, parties, action=None, timeout=None):
        if parties != 1:
            raise RuntimeError(
                "Barrier(parties>1) requires preemptive threads; "
                "python-wasm threading shim is single-threaded"
            )
        self._parties = parties
        self._action = action

    def wait(self, timeout=None):
        if self._action:
            self._action()
        return 0

    def reset(self):
        pass

    def abort(self):
        pass

    @property
    def parties(self):
        return self._parties

    @property
    def n_waiting(self):
        return 0

    @property
    def broken(self):
        return False


class BrokenBarrierError(RuntimeError):
    pass


class local:
    pass


class ExceptHookArgs(tuple):
    @property
    def exc_type(self):
        return self[0]

    @property
    def exc_value(self):
        return self[1]

    @property
    def exc_traceback(self):
        return self[2]

    @property
    def thread(self):
        return self[3]


def _default_excepthook(args):
    if args.exc_type is SystemExit:
        return
    sys.stderr.write(f"Exception in thread {args.thread.name}:\n")
    import traceback
    traceback.print_exception(args.exc_type, args.exc_value, args.exc_traceback,
                              file=sys.stderr)


excepthook = _default_excepthook


class Thread:
    def __init__(self, group=None, target=None, name=None, args=(), kwargs=None,
                 *, daemon=None):
        if group is not None:
            raise ValueError("group argument must be None for now")
        self._target = target
        self._name = name or f"Thread-{id(self)}"
        self._args = args
        self._kwargs = kwargs or {}
        self._daemon = bool(daemon) if daemon is not None else False
        self._started = False
        self._finished = False
        self._ident = None
        self._native_id = None
        self._return = None
        self._exc = None
        self._tstate_lock = None

    def start(self):
        if self._started:
            raise RuntimeError("threads can only be started once")
        self._started = True
        self._ident = get_ident()
        self._native_id = get_native_id()
        _active[self._ident] = self
        # The default `target` path (no run override) is the common case
        # — `Thread(target=fn).start(); ...; thread.join()`. Run it inline
        # so callers that never explicitly join still see the side
        # effects. Subclass-override runs (the rich `_TrackThread` /
        # context-manager pattern) are deferred to join() because they
        # typically loop on an Event the caller sets after running its
        # work — running inline here would deadlock.
        if self._target is not None:
            try:
                self._return = self._target(*self._args, **self._kwargs)
            except SystemExit:
                pass
            except BaseException:
                self._exc = sys.exc_info()
                excepthook(ExceptHookArgs(
                    (self._exc[0], self._exc[1], self._exc[2], self)
                ))
            self._finished = True
            _active.pop(self._ident, None)
        elif type(self).run is Thread.run:
            # No target AND no run override → nothing to do.
            self._finished = True
            _active.pop(self._ident, None)
        # else: deferred — run executes at join() time.

    def run(self):
        # Default — only invoked when start() defers (no-target + run-override).
        # Subclass-overrides via type(self).run get dispatched in join().
        if type(self).run is Thread.run:
            return
        try:
            type(self).run(self)
        except BaseException:
            self._exc = sys.exc_info()
            excepthook(ExceptHookArgs(
                (self._exc[0], self._exc[1], self._exc[2], self)
            ))

    def join(self, timeout=None):
        if not self._started:
            raise RuntimeError("cannot join thread before it is started")
        if not self._finished:
            # Deferred subclass-run path: run now. By the time join is
            # called, any Event the run loop is watching has typically
            # been set (e.g. _TrackThread's __exit__ flow).
            try:
                self.run()
            finally:
                self._finished = True
                _active.pop(self._ident, None)
        return None

    def is_alive(self):
        return self._started and not self._finished

    isAlive = is_alive

    @property
    def name(self):
        return self._name

    @name.setter
    def name(self, value):
        self._name = str(value)

    def getName(self):
        return self._name

    def setName(self, name):
        self._name = str(name)

    @property
    def ident(self):
        return self._ident

    @property
    def native_id(self):
        return self._native_id

    @property
    def daemon(self):
        return self._daemon

    @daemon.setter
    def daemon(self, value):
        if self._started:
            raise RuntimeError("cannot set daemon status of active thread")
        self._daemon = bool(value)

    def isDaemon(self):
        return self._daemon

    def setDaemon(self, value):
        self.daemon = value


class Timer(Thread):
    def __init__(self, interval, function, args=None, kwargs=None):
        super().__init__(target=self._fire, args=())
        self._interval = interval
        self._function = function
        self._cb_args = args or ()
        self._cb_kwargs = kwargs or {}
        self._cancelled = False

    def cancel(self):
        self._cancelled = True

    def _fire(self):
        if self._cancelled:
            return
        if self._interval and self._interval > 0:
            _time.sleep(self._interval)
        if self._cancelled:
            return
        self._function(*self._cb_args, **self._cb_kwargs)


class _MainThread(Thread):
    def __init__(self):
        super().__init__(name="MainThread")
        self._started = True
        self._finished = False
        self._daemon = False
        self._ident = get_ident()
        self._native_id = get_native_id()
        _active[self._ident] = self


_main_thread = _MainThread()


def current_thread():
    return _active.get(get_ident(), _main_thread)


currentThread = current_thread


def main_thread():
    return _main_thread


def active_count():
    return len(_active) or 1


activeCount = active_count


def enumerate():
    return list(_active.values()) or [_main_thread]


def get_ident():  # noqa: F811 -- override module-level alias for stdlib parity
    return _thread.get_ident()


get_ident = _thread.get_ident


def settrace(func):
    sys.settrace(func)


def setprofile(func):
    sys.setprofile(func)


def settrace_all_threads(func):
    sys.settrace(func)


def setprofile_all_threads(func):
    sys.setprofile(func)


def stack_size(size=None):
    if size is None:
        return 0
    return 0


def _register_atexit(*args, **kwargs):
    return None


def _shutdown():
    return None


_after_fork = _no_op_signature
_before_fork = _no_op_signature
_after_at_fork_weak_calls = _no_op_signature


__all__ = [
    "Thread", "Lock", "RLock", "Condition", "Semaphore", "BoundedSemaphore",
    "Event", "Barrier", "BrokenBarrierError", "Timer", "ThreadError",
    "local", "current_thread", "currentThread", "main_thread", "active_count",
    "activeCount", "enumerate", "get_ident", "get_native_id", "TIMEOUT_MAX",
    "settrace", "setprofile", "stack_size", "excepthook", "ExceptHookArgs",
]

ThreadError = _ThreadError
