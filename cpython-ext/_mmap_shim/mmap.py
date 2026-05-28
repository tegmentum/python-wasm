"""Pure-Python mmap shim for python-wasm.

The C extension Modules/mmapmodule.c is not built for wasm32-wasip2 because
the underlying mmap(2) syscall isn't available in WASI. In a single-process
wasm runtime there's no MAP_SHARED-across-processes semantics to preserve
anyway, so the realistic uses of `mmap` collapse to two cases:

  1. Anonymous (fileno == -1)  -- a large contiguous allocation. With no
     process boundary in wasm, this is just a bytearray.

  2. File-backed (fileno >= 0) -- read the file into memory, optionally
     write changes back on flush()/close(). Plain file I/O.

This shim implements the stdlib mmap.mmap API on top of bytearray + file
ops. It is NOT zero-copy and it is NOT shared with other processes -- there
are no other processes. For callers that just want `import mmap` to work
(sqlite, multiprocessing.shared_memory single-process mode, simple
file-mapping for search/seek), it's a drop-in. For callers that genuinely
need OS-level shared memory across components, the right fix is a
`tegmentum:memory` capability, not this shim.
"""

import io
import os

ACCESS_DEFAULT = 0
ACCESS_READ = 1
ACCESS_WRITE = 2
ACCESS_COPY = 3

MAP_SHARED = 0x01
MAP_PRIVATE = 0x02
MAP_ANON = 0x20
MAP_ANONYMOUS = 0x20

PROT_NONE = 0x00
PROT_READ = 0x01
PROT_WRITE = 0x02
PROT_EXEC = 0x04

MADV_NORMAL = 0
MADV_RANDOM = 1
MADV_SEQUENTIAL = 2
MADV_WILLNEED = 3
MADV_DONTNEED = 4

PAGESIZE = 65536
ALLOCATIONGRANULARITY = 65536

error = OSError


