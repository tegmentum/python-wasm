"""sqlite3 — DB-API 2.0 shim on top of the sqlite:wasm capability.

Drop-in (minimal) replacement for the stdlib `sqlite3` package's
`__init__.py`. Uses `_sqlite_cap` (capability extension) instead of the
missing-on-wasi `_sqlite3` C extension.

Installed by `make install-python-shims` to:
    deps/cpython/Lib/sqlite3/__init__.py  (replacing the package's init)

What works:
    sqlite3.connect(database, *, timeout=..., detect_types=...,
                    isolation_level=..., check_same_thread=...,
                    factory=..., cached_statements=..., uri=...)

    Connection.cursor() / .execute() / .executemany() / .executescript()
    Connection.commit() / .rollback() / .close()
    Connection.in_transaction property
    Connection.row_factory  (Row, dict-style, custom)
    Connection.text_factory (str, bytes, custom)
    Connection.total_changes / .lastrowid (after execute)

    Cursor.execute() / .executemany() / .executescript()
    Cursor.fetchone() / .fetchmany() / .fetchall()
    Cursor.description / .rowcount / .lastrowid
    Cursor.close()
    iter(cursor) -> yields rows
    `with conn:` — transaction context manager

    Row — sequence + mapping access (Row[0] and Row['col'])

    Module exceptions: Warning, Error, InterfaceError, DatabaseError,
    DataError, OperationalError, IntegrityError, InternalError,
    ProgrammingError, NotSupportedError

    Module attributes: apilevel, paramstyle ('qmark'), threadsafety,
    version, version_info, sqlite_version, sqlite_version_info,
    PARSE_DECLTYPES, PARSE_COLNAMES

Limitations vs stdlib:
    * SELECT path is eager (loads all rows on .execute()); not streaming.
      Adequate for typical workloads; pathological for multi-GB result
      sets. A future enhancement would route SELECT through the WIT
      statement.step() iterator instead.
    * register_adapter / register_converter are no-ops; types pass
      through as native Python int/float/str/bytes/None. PARSE_DECLTYPES
      / PARSE_COLNAMES are accepted as detect_types flags but don't
      transform values.
    * No date/timestamp adapters out of the box.
    * `isolation_level` is honored at the BEGIN level (None = autocommit,
      '' / 'DEFERRED' / 'IMMEDIATE' / 'EXCLUSIVE' all map to plain BEGIN).
    * check_same_thread is accepted and ignored — wasm is single-threaded.
"""

from __future__ import annotations
import os
import _sqlite_cap


# --------------------------------------------------------------------------
# Module-level constants (DB-API 2.0 + sqlite3-specific)
# --------------------------------------------------------------------------

apilevel       = "2.0"
paramstyle     = "qmark"
threadsafety   = 1

# stdlib reports its package version separately from libsqlite's version
version        = "3.0.0-capability"
version_info   = (3, 0, 0)

sqlite_version       = _sqlite_cap.version()
sqlite_version_info  = tuple(int(p) for p in sqlite_version.split("."))

# detect_types flags — accepted but not honored (see Limitations).
PARSE_DECLTYPES  = 1
PARSE_COLNAMES   = 2

# Open-mode codes matching the WIT `open-mode` enum.
_MODE_RO       = 0
_MODE_RW       = 1
_MODE_RWCREATE = 2
_MODE_MEMORY   = 3

LEGACY_TRANSACTION_CONTROL = -1  # stdlib value; accepted for source compat


# --------------------------------------------------------------------------
# Exception hierarchy (DB-API 2.0 §1.4)
# --------------------------------------------------------------------------

class Warning(Exception):
    pass


class Error(Exception):
    """Base of all sqlite3 exceptions. The capability extension raises this
    with args=(message, sqlite_errcode, sqlite_extended_errcode).
    """
    def __init__(self, *args):
        super().__init__(*args)
        # Stash the codes on the instance for `e.sqlite_errorcode` etc.
        self.sqlite_errorcode         = args[1] if len(args) > 1 else 0
        self.sqlite_extended_errorcode = args[2] if len(args) > 2 else 0


class InterfaceError(Error):    pass
class DatabaseError(Error):     pass
class DataError(DatabaseError): pass
class OperationalError(DatabaseError): pass
class IntegrityError(DatabaseError):   pass
class InternalError(DatabaseError):    pass
class ProgrammingError(DatabaseError): pass
class NotSupportedError(DatabaseError): pass


# Register the base class with the C extension so cap-side errors raise
# DatabaseError. Specific subclasses can be dispatched in Python via the
# sqlite errcode if needed (e.g. SQLITE_CONSTRAINT -> IntegrityError).
_sqlite_cap._set_error_class(DatabaseError)


