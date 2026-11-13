--TEST--
Locale/timezone: a copy of a scope that never set a timezone keeps following ini_set('date.timezone'); a copy of one that did has the creator's value
--EXTENSIONS--
scope
--INI--
date.timezone=UTC
--FILE--
<?php
$a = new Scope();                            // root never set a timezone
echo $a->run(fn() => date_default_timezone_get()), "\n";
ini_set('date.timezone', 'Europe/Oslo');
echo $a->run(fn() => date_default_timezone_get()), " ", date_default_timezone_get(), "\n";
$a->run(function () {
    $b = new Scope();                        // a never set one either
    ini_set('date.timezone', 'Asia/Tokyo');
    echo $b->run(fn() => date_default_timezone_get()), "\n";
    date_default_timezone_set('America/Chicago');
    $c = new Scope();                        // a has set one: c has it, and ignores the ini from now
    ini_set('date.timezone', 'UTC');
    echo $c->run(fn() => date_default_timezone_get()), " ", date_default_timezone_get(), "\n";
    $c->run(fn() => date_default_timezone_set('Pacific/Auckland'));
    echo date_default_timezone_get(), "\n";
});
echo date_default_timezone_get(), "\n";
date_default_timezone_set('Europe/Paris');
$d = new Scope();
date_default_timezone_set('Europe/Rome');
echo $d->run(fn() => date_default_timezone_get()), " ", date_default_timezone_get(), "\n";
--EXPECT--
UTC
Europe/Oslo Europe/Oslo
Asia/Tokyo
America/Chicago America/Chicago
America/Chicago
UTC
Europe/Paris Europe/Rome
