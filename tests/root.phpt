--TEST--
The root scope is one object for the request: current in {main} and in fibers constructed there, runnable, and kept across garbage collection
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$root = Scope::getRootScope();
echo "identical across calls: ", var_export($root === Scope::getRootScope(), true), "\n";
echo "current in {main}: ", var_export(Scope::getCurrent() === $root, true), "\n";
$f = new Fiber(function () { echo "inside fiber from {main}: ", current_name(), "\n"; });
echo "fiber from {main}: ", name($f->getScope()), "\n";
$f->start();
$g = (function () { yield current_name(); })();
echo "generator from {main}: ", name($g->getScope()), ", inside: ", $g->current(), "\n";

$r = named('R');
$r->run(function () use ($root) {
    echo "in request: ", current_name(), "\n";
    $root->run(function () {
        echo "root->run() inside request: ", current_name(), "\n";
        $f = new Fiber(fn() => null);
        echo "fiber constructed there: ", name($f->getScope()), "\n";
    });
    echo "restored: ", current_name(), "\n";
});

$id = spl_object_id($root);
unset($root);
gc_collect_cycles();
echo "same object after gc: ", var_export(spl_object_id(Scope::getRootScope()) === $id, true), "\n";
$w = WeakReference::create(Scope::getRootScope());
gc_collect_cycles();
echo "still alive unreferenced: ", var_export($w->get() === Scope::getRootScope(), true), "\n";
echo "Scope::getCurrent() return type: ", (new ReflectionMethod('Scope', 'getCurrent'))->getReturnType(), "\n";
echo "Fiber::getScope() return type: ", (new ReflectionMethod('Fiber', 'getScope'))->getReturnType(), "\n";
?>
--EXPECT--
identical across calls: true
current in {main}: true
fiber from {main}: root
inside fiber from {main}: root
generator from {main}: root, inside: root
in request: R
root->run() inside request: root
fiber constructed there: root
restored: R
same object after gc: true
still alive unreferenced: true
Scope::getCurrent() return type: Scope
Fiber::getScope() return type: Scope
