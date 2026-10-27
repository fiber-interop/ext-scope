--TEST--
Before the first Scope: object ids are as without the extension, and fibers and generators created then belong to the root
--EXTENSIONS--
scope
--SKIPIF--
<?php if (extension_loaded("parallel")) die("skip parallel allocates an object of its own at startup"); ?>
--FILE--
<?php
var_dump(spl_object_id(new stdClass));   // the extension allocates no object of its own until a Scope exists
$gen = (function () { while (true) { yield Scope::getCurrent(); } })();
$fib = new Fiber(function () { while (true) { Fiber::suspend(Scope::getCurrent()); } });
$s = new Scope();
$s->run(function () use ($gen, $fib) {
    echo "generator: ", $gen->current() === Scope::getRootScope() ? "root" : "other", "\n";
    $gen->next();
    echo "generator after next: ", $gen->current() === Scope::getRootScope() ? "root" : "other", "\n";
    echo "fiber: ", $fib->start() === Scope::getRootScope() ? "root" : "other", "\n";
    echo "fiber resumed: ", $fib->resume() === Scope::getRootScope() ? "root" : "other", "\n";
    echo "in run: ", Scope::getCurrent() === $GLOBALS['s'] ? "s" : "other", "\n";
});
echo "getScope: ", $gen->getScope() === Scope::getRootScope() ? "root" : "other", " ", $fib->getScope() === Scope::getRootScope() ? "root" : "other", "\n";
?>
--EXPECT--
int(1)
generator: root
generator after next: root
fiber: root
fiber resumed: root
in run: s
getScope: root root
