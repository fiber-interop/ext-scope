<?php

/**
 * @generate-class-entries
 * @undocumentable
 */

/**
 * Identifies one logical operation, such as a request, a job or a transaction.
 *
 * With the ini setting scope.superglobals=1 (the default), $_GET, $_POST, $_COOKIE, $_FILES,
 * $_REQUEST and $_SERVER follow the scope: the root scope's arrays are the SAPI's, a new scope
 * starts with copy-on-write copies of the creating scope's arrays, and writes inside a scope stay
 * in it. $_ENV and $_SESSION are not in scope.
 *
 * With scope.locale_timezone=1 (the default) the default timezone (date_default_timezone_get()/
 * set()) and, when intl is loaded, the default locale (Locale::getDefault()/setDefault()) follow
 * the scope the same way: a new scope inherits its creator's values, a change stays in the scope.
 *
 * @strict-properties
 * @not-serializable
 */
final class Scope
{
    public function __construct() {}

    /** Calls $callback with this scope as the current scope, and returns its result. */
    public function run(callable $callback, mixed ...$args): mixed {}

    /**
     * A new closure that runs $closure with this scope current, as run() does, whoever calls it.
     * It has $closure's signature, $this and class scope; $closure itself is not modified.
     */
    public function bind(Closure $closure): Closure {}

    /** The current scope; the root scope outside any run(). */
    public static function getCurrent(): Scope {}

    /** The root scope: one object for the whole request, current in {main} outside any run(). */
    public static function getRootScope(): Scope {}
}
