--TEST--
Locale/timezone: sibling scopes are isolated, and nested runs restore the outer value
--EXTENSIONS--
scope
--FILE--
<?php
date_default_timezone_set("UTC");
$a = new Scope(); $b = new Scope();
$a->run(fn() => date_default_timezone_set("Asia/Tokyo"));
$b->run(fn() => date_default_timezone_set("America/Chicago"));
echo date_default_timezone_get(), " ", $a->run(fn() => date_default_timezone_get()), " ", $b->run(fn() => date_default_timezone_get()), "\n";
echo $a->run(fn() => date_default_timezone_get() . " " . $b->run(fn() => date_default_timezone_get()) . " " . date_default_timezone_get()), "\n";
--EXPECT--
UTC Asia/Tokyo America/Chicago
Asia/Tokyo America/Chicago Asia/Tokyo
