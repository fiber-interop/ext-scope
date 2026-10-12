/*
 * scope: the Scope class proposed in the "PHP RFC: Scope", for current PHP versions.
 *
 * Copyright (c) 2026 Frode Børli. MIT License, see LICENSE.
 *
 * Every execution context has a small PHP array describing it:
 *
 *   [0] the scope it belongs to (fibers and generators; null for {main})
 *   [1] its current scope
 *   [2...] the scopes saved by the Scope::run() calls active in it, innermost last
 *
 * Keeping the saved scopes in the context's array, not on the C stack, keeps them
 * with a fiber that suspends inside run(), and visible to the cycle collector.
 *
 * The arrays of fibers and generators live in a WeakMap keyed by the object, so an
 * entry goes away with its fiber or generator, and (since PHP 8.3) the cycle
 * collector sees through it. {main} has its own array. SCOPE_G(cur) points at the
 * array of the code running now. It changes when execution switches fibers (the
 * fiber switch observer) and when a generator's body is entered or left (the call
 * observer, registered for generator functions only).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "zend_fibers.h"
#include "zend_generators.h"
#include "zend_observer.h"
#include "zend_weakrefs.h"
#include "ext/json/php_json.h"
#include "scope_arginfo.h"

#define PHP_SCOPE_VERSION "0.1.0"

#define SCOPE_MEMBER  0
#define SCOPE_CURRENT 1
#define SCOPE_SAVED   2

/* ---- per-scope state: the internal API outlined in the RFC ----------------
 *
 * A piece of some module's globals that gets one copy per scope. The globals
 * always hold the copy of the scope that is current; when the current scope
 * changes, the engine saves the old scope's copy and loads the new one's. Outside
 * any scope the globals hold what they hold without this extension. */
typedef struct {
	void *live;                  /* the state in the module's globals */
	size_t size;
	void (*init)(void *state);   /* the state of a new scope */
	void (*destroy)(void *state);
} scope_state;

#define SCOPE_MAX_STATES 16
static scope_state scope_states[SCOPE_MAX_STATES];
static size_t scope_state_offset[SCOPE_MAX_STATES];
static size_t scope_states_n, scope_states_size;

static int scope_register_state(const scope_state *state)
{
	if (scope_states_n == SCOPE_MAX_STATES) {
		return FAILURE;
	}
	scope_state_offset[scope_states_n] = scope_states_size;
	scope_states[scope_states_n++] = *state;
	scope_states_size += ZEND_MM_ALIGNED_SIZE(state->size);
	return SUCCESS;
}

/* A Scope object, with its copy of the registered state (allocated on first use). */
typedef struct {
	char *state;
	zend_object std;
} scope_object;

#define SCOPE_OBJECT(obj) ((scope_object *) ((char *) (obj) - XtOffsetOf(scope_object, std)))

ZEND_BEGIN_MODULE_GLOBALS(scope)
	bool isolate_json_errors;   /* INI scope.isolate_json_errors */
	zend_object *applied;   /* the scope whose state is in the globals; NULL: none */
	char *root_state;       /* the globals' own state while a scope's is applied */
	bool active;            /* between RINIT and RSHUTDOWN */
	zval main;              /* {main}'s array */
	zend_array *cur;        /* the array of the code running now */
	zval members;           /* WeakMap: Fiber|Generator => its array */
	HashTable ctx_cur;      /* (uintptr_t)zend_fiber_context => zend_array* of the code it was running */
	HashTable ctx_owned;    /* arrays of fiber contexts that aren't Fiber objects */
	HashTable gen_prev;     /* (uintptr_t)zend_generator => zend_array* to return to on yield */
ZEND_END_MODULE_GLOBALS(scope)

ZEND_DECLARE_MODULE_GLOBALS(scope)

#ifdef ZTS
# define SCOPE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(scope, v)
#else
# define SCOPE_G(v) (scope_globals.v)
#endif

static zend_class_entry *scope_ce;
static zend_class_entry *scope_weakmap_ce;   /* WeakMap's entry isn't exported */
static zend_object_handlers scope_handlers;
static zend_object *(*scope_fiber_create_orig)(zend_class_entry *ce);
static zend_object *(*scope_generator_create_orig)(zend_class_entry *ce);

