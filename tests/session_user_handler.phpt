--TEST--
Session: a user save handler is written and closed when its scope is destroyed, collected in a cycle, or still alive at request end
--EXTENSIONS--
scope
session
--INI--
session.use_cookies=0
session.cache_limiter=
--FILE--
<?php
ob_start();   // in the CLI the first output counts as sent headers, and session_set_save_handler() refuses after that
class Handler implements SessionHandlerInterface
{
    public function __construct(private string $name) {}
    public function open(string $path, string $name): bool { return true; }
    public function read(string $id): string|false { return ''; }
    public function write(string $id, string $data): bool { echo "$this->name write $id $data\n"; return true; }
    public function close(): bool { echo "$this->name close\n"; return true; }
    public function destroy(string $id): bool { return true; }
    public function gc(int $max): int|false { return 0; }
}

function start(string $name): Scope
{
    $scope = new Scope();
    $scope->run(function () use ($name) {
        session_set_save_handler(new Handler($name));
        session_id($name);
        session_start();
        $_SESSION['n'] = $name;
    });
    return $scope;
}

$a = start('a');
echo "root: ", session_status() === PHP_SESSION_NONE ? "none" : "active", "\n";
unset($a);
echo "after unset\n";

$o = new stdClass();
$o->self = $o;
$o->scope = start('b');
unset($o);
echo "before gc\n";
gc_collect_cycles();
echo "after gc\n";

$c = start('c');
echo "end of script\n";
?>
--EXPECT--
root: none
a write a n|s:1:"a";
a close
after unset
before gc
b write b n|s:1:"b";
b close
after gc
end of script
c write c n|s:1:"c";
c close
