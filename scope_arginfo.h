/* Hand-edited (getRootScope, bind and __run_without added, getCurrent non-nullable): gen_stub.php was not available.
 * The stub hash below is stale; regenerate from scope.stub.php with gen_stub.php. */
/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: f432f6fe82e64a3852a02737b718f36d89f983d9 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_Scope___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_Scope_run, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, args, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_Scope_bind, 0, 1, Closure, 0)
	ZEND_ARG_OBJ_INFO(0, closure, Closure, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_Scope_getCurrent, 0, 0, Scope, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_Scope_getRootScope arginfo_class_Scope_getCurrent

#define arginfo_class_Scope___run_without arginfo_class_Scope_run

ZEND_METHOD(Scope, __construct);
ZEND_METHOD(Scope, run);
ZEND_METHOD(Scope, bind);
ZEND_METHOD(Scope, getCurrent);
ZEND_METHOD(Scope, getRootScope);
ZEND_METHOD(Scope, __run_without);

static const zend_function_entry class_Scope_methods[] = {
	ZEND_ME(Scope, __construct, arginfo_class_Scope___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(Scope, run, arginfo_class_Scope_run, ZEND_ACC_PUBLIC)
	ZEND_ME(Scope, bind, arginfo_class_Scope_bind, ZEND_ACC_PUBLIC)
	ZEND_ME(Scope, getCurrent, arginfo_class_Scope_getCurrent, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(Scope, getRootScope, arginfo_class_Scope_getRootScope, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(Scope, __run_without, arginfo_class_Scope___run_without, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_Scope(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "Scope", class_Scope_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}
