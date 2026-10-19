--TEST--
Superglobals: $_SERVER, $_ENV and $_SESSION are not in scope
--EXTENSIONS--
scope
--FILE--
<?php
$_SERVER["X"] = "root"; $_ENV["E"] = "root"; $_SESSION = ["s" => "root"];
$s = new Scope();
$s->run(function () { echo $_SERVER["X"], $_ENV["E"], $_SESSION["s"], "\n"; $_SERVER["X"] = "child"; $_ENV["E"] = "child"; $_SESSION["s"] = "child"; });
echo $_SERVER["X"], $_ENV["E"], $_SESSION["s"], "\n";
--EXPECT--
rootrootroot
childchildchild
