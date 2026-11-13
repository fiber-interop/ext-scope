--TEST--
Copy on construction: a scope constructed from the root scope gets the CLI's arrays and no session
--EXTENSIONS--
scope
session
--FILE--
<?php
$s = new Scope();
$s->run(function () {
    var_dump(isset($_SERVER["argv"]), $_SERVER["argc"] === 1, isset($_SERVER["PATH"]), $_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST);
    var_dump(session_status() === PHP_SESSION_NONE, isset($_SESSION));
    var_dump(date_default_timezone_get());
});
// the server case: fill the request's entries over the environment the copy brought
$s->run(function () {
    $_GET = ["q" => 1];
    $_SERVER = ["REQUEST_URI" => "/x"] + $_SERVER;
});
echo json_encode([$_GET, isset($_SERVER["REQUEST_URI"])]), "\n";
echo json_encode($s->run(fn() => [$_GET, $_SERVER["REQUEST_URI"], isset($_SERVER["argv"])])), "\n";
--EXPECT--
bool(true)
bool(true)
bool(true)
array(0) {
}
array(0) {
}
array(0) {
}
array(0) {
}
array(0) {
}
bool(true)
bool(false)
string(3) "UTC"
[[],false]
[{"q":1},"\/x",true]
