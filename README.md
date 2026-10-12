# scope

The `Scope` class from the *PHP RFC: Scope*, as an extension for PHP 8.2 and later.

A `Scope` identifies the logical operation running code belongs to, such as an HTTP request, a
queued job or a transaction, and keeps that identity across fibers and generators. Service
containers can key request-scoped services by it, and libraries can keep per-operation state
without depending on a particular event loop.

```php
final class Scope
{
    public function __construct() {}
    public function run(callable $callback, mixed ...$args): mixed {}
    public static function getCurrent(): ?Scope {}
}

Fiber::getScope(): ?Scope;       // added by this extension
Generator::getScope(): ?Scope;   // added by this extension
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
Scope::getCurrent();              // null
```

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