#define SCOPE_KEY(p) ((zend_ulong) (uintptr_t) (p))

/* ---- the per-context arrays ---------------------------------------------- */

static zend_array *scope_array_new(zval *member)
{
	zend_array *a = zend_new_array(4);
	zval z;

	ZVAL_COPY(&z, member);
	zend_hash_index_add_new(a, SCOPE_MEMBER, &z);
	ZVAL_COPY(&z, member);
	zend_hash_index_add_new(a, SCOPE_CURRENT, &z);
	return a;
}

static zend_always_inline zval *scope_current(zend_array *a)
{
	return zend_hash_index_find(a, SCOPE_CURRENT);
}

/* The array of a Fiber or Generator object, or NULL. */
static zend_array *scope_member_array(zend_object *obj)
{
	zval key, rv, *v;

	if (!SCOPE_G(active)) {
		return NULL;
	}
	ZVAL_OBJ(&key, obj);
	v = Z_OBJ_HT(SCOPE_G(members))->read_dimension(Z_OBJ(SCOPE_G(members)), &key, BP_VAR_IS, &rv);
	return v && Z_TYPE_P(v) == IS_ARRAY ? Z_ARR_P(v) : NULL;
}

/* A Fiber or Generator belongs to the scope that is current when it is created. */
static void scope_register_member(zend_object *obj)
{
	zval key, arr;

	if (!SCOPE_G(active) || SCOPE_G(cur) == NULL) {
		return;
	}
	ZVAL_ARR(&arr, scope_array_new(scope_current(SCOPE_G(cur))));
	ZVAL_OBJ(&key, obj);
	Z_OBJ_HT(SCOPE_G(members))->write_dimension(Z_OBJ(SCOPE_G(members)), &key, &arr);
	zval_ptr_dtor(&arr);   /* the WeakMap holds it now */
}

static zend_object *scope_fiber_create(zend_class_entry *ce)
{
	zend_object *obj = scope_fiber_create_orig(ce);
	scope_register_member(obj);
	return obj;
}

static zend_object *scope_generator_create(zend_class_entry *ce)
{
	zend_object *obj = scope_generator_create_orig(ce);
	scope_register_member(obj);
	return obj;
}

/* Make the globals hold the state of the scope that is current now. Called
 * wherever the current scope can change. */
static void scope_apply(void)
{
	zend_object *want = NULL;
	char *save, *load;
	zval *current;

	if (scope_states_n == 0 || SCOPE_G(cur) == NULL) {
		return;
	}
	current = scope_current(SCOPE_G(cur));
	if (Z_TYPE_P(current) == IS_OBJECT) {
		want = Z_OBJ_P(current);
	}
	if (want == SCOPE_G(applied)) {
		return;
	}
	save = SCOPE_G(applied) ? SCOPE_OBJECT(SCOPE_G(applied))->state : SCOPE_G(root_state);
	for (size_t i = 0; i < scope_states_n; i++) {
		memcpy(save + scope_state_offset[i], scope_states[i].live, scope_states[i].size);
	}
	if (want) {
		scope_object *so = SCOPE_OBJECT(want);
		if (so->state == NULL) {
			so->state = emalloc(scope_states_size);
			for (size_t i = 0; i < scope_states_n; i++) {
				scope_states[i].init(so->state + scope_state_offset[i]);
			}
		}
		load = so->state;
	} else {
		load = SCOPE_G(root_state);
	}
	for (size_t i = 0; i < scope_states_n; i++) {
		memcpy(scope_states[i].live, load + scope_state_offset[i], scope_states[i].size);
	}
	SCOPE_G(applied) = want;
}

/* ---- fibers: switch the running array with the running context ------------ */

