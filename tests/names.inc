<?php
// Short names for scopes in test output.
function named(string $name): Scope { $s = new Scope(); $GLOBALS['__scope_names'][spl_object_id($s)] = $name; return $s; }
function name(Scope $s): string { return $s === Scope::getRootScope() ? 'root' : ($GLOBALS['__scope_names'][spl_object_id($s)] ?? '?'); }
function current_name(): string { return name(Scope::getCurrent()); }
