--TEST--
Session copy: the lock belongs to the opener; a copy is no second opening, destroying an active copy leaves the opener's session open, a real second session_start() of the id still throws
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
$id = "scopelock" . getmypid();
@unlink("$dir/sess_$id");
session_id($id);
session_start();
$_SESSION["v"] = 1;

$copy = new Scope();
$nested = $copy->run(fn() => new Scope());     // a copy of the copy
echo "constructed\n";
// a second real opening of the id in another scope throws, whether or not copies exist
$other = new Scope();
$other->run(function () use ($id) {
    // the copy it got is active; close it, then try to open for real
    session_write_close();
    session_id($id);
    try { session_start(); echo "started\n"; } catch (RuntimeException $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
});
// destroying active copies does not close or unlock the opener's session
unset($copy, $nested, $other);
echo session_status() === PHP_SESSION_ACTIVE ? "opener active\n" : "opener broken\n";
$_SESSION["v"] = 2;
// the lock is still held: a flock from another descriptor must not get it
$fp = fopen("$dir/sess_$id", "r");
var_dump(flock($fp, LOCK_EX | LOCK_NB));
fclose($fp);
session_write_close();
echo file_get_contents("$dir/sess_$id"), "\n";
$fp = fopen("$dir/sess_$id", "r");
var_dump(flock($fp, LOCK_EX | LOCK_NB));
fclose($fp);
// regenerating the id in a copy would wait for the opener's lock
session_start();
$c = new Scope();
$c->run(function () {
    try { session_regenerate_id(); } catch (RuntimeException $e) { echo get_class($e), "\n"; }
});
unset($c);
session_write_close();
// a copy does not count as an opening: once the opener closed, another scope may start the session
session_start();
$d = new Scope();
session_write_close();
$e = new Scope();
$e->run(function () use ($id) { session_id($id); session_start(); echo "started in e\n"; session_write_close(); });
$d->run(function () { echo session_status() === PHP_SESSION_ACTIVE ? "d active\n" : "d not active\n"; });
$d->run(function () { session_write_close(); echo "d closed\n"; });
echo "done\n";
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopelock*") as $f) @unlink($f);
--EXPECTF--
constructed
%s
opener active
bool(false)
v|i:2;
bool(true)
RuntimeException
started in e
d active
d closed
done