static void scope_fiber_init(zend_fiber_context *ctx)
{
	zend_array *a = NULL;

	if (!SCOPE_G(active)) {
		return;
	}
	if (ctx->kind == zend_ce_fiber) {
		a = scope_member_array(&((zend_fiber *) ((char *) ctx - XtOffsetOf(zend_fiber, context)))->std);
	}
	if (a == NULL) {
		/* Another kind of fiber context: it belongs to the scope that starts it. */
		zval arr;
		ZVAL_ARR(&arr, scope_array_new(scope_current(SCOPE_G(cur))));
		zend_hash_index_update(&SCOPE_G(ctx_owned), SCOPE_KEY(ctx), &arr);
		a = Z_ARR(arr);
	}
	zend_hash_index_update_ptr(&SCOPE_G(ctx_cur), SCOPE_KEY(ctx), a);
}

static void scope_fiber_switch(zend_fiber_context *from, zend_fiber_context *to)
{
	zend_array *a;

	if (!SCOPE_G(active)) {
		return;
	}
	zend_hash_index_update_ptr(&SCOPE_G(ctx_cur), SCOPE_KEY(from), SCOPE_G(cur));
	a = zend_hash_index_find_ptr(&SCOPE_G(ctx_cur), SCOPE_KEY(to));
	SCOPE_G(cur) = a ? a : Z_ARR(SCOPE_G(main));
	scope_apply();
}

static void scope_fiber_destroy(zend_fiber_context *ctx)
{
	if (!SCOPE_G(active)) {
		return;
	}
	zend_hash_index_del(&SCOPE_G(ctx_cur), SCOPE_KEY(ctx));
	zend_hash_index_del(&SCOPE_G(ctx_owned), SCOPE_KEY(ctx));
}

/* ---- generators: enter the generator's array while its body runs ---------- */

/* For a generator's frame, EX(return_value) holds the generator itself. */
static void scope_generator_begin(zend_execute_data *execute_data)
{
	zend_generator *g = (zend_generator *) EX(return_value);
	zend_array *a;

	if (g == NULL || (a = scope_member_array(&g->std)) == NULL) {
		return;
	}
	zend_hash_index_update_ptr(&SCOPE_G(gen_prev), SCOPE_KEY(g), SCOPE_G(cur));
	SCOPE_G(cur) = a;
	scope_apply();
}

static void scope_generator_end(zend_execute_data *execute_data, zval *retval)
{
	zend_generator *g = (zend_generator *) EX(return_value);
	zend_array *prev;

	if (!SCOPE_G(active) || g == NULL) {
		return;
	}
	if ((prev = zend_hash_index_find_ptr(&SCOPE_G(gen_prev), SCOPE_KEY(g))) != NULL) {
		SCOPE_G(cur) = prev;
		zend_hash_index_del(&SCOPE_G(gen_prev), SCOPE_KEY(g));
		scope_apply();
	}
}

static zend_observer_fcall_handlers scope_observer_init(zend_execute_data *execute_data)
{
	if (EX(func)->common.fn_flags & ZEND_ACC_GENERATOR) {
		return (zend_observer_fcall_handlers) { scope_generator_begin, scope_generator_end };
	}
	return (zend_observer_fcall_handlers) { NULL, NULL };
}

/* ---- Scope ---------------------------------------------------------------- */

static zend_object *scope_create(zend_class_entry *ce)
{
	scope_object *so = zend_object_alloc(sizeof(scope_object), ce);
	zend_object_std_init(&so->std, ce);
	object_properties_init(&so->std, ce);
	so->std.handlers = &scope_handlers;
	return &so->std;
}

static void scope_free(zend_object *obj)
{
	scope_object *so = SCOPE_OBJECT(obj);
	if (so->state) {
		for (size_t i = 0; i < scope_states_n; i++) {
			if (scope_states[i].destroy) {
				scope_states[i].destroy(so->state + scope_state_offset[i]);
			}
		}
		efree(so->state);
	}
	zend_object_std_dtor(obj);
}

ZEND_METHOD(Scope, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();
}

