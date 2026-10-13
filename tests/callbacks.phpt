--TEST--
An event loop runs queued callbacks in the scope they were queued from; per-scope state lives in a WeakMap
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';

final class RequestServices
{
    private static ?WeakMap $services = null;
    public static function get(string $id): object
    {
        $scope = Scope::getCurrent();
        if ($scope === Scope::getRootScope()) throw new LogicException('Not running inside a request scope');
        self::$services ??= new WeakMap();
        self::$services[$scope] ??= [];
        return self::$services[$scope][$id] ??= new ArrayObject([$id, current_name()]);
    }
}

$queue = [];
$defer = function (Closure $cb) use (&$queue) { $queue[] = [Scope::getCurrent(), $cb]; };

foreach (['R1', 'R2'] as $r) {
    named($r)->run(function () use ($defer) {
        $db = RequestServices::get('db');
        $defer(function () use ($db) {
            $again = RequestServices::get('db');
            echo current_name(), ": same db in deferred callback: ", var_export($again === $db, true), "\n";
        });
    });
}
while ($queue) {
    [$scope, $cb] = array_shift($queue);
    $scope->run($cb);
}
try {
    RequestServices::get('db');
} catch (LogicException $e) {
    echo "outside a scope: ", $e->getMessage(), "\n";
}
?>
--EXPECT--
R1: same db in deferred callback: true
R2: same db in deferred callback: true
outside a scope: Not running inside a request scope
