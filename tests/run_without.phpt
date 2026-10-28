--TEST--
Scope::__run_without() (not part of the RFC): while the callback runs, the calling scope's state is not applied when it becomes current; the state of the scope that ran before stays live; it is applied again when the callback returns or throws; a live scope destroyed meanwhile gives its state back first; not from the root scope, not twice
--EXTENSIONS--
scope
--FILE--
<?php
try { Scope::__run_without(fn() => 1); } catch (Error $e) { echo $e->getMessage(), "\n"; }

$loop = new Scope;
$loop->run(fn() => $_GET = ['who' => 'loop']);
$a = new Scope;
$b = new Scope;
$fa = $a->run(fn() => new Fiber(function () {
    $_GET = ['who' => 'a'];
    while (Fiber::suspend()) echo "  in a: ", $_GET['who'], "\n";
}));
$fb = $b->run(fn() => new Fiber(function () {
    $_GET = ['who' => 'b'];
    Fiber::suspend();
    echo "  in b: ", $_GET['who'], "\n";
}));

$loop->run(function () use ($loop, &$fa, $fb) {
    echo "loop before: ", $_GET['who'], "\n";
    $r = Scope::__run_without(function ($x) use ($loop, &$fa, $fb) {
        try { Scope::__run_without(fn() => 1); } catch (Error $e) { echo $e->getMessage(), "\n"; }
        $fa->start();
        echo "loop after a: ", $_GET['who'], ", current is the loop: ", var_export(Scope::getCurrent() === $loop, true), "\n";
        $fa->resume(true);
        $fb->start();
        echo "loop after b: ", $_GET['who'], "\n";
        $fa->resume(true);
        echo "loop after a: ", $_GET['who'], "\n";
        $fb->resume();   // b ends; its scope is still referenced by $b
        return $x * 2;
    }, 21);
    echo "returned $r, loop again: ", $_GET['who'], "\n";

    // a scope whose state is live is destroyed: its state goes back, the loop's is applied
    $fc = (new Scope)->run(fn() => new Fiber(function () { $_GET = ['who' => 'c']; Fiber::suspend(); }));
    Scope::__run_without(function () use (&$fc) {
        $fc->start();
        echo "loop after c: ", $_GET['who'], "\n";
        $fc = null;   // the fiber, and with it the only reference to its scope
        gc_collect_cycles();
        echo "after c is gone: ", $_GET['who'], "\n";
    });

    try {
        Scope::__run_without(function () use (&$fa) { $fa->resume(true); throw new Exception('thrown'); });
    } catch (Exception $e) {
        echo $e->getMessage(), ", loop again: ", $_GET['who'], "\n";
    }
});
var_dump($_GET);
?>
--EXPECT--
Scope::__run_without() can't be called from the root scope
loop before: loop
Scope::__run_without() is already running
loop after a: a, current is the loop: true
  in a: a
loop after b: b
  in a: a
loop after a: a
  in b: b
returned 42, loop again: loop
loop after c: c
after c is gone: loop
  in a: a
thrown, loop again: loop
array(0) {
}
