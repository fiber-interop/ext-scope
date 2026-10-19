--TEST--
Superglobals: 1000 scopes switching leak nothing
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["a" => str_repeat("x", 100)]; $_REQUEST = $_GET;
function churn() { $f = []; for ($i = 0; $i < 1000; $i++) { $s = new Scope(); $s->run(function () use ($i) { $_GET["i"] = $i; $_POST = [$i]; $x = new Fiber(function () use ($i) { Fiber::suspend(); assert($_GET["i"] === $i); }); $x->start(); $x->resume(); }); } }
churn(); gc_collect_cycles();
$before = memory_get_usage();
churn(); gc_collect_cycles();
$after = memory_get_usage();
echo $after - $before < 4096 ? "no leak" : "leak: " . ($after - $before), "\n";
echo json_encode(array_keys($_GET)), json_encode($_POST), "\n";
--EXPECT--
no leak
["a"][]
