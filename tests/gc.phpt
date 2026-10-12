--TEST--
Scopes live as long as something references them: fibers keep theirs (also across a suspended run()), and cycles through per-scope state are collected
--EXTENSIONS--
scope
--SKIPIF--
<?php if (PHP_VERSION_ID < 80300) die('skip WeakMap entries whose value references the key are collected since PHP 8.3'); ?>
--FILE--
<?php
$s = new Scope(); $w = WeakReference::create($s); $s->run(fn() => null); unset($s);
echo "unreferenced scope freed: ", var_export($w->get() === null, true), "\n";

$f = (new Scope())->run(fn() => new Fiber(fn() => Fiber::suspend()));
$w = WeakReference::create($f->getScope());
$f->start();
echo "kept by its suspended fiber: ", var_export($w->get() !== null, true), "\n";
unset($f); gc_collect_cycles();
echo "freed with the fiber: ", var_export($w->get() === null, true), "\n";

$a = new Scope(); $b = new Scope(); $wa = WeakReference::create($a); $wb = WeakReference::create($b);
$f = $a->run(fn() => new Fiber(function () use ($b) { $b->run(fn() => Fiber::suspend()); }));
$f->start(); unset($a, $b); gc_collect_cycles();
echo "membership and run() scope kept while suspended: ", var_export($wa->get() !== null && $wb->get() !== null, true), "\n";
unset($f); gc_collect_cycles();
echo "both freed after: ", var_export($wa->get() === null && $wb->get() === null, true), "\n";

$state = new WeakMap();
$s = new Scope(); $w = WeakReference::create($s);
$f = $s->run(function () use ($s, $state) {
    $f = new Fiber(function () use ($s) { Fiber::suspend(); });
    $state[$s] = $f;
    return $f;
});
$f->start(); unset($s, $f); gc_collect_cycles();
echo "cycle through per-scope state collected: ", var_export($w->get() === null, true), ", entries left: ", count($state), "\n";
?>
--EXPECT--
unreferenced scope freed: true
kept by its suspended fiber: true
freed with the fiber: true
membership and run() scope kept while suspended: true
both freed after: true
cycle through per-scope state collected: true, entries left: 0
