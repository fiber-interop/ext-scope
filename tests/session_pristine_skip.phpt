--TEST--
Session: scopes without a session share nothing when the session is not moved between them; a scope that starts one, or changes its module, gets its own
--EXTENSIONS--
scope
session
--INI--
session.save_handler=files
session.save_path={TMP}
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
ob_start();   // in the CLI the first output counts as sent headers, and the save handler refuses changes after that
$p = "scopepristine" . getmypid();
$a = new Scope();
$b = new Scope();
$c = new Scope();
$st = fn() => session_status() . ":" . session_module_name() . ":" . (session_id() === "" ? "-" : substr(session_id(), -1));

for ($i = 0; $i < 2; $i++) { $a->run(fn() => null); $b->run(fn() => null); }
echo "pristine: ", $a->run($st), " ", $b->run($st), " ", $st(), "\n";

$b->run(function () use ($p) { session_id("{$p}b"); session_start(); $_SESSION["b"] = 1; });
echo "b started: ", $a->run($st), " ", $b->run($st), " ", $st(), "\n";

$c->run(function () { session_set_save_handler(new class extends SessionHandler {}); });
echo "c handler: ", $c->run(fn() => session_module_name()), " a: ", $a->run(fn() => session_module_name()), "\n";

$a->run(function () { session_id("x"); });   // an id without a session is the scope's too
echo "ids: ", $a->run($st), " ", $c->run($st), " ", $st(), "\n";
$b->run(fn() => session_write_close());
echo "b: ", $b->run(fn() => json_encode($_SESSION)), "; a has \$_SESSION: ", $a->run(fn() => var_export(isset($_SESSION), true)), "\n";
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopepristine*") as $f) @unlink($f);
--EXPECT--
pristine: 1:files:- 1:files:- 1:files:-
b started: 1:files:- 2:files:b 1:files:-
c handler: user a: files
ids: 1:files:x 1:user:- 1:files:-
b: {"b":1}; a has $_SESSION: false
