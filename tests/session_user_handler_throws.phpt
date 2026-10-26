--TEST--
Session: an exception from a user save handler's write() at scope destruction propagates as from a destructor
--EXTENSIONS--
scope
session
--INI--
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
ob_start();
class Handler implements SessionHandlerInterface
{
    public function open(string $path, string $name): bool { return true; }
    public function read(string $id): string|false { return ''; }
    public function write(string $id, string $data): bool { throw new RuntimeException("write failed"); }
    public function close(): bool { echo "close\n"; return true; }
    public function destroy(string $id): bool { return true; }
    public function gc(int $max): int|false { return 0; }
}

$s = new Scope();
$s->run(function () {
    session_set_save_handler(new Handler());
    session_id('x');
    session_start();
    $_SESSION['n'] = 1;
});
try {
    unset($s);
    echo "no exception\n";
} catch (RuntimeException $e) {
    echo "caught: ", $e->getMessage(), "\n";
}
echo "root: ", session_status() === PHP_SESSION_NONE ? "none" : "active", "\n";
echo "done\n";
?>
--EXPECT--
caught: write failed
root: none
done
