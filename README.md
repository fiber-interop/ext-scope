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
