--TEST--
Superglobals: a new scope's $_SERVER starts as a copy of its creator's, environment entries included
--EXTENSIONS--
scope
--FILE--
<?php
// "argv" comes from the SAPI (the environment), not from anything this script set.
var_dump(isset($_SERVER["argv"]));
$s = new Scope();
$_SERVER["late"] = 1; // after creation: the child must not see it
echo $s->run(function () { return json_encode(["argv" => isset($_SERVER["argv"]), "late" => isset($_SERVER["late"])]); }), "\n";
$s->run(function () { $_SERVER["child"] = 1; });
echo json_encode(["child" => isset($_SERVER["child"])]), "\n";
// a grandchild inherits from its creator scope, not from the root
$s->run(function () { $g = new Scope(); echo json_encode($g->run(fn() => ["argv" => isset($_SERVER["argv"]), "late" => isset($_SERVER["late"]), "child" => isset($_SERVER["child"])])), "\n"; });
--EXPECT--
bool(true)
{"argv":true,"late":false}
{"child":false}
{"argv":true,"late":false,"child":true}