ZEND_METHOD(Scope, run)
{
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	zend_array *a = SCOPE_G(cur);
	zval *current, saved;
	bool bailout = false;

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_FUNC(fci, fcc)
		Z_PARAM_VARIADIC_WITH_NAMED(fci.params, fci.param_count, fci.named_params)
	ZEND_PARSE_PARAMETERS_END();

	if (a == NULL) {
		zend_throw_error(NULL, "Scope::run() is not available during shutdown");
		RETURN_THROWS();
	}

	/* Push the current scope and make this one current. The array is the running
	 * code's own, so it is the same one again when the callback returns, wherever
	 * the callback suspended in between. */
	current = scope_current(a);
	ZVAL_COPY_VALUE(&saved, current);
	zend_hash_next_index_insert_new(a, &saved);
	ZVAL_OBJ_COPY(scope_current(a), Z_OBJ_P(ZEND_THIS));
	scope_apply();

	fci.retval = return_value;
	zend_try {
		zend_call_function(&fci, &fcc);
	} zend_catch {
		bailout = true;
	} zend_end_try();

	/* Pop: the saved scope becomes current again. */
	{
		zend_ulong top = a->nNextFreeElement - 1;
		zval *slot = zend_hash_index_find(a, top);
		current = scope_current(a);
		zval_ptr_dtor(current);
		ZVAL_COPY_VALUE(current, slot);
		ZVAL_UNDEF(slot);
		zend_hash_index_del(a, top);
		a->nNextFreeElement = top;
	}
	scope_apply();

	if (bailout) {
		zend_bailout();
	}
}

ZEND_METHOD(Scope, getCurrent)
{
	ZEND_PARSE_PARAMETERS_NONE();

	if (SCOPE_G(cur) == NULL) {
		RETURN_NULL();
	}
	RETURN_COPY(scope_current(SCOPE_G(cur)));
}

/* ---- Fiber::getScope() and Generator::getScope() --------------------------- */

static void scope_get_member_scope(INTERNAL_FUNCTION_PARAMETERS)
{
	zend_array *a;

	ZEND_PARSE_PARAMETERS_NONE();

	if ((a = scope_member_array(Z_OBJ_P(ZEND_THIS))) == NULL) {
		RETURN_NULL();
	}
	RETURN_COPY(zend_hash_index_find(a, SCOPE_MEMBER));
}

