--TEST--
Superglobals: with auto_globals_jit=1, a file compiled inside a scope sees the scope's $_SERVER and $_REQUEST, not a lazily built SAPI copy
--EXTENSIONS--
scope
--INI--
auto_globals_jit=1
register_argc_argv=0
variables_order=EGPCS
--ENV--
PATH=/usr/bin
--FILE--
<?php
// this file never names $_SERVER or $_REQUEST directly, so with jit on neither is built yet
$s = new Scope();
$s->run(function () {
    ${'_SERVER'}['who'] = 'scope';
    echo json_encode(include __DIR__ . '/auto_globals_jit.inc'), "\n";
});
echo json_encode(include __DIR__ . '/auto_globals_jit.inc'), "\n";
$s->run(function () { echo json_encode(include __DIR__ . '/auto_globals_jit.inc'), "\n"; });
?>
--EXPECT--
["scope","scope","-"]
["env","-","-"]
["scope","scope","-"]