class mmap:
    def __init__(self, fileno, length, flags=MAP_SHARED, prot=PROT_READ | PROT_WRITE,
                 access=ACCESS_DEFAULT, offset=0):
        if length < 0:
            raise OverflowError("memory mapped length must be positive")
        if offset < 0:
            raise OverflowError("memory mapped offset must be positive")
        if offset and offset % ALLOCATIONGRANULARITY:
            raise ValueError("offset must be multiple of ALLOCATIONGRANULARITY")

        if access == ACCESS_DEFAULT:
            if prot & PROT_WRITE:
                resolved_access = ACCESS_WRITE
            elif prot & PROT_READ:
                resolved_access = ACCESS_READ
            else:
                resolved_access = ACCESS_WRITE
        else:
            resolved_access = access

        if resolved_access not in (ACCESS_READ, ACCESS_WRITE, ACCESS_COPY):
            raise ValueError("mmap invalid access parameter")

        self._access = resolved_access
        self._fileno = fileno
        self._offset = offset
        self._pos = 0
        self._closed = False

        if fileno == -1:
            if length == 0:
                raise ValueError("cannot mmap an empty anonymous region")
            self._buf = bytearray(length)
            self._file_backed = False
        else:
            try:
                fsize = os.fstat(fileno).st_size
            except OSError:
                fsize = None

            if length == 0:
                if fsize is None:
                    raise ValueError("cannot mmap an empty file (fstat failed)")
                if fsize == 0:
                    raise ValueError("cannot mmap an empty file")
                length = fsize - offset
                if length <= 0:
                    raise ValueError("mmap offset is greater than file size")

            saved_pos = os.lseek(fileno, 0, os.SEEK_CUR)
            try:
                os.lseek(fileno, offset, os.SEEK_SET)
                chunks = []
                remaining = length
                while remaining > 0:
                    chunk = os.read(fileno, remaining)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    remaining -= len(chunk)
            finally:
                try:
                    os.lseek(fileno, saved_pos, os.SEEK_SET)
                except OSError:
                    pass

            buf = bytearray().join(chunks)
            if len(buf) < length:
                buf.extend(b"\x00" * (length - len(buf)))
            self._buf = buf
            self._file_backed = True

    def _check_open(self):
        if self._closed:
            raise ValueError("mmap closed or invalid")

    def _check_writable(self):
        self._check_open()
        if self._access == ACCESS_READ:
            raise TypeError("mmap can't modify a read-only memory map")

    def _check_resizable(self):
        self._check_open()
        if self._access not in (ACCESS_WRITE, ACCESS_DEFAULT):
            raise TypeError("mmap can't resize a readonly or copy-on-write memory map")

    @property
    def closed(self):
        return self._closed

    def close(self):
        if self._closed:
            return
        if self._file_backed and self._access in (ACCESS_WRITE, ACCESS_DEFAULT):
            try:
                self.flush()
            except OSError:
                pass
        self._closed = True
        self._buf = bytearray()

    def __enter__(self):
        self._check_open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __len__(self):
        self._check_open()
        return len(self._buf)

    def size(self):
        if self._file_backed:
            try:
                return os.fstat(self._fileno).st_size
            except OSError:
                pass
        self._check_open()
        return len(self._buf)

    def tell(self):
        self._check_open()
        return self._pos

    def seek(self, pos, whence=io.SEEK_SET):
        self._check_open()
        if whence == io.SEEK_SET:
            new_pos = pos
        elif whence == io.SEEK_CUR:
            new_pos = self._pos + pos
        elif whence == io.SEEK_END:
            new_pos = len(self._buf) + pos
        else:
            raise ValueError("unknown seek type")
        if new_pos < 0 or new_pos > len(self._buf):
            raise ValueError("seek out of range")
        self._pos = new_pos
        return new_pos

    def read(self, n=None):
        self._check_open()
        if n is None or n < 0:
            n = len(self._buf) - self._pos
        end = min(self._pos + n, len(self._buf))
        out = bytes(self._buf[self._pos:end])
        self._pos = end
        return out

    def read_byte(self):
        self._check_open()
        if self._pos >= len(self._buf):
            raise ValueError("read byte out of range")
        b = self._buf[self._pos]
        self._pos += 1
        return b

    def readline(self):
        self._check_open()
        end = self._buf.find(b"\n", self._pos)
        if end == -1:
            end = len(self._buf)
        else:
            end += 1
        line = bytes(self._buf[self._pos:end])
        self._pos = end
        return line

    def write(self, data):
        self._check_writable()
        data = bytes(data)
        end = self._pos + len(data)
        if end > len(self._buf):
            raise ValueError("data out of range")
        self._buf[self._pos:end] = data
        written = len(data)
        self._pos = end
        return written

    def write_byte(self, byte):
        self._check_writable()
        if self._pos >= len(self._buf):
            raise ValueError("write byte out of range")
        if isinstance(byte, int):
            self._buf[self._pos] = byte
        else:
            b = bytes(byte)
            if len(b) != 1:
                raise TypeError("write_byte requires a single byte")
            self._buf[self._pos] = b[0]
        self._pos += 1

    def find(self, sub, start=None, end=None):
        self._check_open()
        if start is None:
            start = self._pos
        if end is None:
            end = len(self._buf)
        return self._buf.find(bytes(sub), start, end)

    def rfind(self, sub, start=None, end=None):
        self._check_open()
        if start is None:
            start = self._pos
        if end is None:
            end = len(self._buf)
        return self._buf.rfind(bytes(sub), start, end)

    def move(self, dest, src, count):
        self._check_writable()
        if dest < 0 or src < 0 or count < 0:
            raise ValueError("source, destination, or count must be non-negative")
        if dest + count > len(self._buf) or src + count > len(self._buf):
            raise ValueError("source or destination out of range")
        chunk = bytes(self._buf[src:src + count])
        self._buf[dest:dest + count] = chunk

    def flush(self, offset=0, size=None):
        self._check_open()
        if not self._file_backed:
            return
        if self._access == ACCESS_READ:
            return
        if size is None:
            size = len(self._buf) - offset
        if offset < 0 or size < 0 or offset + size > len(self._buf):
            raise ValueError("flush values out of range")
        saved_pos = os.lseek(self._fileno, 0, os.SEEK_CUR)
        try:
            os.lseek(self._fileno, self._offset + offset, os.SEEK_SET)
            view = memoryview(self._buf)[offset:offset + size]
            written = 0
            while written < size:
                n = os.write(self._fileno, view[written:])
                if n == 0:
                    break
                written += n
        finally:
            try:
                os.lseek(self._fileno, saved_pos, os.SEEK_SET)
            except OSError:
                pass

    def resize(self, newsize):
        self._check_resizable()
        if newsize < 0:
            raise ValueError("new size must be non-negative")
        if newsize < len(self._buf):
            del self._buf[newsize:]
        else:
            self._buf.extend(b"\x00" * (newsize - len(self._buf)))
        if self._pos > len(self._buf):
            self._pos = len(self._buf)
        if self._file_backed:
            try:
                os.ftruncate(self._fileno, self._offset + newsize)
            except OSError:
                pass

    def madvise(self, option, start=0, length=None):
        self._check_open()
        return 0

    def __getitem__(self, key):
        self._check_open()
        if isinstance(key, int):
            if key < 0:
                key += len(self._buf)
            if key < 0 or key >= len(self._buf):
                raise IndexError("mmap index out of range")
            return self._buf[key]
        if isinstance(key, slice):
            return bytes(self._buf[key])
        raise TypeError("mmap indices must be integers or slices")

    def __setitem__(self, key, value):
        self._check_writable()
        if isinstance(key, int):
            if key < 0:
                key += len(self._buf)
            if key < 0 or key >= len(self._buf):
                raise IndexError("mmap index out of range")
            if isinstance(value, int):
                self._buf[key] = value
            else:
                b = bytes(value)
                if len(b) != 1:
                    raise IndexError("mmap assignment must be single byte")
                self._buf[key] = b[0]
            return
        if isinstance(key, slice):
            start, stop, step = key.indices(len(self._buf))
            if step != 1:
                raise IndexError("mmap slice assignment must have step 1")
            data = bytes(value)
            if len(data) != stop - start:
                raise IndexError("mmap slice assignment is wrong size")
            self._buf[start:stop] = data
            return
        raise TypeError("mmap indices must be integers or slices")

    def __buffer__(self, flags):
        self._check_open()
        return memoryview(self._buf).__buffer__(flags)
