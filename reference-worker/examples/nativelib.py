"""A stand-in "native-only" package: not built for wasip2, so the wasip2
interpreter can't import it directly. The offload worker runs it on a host
interpreter and returns the result across the boundary. Reporting `sys.platform`
makes it obvious the call executed off-box, not in the wasip2 caller."""

import sys


def crunch(x):
    return {"squared": x * x, "ran_on": sys.platform, "py": sys.version.split()[0]}
