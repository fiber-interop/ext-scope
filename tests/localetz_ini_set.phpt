--TEST--
Locale/timezone: ini_set('date.timezone') is a request-wide ini; it is the fallback of a scope that has not set its own
--EXTENSIONS--
scope
--INI--
date.timezone=UTC
--FILE--
<?php
$a = new Scope(); $b = new Scope();
$a->run(function () { date_default_timezone_set("Asia/Tokyo"); ini_set("date.timezone", "Europe/Oslo"); });
// a has its own value, which the ini does not override; b never set one and follows the ini
echo $a->run(fn() => date_default_timezone_get()), " ", $b->run(fn() => date_default_timezone_get()), " ", date_default_timezone_get(), "\n";
--EXPECT--
Asia/Tokyo Europe/Oslo Europe/Oslo
