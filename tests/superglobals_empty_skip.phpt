--TEST--
Superglobals: arrays left empty by both scopes are not moved, and a scope that writes to one still gets its own (writes, unset, a reference, an emptied array keeping its next key); unset() in a scope is that scope's only
--EXTENSIONS--
scope
--FILE--
<?php
$a = new Scope();
$b = new Scope();
$show = fn() => json_encode([$_GET, $_POST, $_COOKIE ?? null]);

$a->run(function () { $_GET["a"] = 1; });
echo "root: ", $show(), "\n";
echo "b: ", $b->run($show), "\n";
echo "a: ", $a->run($show), "\n";

$b->run(function () { $_POST[] = "x"; unset($_POST[0]); });   // empty, but the next key is 1
$b->run(function () { $_POST[] = "y"; });
echo "b post: ", $b->run(fn() => json_encode($_POST)), "\n";

$a->run(function () { unset($_COOKIE); });
echo "a cookie set: ", $a->run(fn() => var_export(isset($_COOKIE), true)), "; root: ", var_export(isset($_COOKIE), true), "\n";

$ref = $b->run(function () { $r = &$_GET; return function () use (&$r) { $r["via"] = "ref"; }; });
$ref();   // writes to b's $_GET through the reference, from the root
echo "b get: ", $b->run(fn() => json_encode($_GET)), "; root get: ", json_encode($_GET), "\n";

for ($i = 0; $i < 3; $i++) {
    $a->run(fn() => null); $b->run(fn() => null);
}
echo "after switching: ", $a->run($show), " ", $b->run($show), " ", $show(), "\n";
--EXPECT--
root: [[],[],[]]
b: [[],[],[]]
a: [{"a":1},[],[]]
b post: {"1":"y"}
a cookie set: false; root: true
b get: {"via":"ref"}; root get: []
after switching: [{"a":1},[],null] [{"via":"ref"},{"1":"y"},[]] [[],[],[]]
