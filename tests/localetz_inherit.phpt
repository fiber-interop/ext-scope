--TEST--
Locale/timezone: a new scope starts with its creator's timezone as it is when the scope is constructed
--EXTENSIONS--
scope
--INI--
date.timezone=UTC
--FILE--
<?php
date_default_timezone_set("Europe/Oslo");
$a = new Scope();                       // created while root is Oslo
date_default_timezone_set("Asia/Tokyo");
echo $a->run(fn() => date_default_timezone_get()), "\n";   // Oslo
$b = new Scope();                       // created while root is Tokyo
echo $b->run(fn() => date_default_timezone_get()), "\n";   // Tokyo
$a->run(function () {
    date_default_timezone_set("America/Chicago");
    $c = new Scope();                   // created inside a: starts with a's Chicago
    date_default_timezone_set("UTC");
    echo $c->run(fn() => date_default_timezone_get()), "\n";
});
echo date_default_timezone_get(), "\n";
--EXPECT--
Europe/Oslo
Asia/Tokyo
America/Chicago
Asia/Tokyo
