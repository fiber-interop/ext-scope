--TEST--
Before the first Scope: object ids are as without the extension, and generators created then belong to the root, and fibers constructed then take the scope they are started in
--EXTENSIONS--
scope
--SKIPIF--
<?php if (extension_loaded("parallel")) die("skip parallel allocates an object of its own at startup"); ?>
--FILE--
<?php
var_dump(spl_object_id(new stdClass));   // the extension allocates no object of its own until a Scope exists
$gen = (function () { while (true) { yield Scope::getCurrent(); } })();
$fib = new Fiber(function () { while (true) { Fiber::suspend(Scope::getCurrent()); } });
$fib2 = new Fiber(function () { while (true) { Fiber::suspend(Scope::getCurrent()); } });
$s = new Scope();
$s->run(function () use ($gen, $fib) {
    echo "generator: ", $gen->current() === Scope::getRootScope() ? "root" : "other", "\n";
    $gen->next();
    echo "generator after next: ", $gen->current() === Scope::getRootScope() ? "root" : "other", "\n";
    echo "fiber: ", $fib->start() === $GLOBALS['s'] ? "s" : "other", "\n";
    echo "fiber resumed: ", $fib->resume() === $GLOBALS['s'] ? "s" : "other", "\n";
    echo "in run: ", Scope::getCurrent() === $GLOBALS['s'] ? "s" : "other", "\n";
});
echo "getScope: ", $gen->getScope() === Scope::getRootScope() ? "root" : "other", "\n";
echo "fiber resumed from root: ", $fib->resume() === $s ? "s" : "other", "\n";
echo "other fiber started from root: ", $fib2->start() === Scope::getRootScope() ? "root" : "other", "\n";
?>
--EXPECT--
int(1)
generator: root
generator after next: root
fiber: s
fiber resumed: s
in run: s
getScope: root
fiber resumed from root: s
other fiber started from root: root
