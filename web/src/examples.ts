export interface Example {
  name: string
  code: string
}

export const examples: Example[] = [
  {
    name: 'Hello World',
    code: 'print("Hello from CPython on WASI P2!")',
  },
  {
    name: 'Fibonacci',
    code: `def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a

for i in range(10):
    print(f"fib({i}) = {fib(i)}")`,
  },
  {
    name: 'System Info',
    code: `import sys
import platform

print(f"Python {sys.version}")
print(f"Platform: {platform.platform()}")
print(f"Machine: {platform.machine()}")
print(f"Byte order: {sys.byteorder}")
print(f"Max int: {sys.maxsize}")`,
  },
  {
    name: 'List Comprehensions',
    code: `# Pythagorean triples with a, b, c <= 20
triples = [
    (a, b, c)
    for a in range(1, 21)
    for b in range(a, 21)
    for c in range(b, 21)
    if a**2 + b**2 == c**2
]

for a, b, c in triples:
    print(f"{a}^2 + {b}^2 = {c}^2  ({a**2} + {b**2} = {c**2})")`,
  },
  {
    name: 'JSON Processing',
    code: `import json

data = {
    "name": "CPython WASI",
    "version": "3.14",
    "features": ["wasm", "browser", "component-model"],
    "nested": {"key": [1, 2, 3]}
}

formatted = json.dumps(data, indent=2)
print(formatted)

# Round-trip
parsed = json.loads(formatted)
print(f"\\nFeatures: {', '.join(parsed['features'])}")`,
  },
  {
    name: 'Math & Statistics',
    code: `import math
import statistics

data = [2, 4, 4, 4, 5, 5, 7, 9]

print(f"Mean:   {statistics.mean(data)}")
print(f"Median: {statistics.median(data)}")
print(f"Stdev:  {statistics.stdev(data):.4f}")
print(f"Var:    {statistics.variance(data):.4f}")
print()
print(f"pi:     {math.pi}")
print(f"e:      {math.e}")
print(f"tau:    {math.tau}")
print(f"sqrt(2): {math.sqrt(2):.10f}")`,
  },
  {
    name: 'Error Handling',
    code: `import traceback
import io

def divide(a, b):
    return a / b

# Successful call
print(f"10 / 3 = {divide(10, 3):.4f}")

# Caught exception
try:
    divide(1, 0)
except ZeroDivisionError as e:
    print(f"Caught: {e}")

# Traceback formatting
try:
    divide("a", "b")
except TypeError:
    buf = io.StringIO()
    traceback.print_exc(file=buf)
    print(buf.getvalue())`,
  },
]
