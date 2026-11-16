--TEST--
Superglobals: $_ENV is not in scope; $_SESSION is (a new scope has none, as a new request)
--EXTENSIONS--
scope
session
--FILE--
<?php
$_ENV["E"] = "root"; $_SESSION = ["s" => "root"];
$s = new Scope();
$s->run(function () {
    echo $_ENV["E"], " ", isset($_SESSION) ? "set" : "unset", "\n";
    $_ENV["E"] = "child"; $_SESSION["s"] = "child";
});
echo $_ENV["E"], " ", $_SESSION["s"], "\n";
$s->run(function () { echo $_SESSION["s"], "\n"; });
--EXPECT--
root unset
child root
child
