# fiber-interop/ext-scope

The `Scope` class from the [PHP RFC: Scope](https://wiki.php.net/rfc/scope), as an extension for
PHP 8.2 and later. A userland polyfill with the same class is `fiber-interop/scope`.

A `Scope` identifies the logical operation running code belongs to, such as an HTTP request, a
queued job or a transaction, and keeps that identity across fibers and generators. Service
containers can key request-scoped services by it, and libraries can keep per-operation state
without depending on a particular event loop.

```php
final class Scope
{
    public function __construct() {}
    public function run(callable $callback, mixed ...$args): mixed {}
    public function bind(Closure $closure): Closure {}
    public static function getCurrent(): Scope {}
    public static function getRootScope(): Scope {}
}

Fiber::getScope(): Scope;       // added by the extension
Generator::getScope(): Scope;   // added by the extension
```

```php
$scope = new Scope();
$scope->run(function () {
    Scope::getCurrent();          // $scope
    $fiber = new Fiber(function () {
        Scope::getCurrent();      // $scope: fibers join the scope they are constructed in
    });
    $fiber->start();
});
Scope::getCurrent();              // Scope::getRootScope()
```

The root scope is one object for the whole request (process, on the CLI). It is current in
`{main}` outside any `run()`, and in fibers and generators created there, so `getCurrent()` and
`getScope()` never return `null`. "Am I inside an operation" is
`Scope::getCurrent() !== Scope::getRootScope()`. To start something that belongs to no request
from inside one: `Scope::getRootScope()->run(fn() => /* start the service */)`.

## Release 0.4.4

0.4.4 implements the session locking rule of the Scope RFC 0.9.3: a session whose save handler locks it is open in
one scope of a process at a time, and `session_start()` throws a `RuntimeException` when the session id is open in
another scope, instead of blocking the whole process on a lock only that scope can release (see the session section
below).

## Release 0.4.3

0.4.3 builds and works on thread-safe (ZTS) PHP, where it previously did not compile. The session and the locale
follow the scope there too, and new tests run scopes in several threads at once with the
[parallel](https://github.com/krakjoe/parallel) extension; CI builds ZTS as well as NTS for PHP 8.2 to 8.5. It also
fixes `unset($_GET)` (or any of the six) inside a scope, which removed the superglobal from every scope instead of only
that one. Switching is cheaper: a superglobal that both scopes leave empty is not moved, the session is not moved when
neither scope has one, and the C-side copies of the request arrays are updated without feeding the cycle collector. A
fiber resumed from the root into another scope went from about 322 ns to about 260 ns (PHP 8.5, opcache on, JIT off);
within one scope nothing changed.

## Release 0.4.2

0.4.2 makes the session settings request-wide, as the RFC specifies: `session_name()`, `session_set_cookie_params()`,
`session_cache_limiter()` and `ini_set()` of `session.*` change them for every scope, and a new scope's save handler is
the current `session.save_handler`. Only the session itself (its id, status, save handler state and `$_SESSION`) is per
scope. Switching scopes is also faster: a generator step that crosses scopes went from about 530 ns to about 230 ns
(PHP 8.5, opcache on, JIT off), and steps within one scope cost what they did.

## Release 0.4.1

0.4.1 fixes a crash in generators whose `finally` block ends with a `yield`, when an exception or a `return`
enters the block (found by running php-src's tests with the extension loaded: Zend, standard, session, date, intl,
reflection and SPL now fail the same tests with and without it). The extension also allocates no object until the
first `Scope` is created, so object ids (`#1`, `spl_object_id()`) are the same as without it in code that never uses
one. And it fixes the session's end when a scope is destroyed: it now runs in the destructor phase, so user save handlers
run safely, also when the scope is collected as a cycle or is still alive at request shutdown (this fixes a
use-after-free when ending the session released the last reference to the scope). `Scope` reports its state to the
cycle collector, and `$_SESSION` is per scope like the other superglobals.

## Release 0.4.0

0.4.0 implements the Scope RFC 0.9:

- **A new scope starts empty** (vote A): `$_GET`, `$_POST`, `$_COOKIE`, `$_FILES`, `$_REQUEST` and `$_SERVER` are
  empty arrays in a new `Scope`, instead of copy-on-write copies of its creator's. The root scope keeps the SAPI's
  arrays; fibers and generators still join the scope they are created in.
- **The session follows the scope** (vote A, ini `scope.session`): see "Session" below.
- **Locale and timezone start from ini** (vote B): a new scope starts with the `date.timezone` ini (or its
  fallback) and the `intl.default_locale` ini, not with its creator's values.
- **`Scope::bind(Closure $closure): Closure`**: see "bind()" below.

Code that relied on a new scope inheriting its creator's superglobals, timezone or locale must now seed the scope.

## Release 0.3.1

0.3.1 removes the per-call cost of loading the extension: generators now switch scope through calls compiled into generator bodies only, instead of an fcall observer that put every function call on PHP's observed path (#3). Same spec level as 0.3.0.

## Release 0.3.0

0.3.0 is the "Scope spec level": the RFC draft plus both secondary votes. It contains the `Scope` class (`getRootScope()`, `getCurrent()`, `run()`), `Fiber::getScope()` and `Generator::getScope()`, per-scope request superglobals (vote A, ini `scope.superglobals`), per-scope default timezone and locale (vote B, ini `scope.locale_timezone`), and PIE metadata (`composer.json`, `config.m4`). The polyfill `fiber-interop/scope` 0.3.0 implements the same level.

## Install

- **PIE**: `pie install fiber-interop/scope`
- **PECL**: `pecl install scope`
A project that declares its own global `Scope` class cannot install this package.

## Rules

- `$scope->run($callback)` makes `$scope` current while `$callback` runs, and restores the
  previous scope afterwards, also on exceptions and fatal errors.
- A fiber belongs to the scope current when it is constructed; a generator to the scope current
  when the generator function is called. Wherever either is resumed from, it runs in its own
  scope.
- Functions and closures run in their caller's scope.
- The scope never changes within an executing function, except through `run()` calls it makes:
  across a `yield` or a `Fiber::suspend()`, `Scope::getCurrent()` returns what it returned before.
- A scope carries no data. Keep per-scope state in a `WeakMap` keyed by the scope; entries go
  away once the scope is unreachable.

The RFC has the full motivation, the rules and prior art in other languages.

## Request superglobals (secondary vote A)

`$_GET`, `$_POST`, `$_COOKIE`, `$_FILES`, `$_REQUEST` and `$_SERVER` follow the scope:

- In the root scope they are the SAPI's arrays, so code that never creates a scope sees today's behaviour.
- A new scope starts with six empty arrays, wherever it is created; whoever creates it seeds them, as a server
  does for each request. The creator's arrays are not copied.
- An assignment or write inside a scope changes only that scope's array. Reads see the current scope's array;
  every fiber and generator of the scope shares it. `$_REQUEST` is an array like the others, not a live merge.
- `$_SERVER` carries both the request (`REQUEST_URI`, `HTTP_*`, ...) and the environment (`PATH`,
  `DOCUMENT_ROOT`, ...); both follow the scope the same way, so a new scope's `$_SERVER` starts empty, environment
  entries included (read what you need from the root's `$_SERVER` before seeding).
- `$_ENV` describes the process and is not in scope. `$_SESSION` follows the scope's session (below).

When the current scope changes (`run()` entering or leaving, a fiber switch, a generator resuming or yielding) the
six arrays are moved between the scope's storage and the live `EG(symbol_table)` entries, which is where compiled
`$_GET` accesses read and write, and `PG(http_globals)[TRACK_VARS_*]` is re-pointed at the same array for C code
such as `filter_input()`. The cost of a switch is six symbol-table lookups and pointer moves; no array is copied.
With `auto_globals_jit` (the default), `$_SERVER` and `$_REQUEST` are built only when a compiled file first uses
them. The first scope creation, and every switch, makes the engine build them first, so a file compiled later inside
a scope can't have its build overwrite the scope's array. Before the first `Scope`, the lazy build is unchanged.

`scope.superglobals` (default `1`, `PHP_INI_SYSTEM`) turns this off, so the vote can be shown independently
of the base proposal: with `0`, all scopes share the SAPI's arrays (and the session, see below).

### Session

When ext/session is loaded (it is an optional dependency), its request state follows the scope too:

- Swapped at each scope change: `$_SESSION`, the scope's own like the other superglobals (a new scope has none, and
  it keeps its array after `session_write_close()`, `session_abort()` or `session_destroy()`, as in a request), the session id,
  status, the save handler module and its data (`mod`, `mod_data`), user save handlers
  (`session_set_save_handler()`: the handler callables, class name, open/implemented flags), the serialized original
  data (`session_vars`), `send_cookie`, `define_sid`, `in_save_handler`, `set_handler` and the "started at" file and
  line. These are the `PS()` globals ext/session's RINIT and RSHUTDOWN reset; phasync's `sapi()` swaps the same set.
- Not swapped: the session's settings. The `session.*` ini entries, and `session_name()`, `session_save_path()`,
  `session_set_cookie_params()` and `session_cache_limiter()`/`session_cache_expire()` that write them, stay
  request-wide, as `ini_set()` does. A started session can't have them changed (ext/session refuses while it is
  active), but a scope without an active session can change them for the whole request, so a started session that
  reads them again (`session_regenerate_id()`, the getters) sees the request's current values.
- A new scope has no session (`PHP_SESSION_NONE`) and the save handler of the current `session.save_handler` setting
  (not a handler the creator set with `session_set_save_handler()`).
- `session_start()` inside a scope starts that scope's session; all its fibers and generators share it.
- A scope destroyed with its session active writes and closes it, as request end does. This happens in the
  destructor phase, so a user save handler's `write()` and `close()` run as a destructor would: when the last
  reference goes, before `gc_collect_cycles()` frees a cycle the scope is in, or at request shutdown for scopes
  still alive, before ext/session's own shutdown handles the root's. An exception from the handler propagates as one
  from a destructor. `Scope` reports its superglobals, `$_SESSION` and save handler to the cycle collector.

- **A locking session is open in one scope of a process at a time.** The files handler holds an `flock()` on the
  session file while the session is open, and memcached and redis lock too when their session locking is on. If two
  scopes in one process opened the same session id at once (a browser's parallel requests do), the second
  `session_start()` would wait for a lock only the first scope can release, while that scope can't run. Instead,
  `session_start()` throws a `RuntimeException` when the session id is open in another scope of the process and the
  save handler locks; the message does not contain the id. A user handler built on `SessionHandler` locks when the
  handler it wraps does; other user handlers are taken not to lock, and behave as under php-fpm. A lock held by
  another process or thread is waited for as before. `read_and_close` does not avoid the error while the other scope
  holds the session, because reading takes the lock; it is not applied implicitly. The application or its runtime
  decides what to do, for example retry after the other scope closes the session.

`scope.session` (default `1`, `PHP_INI_SYSTEM`) turns only this off; it is also off when `scope.superglobals=0`.

## Default locale and timezone (secondary vote B)

`date_default_timezone_get()/set()` (ext/date) and `Locale::getDefault()/setDefault()` (ext/intl, only when intl is
loaded) follow the scope with the same rules as the superglobals: the root scope's values are today's (from ini and
the SAPI), a new scope starts with the ini values (`date.timezone`, or its fallback when unset; `intl.default_locale`,
or ICU's default at request start when unset), not with its creator's, a change
inside a scope affects only that scope, and code outside any scope sees no change. Fibers and generators of a scope
share its values.

How it is done, and what a switch costs:

- **Timezone**: the string `date_default_timezone_set()` writes (ext/date's `timezone` global, `NULL` meaning "use
  the `date.timezone` ini") is swapped as a pointer. ext/date does not export its globals, but `php_date.h` has the
  struct and the module entry carries its address, found with `zend_hash_str_find_ptr(&module_registry, "date")`
  at startup. Each scope owns a private copy, because `date_default_timezone_set()` frees
  the old string. A switch is one pointer swap; no allocation.
- **Locale**: ext/intl keeps no state of its own: `Locale::setDefault()` writes the `intl.default_locale` ini entry
  and `Locale::getDefault()` reads it. The extension holds a reference to the outgoing scope's value and, when the
  incoming scope's value differs, writes the entry exactly as `Locale::setDefault()` does (`zend_alter_ini_entry`).
  intl's globals (and headers) are not reachable from another extension, and going through the ini entry also keeps
  the engine's ini restore at request end correct. When two scopes have the same locale a switch is a string
  comparison; when they differ it allocates the new ini string (the one cost above two words).
- `ini_set('date.timezone')` is a request-wide ini, not scope state. A scope that has set its own timezone is not
  affected by it; a scope that never set one uses the ini value, which is the same for all of them.

`scope.locale_timezone` (default `1`, `PHP_INI_SYSTEM`) turns this off independently of `scope.superglobals`:
with `0`, all scopes share one timezone and one locale.

## bind()

`$scope->bind($closure)` returns a new closure that runs `$closure` with `$scope` current, exactly as `run()` would,
whoever calls it and from whichever scope: the previous scope is restored when it returns or throws, and calls nest.
Generators and fibers created inside such a call belong to `$scope`. `$closure` itself is unchanged and still runs in
its caller's scope. Binding a closure that `bind()` returned replaces its scope rather than nesting:
`$a->bind($b->bind($fn))` runs `$fn` in `$a`.

The returned closure is a native wrapper that uses `$closure`'s own parameter info, so it keeps its signature
(by-reference parameters, defaults, variadics, return type, named arguments) and its `$this` and class scope; it is not
a variadic `fn(...$a) => $scope->run($closure, ...$a)`, which would lose by-reference parameters. Reflection reports
it as an internal function with `$closure`'s parameters; default values of its parameters are not visible to
Reflection, and it does not return by reference.

## Per-scope state (prototype)

A follow-up to the RFC plans an internal API with two layers, so that extensions can give implicit state one
copy per scope without changing anything for code outside scopes. This extension implements both,
each with one user that is off by default.

**Slots** are for new code: an extension allocates a slot at startup, and `scope_slot()` returns
the current scope's copy of its state, created on first use. Outside any scope it returns the copy
of the root scope, so extension code never handles a missing scope; `Scope::getRootScope()` is
that root scope's object, and `getRootScope()->run()` makes the root's state current. A change of scope updates a
pointer. The demonstration user is a per-scope counter:

```ini
scope.slot_demo = 1    ; adds scope_demo_counter(): int
```

**Swapped globals** are a bridge for state that stays in an extension's globals: the globals always
hold the current scope's copy, and are swapped when execution moves between scopes, so code that
reads them directly keeps working. The demonstration user is `json_last_error()`:

```ini
scope.isolate_json_errors = 1
```

With it, `json_last_error()` and `json_last_error_msg()` are per scope: a new scope starts with no
error, the fibers of one scope share it, and requests no longer see each other's last error. Code
outside any scope sees the global, as without the extension. This prototype is built for NTS only; on ZTS builds the setting has no effect. Swapping costs roughly 25–55 ns per
switch between fibers of different scopes; switches within one scope cost a pointer comparison.

## Build

```sh
phpize
./configure --enable-scope
make
make test
php -d extension=modules/scope.so your-script.php
```

## Cost

An extension can't add fields to the engine's fiber and generator objects, so this one keeps each
fiber's and generator's scope in a `WeakMap`. It follows fiber switches with the engine's fiber
observer. For generators it compiles scope switches into generator bodies only (a `zend_extension`
op_array handler, registered by the extension itself): the body switches to the generator's scope
when it starts, after each `yield` and at each `catch` and `finally`, and back to the resumer's scope
before each `yield`, `yield from` and `return`. An exception escaping the body is handled where the
generator is resumed (the `Generator` methods, its iterator for `foreach`, its destructor). Every other
function compiles exactly as without the extension, and there is no function call observer, so ordinary
calls cost nothing extra and the JIT stays on. Measured on PHP 8.5 (x86-64, `php -n`):

| | Without | With |
|---|---|---|
| Function call | ~25 ns | ~25 ns |
| Generator step (`valid()`, `current()`, `next()`) | ~67 ns | ~98 ns |
| Generator step (`foreach`) | ~37 ns | ~70 ns |
| Fiber resume and suspend | ~108 ns | ~163 ns |

An implementation in the engine stores the scope in the fiber or generator itself, which makes
switching it a pointer assignment.

On PHP 8.2, a cycle that runs through a scope's `WeakMap` state back to a fiber of that scope isn't
collected: the cycle collector learned to see through `WeakMap` entries in PHP 8.3.

## License

MIT