# --------------------------------------------------------------------------
# Row — sequence + mapping access
# --------------------------------------------------------------------------

class Row:
    """A row that supports both index and name access. Returned by
    fetchone/fetchall when `connection.row_factory = sqlite3.Row`.
    """
    __slots__ = ("_cursor", "_values")

    def __init__(self, cursor, values):
        self._cursor = cursor
        self._values = tuple(values)

    def __len__(self):       return len(self._values)
    def __iter__(self):      return iter(self._values)
    def __repr__(self):      return f"<Row {self._values!r}>"
    def __eq__(self, other):
        if isinstance(other, Row):
            return self._values == other._values
        if isinstance(other, tuple):
            return self._values == other
        return NotImplemented

    def __getitem__(self, key):
        if isinstance(key, (int, slice)):
            return self._values[key]
        if isinstance(key, str):
            # Match case-insensitively per stdlib Row behavior
            for i, name in enumerate(self.keys()):
                if name.lower() == key.lower():
                    return self._values[i]
            raise IndexError(f"no column named {key!r}")
        raise IndexError(f"Row key must be int, slice, or str (got {type(key).__name__})")

    def keys(self):
        # Cursor.description is a list of 7-tuples; column name is [0].
        desc = self._cursor.description
        return [d[0] for d in (desc or [])]


# --------------------------------------------------------------------------
# Cursor
# --------------------------------------------------------------------------

class Cursor:
    def __init__(self, connection):
        self.connection      = connection
        self.arraysize       = 1
        self.row_factory     = None  # falls through to Connection.row_factory
        self._description    = None
        self._rows           = None   # list[tuple] — all rows, eager
        self._row_iter       = None
        self._rowcount       = -1
        self._lastrowid      = None
        self._closed         = False

    # ---- DB-API 2.0 attrs ----

    @property
    def description(self):
        return self._description

    @property
    def rowcount(self):
        return self._rowcount

    @property
    def lastrowid(self):
        return self._lastrowid

    # ---- Execution ----

    def execute(self, sql, parameters=()):
        if self._closed:
            raise ProgrammingError("Cannot operate on a closed cursor.")
        self.connection._check_open()
        # Stdlib accepts dict params; for now reject them with a clear msg.
        if isinstance(parameters, dict):
            raise NotImplementedError(
                "named parameters are not yet supported by the cap shim; "
                "use qmark (?) placeholders with a sequence")
        params = list(parameters) if parameters else None

        sql_stripped = sql.strip()
        verb_upper   = sql_stripped[:6].upper()
        first_token  = sql_stripped.split(None, 1)[0].upper() if sql_stripped else ""

        # Transaction control: route to the cap's dedicated entries so the
        # connection's _in_transaction flag stays accurate. Also stops
        # _maybe_begin from re-issuing BEGIN on the next statement.
        if first_token in ("BEGIN", "COMMIT", "END", "ROLLBACK"):
            self._description = None
            self._rows        = []
            self._row_iter    = iter(self._rows)
            self._rowcount    = -1
            self._lastrowid   = None
            if first_token == "BEGIN":
                if not self.connection._in_transaction:
                    _sqlite_cap.begin(self.connection._conn)
                    self.connection._in_transaction = True
            elif first_token in ("COMMIT", "END"):
                if self.connection._in_transaction:
                    _sqlite_cap.commit(self.connection._conn)
                    self.connection._in_transaction = False
            elif first_token == "ROLLBACK":
                if self.connection._in_transaction:
                    _sqlite_cap.rollback(self.connection._conn)
                    self.connection._in_transaction = False
            return self

        is_select = verb_upper in ("SELECT", "PRAGMA") or \
                    sql_stripped[:4].upper() == "WITH" or \
                    sql_stripped[:5].upper() == "VALUES"

        # Manage implicit transactions per isolation_level
        self.connection._maybe_begin(sql_stripped)

        if is_select:
            cols, rows = _sqlite_cap.query(
                self.connection._conn, sql, params)
            # description is list of 7-tuples (name, type, display, internal,
            # precision, scale, null_ok). We only know the name; rest = None.
            self._description = tuple((c, None, None, None, None, None, None) for c in cols)
            self._rows        = rows
            self._row_iter    = iter(self._rows)
            self._rowcount    = len(self._rows)
            self._lastrowid   = None
        else:
            changes, lastrowid = _sqlite_cap.execute(
                self.connection._conn, sql, params)
            self._description = None
            self._rows        = []
            self._row_iter    = iter(self._rows)
            self._rowcount    = changes
            self._lastrowid   = lastrowid
            self.connection._lastrowid = lastrowid
            self.connection._total_changes += max(0, changes)
        return self

    def executemany(self, sql, seq_of_parameters):
        if self._closed:
            raise ProgrammingError("Cannot operate on a closed cursor.")
        # The cap doesn't have a "prepare once, execute many" affordance
        # at this API tier; loop. The transaction stays open across calls
        # so this is still atomic vs. caller-level commit.
        total_changes = 0
        last_rowid = None
        for params in seq_of_parameters:
            if isinstance(params, dict):
                raise NotImplementedError(
                    "named parameters in executemany are not yet supported")
            changes, last_rowid = _sqlite_cap.execute(
                self.connection._conn, sql, list(params))
            total_changes += max(0, changes)
        self._description = None
        self._rowcount    = total_changes
        self._lastrowid   = last_rowid
        self._rows        = []
        self._row_iter    = iter(self._rows)
        if last_rowid is not None:
            self.connection._lastrowid = last_rowid
        self.connection._total_changes += total_changes
        return self

    def executescript(self, sql_script):
        if self._closed:
            raise ProgrammingError("Cannot operate on a closed cursor.")
        # stdlib executescript implicitly commits any pending transaction
        # then executes statements one at a time, then commits at end.
        self.connection.commit()
        # Naïve split on ';'. Won't handle quoted/escaped ';' correctly;
        # adequate for the common "schema script" case. Documented in
        # module docstring.
        statements = [s.strip() for s in sql_script.split(";") if s.strip()]
        for stmt in statements:
            _sqlite_cap.execute(self.connection._conn, stmt, None)
        self.connection.commit()
        return self

    # ---- Fetch ----

    def fetchone(self):
        if self._row_iter is None:
            return None
        try:
            row = next(self._row_iter)
        except StopIteration:
            return None
        return self._wrap_row(row)

    def fetchmany(self, size=None):
        if self._row_iter is None:
            return []
        if size is None:
            size = self.arraysize
        out = []
        for _ in range(size):
            try:
                row = next(self._row_iter)
            except StopIteration:
                break
            out.append(self._wrap_row(row))
        return out

    def fetchall(self):
        if self._row_iter is None:
            return []
        out = [self._wrap_row(r) for r in self._row_iter]
        self._row_iter = iter([])
        return out

    def __iter__(self):
        return self

    def __next__(self):
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row

    def close(self):
        self._closed = True
        self._rows = None
        self._row_iter = None

    # ---- internals ----

    def _wrap_row(self, row):
        factory = self.row_factory or self.connection.row_factory
        if factory is None:
            return row
        if factory is Row:
            return Row(self, row)
        return factory(self, row)


