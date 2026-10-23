--TEST--
Superglobals: the root scope's arrays are the SAPI's, unchanged for code that never creates a scope
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["a" => 1]; $_POST = ["p" => 2]; $_COOKIE = ["c" => 3]; $_FILES = []; $_REQUEST = ["r" => 4];
function f() { return $_GET["a"] . $_POST["p"] . $_COOKIE["c"] . $_REQUEST["r"]; }
echo f(), "\n";
$_GET["b"] = 5;
echo json_encode($_GET), "\n";
echo json_encode(Scope::getRootScope()->run(fn() => $GLOBALS["_GET"])), "\n";
// $_SERVER is the SAPI's own, argv included: CLI populates it before any scope exists.
var_dump(isset($_SERVER["argv"]));
echo Scope::getRootScope()->run(fn() => isset($_SERVER["argv"]) ? "yes" : "no"), "\n";
$_SERVER["X"] = 6;
echo Scope::getRootScope()->run(fn() => $_SERVER["X"]), "\n";
--EXPECT--
1234
{"a":1,"b":5}
{"a":1,"b":5}
bool(true)
yes
6
