--TEST--
Locale/timezone: the default locale follows the scope: root, ini value at creation, isolation, sharing
--EXTENSIONS--
scope
--SKIPIF--
<?php if (!extension_loaded("intl")) die("skip intl not loaded"); ?>
--INI--
intl.default_locale=en_US
--FILE--
<?php
Locale::setDefault("it_IT");
$a = new Scope();                       // starts with the ini value en_US
Locale::setDefault("de_DE");
$b = new Scope();                       // en_US too, not de_DE
$a->run(function () { echo Locale::getDefault(), " "; Locale::setDefault("nb_NO"); });
echo Locale::getDefault(), " ", $a->run(fn() => Locale::getDefault()), " ", $b->run(fn() => Locale::getDefault()), "\n";
$a->run(function () {
    $f = new Fiber(function () { echo Locale::getDefault(), " "; Locale::setDefault("fr_FR"); });
    $f->start();
    $g = (function () { yield Locale::getDefault(); })();
    echo $g->current(), "\n";
});
echo Locale::getDefault(), " ", $a->run(fn() => Locale::getDefault()), " ", ini_get("intl.default_locale"), "\n";
--EXPECT--
en_US de_DE nb_NO en_US
nb_NO fr_FR
de_DE fr_FR de_DE
