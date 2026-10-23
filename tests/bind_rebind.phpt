--TEST--
Scope::bind(): binding a closure bind() returned replaces its scope instead of nesting
--EXTENSIONS--
scope
--FILE--
<?php
$a = new Scope(); $b = new Scope();
$fn = fn() => Scope::getCurrent();
$inB = $b->bind($fn);
$inA = $a->bind($inB);
var_dump($inA() === $a, $inB() === $b, $b->bind($inA)() === $b);
var_dump(Scope::getRootScope()->bind($inA)() === Scope::getRootScope());
unset($fn, $inB);
var_dump($inA() === $a);
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
