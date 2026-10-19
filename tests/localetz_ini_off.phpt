--TEST--
Locale/timezone: with scope.locale_timezone=0 every scope shares the one timezone
--EXTENSIONS--
scope
--INI--
scope.locale_timezone=0
--FILE--
<?php
date_default_timezone_set("UTC");
$s = new Scope();
$s->run(fn() => date_default_timezone_set("Asia/Tokyo"));
echo date_default_timezone_get(), "\n";
--EXPECT--
Asia/Tokyo
