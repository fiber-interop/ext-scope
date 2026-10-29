--TEST--
ZTS: scopes in several threads at once (parallel): each thread has its own root scope, and its scopes keep their superglobals, session, timezone and locale apart while fibers and generators switch between them
--EXTENSIONS--
scope
--SKIPIF--
<?php
if (!PHP_ZTS) die("skip ZTS only");
if (!extension_loaded("parallel")) die("skip parallel not loaded");
?>
--INI--
session.save_handler=files
session.save_path={TMP}
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
$work = function (int $t): string {
    ob_start();
    $out = [];
    $scopes = [];
    foreach (["a", "b"] as $n) {
        $s = new Scope();
        $s->run(function () use ($t, $n) {
            $_GET = ["who" => "$t$n"];
            date_default_timezone_set($n === "a" ? "Europe/Oslo" : "Asia/Tokyo");
            if (extension_loaded("intl")) Locale::setDefault($n === "a" ? "nb_NO" : "ja_JP");
            if (extension_loaded("session")) {
                session_id("scopezts" . getmypid() . "t{$t}{$n}");
                session_start();
                $_SESSION["who"] = "$t$n";
            }
        });
        $scopes[$n] = $s;
    }
    // fibers started in and generators created in both scopes, stepped alternately from the root
    $fibers = $gens = [];
    foreach ($scopes as $n => $s) {
        $fibers[$n] = new Fiber(function () {
            while (true) Fiber::suspend($_GET["who"] . "/" . date_default_timezone_get()
                . "/" . (extension_loaded("intl") ? Locale::getDefault() : "-")
                . "/" . ($_SESSION["who"] ?? "-"));
        });
        $gens[$n] = $s->run(fn() => (function () { while (true) yield $_GET["who"]; })());
    }
    $bad = 0;
    $first = [];
    foreach ($fibers as $n => $f) $first[$n] = $scopes[$n]->run(fn() => $f->start());
    for ($i = 0; $i < 2000; $i++) {
        foreach ($fibers as $n => $f) {
            if ($f->resume() !== $first[$n]) $bad++;
            $gens[$n]->next();
            if ($gens[$n]->current() !== "$t$n") $bad++;
        }
    }
    foreach ($scopes as $s) $s->run(function () { if (session_status() === PHP_SESSION_ACTIVE) session_write_close(); });
    $root = json_encode($_GET) . "/" . date_default_timezone_get() . "/" . (isset($_SESSION) ? "session" : "-");
    ob_end_clean();
    return "$t: " . implode(" ", $first) . " bad=$bad root=$root";
};

$futures = [];
for ($t = 1; $t <= 4; $t++) {
    $futures[$t] = (new parallel\Runtime())->run($work, [$t]);
}
$results = [];
foreach ($futures as $t => $f) $results[$t] = $f->value();
ksort($results);
echo implode("\n", $results), "\n";
echo "main: ", json_encode($_GET), " ", date_default_timezone_get(), "\n";
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopezts*") as $f) @unlink($f);
--EXPECTF--
1: 1a/Europe/Oslo/%s/1a 1b/Asia/Tokyo/%s/1b bad=0 root=[]/UTC/-
2: 2a/Europe/Oslo/%s/2a 2b/Asia/Tokyo/%s/2b bad=0 root=[]/UTC/-
3: 3a/Europe/Oslo/%s/3a 3b/Asia/Tokyo/%s/3b bad=0 root=[]/UTC/-
4: 4a/Europe/Oslo/%s/4a 4b/Asia/Tokyo/%s/4b bad=0 root=[]/UTC/-
main: [] UTC
