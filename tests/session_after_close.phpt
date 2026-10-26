--TEST--
Session: in a scope, $_SESSION keeps its array after session_write_close(), session_abort() and session_destroy(), as in a request, and ending another scope's session leaves it alone
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
$p = "scopeclose" . getmypid();
foreach (["close", "abort", "destroy"] as $how) {
    $s = new Scope();
    $s->run(function () use ($how, $p) {
        session_id("$p$how"); session_start(); $_SESSION["how"] = $how;
        match ($how) { "close" => session_write_close(), "abort" => session_abort(), "destroy" => session_destroy() };
        echo "$how: ", session_status() === PHP_SESSION_NONE ? "none" : "active", " ", json_encode($_SESSION ?? "unset"), "\n";
    });
    $s->run(function () { echo "  again: ", json_encode($_SESSION ?? "unset"), "\n"; });
}
echo "root: ", isset($_SESSION) ? "set" : "unset", "\n";
$a = new Scope();
$a->run(function () use ($p) { session_id("{$p}a"); session_start(); $_SESSION["a"] = 1; });
$_SESSION = ["root" => true];
$a = null;   // its session is written and closed; the root's $_SESSION is the root's
echo "root: ", json_encode($_SESSION), "\n";
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopeclose*") as $f) @unlink($f);
--EXPECT--
close: none {"how":"close"}
  again: {"how":"close"}
abort: none {"how":"abort"}
  again: {"how":"abort"}
destroy: none {"how":"destroy"}
  again: {"how":"destroy"}
root: unset
root: {"root":true}
