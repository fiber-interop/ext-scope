--TEST--
Session: settings are request-wide (session_name(), cookie parameters, the cache limiter and ini_set() change them for every scope), while each scope's session stays its own
--EXTENSIONS--
scope
session
--INI--
session.save_handler=files
session.save_path={TMP}
session.use_cookies=1
session.use_strict_mode=0
session.name=INI
session.cache_limiter=
--FILE--
<?php
ob_start();   // in the CLI the first output counts as sent headers, and session settings refuse changes after that
$show = fn() => session_name() . " " . session_cache_limiter() . " " . session_get_cookie_params()["path"] . " " . ini_get("session.name");
$a = new Scope();
$b = new Scope();
$a->run(function () {
    session_name("A"); session_cache_limiter("private"); session_set_cookie_params(["path" => "/a"]);
});
echo "root: ", $show(), "\n";
echo "b: ", $b->run($show), "\n";
ini_set("session.name", "ROOT");
echo "a: ", $a->run($show), "\n";

$p = "scopeini" . getmypid();
$a->run(function () use ($p) { session_id("{$p}a"); session_start(); $_SESSION["s"] = "a"; });
$b->run(function () use ($p) { session_id("{$p}b"); session_start(); $_SESSION["s"] = "b"; });
$a->run(function () { echo "a session: ", substr(session_id(), -1), " ", $_SESSION["s"], " ", session_name(), "\n"; session_write_close(); });
$b->run(function () { echo "b session: ", substr(session_id(), -1), " ", $_SESSION["s"], " ", session_name(), "\n"; session_write_close(); });
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopeini*") as $f) @unlink($f);
--EXPECT--
root: A private /a A
b: A private /a A
a: ROOT private /a ROOT
a session: a a ROOT
b session: b b ROOT
