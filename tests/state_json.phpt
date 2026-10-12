--TEST--
Per-scope state (prototype): with scope.isolate_json_errors, json_last_error() is per scope; fibers of one scope share it, and code outside scopes sees today's global
--EXTENSIONS--
scope
json
--INI--
scope.isolate_json_errors=1
--FILE--
<?php
$err = fn() => json_last_error_msg();

json_decode('{bad');
echo "main: ", $err(), "\n";

$a = new Scope(); $b = new Scope();
$fa = $a->run(fn() => new Fiber(function () use ($err) {
    echo "A starts fresh: ", $err(), "\n";
    json_decode('[1,');
    Fiber::suspend();
    echo "A after B ran: ", $err(), "\n";
    $child = new Fiber(fn() => print("A's child fiber shares it: " . $err() . "\n"));
    $child->start();
}));
$fb = $b->run(fn() => new Fiber(function () use ($err) {
    echo "B starts fresh: ", $err(), "\n";
    json_decode("\"\xff\"");
    echo "B: ", $err(), "\n";
}));
$fa->start();
$fb->start();
$fa->resume();
echo "main still: ", $err(), "\n";
json_decode('1');
echo "main after a good decode: ", $err(), "\n";
$a->run(fn() => print("A, entered again from main: " . $err() . "\n"));
?>
--EXPECT--
main: Syntax error
A starts fresh: No error
B starts fresh: No error
B: Malformed UTF-8 characters, possibly incorrectly encoded
A after B ran: Syntax error
A's child fiber shares it: Syntax error
main still: Syntax error
main after a good decode: No error
A, entered again from main: Syntax error
