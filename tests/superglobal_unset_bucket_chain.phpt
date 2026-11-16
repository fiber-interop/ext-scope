--TEST--
unset() of a superglobal inside a scope, with $_REQUEST set too, leaves the symbol table's hash chains intact (hang on PHP 8.2-8.4 before 0.6.1)
--EXTENSIONS--
scope
--FILE--
<?php
$_GET = ["g" => 1]; $_POST = ["p" => 1]; $_COOKIE = ["c" => 1]; $_REQUEST = ["r" => 1];
$a = new Scope();
$a->run(function () {
    $_GET = ["a" => 1]; $_REQUEST = ["ra" => 1];
    unset($_GET);
});
for ($i = 0; $i < 3; $i++) {
    echo json_encode([$_GET, $_REQUEST]), " ";
    echo $a->run(fn() => isset($_GET) ? "set" : "unset"), " ";
    echo json_encode($a->run(fn() => $_REQUEST)), "\n";
}
$a->run(function () { $_GET = ["again" => 1]; });
echo json_encode($a->run(fn() => $_GET)), " ", json_encode($_GET), "\n";
--EXPECT--
[{"g":1},{"r":1}] unset {"ra":1}
[{"g":1},{"r":1}] unset {"ra":1}
[{"g":1},{"r":1}] unset {"ra":1}
{"again":1} {"g":1}
