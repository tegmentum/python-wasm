# Worker entrypoint demonstrating the importable pyhash module over the hashing
# multiplexer capability (Python -> WIT -> Rust, composed via wac).
import wit_world
import pyhash


class WitWorld(wit_world.WitWorld):
    def run(self) -> str:
        out = [
            "algorithms=" + ",".join(pyhash.algorithms()),
            "xxh64('abc')=" + format(pyhash.xxh64(b"abc"), "016x"),
            "crc32('abc')=" + format(pyhash.crc32(b"abc"), "08x"),
            "murmur3('abc')=" + format(pyhash.murmur3(b"abc"), "08x"),
            "blake3('abc')=" + pyhash.blake3(b"abc").hex(),
        ]
        h = pyhash.Hasher("xxh64")
        h.update(b"ab")
        h.update(b"c")
        out.append("stream xxh64('abc')=" + format(h.intdigest(), "016x"))
        return "\n".join(out)
