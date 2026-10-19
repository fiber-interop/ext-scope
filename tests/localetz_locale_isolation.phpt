--TEST--
Locale/timezone: the default locale follows the scope: root, inheritance, isolation, sharing
--EXTENSIONS--
scope
--SKIPIF--
<?php if (!extension_loaded("intl")) die("skip intl not loaded"); ?>
--FILE--
<?php
Locale::setDefault("en_US");
$a = new Scope();                       // inherits en_US
Locale::setDefault("de_DE");
$b = new Scope();                       // inherits de_DE
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
en_US de_DE nb_NO de_DE
nb_NO fr_FR
de_DE fr_FR de_DE
