--TEST--
Per-scope slots (prototype): scope_slot() gives the current scope's copy, the root scope's outside any scope; fibers and generators of one scope share it
--EXTENSIONS--
scope
--INI--
scope.slot_demo=1
--FILE--
<?php
echo "root: ", scope_demo_counter(), scope_demo_counter(), "\n";
echo "root->run(): ", Scope::getRootScope()->run(fn() => scope_demo_counter()), "\n";   // the public root object is the engine's implicit root

$a = new Scope(); $b = new Scope();
echo "A: ", $a->run(fn() => scope_demo_counter() . scope_demo_counter()), "\n";
echo "B: ", $b->run(fn() => scope_demo_counter()), "\n";
echo "root again: ", scope_demo_counter(), "\n";

$f = $a->run(fn() => new Fiber(function () { echo "A's fiber: ", scope_demo_counter(), "\n"; Fiber::suspend(); echo "A's fiber resumed: ", scope_demo_counter(), "\n"; }));
$g = $a->run(fn() => (function () { yield scope_demo_counter(); })());
$b->run(fn() => $f->start());
echo "A's generator, consumed in B: ", $b->run(fn() => $g->current()), "\n";
$f->resume();
echo "A: ", $a->run(fn() => scope_demo_counter()), "\n";

$w = WeakReference::create($b); unset($b);
echo "B freed with its slots: ", var_export($w->get() === null, true), "\n";
?>
--EXPECT--
root: 12
root->run(): 3
A: 12
B: 1
root again: 4
A's fiber: 3
A's generator, consumed in B: 4
A's fiber resumed: 5
A: 6
B freed with its slots: true
