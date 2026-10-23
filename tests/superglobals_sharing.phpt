--TEST--
Superglobals: all fibers and generators of one scope share its arrays, across suspensions of other scopes
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["n" => 0];
$a = new Scope(); $b = new Scope();
$a->run(function () { $_GET = ["n" => 0]; });
$gen = $a->run(function () { return (function () { $_GET["n"]++; yield $_GET["n"]; $_GET["n"]++; yield $_GET["n"]; })(); });
$f = $a->run(fn() => new Fiber(function () { $_GET["n"] += 10; Fiber::suspend(); echo "A fiber sees ", $_GET["n"], "\n"; }));
$fb = $b->run(fn() => new Fiber(function () { $_GET["n"] = "B"; Fiber::suspend(); echo "B fiber sees ", $_GET["n"], "\n"; }));
$f->start(); $fb->start();
echo "main ", $_GET["n"], "\n";
echo $gen->current(), "\n";   // runs in A
echo "main ", $_GET["n"], "\n";
$f->resume(); $fb->resume();
$gen->next(); echo $gen->current(), "\n";
$f2 = $a->run(fn() => new Fiber(fn() => print("A new fiber sees " . $_GET["n"] . "\n"))); $f2->start();
--EXPECT--
main 0
11
main 0
A fiber sees 11
B fiber sees B
12
A new fiber sees 12
