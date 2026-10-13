--TEST--
Scope::run() sets the current scope for the callback and restores the previous one, also on exceptions; calls nest
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$a = named('A'); $b = named('B');

echo "main: ", current_name(), "\n";
echo "run with args: ", $a->run(fn($x, $y) => current_name() . $x . $y, '1', y: '2'), "\n";
echo "after: ", current_name(), "\n";
$a->run(function () use ($a, $b) {
    $b->run(fn() => print("nested: " . current_name() . "\n"));
    echo "back in: ", current_name(), "\n";
    $a->run(fn() => print("same scope again: " . current_name() . "\n"));
});
try {
    $a->run(fn() => throw new RuntimeException('boom'));
} catch (RuntimeException $e) {
    echo "after exception: ", current_name(), "\n";
}
try { clone $a; } catch (Error $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
try { serialize($a); } catch (Exception $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
?>
--EXPECT--
main: root
run with args: A12
after: root
nested: B
back in: A
same scope again: A
after exception: root
Error: Trying to clone an uncloneable object of class Scope
Exception: Serialization of 'Scope' is not allowed
