--TEST--
Copy on construction: the six arrays are copied when the Scope is constructed and are independent after; unset in the copy stays in the copy; objects are shared; nested copies
--EXTENSIONS--
scope
--FILE--
<?php
$o = new stdClass; $o->n = 1;
$_GET = ["g" => 1, "o" => $o]; $_POST = ["p" => 1]; $_COOKIE = ["c" => 1]; $_FILES = ["f" => 1]; $_REQUEST = ["r" => 1];
$_SERVER["marker"] = "root";
function names() { return implode(",", array_filter(["_GET", "_POST", "_COOKIE", "_FILES", "_REQUEST", "_SERVER"], fn($n) => isset($GLOBALS[$n]))); }
$a = new Scope();
$_GET["late"] = 1;                        // creator writes after construction: not in the copy
echo json_encode($a->run(fn() => [isset($_GET["late"]), $_GET["g"], $_POST, $_COOKIE, $_FILES, $_REQUEST, $_SERVER["marker"]])), "\n";

// writes in the copy do not reach the creator
$a->run(function () { $_GET["child"] = 1; $_POST["p"] = 2; $_SERVER["marker"] = "a"; });
echo json_encode([isset($_GET["child"]), $_POST["p"], $_SERVER["marker"]]), "\n";

// unset in the copy
$a->run(function () { unset($_GET); });
echo isset($_GET) ? "root has _GET\n" : "root lost _GET\n";
echo $a->run(fn() => isset($_GET)) ? "copy has _GET\n" : "copy has no _GET\n";
$a->run(function () { $_GET = ["again" => 1]; });
echo json_encode($a->run(fn() => $_GET)), "\n";

// a creator that has unset one: the copy has none either
unset($_COOKIE);
$u = new Scope();
$_COOKIE = ["back" => 1];
echo $u->run(fn() => isset($_COOKIE)) ? "set\n" : "unset in copy\n";

// objects are shared instances
$b = new Scope();
$b->run(function () use ($o) { $_GET["o"]->n = 2; var_dump($_GET["o"] === $o); });
echo $o->n, "\n";

// nested: a copy of a copy
$a->run(function () {
    $_POST["inA"] = 1;
    $c = new Scope();
    $_POST["afterC"] = 1;
    echo json_encode($c->run(fn() => $_POST)), "\n";
    $c->run(function () { $_POST["inC"] = 1; });
    echo json_encode($_POST), "\n";
});
echo json_encode($_POST), "\n";
--EXPECT--
[false,1,{"p":1},{"c":1},{"f":1},{"r":1},"root"]
[false,1,"root"]
root has _GET
copy has no _GET
{"again":1}
unset in copy
bool(true)
2
{"p":2,"inA":1}
{"p":2,"inA":1,"afterC":1}
{"p":1}
