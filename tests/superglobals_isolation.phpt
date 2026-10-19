--TEST--
Superglobals: an assignment in one scope changes only that scope's array; sibling scopes are isolated
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["v" => "root"];
$a = new Scope(); $b = new Scope();
$a->run(function () { $_GET["v"] = "A"; });
$b->run(function () { $_GET["v"] = "B"; });
echo $_GET["v"], $a->run(fn() => $_GET["v"]), $b->run(fn() => $_GET["v"]), "\n";
echo $a->run(function () use ($b) { return $_GET["v"] . $b->run(fn() => $_GET["v"]) . $_GET["v"]; }), "\n";
function g() { return $_GET["v"]; }
echo g(), $b->run(g(...)), "\n";
--EXPECT--
rootAB
ABA
rootB
