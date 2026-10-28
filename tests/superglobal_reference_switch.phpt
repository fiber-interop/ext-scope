--TEST--
A superglobal the scope has made a reference ($s = &$_SERVER) or a non-array ($_GET = 1) leaves http_globals an array across a switch, so C code that reads it (ob_gzhandler's Accept-Encoding lookup) does not crash; an emptied array keeps its next key
--EXTENSIONS--
scope
zlib
--FILE--
<?php
$back = new Fiber(function () { while (true) Fiber::suspend(); });
$switch = fn() => $back->isStarted() ? $back->resume() : $back->start();   // root fiber and back

(new Scope)->run(function () use ($switch) {
    $_SERVER['HTTP_ACCEPT_ENCODING'] = 'gzip';
    $s = &$_SERVER;
    $switch();
    var_dump(strlen(@ob_gzhandler(str_repeat('x', 100), PHP_OUTPUT_HANDLER_START | PHP_OUTPUT_HANDLER_FINAL)) < 100);
    $s['marker'] = 1;
    $switch();
    var_dump($_SERVER['marker']);   // still the same array as $s

    $_SERVER = 1;
    $switch();
    @ob_gzhandler('x', PHP_OUTPUT_HANDLER_START | PHP_OUTPUT_HANDLER_FINAL);   // reads http_globals again
    var_dump($_SERVER);
});

// $_GET emptied after an append: the next append continues its keys, in this scope only
$a = new Scope;
$a->run(function () { $_GET = []; $_GET[] = 'x'; unset($_GET[0]); });
(new Scope)->run(function () use ($switch) { $_GET = []; $switch(); });
$a->run(function () use ($switch) { $switch(); $_GET[] = 'y'; var_dump(array_keys($_GET)); });
?>
--EXPECT--
bool(true)
int(1)
int(1)
array(1) {
  [0]=>
  int(1)
}
