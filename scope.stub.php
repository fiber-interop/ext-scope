<?php

/**
 * @generate-class-entries
 * @undocumentable
 */

/**
 * Identifies one logical operation, such as a request, a job or a transaction.
 *
 * @strict-properties
 * @not-serializable
 */
final class Scope
{
    public function __construct() {}

    /** Calls $callback with this scope as the current scope, and returns its result. */
    public function run(callable $callback, mixed ...$args): mixed {}

    /** The current scope, or null outside any scope. */
    public static function getCurrent(): ?Scope {}
}
