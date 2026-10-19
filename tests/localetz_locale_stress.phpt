--TEST--
Locale/timezone: 1000 scopes with their own locale and timezone, no leak, root intact
--EXTENSIONS--
scope
--SKIPIF--
<?php if (!extension_loaded("intl")) die("skip intl not loaded"); ?>
--FILE--
<?php
Locale::setDefault("en_US"); date_default_timezone_set("UTC");
$l=["nb_NO","de_DE","fr_FR"]; $z=["Asia/Tokyo","Europe/Oslo"];
for($r=0;$r<2;$r++){ $ss=[];
for($i=0;$i<1000;$i++){ $s=new Scope(); $s->run(function() use($i,$l,$z){ Locale::setDefault($l[$i%3]); date_default_timezone_set($z[$i%2]); }); $ss[]=$s; }
$bad=0; foreach($ss as $i=>$s){ if($s->run(fn()=>Locale::getDefault().date_default_timezone_get())!==$l[$i%3].$z[$i%2]) $bad++; }
echo "bad=$bad ", Locale::getDefault(), date_default_timezone_get(), "\n"; }
--EXPECT--
bad=0 en_USUTC
bad=0 en_USUTC
