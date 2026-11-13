--TEST--
Session copy: with a user save handler the copy writes through the handler but never calls its open, read or close; a dropped copy calls nothing
--EXTENSIONS--
scope
session
--INI--
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
ob_start();
class H implements SessionHandlerInterface {
    public static array $log = [], $store = [];
    public function open($p, $n): bool { self::$log[] = "open"; return true; }
    public function close(): bool { self::$log[] = "close"; return true; }
    public function read($id): string|false { self::$log[] = "read"; return self::$store[$id] ?? ""; }
    public function write($id, $d): bool { self::$log[] = "write"; self::$store[$id] = $d; return true; }
    public function destroy($id): bool { self::$log[] = "destroy"; return true; }
    public function gc($t): int|false { return 0; }
}
session_set_save_handler(new H);
session_id("u1");
session_start();
$_SESSION["a"] = 1;
H::$log = [];
$c = new Scope();
$c->run(function () { $_SESSION["b"] = 2; session_write_close(); });
echo implode(",", H::$log), "\n";                 // write only: the opener's close is not the copy's
H::$log = [];
$d = new Scope();
unset($d);                                      // dropped while active: nothing called
echo implode(",", H::$log) ?: "nothing", "\n";
session_write_close();
echo implode(",", H::$log), "\n";
echo H::$store["u1"], "\n";
--EXPECT--
write
nothing
write,close
a|i:1;
