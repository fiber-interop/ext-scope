--TEST--
Locale/timezone: with scope.locale_timezone=0 the locale is shared too
--EXTENSIONS--
scope
--SKIPIF--
<?php if (!extension_loaded("intl")) die("skip intl not loaded"); ?>
--INI--
scope.locale_timezone=0
--FILE--
<?php
Locale::setDefault("en_US");
(new Scope())->run(fn() => Locale::setDefault("nb_NO"));
echo Locale::getDefault(), "\n";
--EXPECT--
nb_NO
