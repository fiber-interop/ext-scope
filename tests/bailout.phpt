--TEST--
A fatal error inside nested Scope::run() calls restores the scope before shutdown, with a fiber left suspended inside run()
--EXTENSIONS--
scope
--INI--
display_errors=1
log_errors=0
--FILE--
<?php
register_shutdown_function(function () {
    echo "shutdown: current scope ", Scope::getCurrent() === Scope::getRootScope() ? 'root' : 'not root', "\n";
    gc_collect_cycles();
});
$a = new Scope(); $b = new Scope();
$f = $a->run(fn() => new Fiber(fn() => $b->run(fn() => Fiber::suspend())));
$f->start();
$a->run(fn() => $b->run(function () { eval('class Twice {} class Twice {}'); }));
?>
--EXPECTF--
Fatal error: Cannot %s class Twice%s
shutdown: current scope root
