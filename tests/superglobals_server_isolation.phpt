--TEST--
Superglobals: $_SERVER follows the scope like the others; an assignment in a child leaves the creator unchanged
--EXTENSIONS--
scope
--FILE--
<?php
$_SERVER["X"] = "root";
$a = new Scope(); $b = new Scope();
$a->run(function () { $_SERVER["X"] = "A"; });
$b->run(function () { $_SERVER["X"] = "B"; });
echo $_SERVER["X"], $a->run(fn() => $_SERVER["X"]), $b->run(fn() => $_SERVER["X"]), "\n";
echo $a->run(function () use ($b) { return $_SERVER["X"] . $b->run(fn() => $_SERVER["X"]) . $_SERVER["X"]; }), "\n";
--EXPECT--
rootAB
ABA
