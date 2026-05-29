

# --- WASI user-id stubs ---------------------------------------------------
# WASI Preview 2 has no user/group model; the posix C extension built for
# wasm doesn't expose getuid/geteuid/getgid/getegid/getppid/getlogin.
# Several pure-Python deps (pip's bundled platformdirs, setuptools, etc.)
# do `from os import getuid` unconditionally at import time. Provide
# stable stub values so those imports succeed.
#
# Values match what a container-rooted single-user wasi guest would
# reasonably report: uid/gid 0, ppid 1, login "wasi".
def getuid():    return 0
def geteuid():   return 0
def getgid():    return 0
def getegid():   return 0
def getppid():   return 1
def getlogin():  return "wasi"