static ZEND_NAMED_FUNCTION(scope_fiber_get_scope)
{
	scope_get_member_scope(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static ZEND_NAMED_FUNCTION(scope_generator_get_scope)
{
	scope_get_member_scope(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_scope_get_scope, 0, 0, Scope, 1)
ZEND_END_ARG_INFO()

#if PHP_VERSION_ID >= 80400
# define SCOPE_FENTRY(name, handler) ZEND_RAW_FENTRY(name, handler, arginfo_scope_get_scope, ZEND_ACC_PUBLIC, NULL, NULL)
#else
# define SCOPE_FENTRY(name, handler) ZEND_RAW_FENTRY(name, handler, arginfo_scope_get_scope, ZEND_ACC_PUBLIC)
#endif

static const zend_function_entry scope_fiber_methods[] = {
	SCOPE_FENTRY("getScope", scope_fiber_get_scope)
	ZEND_FE_END
};

static const zend_function_entry scope_generator_methods[] = {
	SCOPE_FENTRY("getScope", scope_generator_get_scope)
	ZEND_FE_END
};

/* ---- the first user: json_last_error() per scope (INI scope.isolate_json_errors) */

static void scope_json_init(void *state)
{
	*(php_json_error_code *) state = PHP_JSON_ERROR_NONE;
}

static void scope_register_json(void)
{
	zend_module_entry *json = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("json"));
	scope_state state = { NULL, sizeof(php_json_error_code), scope_json_init, NULL };

	if (json == NULL || json->globals_size != sizeof(zend_json_globals)) {
		return;
	}
#ifdef ZTS
	return;   /* the prototype keeps to NTS builds */
#else
	state.live = &((zend_json_globals *) json->globals_ptr)->error_code;
	scope_register_state(&state);
#endif
}

/* ---- module -------------------------------------------------------------- */

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN("scope.isolate_json_errors", "0", PHP_INI_SYSTEM, OnUpdateBool,
		isolate_json_errors, zend_scope_globals, scope_globals)
PHP_INI_END()

static PHP_GINIT_FUNCTION(scope)
{
#if defined(COMPILE_DL_SCOPE) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(scope_globals, 0, sizeof(*scope_globals));
}

static PHP_MINIT_FUNCTION(scope)
{
	REGISTER_INI_ENTRIES();
	scope_ce = register_class_Scope();
	memcpy(&scope_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	scope_handlers.offset = XtOffsetOf(scope_object, std);
	scope_handlers.free_obj = scope_free;
	scope_handlers.clone_obj = NULL;   /* a scope is an identity: a copy would be another scope */
	scope_ce->create_object = scope_create;
	scope_weakmap_ce = zend_hash_str_find_ptr(CG(class_table), ZEND_STRL("weakmap"));
	if (scope_weakmap_ce == NULL) {
		return FAILURE;
	}

	zend_register_functions(zend_ce_fiber, scope_fiber_methods, &zend_ce_fiber->function_table, MODULE_PERSISTENT);
	zend_register_functions(zend_ce_generator, scope_generator_methods, &zend_ce_generator->function_table, MODULE_PERSISTENT);

	scope_fiber_create_orig = zend_ce_fiber->create_object;
	zend_ce_fiber->create_object = scope_fiber_create;
	scope_generator_create_orig = zend_ce_generator->create_object;
	zend_ce_generator->create_object = scope_generator_create;

	zend_observer_fiber_init_register(scope_fiber_init);
	zend_observer_fiber_switch_register(scope_fiber_switch);
	zend_observer_fiber_destroy_register(scope_fiber_destroy);
	zend_observer_fcall_register(scope_observer_init);
	if (SCOPE_G(isolate_json_errors)) {
		scope_register_json();
	}
	return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(scope)
{
	zend_ce_fiber->create_object = scope_fiber_create_orig;
	zend_ce_generator->create_object = scope_generator_create_orig;
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

static PHP_RINIT_FUNCTION(scope)
{
#if defined(COMPILE_DL_SCOPE) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	zval null;

	ZVAL_NULL(&null);
	ZVAL_ARR(&SCOPE_G(main), scope_array_new(&null));
	SCOPE_G(cur) = Z_ARR(SCOPE_G(main));
	object_init_ex(&SCOPE_G(members), scope_weakmap_ce);
	zend_hash_init(&SCOPE_G(ctx_cur), 16, NULL, NULL, 0);
	zend_hash_init(&SCOPE_G(ctx_owned), 8, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&SCOPE_G(gen_prev), 16, NULL, NULL, 0);
	SCOPE_G(applied) = NULL;
	SCOPE_G(root_state) = scope_states_size ? ecalloc(1, scope_states_size) : NULL;
	SCOPE_G(active) = true;
	return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(scope)
{
	/* Fibers and generators can outlive this point (objects are freed later in
	 * shutdown); from here on they simply have no scope. */
	SCOPE_G(cur) = Z_ARR(SCOPE_G(main));   /* {main}, outside any scope: the globals' own state */
	zval_ptr_dtor(scope_current(SCOPE_G(cur)));
	ZVAL_NULL(scope_current(SCOPE_G(cur)));
	scope_apply();
	if (SCOPE_G(root_state)) {
		efree(SCOPE_G(root_state));
		SCOPE_G(root_state) = NULL;
	}
	SCOPE_G(active) = false;
	SCOPE_G(cur) = NULL;
	zend_hash_destroy(&SCOPE_G(gen_prev));
	zend_hash_destroy(&SCOPE_G(ctx_owned));
	zend_hash_destroy(&SCOPE_G(ctx_cur));
	zval_ptr_dtor(&SCOPE_G(members));
	zval_ptr_dtor(&SCOPE_G(main));
	return SUCCESS;
}

static PHP_MINFO_FUNCTION(scope)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "Scope support", "enabled");
	php_info_print_table_row(2, "Version", PHP_SCOPE_VERSION);
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

zend_module_entry scope_module_entry = {
	STANDARD_MODULE_HEADER,
	"scope",
	NULL,
	PHP_MINIT(scope),
	PHP_MSHUTDOWN(scope),
	PHP_RINIT(scope),
	PHP_RSHUTDOWN(scope),
	PHP_MINFO(scope),
	PHP_SCOPE_VERSION,
	PHP_MODULE_GLOBALS(scope),
	PHP_GINIT(scope),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_SCOPE
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(scope)
#endif
