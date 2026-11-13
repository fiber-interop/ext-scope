--TEST--
Superglobals: fibers created in a scope join it and share its arrays, also through a reference; the root is unchanged
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["root" => 1];
$s = new Scope();
$s->run(function () {
    echo json_encode($_GET), "\n";   // a copy of the root's
    $a = new Fiber(function () { $g = &$_GET; $g['t'] = 1; Fiber::suspend(); $g['u'] = 2; });
    $b = new Fiber(function () { echo json_encode($_GET), "\n"; Fiber::suspend(); echo json_encode($_GET), "\n"; });
    $a->start(); $b->start();
    $a->resume(); $b->resume();
});
echo json_encode($_GET), "|", json_encode($s->run(fn() => $_GET)), "\n";
--EXPECT--
{"root":1}
{"root":1,"t":1}
{"root":1,"t":1,"u":2}
{"root":1}|{"root":1,"t":1,"u":2}
