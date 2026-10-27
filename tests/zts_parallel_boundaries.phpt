--TEST--
ZTS: a Scope never crosses a thread (parallel refuses it as an argument), each thread's root scope is its own, and threads that end with scopes alive, sessions open, shut down cleanly
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
$root = Scope::getRootScope();
try {
    (new parallel\Runtime())->run(fn(Scope $s) => 1, [new Scope()])->value();
    echo "passed a Scope\n";
} catch (Throwable $e) {
    echo "refused: ", get_class($e), "\n";
}

// a thread's root is the root of that thread: not equal to ours, and stable within it
echo (new parallel\Runtime())->run(function () {
    $r = Scope::getRootScope();
    $s = new Scope();
    return ($r === Scope::getCurrent() ? "root" : "?") . " "
        . ($s->run(fn() => Scope::getCurrent()) === $s ? "own" : "?") . " "
        . ($s->run(fn() => Scope::getRootScope()) === $r ? "same root" : "?");
})->value(), "\n";

// threads that end while scopes are alive, with active sessions and suspended fibers
$fs = [];
for ($t = 0; $t < 4; $t++) {
    $fs[] = (new parallel\Runtime())->run(function (int $t) {
        global $keep;
        $keep = [];
        for ($i = 0; $i < 20; $i++) {
            $s = new Scope();
            $s->run(function () use ($t, $i) {
                $_POST = ["n" => $i];
                if (extension_loaded("session")) {
                    session_id("scopeztsend" . getmypid() . "t{$t}i{$i}");
                    session_start();
                    $_SESSION["i"] = $i;
                }
            });
            $f = $s->run(fn() => new Fiber(fn() => Fiber::suspend()));
            $f->start();
            $keep[] = [$s, $f];
        }
        return $t;
    }, [$t]);
}
$sum = 0;
foreach ($fs as $f) $sum += $f->value();
echo "threads done: $sum\n";
echo "main root unchanged: ", var_export(Scope::getRootScope() === $root, true), "\n";
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopezts*") as $f) @unlink($f);
--EXPECTF--
refused: %s
root own same root
threads done: 6
main root unchanged: true
