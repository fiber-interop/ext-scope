--TEST--
Locale/timezone: fibers and generators of one scope share its timezone
--EXTENSIONS--
scope
--FILE--
<?php
date_default_timezone_set("UTC");
$s = new Scope();
$s->run(function () {
    date_default_timezone_set("Asia/Tokyo");
    $f = new Fiber(function () { echo date_default_timezone_get(), "\n"; Fiber::suspend(); date_default_timezone_set("Europe/Oslo"); });
    $f->start();
    $g = (function () { yield date_default_timezone_get(); yield date_default_timezone_get(); })();
    echo $g->current(), "\n";
    $f->resume();
    $g->next();
    echo $g->current(), "\n";
});
echo date_default_timezone_get(), "\n";
--EXPECT--
Asia/Tokyo
Asia/Tokyo
Europe/Oslo
UTC
