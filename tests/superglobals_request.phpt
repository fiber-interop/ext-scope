--TEST--
Superglobals: $_REQUEST is a scope-local snapshot, not a live merge
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["k" => "get"]; $_POST = []; $_REQUEST = ["k" => "get"];
$s = new Scope();
$s->run(function () { $_GET["k"] = "changed"; echo $_REQUEST["k"], "\n"; $_REQUEST["k"] = "own"; });
echo $_REQUEST["k"], "|", $s->run(fn() => $_REQUEST["k"]), "|", $s->run(fn() => $_GET["k"]), "\n";
--EXPECT--
get
get|own|changed
