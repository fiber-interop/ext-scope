--TEST--
Scope::bind(): runs in the bound scope from anywhere, keeps the signature, $this and class scope; restored on throw
--EXTENSIONS--
scope
--FILE--
<?php
class Box {
    private $v = "box";
    function make() { return function (int &$x, int $step = 5): int { $x += $step; return $this->v === "box" ? $x : -1; }; }
}
$a = new Scope(); $b = new Scope();
$fn = (new Box)->make();
$bound = $a->bind($fn);
var_dump($bound !== $fn);
$n = 1;
var_dump($b->run(function () use ($bound, &$n, $a, $b) {
    $r = $bound($n);                          // by-ref param and default value
    return [$r, Scope::getCurrent() === $b];
}));
var_dump($n, $bound($n, step: 2), $n);
$r = new ReflectionFunction($bound);
var_dump($r->getNumberOfParameters(), $r->getNumberOfRequiredParameters(), $r->getParameters()[0]->isPassedByReference(),
    (string) $r->getReturnType());
// the original is unchanged: it runs in its caller's scope
$where = fn() => Scope::getCurrent();
var_dump($b->run($where) === $b, $a->bind($where)() === $a, $where() === Scope::getRootScope());
// a generator created inside a bound call belongs to the bound scope
$gen = $a->bind(function () { return (function () { yield Scope::getCurrent(); })(); })();
var_dump($gen->getScope() === $a, $gen->current() === $a);
// an exception propagates and the scope is restored
$b->run(function () use ($a, $b) {
    try { $a->bind(function () { throw new Exception("boom"); })(); }
    catch (Exception $e) { var_dump($e->getMessage(), Scope::getCurrent() === $b); }
});
var_dump(Scope::getCurrent() === Scope::getRootScope());
// nesting: a bound closure calling another
$inner = $b->bind(fn() => Scope::getCurrent());
var_dump($a->bind(fn() => [$inner() === $b, Scope::getCurrent() === $a])());
// static closures and variadics
$v = $a->bind(static fn(...$xs) => count($xs) . (Scope::getCurrent() === $a ? "a" : "?"));
var_dump($v(1, 2, 3));
--EXPECT--
bool(true)
array(2) {
  [0]=>
  int(6)
  [1]=>
  bool(true)
}
int(6)
int(8)
int(8)
int(2)
int(1)
bool(true)
string(3) "int"
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
string(4) "boom"
bool(true)
bool(true)
array(2) {
  [0]=>
  bool(true)
  [1]=>
  bool(true)
}
string(2) "3a"
