--TEST--
Locale/timezone: a new scope starts with the ini values, not its creator's, wherever it is created
--EXTENSIONS--
scope
--INI--
date.timezone=UTC
--FILE--
<?php
date_default_timezone_set("Europe/Oslo");
$a = new Scope();                       // created while root is Oslo
date_default_timezone_set("Asia/Tokyo");
echo $a->run(fn() => date_default_timezone_get()), "\n";   // ini
$b = new Scope();                       // created while root is Tokyo
echo $b->run(fn() => date_default_timezone_get()), "\n";   // ini
$a->run(function () {
    date_default_timezone_set("America/Chicago");
    $c = new Scope();                   // created inside a: starts with the ini value
    date_default_timezone_set("UTC");
    echo $c->run(fn() => date_default_timezone_get()), "\n";
});
echo date_default_timezone_get(), "\n";
--EXPECT--
UTC
UTC
UTC
Asia/Tokyo
