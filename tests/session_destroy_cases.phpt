--TEST--
Session: a scope's session is written and closed when the scope is destroyed while another scope is current, collected as a cycle (also one through its save handler), or still alive at request shutdown
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
    public ?object $owner = null;
    public function __construct(private string $name) {}
    public function open(string $path, string $name): bool { return true; }
    public function read(string $id): string|false { return ''; }
    public function write(string $id, string $data): bool
    {
        echo "$this->name write $id $data", $this->owner ? " owner=" . $this->owner->tag : "", "\n";
        return true;
    }
    public function close(): bool { echo "$this->name close\n"; return true; }
    public function destroy(string $id): bool { return true; }
    public function gc(int $max): int|false { return 0; }
}

function start(string $name, ?Handler $h = null): Scope
{
    $scope = new Scope();
    $scope->run(function () use ($name, $h) {
        session_set_save_handler($h ?? new Handler($name));
        session_id($name);
        session_start();
        $_SESSION['n'] = $name;
    });
    return $scope;
}

function state(): string
{
    return (session_status() === PHP_SESSION_ACTIVE ? "active " . session_id() : "none") . " " . json_encode($_SESSION ?? null);
}

echo "-- destroyed while another scope is current\n";
$a = start('a');
$b = start('b');
$b->run(function () use (&$a) {
    $a = null;
    echo "b after: ", state(), "\n";
});
echo "root: ", state(), "\n";

echo "-- collected as a cycle while another scope is current\n";
$o = new stdClass();
$o->self = $o;
$o->scope = start('c');
unset($o);
$b->run(function () {
    $n = gc_collect_cycles();
    echo "collected: ", $n > 0 ? "yes" : "no", "\n";
    echo "b after: ", state(), "\n";
});

echo "-- a cycle through the save handler\n";
$o = new stdClass();
$o->tag = 'alive';
$h = new Handler('d');
$h->owner = $o;
$o->scope = start('d', $h);
unset($h, $o);
$n = gc_collect_cycles();
echo "collected: ", $n > 0 ? "yes" : "no", "\n";
echo "root: ", state(), "\n";

echo "-- alive at request shutdown, one of them only through a cycle\n";
$o = new stdClass();
$o->tag = 'alive';
$h = new Handler('e');
$h->owner = $o;
$o->scope = start('e', $h);
unset($h, $o);
$f = start('f');
echo "end of script\n";
?>
--EXPECT--
-- destroyed while another scope is current
a write a n|s:1:"a";
a close
b after: active b {"n":"b"}
root: none null
-- collected as a cycle while another scope is current
c write c n|s:1:"c";
c close
collected: yes
b after: active b {"n":"b"}
-- a cycle through the save handler
d write d n|s:1:"d"; owner=alive
d close
collected: yes
root: none null
-- alive at request shutdown, one of them only through a cycle
end of script
f write f n|s:1:"f";
f close
b write b n|s:1:"b";
b close
e write e n|s:1:"e"; owner=alive
e close
