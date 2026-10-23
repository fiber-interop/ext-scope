--TEST--
Superglobals: two scopes alternating each see their own $_SERVER and $_REQUEST across fiber suspensions
--EXTENSIONS--
scope
--FILE--
<?php
$_SERVER["V"] = "root"; $_REQUEST["V"] = "root";
$a = new Scope(); $b = new Scope();
$fa = $a->run(fn() => new Fiber(function () {
    $_SERVER["V"] = "A"; $_REQUEST["V"] = "A";
    Fiber::suspend();
    echo "A sees ", $_SERVER["V"], " ", $_REQUEST["V"], "\n";
}));
$fb = $b->run(fn() => new Fiber(function () {
    $_SERVER["V"] = "B"; $_REQUEST["V"] = "B";
    Fiber::suspend();
    echo "B sees ", $_SERVER["V"], " ", $_REQUEST["V"], "\n";
}));
$fa->start();
$fb->start();
echo "root ", $_SERVER["V"], " ", $_REQUEST["V"], "\n";
$fa->resume();
$fb->resume();
echo "root ", $_SERVER["V"], " ", $_REQUEST["V"], "\n";
--EXPECT--
root root root
A sees A A
B sees B B
root root root
