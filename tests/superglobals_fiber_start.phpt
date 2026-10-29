--TEST--
Superglobals: a fiber sees the arrays of the scope it is started in, not of the one it was constructed in, across resumes from other scopes
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ['who' => 'root'];
$a = new Scope(); $b = new Scope();
$a->run(function () { $_GET = ['who' => 'A']; });
$b->run(function () { $_GET = ['who' => 'B']; });

$f = $a->run(fn() => new Fiber(function () {
    echo "constructed in A, started in B: ", $_GET['who'], "\n";
    $_GET['seen'] = 1;
    Fiber::suspend();
    echo "resumed from A: ", $_GET['who'], "\n";
}));
$b->run(fn() => $f->start());
$a->run(fn() => $f->resume());
echo "B kept the write: ", $b->run(fn() => $_GET['seen'] ?? 'no'), ", A did not: ", $a->run(fn() => $_GET['seen'] ?? 'no'), "\n";

$f = $a->run(fn() => new Fiber(fn() => print("constructed in A, started from root: " . $_GET['who'] . "\n")));
$f->start();

$f = new Fiber($a->bind(fn() => print("bound to A, started in B: " . $_GET['who'] . "\n")));
$b->run(fn() => $f->start());
echo "main: ", $_GET['who'], "\n";
?>
--EXPECT--
constructed in A, started in B: B
resumed from A: B
B kept the write: 1, A did not: no
constructed in A, started from root: root
bound to A, started in B: A
main: root
