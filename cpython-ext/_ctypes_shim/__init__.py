"""ctypes stub for python-wasm.

The CPython _ctypes C extension wraps libffi to dispatch foreign function
calls. wasm32-wasip2 has no native ABI to call through and no dlopen
substrate, so _ctypes isn't built. Without _ctypes the stdlib
ctypes/__init__.py fails its very first import line.

This stub replaces stdlib Lib/ctypes/__init__.py with the minimum surface
needed for `import ctypes` to succeed and for the common defensive
pattern `try: import ctypes` (used by psutil, cffi probes, etc.) to fall
through cleanly. Concrete operations that would require a real native ABI
— CDLL / cdll / windll / PYDLL — raise NotImplementedError at the point
of use with a clear message so callers can choose to fall back or skip.

The c_* type classes are present as inert Python stand-ins: `c_int(5)`
constructs an object whose `.value` is 5. They do not provide buffer
protocol, sizeof, or pointer semantics that match libffi. sizeof()
returns the declared `_type_` byte width when known, else raises TypeError.

For real foreign-function support in wasm: link the foreign code as a
WIT-typed component (the tegmentum:* capability pattern) instead of
trying to reach unrestricted native code through ctypes.
"""

__version__ = "1.1.0-wasm-stub"

DEFAULT_MODE = 0
RTLD_LOCAL = 0
RTLD_GLOBAL = 256

_NO_FFI = (
    "ctypes is unavailable in python-wasm (wasm32-wasip2 has no native "
    "ABI / dlopen). For foreign code in wasm, link it as a WIT-typed "
    "component instead."
)


class ArgumentError(Exception):
    pass


class _CData:
    _type_ = None
    _length_ = None

    def __init__(self, value=None):
        self.value = value

    def __repr__(self):
        return f"{type(self).__name__}({self.value!r})"


class _SimpleCData(_CData):
    pass


class _Pointer(_CData):
    pass


class Structure(_CData):
    _fields_ = ()

    def __init__(self, *args, **kwargs):
        for (name, _ctype), value in zip(self._fields_, args):
            setattr(self, name, value)
        for name, value in kwargs.items():
            setattr(self, name, value)


class Union(_CData):
    _fields_ = ()


class Array(_CData):
    _type_ = None
    _length_ = 0


class CFuncPtr(_CData):
    def __init__(self, *_args, **_kwargs):
        raise NotImplementedError(_NO_FFI)


_TYPE_SIZES = {
    "c": 1, "b": 1, "B": 1, "?": 1,
    "h": 2, "H": 2,
    "i": 4, "I": 4, "l": 4, "L": 4, "f": 4,
    "q": 8, "Q": 8, "d": 8,
    "P": 4, "z": 4, "Z": 4, "v": 0, "g": 16,
}


def _simple(name, type_code, py_default=0):
    cls = type(name, (_SimpleCData,), {"_type_": type_code})
    cls.__init__ = lambda self, value=py_default: setattr(self, "value", value)
    return cls


c_bool = _simple("c_bool", "?", False)
c_char = _simple("c_char", "c", b"\x00")
c_byte = _simple("c_byte", "b")
c_ubyte = _simple("c_ubyte", "B")
c_short = _simple("c_short", "h")
c_ushort = _simple("c_ushort", "H")
c_int = _simple("c_int", "i")
c_uint = _simple("c_uint", "I")
c_long = _simple("c_long", "l")
c_ulong = _simple("c_ulong", "L")
c_longlong = _simple("c_longlong", "q")
c_ulonglong = _simple("c_ulonglong", "Q")
c_float = _simple("c_float", "f", 0.0)
c_double = _simple("c_double", "d", 0.0)
c_longdouble = _simple("c_longdouble", "g", 0.0)
c_void_p = _simple("c_void_p", "P", None)
c_char_p = _simple("c_char_p", "z", None)
c_wchar_p = _simple("c_wchar_p", "Z", None)
c_wchar = _simple("c_wchar", "u", "\x00")
c_size_t = c_uint
c_ssize_t = c_int
c_int8 = c_byte
c_uint8 = c_ubyte
c_int16 = c_short
c_uint16 = c_ushort
c_int32 = c_int
c_uint32 = c_uint
c_int64 = c_longlong
c_uint64 = c_ulonglong


_pointer_type_cache = {}


def POINTER(cls):
    if cls in _pointer_type_cache:
        return _pointer_type_cache[cls]
    ptr_cls = type(f"LP_{cls.__name__}", (_Pointer,), {"_type_": cls})
    _pointer_type_cache[cls] = ptr_cls
    return ptr_cls


def pointer(obj):
    cls = POINTER(type(obj))
    p = cls()
    p.contents = obj
    return p


def byref(obj, offset=0):
    return obj


def addressof(obj):
    raise NotImplementedError(_NO_FFI)


def alignment(obj_or_type):
    return 1


def sizeof(obj_or_type):
    type_code = getattr(obj_or_type, "_type_", None)
    if type_code in _TYPE_SIZES:
        return _TYPE_SIZES[type_code]
    raise TypeError("sizeof() unavailable for this type in ctypes stub")


def cast(obj, type_):
    raise NotImplementedError(_NO_FFI)


def memmove(dst, src, count):
    raise NotImplementedError(_NO_FFI)


def memset(dst, c, count):
    raise NotImplementedError(_NO_FFI)


def string_at(address, size=-1):
    raise NotImplementedError(_NO_FFI)


def wstring_at(address, size=-1):
    raise NotImplementedError(_NO_FFI)


_errno = 0


def get_errno():
    return _errno


def set_errno(value):
    global _errno
    prev = _errno
    _errno = value
    return prev


class CDLL:
    def __init__(self, *_args, **_kwargs):
        raise NotImplementedError(_NO_FFI)


class PyDLL(CDLL):
    pass


PYDLL = PyDLL


class _CDLLDict:
    def __getattr__(self, _name):
        raise NotImplementedError(_NO_FFI)

    def __getitem__(self, _name):
        raise NotImplementedError(_NO_FFI)

    def LoadLibrary(self, *_args, **_kwargs):
        raise NotImplementedError(_NO_FFI)


cdll = _CDLLDict()
pydll = _CDLLDict()


def CFUNCTYPE(restype, *argtypes, **kwargs):
    class _FuncProto(CFuncPtr):
        _restype_ = restype
        _argtypes_ = argtypes
    return _FuncProto


PYFUNCTYPE = CFUNCTYPE


class py_object(_SimpleCData):
    _type_ = "O"


__all__ = [
    "ArgumentError", "CDLL", "PyDLL", "PYDLL", "cdll", "pydll",
    "CFUNCTYPE", "PYFUNCTYPE",
    "Structure", "Union", "Array", "POINTER", "pointer", "byref",
    "addressof", "alignment", "sizeof", "cast",
    "memmove", "memset", "string_at", "wstring_at",
    "get_errno", "set_errno",
    "RTLD_LOCAL", "RTLD_GLOBAL", "DEFAULT_MODE",
    "py_object",
    "c_bool", "c_char", "c_byte", "c_ubyte", "c_short", "c_ushort",
    "c_int", "c_uint", "c_long", "c_ulong", "c_longlong", "c_ulonglong",
    "c_float", "c_double", "c_longdouble",
    "c_void_p", "c_char_p", "c_wchar_p", "c_wchar",
    "c_size_t", "c_ssize_t",
    "c_int8", "c_uint8", "c_int16", "c_uint16",
    "c_int32", "c_uint32", "c_int64", "c_uint64",
]
