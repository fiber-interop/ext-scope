--TEST--
Session: session_name(), cookie parameters and the cache limiter are per scope; a new scope starts with the ini values
--EXTENSIONS--
scope
session
--INI--
session.name=INI
session.cache_limiter=nocache
session.cookie_path=/
--FILE--
<?php
$a = new Scope();
$a->run(function () {
    session_name("A"); session_cache_limiter("private"); session_set_cookie_params(["path" => "/a", "lifetime" => 60]);
});
session_name("ROOT");
$b = new Scope();
$show = fn() => session_name() . " " . session_cache_limiter() . " " . session_get_cookie_params()["path"] . " " . session_get_cookie_params()["lifetime"] . " " . ini_get("session.name");
echo $show(), "\n", $a->run($show), "\n", $b->run($show), "\n";
$f = $a->run(fn() => new Fiber(function () use ($show) { Fiber::suspend($show()); echo $show(), "\n"; }));
echo $f->start(), "\n"; echo $show(), "\n"; $f->resume();
--EXPECT--
ROOT nocache / 0 ROOT
A private /a 60 A
INI nocache / 0 INI
A private /a 60 A
ROOT nocache / 0 ROOT
A private /a 60 A
