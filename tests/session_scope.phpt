--TEST--
Session: each scope has its own session; a new scope has none; fibers share their scope's; destruction writes it
--EXTENSIONS--
scope
session
--INI--
session.save_handler=files
session.use_cookies=0
session.save_path={TMP}
session.cache_limiter=
--FILE--
<?php
$dir = sys_get_temp_dir();
$p = "scope" . getmypid();
foreach (["a", "b", "c"] as $x) @unlink("$dir/sess_$p$x");
$a = new Scope(); $b = new Scope();
$a->run(function () {
    session_id($GLOBALS["p"] . "a"); session_start(); $_SESSION["who"] = "A";
});
echo "root: ", session_status() === PHP_SESSION_NONE ? "none" : "active", "\n";
$b->run(function () {
    echo "B before: ", session_status() === PHP_SESSION_NONE ? "none" : "active", " ", json_encode($_SESSION ?? null), "\n";
    session_id($GLOBALS["p"] . "b"); session_start(); $_SESSION["who"] = "B";
    echo "B: ", substr(session_id(), -1), " ", json_encode($_SESSION), "\n";
});
$a->run(function () {
    echo "A: ", substr(session_id(), -1), " ", json_encode($_SESSION), "\n";
    $f = new Fiber(function () { $_SESSION["fiber"] = 1; echo "A fiber: ", substr(session_id(), -1), "\n"; });
    $f->start();
});
echo "root: ", json_encode($_SESSION ?? null), "\n";
$b->run(fn() => session_write_close());
echo "B file: ", file_get_contents("$dir/sess_{$p}b"), "\n";
var_dump(file_exists("$dir/sess_{$p}a") ? file_get_contents("$dir/sess_{$p}a") : "");
unset($a);   // destroyed with its session active: written and closed
echo "A file: ", file_get_contents("$dir/sess_{$p}a"), "\n";
$c = new Scope();
$c->run(function () { session_id($GLOBALS["p"] . "c"); session_start(); $_SESSION["c"] = 3; });
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scope*") as $f) @unlink($f);
--EXPECTF--
root: none
B before: none null
B: b {"who":"B"}
A: a {"who":"A"}
A fiber: a
root: null
B file: who|s:1:"B";
string(0) ""
A file: who|s:1:"A";fiber|i:1;
