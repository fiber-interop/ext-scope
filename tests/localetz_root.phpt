--TEST--
Locale/timezone: the root scope's values are today's (ini); a scope's change never leaks to the root
--EXTENSIONS--
scope
--INI--
date.timezone=Europe/Oslo
--FILE--
<?php
echo date_default_timezone_get(), "\n";
date_default_timezone_set("Asia/Tokyo");
$s = new Scope();
$s->run(function () { echo date_default_timezone_get(), "\n"; date_default_timezone_set("America/Chicago"); });
echo date_default_timezone_get(), "\n";
--EXPECT--
Europe/Oslo
Asia/Tokyo
Asia/Tokyo
