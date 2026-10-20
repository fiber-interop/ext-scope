--TEST--
A generator belongs to the scope current when it is created, and the scope never changes across a yield
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$a = named('A'); $b = named('B');

// Created in A, advanced from B: the generator sees A before and after each yield.
$gen = $a->run(fn() => (function () {
    $before = current_name();
    yield;
    echo "generator: $before, after yield ", current_name(), "\n";
    yield;
    echo "generator: after second yield ", current_name(), "\n";
})());
$b->run(function () use ($gen) {
    $gen->current();
    $gen->next();
    $gen->next();
    echo "consumer still in: ", current_name(), "\n";
});
echo "Generator::getScope(): ", name($gen->getScope()), "\n";

// Creation means calling the generator function, not the first next().
$gen = $a->run(fn() => (function () { yield current_name(); })());
echo "first value, consumed from {main}: ", $gen->current(), "\n";

// yield from: the inner generator runs in its own scope.
$inner = $b->run(fn() => (function () { yield current_name(); })());
$outer = $a->run(fn() => (function () use ($inner) { yield current_name(); yield from $inner; yield current_name(); })());
echo "yield from: ", implode(',', iterator_to_array($outer, false)), "\n";

// A generator inside a fiber that suspends while the generator is inside run().
$gen = $a->run(fn() => (function () use ($b) {
    $b->run(fn() => Fiber::suspend());
    yield current_name();
})());
$f = new Fiber(function () use ($gen) { $v = $gen->current(); echo "generator in fiber yields: $v\n"; });
$f->start();
echo "main while the generator is inside run: ", current_name(), "\n";
$f->resume();

// Closures run in their caller's scope.
$closure = $a->run(fn() => fn() => current_name());
echo "closure created in A, called in B: ", $b->run($closure), "\n";
?>
--EXPECT--
generator: A, after yield A
generator: after second yield A
consumer still in: B
Generator::getScope(): A
first value, consumed from {main}: A
yield from: A,B,A
main while the generator is inside run: root
generator in fiber yields: A
closure created in A, called in B: B
