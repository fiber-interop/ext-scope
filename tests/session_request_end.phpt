--TEST--
Session: a scope still alive at request end has its session written before ext/session shuts down
--EXTENSIONS--
scope
session
--INI--
session.save_handler=files
session.use_cookies=0
session.save_path={TMP}
session.cache_limiter=
--FILE--
<?php
class Check
{
    public function __destruct()
    {
        $f = sys_get_temp_dir() . "/sess_scopeend" . getmypid();
        echo "file: ", file_exists($f) ? file_get_contents($f) : "(none)", "\n";
        @unlink($f);
    }
}
$check = new Check();   // global destructors run in reverse order: $c first, then $check
$c = new Scope();
$c->run(function () {
    session_id("scopeend" . getmypid());
    session_start();
    $_SESSION["c"] = 3;
});
echo "end of script\n";
?>
--EXPECT--
end of script
file: c|i:3;
