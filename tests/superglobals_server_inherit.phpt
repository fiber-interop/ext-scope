--TEST--
Superglobals: a new scope's $_SERVER is a copy of its creator's, environment entries included
--EXTENSIONS--
scope
--FILE--
<?php
// "argv" and PATH come from the SAPI and the environment, not from anything this script set.
var_dump(isset($_SERVER["argv"]), isset($_SERVER["PATH"]));
$s = new Scope();
var_dump($s->run(fn() => $_SERVER) === $_SERVER);
$s->run(function () { $_SERVER["child"] = 1; });
echo json_encode(["child" => isset($_SERVER["child"]), "argv" => isset($_SERVER["argv"])]), "\n";
$_SERVER["root"] = 1;
echo json_encode(["root" => $s->run(fn() => isset($_SERVER["root"])), "child" => $s->run(fn() => isset($_SERVER["child"]))]), "\n";
// a grandchild copies its creator scope, which has "child" and not "root"
$s->run(function () { $g = new Scope(); echo json_encode([isset($_SERVER["root"]), $g->run(fn() => isset($_SERVER["child"])), $g->run(fn() => isset($_SERVER["PATH"]))]), "\n"; });
--EXPECT--
bool(true)
bool(true)
bool(true)
{"child":false,"argv":true}
{"root":false,"child":true}
[false,true,true]
