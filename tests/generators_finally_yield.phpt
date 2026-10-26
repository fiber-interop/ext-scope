--TEST--
Generators: a yield that ends a finally block, entered by an exception, a return or a destructor (php-src's own tests crashed here)
--EXTENSIONS--
scope
--FILE--
<?php
function thrower($n) {
    for ($i = 1; $i <= $n; $i++) {
        try {
            throw new Exception("e$i");
        } finally {
            yield $i => Scope::getCurrent();
        }
    }
}
function returner() {
    try {
        return;
    } finally {
        yield 1 => Scope::getCurrent();
    }
}
function cleanup() {
    try {
        yield 0;
    } finally {
        echo "cleanup in scope: ", var_export(Scope::getCurrent() !== Scope::getRootScope(), true), "\n";
        yield 1;
    }
}

$s = new Scope();
$g = $s->run(fn() => thrower(2));
try {
    foreach ($g as $i => $scope) {
        echo "$i ", $scope === $s ? "s" : "other", "\n";
    }
} catch (Exception $e) {
    echo "caught ", $e->getMessage(), "\n";
}
foreach ($s->run(fn() => returner()) as $i => $scope) {
    echo "return: ", $scope === $s ? "s" : "other", "\n";
}
$c = $s->run(fn() => cleanup());
$c->current();
try {
    unset($c);   // the destructor runs the finally, which yields: an error, as without the extension
} catch (Error $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
echo "done\n";
?>
--EXPECT--
1 s
caught e1
return: s
cleanup in scope: true
Error: Cannot yield from finally in a force-closed generator
done
