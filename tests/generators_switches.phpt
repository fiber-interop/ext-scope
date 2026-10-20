--TEST--
Generator bodies switch scope at every entry and exit: catch and finally after an exception from a delegated generator, control flow around yields, fibers interleaving resumes, eval'd code
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$a = named('A'); $b = named('B'); $c = named('C');

// An exception escaping the inner generator is caught in the outer one.
$inner = $b->run(fn() => (function () { yield current_name(); throw new Exception('inner ' . current_name()); })());
$outer = $a->run(fn() => (function () use ($inner) {
    try {
        yield from $inner;
    } catch (Exception $e) {
        yield $e->getMessage() . ', caught in ' . current_name();
    } finally {
        echo "finally in ", current_name(), "\n";
    }
})());
foreach ($outer as $v) { echo $v, " (consumer ", current_name(), ")\n"; }

// Loops, switch, match and goto around yields keep their jump targets.
$gen = $a->run(fn() => (function () {
    for ($i = 0; $i < 3; $i++) {
        switch ($i) {
            case 0: yield "case0 " . current_name(); break;
            case 1: continue 2;
            default: yield match ($i) { 2 => "match2 " . current_name(), default => 'x' };
        }
    }
    $n = 0;
    again:
    if ($n++ < 2) { yield "goto $n " . current_name(); goto again; }
    return current_name();
})());
echo implode(', ', iterator_to_array($gen, false)), "; return ", $gen->getReturn(), "; main ", current_name(), "\n";

// Generators of different scopes resumed from fibers that interleave.
$g1 = $b->run(fn() => (function () { while (true) { $x = yield current_name(); Fiber::suspend(); } })());
$g2 = $c->run(fn() => (function () { while (true) { $x = yield current_name(); Fiber::suspend(); } })());
$f1 = $a->run(fn() => new Fiber(function () use ($g1) { $g1->current(); $g1->send(1); echo "f1 after g1: ", current_name(), "\n"; }));
$f2 = new Fiber(function () use ($g2) { $g2->current(); $g2->send(1); echo "f2 after g2: ", current_name(), "\n"; });
$f1->start(); $f2->start(); $f1->resume(); $f2->resume();
echo "main: ", current_name(), "\n";

// A generator compiled by eval().
$gen = $a->run(fn() => eval('return (function () { yield current_name(); })();'));
echo "eval'd generator: ", $gen->current(), ", main ", current_name(), "\n";
?>
--EXPECT--
B (consumer root)
inner B, caught in A (consumer root)
finally in A
case0 A, match2 A, goto 1 A, goto 2 A; return A; main root
f1 after g1: A
f2 after g2: root
main: root
eval'd generator: A, main root
