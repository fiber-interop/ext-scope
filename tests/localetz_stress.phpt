--TEST--
Locale/timezone: 1000 scopes with their own timezone, no leak, root intact
--EXTENSIONS--
scope
--FILE--
<?php
date_default_timezone_set("UTC");
$zones = ["Asia/Tokyo", "Europe/Oslo", "America/Chicago", "Australia/Sydney"];
$m0 = memory_get_usage();
for ($r = 0; $r < 3; $r++) {
    $scopes = [];
    for ($i = 0; $i < 1000; $i++) {
        $s = new Scope();
        $z = $zones[$i % 4];
        $s->run(fn() => date_default_timezone_set($z));
        $scopes[] = $s;
    }
    $bad = 0;
    foreach ($scopes as $i => $s) { if ($s->run(fn() => date_default_timezone_get()) !== $zones[$i % 4]) $bad++; }
    echo "bad=$bad root=", date_default_timezone_get(), "\n";
    unset($scopes, $s);
    if ($r == 0) { $m1 = memory_get_usage(); }
}
echo memory_get_usage() - $m1 < 65536 ? "no leak\n" : "leak\n";
--EXPECT--
bad=0 root=UTC
bad=0 root=UTC
bad=0 root=UTC
no leak
