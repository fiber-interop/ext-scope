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

`$_GET`, `$_POST`, `$_COOKIE`, `$_FILES` and `$_REQUEST` follow the scope:

- In the root scope they are the SAPI's arrays, so code that never creates a scope sees today's behaviour.
- A new scope starts with its own copies of the five as they are in the scope that creates it
  (copy-on-write: only a reference is taken, nothing is duplicated until someone writes).
- An assignment or write inside a scope changes only that scope's array. Reads see the current scope's array;
  every fiber and generator of the scope shares it. `$_REQUEST` is a snapshot like the others, not a live merge.
- `$_SERVER`, `$_ENV` and `$_SESSION` are not in scope and are left untouched.

When the current scope changes (`run()` entering or leaving, a fiber switch, a generator resuming or yielding) the
five arrays are moved between the scope's storage and the live `EG(symbol_table)` entries, which is where compiled
`$_GET` accesses read and write, and `PG(http_globals)[TRACK_VARS_*]` is re-pointed at the same array for C code
such as `filter_input()`. The cost of a switch is five symbol-table lookups and pointer moves; no array is copied.
The first scope creation makes the engine create the five lazily-created auto-globals (`auto_globals_jit`).

`scope.superglobals` (default `1`, `PHP_INI_SYSTEM`) turns this off, so the vote can be shown independently
of the base proposal: with `0`, all scopes share the SAPI's arrays.

## Default locale and timezone (secondary vote B)

`date_default_timezone_get()/set()` (ext/date) and `Locale::getDefault()/setDefault()` (ext/intl, only when intl is
loaded) follow the scope with the same rules as the superglobals: the root scope's values are today's (from ini and
the SAPI), a new scope inherits its creator's values at creation (later changes in the creator are not seen), a change
inside a scope affects only that scope, and code outside any scope sees no change. Fibers and generators of a scope
share its values.

How it is done, and what a switch costs:

- **Timezone**: the string `date_default_timezone_set()` writes (ext/date's `timezone` global, `NULL` meaning "use
  the `date.timezone` ini") is swapped as a pointer. ext/date does not export its globals, but `php_date.h` has the
  struct and the module entry carries its address, found with `zend_hash_str_find_ptr(&module_registry, "date")`
  at startup. Each scope owns a private copy (made at scope creation), because `date_default_timezone_set()` frees
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
outside any scope sees the global, as without the extension. Swapping costs roughly 25–55 ns per
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
fiber's and generator's scope in a `WeakMap` and follows execution with the engine's observer
API. Measured on PHP 8.5 (x86-64):

| | Without | With |
|---|---|---|
| Function call | ~25 ns | ~29 ns |
| Generator step | ~38 ns | ~79 ns |
| Fiber resume and suspend | ~108 ns | ~163 ns |

An implementation in the engine stores the scope in the fiber or generator itself, which makes
switching it a pointer assignment.

On PHP 8.2, a cycle that runs through a scope's `WeakMap` state back to a fiber of that scope isn't
collected: the cycle collector learned to see through `WeakMap` entries in PHP 8.3.

## License

MIT
