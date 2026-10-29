--TEST--
A fiber's first frame gets the scope current at start(), not at construction; it keeps its scope (and a run() active in it) wherever it is resumed from; bind() gives a fiber a fixed scope; there is no Fiber::getScope()
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$a = named('A'); $b = named('B'); $c = named('C');

// Constructed in A, started inside B's run(): B.
$f = $a->run(fn() => new Fiber(fn() => print("constructed in A, started in B: " . current_name() . "\n")));
$b->run(fn() => $f->start());

// Constructed in A or in {main}, started from {main}: root.
$f = $a->run(fn() => new Fiber(fn() => print("constructed in A, started from root: " . current_name() . "\n")));
$f->start();
$f = new Fiber(fn() => print("constructed in root, started from root: " . current_name() . "\n"));
$f->start();

// A bound closure runs in its scope whoever starts the fiber.
$f = new Fiber($a->bind(fn() => print("bound to A, started from root: " . current_name() . "\n")));
$f->start();
$f = new Fiber($a->bind(function () {
    echo "bound to A, started in B: ", current_name(), "\n";
    Fiber::suspend();
    echo "bound to A, resumed from root: ", current_name(), "\n";
}));
$b->run(fn() => $f->start());
$f->resume();

// Started in A, resumed from B and from {main}: always A inside.
$f = new Fiber(function () {
    $before = current_name();
    Fiber::suspend();
    $middle = current_name();
    Fiber::suspend();
    echo "started in A: $before, resumed from B $middle, resumed from root ", current_name(), "\n";
});
$a->run(fn() => $f->start());
echo "main after start: ", current_name(), "\n";
$b->run(fn() => $f->resume());
$f->resume();

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

// Started in A, suspended inside C's run(), resumed from B: C, then A once that run() returns.
$f = new Fiber(function () use ($c) {
    $c->run(function () { Fiber::suspend(); echo "in C's run, resumed from B: ", current_name(), "\n"; });
    echo "after C's run: ", current_name(), "\n";
    Fiber::suspend();
    echo "resumed from B again: ", current_name(), "\n";
});
$a->run(fn() => $f->start());
$b->run(fn() => $f->resume());
echo "B after resume: ", $b->run(fn() => current_name()), ", main: ", current_name(), "\n";
$b->run(fn() => $f->resume());

// A fiber started inside a fiber gets that fiber's scope at that point.
$f = new Fiber(function () use ($b) {
    $child = new Fiber(fn() => print("child started in the fiber: " . current_name() . "\n"));
    $other = new Fiber(fn() => print("child started in B's run inside the fiber: " . current_name() . "\n"));
    $child->start();
    $b->run(fn() => $other->start());
});
$a->run(fn() => $f->start());

// A fiber that is never started runs nothing and records nothing.
$a->run(fn() => new Fiber(fn() => null));

var_dump(method_exists('Fiber', 'getScope'), method_exists('Generator', 'getScope'));
?>
--EXPECT--
constructed in A, started in B: B
constructed in A, started from root: root
constructed in root, started from root: root
bound to A, started from root: A
bound to A, started in B: A
bound to A, resumed from root: A
main after start: root
started in A: A, resumed from B A, resumed from root A
main while fiber is inside run: root
inside run: A, after suspend A
fiber after run: root
in C's run, resumed from B: C
after C's run: A
B after resume: B, main: root
resumed from B again: A
child started in the fiber: A
child started in B's run inside the fiber: B
bool(false)
bool(true)
