--TEST--
Superglobals: a new scope's $_SERVER starts empty, environment entries included; the root keeps the SAPI's
--EXTENSIONS--
scope
--FILE--
<?php
// "argv" comes from the SAPI (the environment), not from anything this script set.
var_dump(isset($_SERVER["argv"]));
$s = new Scope();
echo $s->run(function () { return json_encode($_SERVER); }), "
";
$s->run(function () { $_SERVER["child"] = 1; });
echo json_encode(["child" => isset($_SERVER["child"]), "argv" => isset($_SERVER["argv"])]), "
";
// a grandchild starts empty too, not with its creator's
$s->run(function () { $g = new Scope(); echo json_encode($g->run(fn() => $_SERVER)), "
"; });
echo json_encode($s->run(fn() => $_SERVER)), "
";
--EXPECT--
bool(true)
[]
{"child":false,"argv":true}
[]
{"child":1}
