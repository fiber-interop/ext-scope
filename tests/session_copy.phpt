--TEST--
Session copy: a scope constructed while a session is active holds the same id and status and a copy of $_SESSION; writes are independent; the later write wins
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
$id = "scopecopy" . getmypid();
@unlink("$dir/sess_$id");
session_id($id);
session_start();
$_SESSION = ["n" => 1, "list" => [1, 2]];
$copy = new Scope();
$_SESSION["after"] = 1;                       // creator writes after construction
$copy->run(function () use ($id) {
    echo session_id() === $id ? "same id" : "other id", ", ", session_status() === PHP_SESSION_ACTIVE ? "active" : "not active", "\n";
    echo json_encode($_SESSION), "\n";
    $_SESSION["n"] = 2; $_SESSION["copy"] = 1;
    echo json_encode($_SESSION), "\n";
});
echo json_encode($_SESSION), "\n";            // the copy's writes did not reach the creator
// the copy writes its $_SESSION under the id; the creator's later write wins
$copy->run(function () { session_write_close(); echo session_status() === PHP_SESSION_NONE ? "copy closed" : "copy open", "\n"; });
echo file_get_contents("$dir/sess_$id"), "\n";
echo session_status() === PHP_SESSION_ACTIVE ? "creator still active\n" : "creator closed\n";
session_write_close();
echo file_get_contents("$dir/sess_$id"), "\n";
// a copy made after the session was closed: same id, status none, $_SESSION as it was
$late = new Scope();
$late->run(function () { echo session_status() === PHP_SESSION_NONE ? "none" : "active", " ", json_encode($_SESSION), "\n"; });
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopecopy*") as $f) @unlink($f);
--EXPECT--
same id, active
{"n":1,"list":[1,2]}
{"n":2,"list":[1,2],"copy":1}
{"n":1,"list":[1,2],"after":1}
copy closed
n|i:2;list|a:2:{i:0;i:1;i:1;i:2;}copy|i:1;
creator still active
n|i:1;list|a:2:{i:0;i:1;i:1;i:2;}after|i:1;
none {"n":1,"list":[1,2],"after":1}
