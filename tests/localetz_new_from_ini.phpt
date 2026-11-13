--TEST--
Locale/timezone: a new scope starts with its creator's timezone and locale as they are when it is constructed
--EXTENSIONS--
scope
intl
--INI--
date.timezone=Europe/Oslo
intl.default_locale=nb_NO
--FILE--
<?php
date_default_timezone_set("Asia/Tokyo"); Locale::setDefault("de_DE");
$a = new Scope();
$a->run(function () {
    echo date_default_timezone_get(), " ", Locale::getDefault(), "\n";
    date_default_timezone_set("America/Chicago"); Locale::setDefault("fr_FR");
    $b = new Scope();
    echo $b->run(fn() => date_default_timezone_get() . " " . Locale::getDefault()), "\n";
    echo date_default_timezone_get(), " ", Locale::getDefault(), "\n";
});
echo date_default_timezone_get(), " ", Locale::getDefault(), "\n";
--EXPECT--
Asia/Tokyo de_DE
America/Chicago fr_FR
America/Chicago fr_FR
Asia/Tokyo de_DE
