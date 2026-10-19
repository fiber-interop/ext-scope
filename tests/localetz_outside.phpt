--TEST--
Locale/timezone: code outside any scope sees no change while scopes run
--EXTENSIONS--
scope
--FILE--
<?php
date_default_timezone_set("UTC");
$s = new Scope();
$f = new Fiber(function () { echo date_default_timezone_get(), "\n"; Fiber::suspend(); echo date_default_timezone_get(), "\n"; });
$s->run(function () use ($f) { date_default_timezone_set("Asia/Tokyo"); });
$f->start();
echo date_default_timezone_get(), "\n";
$f->resume();
--EXPECT--
UTC
UTC
UTC