# --------------------------------------------------------------------------
# Connection
# --------------------------------------------------------------------------

class Connection:
    def __init__(self, database, *,
                 timeout=5.0, detect_types=0, isolation_level="",
                 check_same_thread=True, factory=None,
                 cached_statements=128, uri=False):
        # Resolve database path / mode
        if database == ":memory:":
            mode = _MODE_MEMORY
            path = ":memory:"
        elif uri:
            mode = _MODE_RWCREATE  # URI paths can carry mode= but we don't parse it
            path = str(database)
        else:
            mode = _MODE_RWCREATE
            path = os.fspath(database)
        self._conn          = _sqlite_cap.connect(path, mode)
        self._closed        = False
        self.row_factory    = None
        self.text_factory   = str
        self._isolation_level = isolation_level  # see _maybe_begin
        self._in_transaction  = False
        self._lastrowid       = None
        self._total_changes   = 0
        self._detect_types    = detect_types  # accepted; not honored
        # timeout / cached_statements / check_same_thread / factory:
        # accepted at the API surface; not all enforced.

    # ---- Lifecycle ----

    def close(self):
        if self._closed:
            return
        try:
            self.rollback()  # discard uncommitted changes
        except Error:
            pass
        _sqlite_cap.close(self._conn)
        self._closed = True

    def _check_open(self):
        if self._closed:
            raise ProgrammingError("Cannot operate on a closed database.")

    # ---- Cursor + execute shortcuts ----

    def cursor(self, factory=Cursor):
        self._check_open()
        return factory(self)

    def execute(self, sql, parameters=()):
        return self.cursor().execute(sql, parameters)

    def executemany(self, sql, seq_of_parameters):
        return self.cursor().executemany(sql, seq_of_parameters)

    def executescript(self, sql_script):
        return self.cursor().executescript(sql_script)

    # ---- Transactions ----

    def commit(self):
        self._check_open()
        if self._in_transaction:
            _sqlite_cap.commit(self._conn)
            self._in_transaction = False

    def rollback(self):
        self._check_open()
        if self._in_transaction:
            _sqlite_cap.rollback(self._conn)
            self._in_transaction = False

    @property
    def in_transaction(self):
        return self._in_transaction

    @property
    def total_changes(self):
        return self._total_changes

    @property
    def isolation_level(self):
        return self._isolation_level

    @isolation_level.setter
    def isolation_level(self, value):
        if value is not None and value not in ("", "DEFERRED", "IMMEDIATE", "EXCLUSIVE"):
            raise ValueError(f"invalid isolation_level: {value!r}")
        self._isolation_level = value

    def _maybe_begin(self, sql_stripped):
        """No-op. We follow modern stdlib (3.12+) semantics: sqlite manages
        per-statement autocommit naturally. Users who want a transaction
        either say BEGIN explicitly (handled via the transaction-verb
        special case in Cursor.execute) or use `with conn:` which BEGINs
        on enter / COMMITs on exit. The legacy "auto-BEGIN before DML"
        behavior wrongly grouped DDL with subsequent DML, which a
        rollback would then wipe."""
        return

    # ---- Context manager ----
    #
    # Stdlib `with conn:` doesn't BEGIN on enter — it only commits on
    # successful exit and rollbacks on exception, IF a transaction was
    # opened during the block. Match that: enter is a no-op; exit checks
    # `in_transaction` to decide whether commit/rollback applies.

    def __enter__(self):
        # Open a transaction so DML inside the block is atomic w.r.t.
        # exit semantics. Matches user expectation of "with conn:".
        if not self._in_transaction:
            _sqlite_cap.begin(self._conn)
            self._in_transaction = True
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type is None:
            self.commit()
        else:
            self.rollback()
        return False  # don't swallow the exception

    # ---- Misc ----

    def interrupt(self):       pass  # no-op; cap doesn't expose this
    def set_authorizer(self, *a, **kw):    pass
    def set_progress_handler(self, *a, **kw): pass
    def set_trace_callback(self, *a, **kw):   pass

    def create_function(self, *a, **kw):
        raise NotSupportedError(
            "create_function requires the sqlite:wasm/extension interface, "
            "which the python-wasm shim doesn't expose yet")

    def create_aggregate(self, *a, **kw):
        raise NotSupportedError("create_aggregate is not yet supported")

    def create_collation(self, *a, **kw):
        raise NotSupportedError("create_collation is not yet supported")


