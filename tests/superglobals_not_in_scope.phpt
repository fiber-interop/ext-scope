--TEST--
Superglobals: $_ENV and $_SESSION are not in scope
--EXTENSIONS--
scope
--FILE--
<?php
$_ENV["E"] = "root"; $_SESSION = ["s" => "root"];
$s = new Scope();
$s->run(function () { echo $_ENV["E"], $_SESSION["s"], "\n"; $_ENV["E"] = "child"; $_SESSION["s"] = "child"; });
echo $_ENV["E"], $_SESSION["s"], "\n";
--EXPECT--
rootroot
childchild
