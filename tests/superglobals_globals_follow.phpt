--TEST--
Superglobals: $GLOBALS['_SERVER'] and `global $_SERVER` reach the scope's array, not the SAPI's
--EXTENSIONS--
scope
--FILE--
<?php
$_SERVER["V"] = "root";
$s = new Scope();
$s->run(function () {
    $GLOBALS["_SERVER"]["V"] = "child-via-GLOBALS";
    function f() { global $_SERVER; return $_SERVER["V"]; }
    echo f(), "\n";
});
echo $_SERVER["V"], "|", $GLOBALS["_SERVER"]["V"], "\n";
--EXPECT--
child-via-GLOBALS
root|root
