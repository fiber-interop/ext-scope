--TEST--
Superglobals: with scope.superglobals=0 the arrays are shared by every scope
--EXTENSIONS--
scope
--INI--
scope.superglobals=0
--FILE--
<?php
$_GET = ["v" => "root"];
$s = new Scope();
$s->run(function () { echo $_GET["v"], "\n"; $_GET["v"] = "child"; });
echo $_GET["v"], "\n";
--EXPECT--
root
child
