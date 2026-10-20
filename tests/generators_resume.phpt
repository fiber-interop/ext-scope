--TEST--
A generator's body runs in its own scope however it is resumed (foreach, send, throw, destructor), and the caller's scope is back after each yield, return or throw
--EXTENSIONS--
scope
--FILE--
<?php
require __DIR__ . '/names.inc';
$a = named('A'); $b = named('B');

$make = fn() => $a->run(fn() => (function () {
    try {
        $got = yield 'body ' . current_name();
        echo "  sent '$got', body ", current_name(), "\n";
        yield 'body ' . current_name();
        yield 'body ' . current_name();
    } catch (Exception $e) {
        echo "  caught '", $e->getMessage(), "', body ", current_name(), "\n";
        yield 'body after catch ' . current_name();
    } finally {
        echo "  finally, body ", current_name(), "\n";
    }
})());

echo "foreach from B:\n";
$b->run(function () use ($make) {
    foreach ($make() as $k => $v) {
        echo "  $k: $v, consumer ", current_name(), "\n";
    }
    echo "  after foreach: ", current_name(), "\n";
});

echo "send from B:\n";
$b->run(function () use ($make) {
    $g = $make();
    echo "  ", $g->current(), ", consumer ", current_name(), "\n";
    $v = $g->send('x');
    echo "  $v, consumer ", current_name(), "\n";
});

echo "throw from B:\n";
$b->run(function () use ($make) {
    $g = $make();
    $g->current();
    $v = $g->throw(new Exception('boom'));
    echo "  $v, consumer ", current_name(), "\n";
    unset($g);
    echo "  after destructor: ", current_name(), "\n";
});

echo "uncaught throw from B:\n";
$b->run(function () {
    $g = $GLOBALS['a']->run(fn() => (function () { yield 1; throw new Exception('out of ' . current_name()); })());
    $g->current();
    try { $g->next(); } catch (Exception $e) { echo "  ", $e->getMessage(), ", consumer ", current_name(), "\n"; }
    echo "  getReturn on unfinished generator: ";
    $h = $GLOBALS['a']->run(fn() => (function () { echo "body ", current_name(), "; "; yield 1; return 2; })());
    try { $h->getReturn(); } catch (Exception $e) { echo get_class($e), ", consumer ", current_name(), "\n"; }
});
echo "main: ", current_name(), "\n";
?>
--EXPECT--
foreach from B:
  0: body A, consumer B
  sent '', body A
  1: body A, consumer B
  2: body A, consumer B
  finally, body A
  after foreach: B
send from B:
  body A, consumer B
  sent 'x', body A
  body A, consumer B
  finally, body A
throw from B:
  caught 'boom', body A
  body after catch A, consumer B
  finally, body A
  after destructor: B
uncaught throw from B:
  out of A, consumer B
  getReturn on unfinished generator: body A; Exception, consumer B
main: root