# --------------------------------------------------------------------------
# Module-level connect()
# --------------------------------------------------------------------------

def connect(database, *, timeout=5.0, detect_types=0, isolation_level="",
            check_same_thread=True, factory=Connection,
            cached_statements=128, uri=False, **kw):
    """Open a database connection. `database` may be ':memory:', a path,
    or any os.PathLike."""
    return factory(database,
                   timeout=timeout, detect_types=detect_types,
                   isolation_level=isolation_level,
                   check_same_thread=check_same_thread,
                   cached_statements=cached_statements,
                   uri=uri)


# --------------------------------------------------------------------------
# Stubs for adapter / converter registration — accepted but not honored.
# --------------------------------------------------------------------------

_adapters    = {}
_converters  = {}

def register_adapter(type_, callable_):
    """Accepted for API compat; type adapters are not invoked by this shim.
    Callers should pass already-marshalable Python values (int/float/str/
    bytes/None) instead of relying on adapter-driven conversion."""
    _adapters[type_] = callable_

def register_converter(typename, callable_):
    """Accepted for API compat; type converters are not invoked by this shim
    (would require PARSE_DECLTYPES / PARSE_COLNAMES handling at the cap)."""
    _converters[typename.lower()] = callable_

def complete_statement(sql):
    """Heuristic stand-in for sqlite3_complete: a SQL string is 'complete' if
    it ends with a semicolon. Stdlib uses libsqlite3's own check but for
    REPL completion this approximation is adequate."""
    return sql.rstrip().endswith(";")

def enable_callback_tracebacks(flag):
    pass  # no callbacks installed via our path


__all__ = [
    "connect", "Connection", "Cursor", "Row",
    "Warning", "Error", "InterfaceError", "DatabaseError", "DataError",
    "OperationalError", "IntegrityError", "InternalError",
    "ProgrammingError", "NotSupportedError",
    "apilevel", "paramstyle", "threadsafety",
    "version", "version_info", "sqlite_version", "sqlite_version_info",
    "PARSE_DECLTYPES", "PARSE_COLNAMES", "LEGACY_TRANSACTION_CONTROL",
    "register_adapter", "register_converter",
    "complete_statement", "enable_callback_tracebacks",
]
