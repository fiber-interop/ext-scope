--TEST--
A superglobal deleted from the end of the symbol table and another variable added in its place (the table's layout is as before) is not taken for the superglobal at the next switch
--EXTENSIONS--
scope
--FILE--
<?php
// No variables in the main script: $_REQUEST, created with the first Scope, is the table's last entry.
(function () {
    $back = new Fiber(function () { while (true) Fiber::suspend(); });
    $switch = function () use ($back) { $back->isStarted() ? $back->resume() : $back->start(); };
    $scope = new Scope;
    $scope->run(function () use ($switch) { $switch(); });   // a switch: the buckets are cached
    $keys = array_keys($GLOBALS);
    var_dump(end($keys));
    unset($GLOBALS['_REQUEST']);
    $GLOBALS['added'] = ['mine'];   // in the freed last bucket: the table's layout is as before
    $scope->run(function () use ($switch) {
        $switch();
        var_dump($GLOBALS['added'], $_REQUEST);   // the root's variable, untouched; the scope's own $_REQUEST
    });
    var_dump($GLOBALS['added'], isset($_REQUEST));
})();
?>
--EXPECT--
string(8) "_REQUEST"
array(1) {
  [0]=>
  string(4) "mine"
}
array(0) {
}
array(1) {
  [0]=>
  string(4) "mine"
}
bool(false)
