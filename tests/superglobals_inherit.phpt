--TEST--
Superglobals: a new scope starts with the arrays of the scope that creates it; writes in the child do not touch the parent (copy-on-write)
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["a" => 1]; $_POST = ["p" => 1]; $_COOKIE = ["c" => 1]; $_FILES = ["f" => 1]; $_REQUEST = ["r" => 1];
$s = new Scope();
$_GET["late"] = 1; // after creation: the child must not see it
function show() { return json_encode([$_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST]); }
echo $s->run(show(...)), "\n";
$s->run(function () { $_GET["child"] = 1; $_POST = ["new" => 1]; $_COOKIE["x"] = 1; $_FILES = []; $_REQUEST["y"] = 1; });
echo show(), "\n";
// grandchild inherits from its creator scope, not from the root
$s->run(function () { $g = new Scope(); echo json_encode($g->run(fn() => $_GET)), "\n"; });
--EXPECT--
[{"a":1},{"p":1},{"c":1},{"f":1},{"r":1}]
[{"a":1,"late":1},{"p":1},{"c":1},{"f":1},{"r":1}]
{"a":1,"child":1}
