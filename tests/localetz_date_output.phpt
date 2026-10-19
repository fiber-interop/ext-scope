--TEST--
Locale/timezone: date() output reflects the scope's timezone
--EXTENSIONS--
scope
--FILE--
<?php
date_default_timezone_set("UTC");
$s = new Scope();
$s->run(fn() => date_default_timezone_set("Asia/Tokyo"));
echo date("H:i T", 0), " | ", $s->run(fn() => date("H:i T", 0)), " | ", date("H:i T", 0), "\n";
echo $s->run(fn() => (new DateTime("@0"))->setTimezone(new DateTimeZone(date_default_timezone_get()))->format("H:i")), "\n";
--EXPECT--
00:00 UTC | 09:00 JST | 00:00 UTC
09:00
