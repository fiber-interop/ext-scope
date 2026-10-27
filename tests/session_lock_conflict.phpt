--TEST--
Session: with a locking save handler, session_start() throws a RuntimeException instead of blocking when the session is open in another scope of the process (also with read_and_close, between the root and a scope, after session_regenerate_id(), and for a user handler built on SessionHandler); handlers that do not lock are not affected
--EXTENSIONS--
scope
session
--INI--
session.save_handler=files
session.save_path={TMP}
session.use_cookies=0
session.use_strict_mode=0
session.cache_limiter=
--FILE--
<?php
ob_start();   // in the CLI the first output counts as sent headers
$p = "scopelock" . getmypid();
$try = function (Scope $s, string $id, array $opts = []) {
    return $s->run(function () use ($id, $opts) {
        session_id($id);
        try {
            session_start($opts);
            return "started " . json_encode($_SESSION);
        } catch (RuntimeException $e) {
            return "RuntimeException (" . (str_contains($e->getMessage(), "another scope") ? "msg ok" : $e->getMessage())
                . ", id not in it: " . var_export(!str_contains($e->getMessage(), $id), true)
                . ") status " . session_status() . " \$_SESSION " . json_encode($_SESSION ?? null);   // as when a read fails without scopes
        }
    });
};
$a = new Scope();
$b = new Scope();
$a->run(function () use ($p) { session_id("{$p}1"); session_start(); $_SESSION["a"] = 1; });

echo "b, same id: ", $try($b, "{$p}1"), "\n";
echo "b, read_and_close: ", $try($b, "{$p}1", ["read_and_close" => true]), "\n";
echo "b, other id: ", $try($b, "{$p}2"), "\n";
$b->run(fn() => session_write_close());

echo "root, a's id: ", $try(Scope::getRootScope(), "{$p}1"), "\n";
$a->run(fn() => session_write_close());
echo "b, after a closed: ", $try($b, "{$p}1"), "\n";
echo "a, while b holds it: ", $try($a, "{$p}1"), "\n";
$b->run(fn() => session_abort());
echo "a, after b aborted: ", $try($a, "{$p}1"), "\n";

$a->run(function () use ($p) { session_regenerate_id(); $GLOBALS["newid"] = session_id(); });
echo "b, a's old id after regenerate: ", $try($b, "{$p}1"), "\n";
$b->run(fn() => session_write_close());
echo "b, a's new id: ", str_replace($newid, "NEW", $try($b, $newid)), "\n";
$a->run(fn() => session_destroy());
echo "b, after a destroyed it: ", str_replace($newid, "NEW", $try($b, $newid)), "\n";
$b->run(fn() => session_write_close());

// a user handler that does not lock: both scopes may open the same id, as under php-fpm
class MemoryHandler implements SessionHandlerInterface {
    public static array $data = [];
    public function open(string $path, string $name): bool { return true; }
    public function close(): bool { return true; }
    public function read(string $id): string|false { return self::$data[$id] ?? ""; }
    public function write(string $id, string $data): bool { self::$data[$id] = $data; return true; }
    public function destroy(string $id): bool { unset(self::$data[$id]); return true; }
    public function gc(int $max): int|false { return 0; }
}
$c = new Scope();
$d = new Scope();
$c->run(fn() => session_set_save_handler(new MemoryHandler()));
$d->run(fn() => session_set_save_handler(new MemoryHandler()));
echo "c, user handler: ", $try($c, "{$p}u"), "\n";
echo "d, user handler, same id: ", $try($d, "{$p}u"), "\n";
$c->run(fn() => session_write_close());
$d->run(fn() => session_write_close());

// a user handler built on SessionHandler reads through the files handler, and locks
$e = new Scope();
$f = new Scope();
$e->run(fn() => session_set_save_handler(new class extends SessionHandler {}));
$f->run(fn() => session_set_save_handler(new class extends SessionHandler {}));
echo "e, SessionHandler: ", $try($e, "{$p}h"), "\n";
echo "f, SessionHandler, same id: ", $try($f, "{$p}h"), "\n";
$e->run(fn() => session_write_close());
ob_end_flush();
--CLEAN--
<?php
foreach (glob(sys_get_temp_dir() . "/sess_scopelock*") as $f) @unlink($f);
--EXPECT--
b, same id: RuntimeException (msg ok, id not in it: true) status 1 $_SESSION []
b, read_and_close: RuntimeException (msg ok, id not in it: true) status 1 $_SESSION []
b, other id: started []
root, a's id: RuntimeException (msg ok, id not in it: true) status 1 $_SESSION []
b, after a closed: started {"a":1}
a, while b holds it: RuntimeException (msg ok, id not in it: true) status 1 $_SESSION []
a, after b aborted: started {"a":1}
b, a's old id after regenerate: started {"a":1}
b, a's new id: RuntimeException (msg ok, id not in it: true) status 1 $_SESSION []
b, after a destroyed it: started []
c, user handler: started []
d, user handler, same id: started []
e, SessionHandler: started []
f, SessionHandler, same id: RuntimeException (msg ok, id not in it: true) status 1 $_SESSION []
