"""Runs inside the wasip2 python.wasm.

Routes the `nativelib` package through the py-offload import hook to a host worker
over the file mailbox at /work/mailbox, then calls it as if it were local — even
though `nativelib` is not importable in this interpreter at all.
"""

import sys

from py_offload.importhook import install
from py_offload.mailbox import MailboxClient


def main():
    client = MailboxClient("/work/mailbox", timeout=30)
    with install(["nativelib"], client):
        import nativelib  # resolved by the offload hook, not a real wasip2 module

        result = nativelib.crunch(7)
    print(f"offloaded nativelib.crunch(7) = {result}")
    print(f"caller interpreter: {sys.platform}  (the call ran on {result['ran_on']})")


if __name__ == "__main__":
    main()
