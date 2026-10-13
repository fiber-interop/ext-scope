--TEST--
A fiber belongs to the scope current at its construction, keeps it wherever it is resumed from, and keeps run() scopes across suspension
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$a = named('A'); $b = named('B');

// Constructed in A, started from B and resumed from {main}: always A inside.
$f = $a->run(fn() => new Fiber(function () {
    $before = current_name();
    Fiber::suspend();
    echo "fiber: $before, after suspend ", current_name(), "\n";
}));
$b->run(fn() => $f->start());
echo "main after start: ", current_name(), "\n";
$f->resume();
echo "Fiber::getScope(): ", name($f->getScope()), "\n";
echo "fiber from {main}: ", name((new Fiber(fn() => null))->getScope()), "\n";

// Suspending inside run() doesn't leave run(): the resumer keeps its own scope.
$f = new Fiber(function () use ($a) {
    $a->run(function () {
        $before = current_name();
        Fiber::suspend();
        echo "inside run: $before, after suspend ", current_name(), "\n";
    });
    echo "fiber after run: ", current_name(), "\n";
});
$f->start();
echo "main while fiber is inside run: ", current_name(), "\n";
$f->resume();

// Constructed in A, suspended inside B's run().
$f = $a->run(fn() => new Fiber(function () use ($b) {
    $b->run(function () { Fiber::suspend(); echo "in B's run: ", current_name(), "\n"; });
    echo "back in own scope: ", current_name(), "\n";
}));
$f->start();
$f->resume();

// Fibers constructed inside a fiber join its scope.
$f = $a->run(fn() => new Fiber(function () {
    $child = new Fiber(fn() => print("child: " . current_name() . "\n"));
    echo "child belongs to: ", name($child->getScope()), "\n";
    $child->start();
}));
$f->start();
?>
--EXPECT--
main after start: root
fiber: A, after suspend A
Fiber::getScope(): A
fiber from {main}: root
main while fiber is inside run: root
inside run: A, after suspend A
fiber after run: root
in B's run: B
back in own scope: A
child belongs to: A
child: A
