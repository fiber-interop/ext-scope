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
#include "SAPI.h"
#include "main/php_globals.h"
#include "zend_exceptions.h"
#include "zend_fibers.h"
#include "zend_generators.h"
#include "zend_observer.h"
#include "zend_weakrefs.h"
#include "ext/json/php_json.h"
#include "ext/date/php_date.h"
#include "scope_arginfo.h"

#define PHP_SCOPE_VERSION "0.3.0"

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

#define SCOPE_SG_N 5

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

/* Per-scope slots: state reached through scope_slot(), which returns the current
 * scope's copy, or the copy of an implicit root scope outside any scope. */
typedef struct {
	size_t size;
	void (*init)(void *state);
	void (*destroy)(void *state);
} scope_slot_type;

#define SCOPE_MAX_SLOTS 16
static scope_slot_type scope_slot_types[SCOPE_MAX_SLOTS];
static int scope_slots_n;

static int scope_slot_allocate(size_t size, void (*init)(void *), void (*destroy)(void *))
{
	if (scope_slots_n == SCOPE_MAX_SLOTS) {
		return -1;
	}
	scope_slot_types[scope_slots_n] = (scope_slot_type) { size, init, destroy };
	return scope_slots_n++;
}

static void scope_slots_free(void **slots)
{
	if (slots == NULL) {
		return;
	}
	for (int i = 0; i < scope_slots_n; i++) {
		if (slots[i]) {
			if (scope_slot_types[i].destroy) {
				scope_slot_types[i].destroy(slots[i]);
			}
			efree(slots[i]);
		}
	}
	efree(slots);
}

/* The default timezone and locale of a scope that is not the applied one (secondary vote B).
 * The timezone is ext/date's SCOPE_DATEG()->timezone (the string date_default_timezone_set() writes;
 * NULL: fall back to the date.timezone ini), owned by the holder. The locale is the value of the
 * ini entry intl.default_locale, which is what Locale::setDefault() writes. */
typedef struct {
	char *tz;
	zend_string *loc;
	bool ready;
} scope_tl;

/* A Scope object, with its swapped globals and its slots (both allocated on first use). */
typedef struct {
	char *state;
	void **slots;
	zval *sg;   /* the five request superglobals while this scope is not the applied one (NULL: not tracked) */
	scope_tl tl; /* the default timezone and locale while this scope is not the applied one */
	zend_object std;
} scope_object;

#define SCOPE_OBJECT(obj) ((scope_object *) ((char *) (obj) - XtOffsetOf(scope_object, std)))

ZEND_BEGIN_MODULE_GLOBALS(scope)
	bool isolate_json_errors;   /* INI scope.isolate_json_errors */
	zend_object *applied;   /* the scope whose state is in the globals; NULL: none */
	zend_object *cur_scope; /* the current scope, NULL outside any (kept by scope_apply()) */
	void **root_slots;      /* the implicit root scope's slots */
	bool slot_demo;         /* INI scope.slot_demo */
	bool superglobals;      /* INI scope.superglobals */
	bool locale_timezone;   /* INI scope.locale_timezone */
	scope_tl root_tl;       /* the root scope's timezone and locale while another scope is applied */
	zend_ini_entry *loc_ini; /* intl.default_locale; NULL when intl is not loaded */
	zval root_sg[SCOPE_SG_N]; /* the root scope's request superglobals while another scope is applied */
	char *root_state;       /* the globals' own state while a scope's is applied */
	bool active;            /* between RINIT and RSHUTDOWN */
	zend_object *root;      /* Scope::getRootScope(), created on first use; internally the root is NULL */
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

/* ---- the request superglobals (secondary vote A) ---------------------------
 *
 * $_GET, $_POST, $_COOKIE, $_FILES and $_REQUEST follow the scope. Compiled code
 * reads and writes them through their entries in EG(symbol_table); C code (the
 * filter extension, phpinfo) reads PG(http_globals)[TRACK_VARS_*], which holds a
 * second reference to the same array. A switch moves the array of the scope being
 * left out of the symbol table into that scope's storage, moves the incoming one
 * in, and re-points http_globals at it: no array is copied or duplicated. */

static const char *const scope_sg_names[SCOPE_SG_N] = { "_GET", "_POST", "_COOKIE", "_FILES", "_REQUEST" };
static const int scope_sg_track[SCOPE_SG_N] = { TRACK_VARS_GET, TRACK_VARS_POST, TRACK_VARS_COOKIE, TRACK_VARS_FILES, -1 };

/* The live entry of a superglobal. The auto-globals are created lazily (auto_globals_jit),
 * so the first time we need one, ask the engine to create it. */
static zval *scope_sg_live(int i)
{
	zval *z = zend_hash_str_find(&EG(symbol_table), scope_sg_names[i], strlen(scope_sg_names[i]));

	if (z == NULL) {
		zend_is_auto_global_str(scope_sg_names[i], strlen(scope_sg_names[i]));
		z = zend_hash_str_find(&EG(symbol_table), scope_sg_names[i], strlen(scope_sg_names[i]));
	}
	if (z != NULL && Z_TYPE_P(z) == IS_INDIRECT) {
		z = Z_INDIRECT_P(z);
	}
	return z;
}

/* A new scope's own arrays: references to the creating scope's (copy-on-write). */
static zval *scope_sg_snapshot(void)
{
	zval *sg = ecalloc(SCOPE_SG_N, sizeof(zval));

	for (int i = 0; i < SCOPE_SG_N; i++) {
		zval *live = scope_sg_live(i);
		if (live != NULL) {
			ZVAL_COPY_DEREF(&sg[i], live);
		} else {
			array_init(&sg[i]);
		}
	}
	return sg;
}

static void scope_sg_free(zval *sg)
{
	if (sg) {
		for (int i = 0; i < SCOPE_SG_N; i++) {
			zval_ptr_dtor(&sg[i]);
		}
		efree(sg);
	}
}

/* Swap the live arrays for the incoming scope's: out goes into from[], in comes from to[]. */
static void scope_sg_swap(zval *from, zval *to)
{
	for (int i = 0; i < SCOPE_SG_N; i++) {
		zval *live = scope_sg_live(i);

		if (live == NULL || Z_TYPE(to[i]) == IS_UNDEF) {
			continue;
		}
		ZVAL_COPY_VALUE(&from[i], live);
		ZVAL_COPY_VALUE(live, &to[i]);
		ZVAL_UNDEF(&to[i]);
		if (scope_sg_track[i] >= 0) {
			zval_ptr_dtor(&PG(http_globals)[scope_sg_track[i]]);
			ZVAL_COPY(&PG(http_globals)[scope_sg_track[i]], live);
		}
	}
}

/* ---- the default timezone and locale (secondary vote B) -------------------- */

/* ext/date does not export its globals to other extensions, but the struct layout is in
 * php_date.h and the module entry carries the globals' address. */
static zend_module_entry *scope_date_module;
#ifdef ZTS
# define SCOPE_DATEG() TSRMG_BULK_STATIC(*scope_date_module->globals_id_ptr, zend_date_globals *)
#else
# define SCOPE_DATEG() ((zend_date_globals *) scope_date_module->globals_ptr)
#endif

static void scope_tl_free(scope_tl *t)
{
	if (t->tz) {
		efree(t->tz);
	}
	if (t->loc) {
		zend_string_release(t->loc);
	}
	memset(t, 0, sizeof(*t));
}

/* Copy the live values into t, replacing what t held. */
static void scope_tl_save(scope_tl *t)
{
	if (t->tz != SCOPE_DATEG()->timezone) {
		if (t->tz) {
			efree(t->tz);
		}
		t->tz = SCOPE_DATEG()->timezone ? estrdup(SCOPE_DATEG()->timezone) : NULL;
	}
	if (SCOPE_G(loc_ini) && t->loc != SCOPE_G(loc_ini)->value) {
		zend_string *live = SCOPE_G(loc_ini)->value;
		if (t->loc) {
			zend_string_release(t->loc);
		}
		t->loc = live ? zend_string_copy(live) : NULL;
	}
	t->ready = true;
}

/* A new scope inherits the live values, which are its creator's. */
static void scope_tl_inherit(scope_tl *t)
{
	memset(t, 0, sizeof(*t));
	scope_tl_save(t);
}

/* Swap the live values with a scope's: the timezone is a pointer swap (the holder owns its
 * string, so date_default_timezone_set() inside a scope frees only that scope's); the locale
 * goes through the ini entry, as Locale::setDefault() does, and only when the two differ. */
static void scope_tl_swap(scope_tl *from, scope_tl *to)
{
	char *tz = SCOPE_DATEG()->timezone;

	if (!from->ready || !to->ready) {
		return;
	}
	SCOPE_DATEG()->timezone = to->tz;
	to->tz = NULL;
	if (from->tz) {
		efree(from->tz);
	}
	from->tz = tz;   /* the outgoing scope's string is now owned by its holder */
	if (SCOPE_G(loc_ini)) {
		zend_string *live = SCOPE_G(loc_ini)->value;
		zend_string *want = to->loc;
		if (from->loc != live) {
			if (from->loc) {
				zend_string_release(from->loc);
			}
			from->loc = live ? zend_string_copy(live) : NULL;
		}
		if (want && (live == NULL || !zend_string_equals(want, live))) {
			zend_alter_ini_entry(SCOPE_G(loc_ini)->name, want, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		}
	}
}

/* Make the globals hold the state of the scope that is current now. Called
 * wherever the current scope can change. */
static void scope_apply(void)
{
	zend_object *want = NULL;
	char *save, *load;
	zval *current;

	if (SCOPE_G(cur) == NULL) {
		return;
	}
	current = scope_current(SCOPE_G(cur));
	if (Z_TYPE_P(current) == IS_OBJECT) {
		want = Z_OBJ_P(current);
	}
	SCOPE_G(cur_scope) = want;
	if (want == SCOPE_G(applied)) {
		return;
	}
	if (SCOPE_G(superglobals)) {
		zval *from = SCOPE_G(applied) ? SCOPE_OBJECT(SCOPE_G(applied))->sg : SCOPE_G(root_sg);
		zval *to = want ? SCOPE_OBJECT(want)->sg : SCOPE_G(root_sg);

		if (from == NULL || to == NULL) {
			/* a scope that was created before the first switch has no storage only if
			 * the ini is off; nothing to swap */
		} else {
			scope_sg_swap(from, to);
		}
	}
	if (SCOPE_G(locale_timezone)) {
		scope_tl_swap(SCOPE_G(applied) ? &SCOPE_OBJECT(SCOPE_G(applied))->tl : &SCOPE_G(root_tl),
			want ? &SCOPE_OBJECT(want)->tl : &SCOPE_G(root_tl));
	}
	if (scope_states_n == 0) {
		SCOPE_G(applied) = want;
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

/* The current scope's copy of a slot; the root scope's outside any scope. */
static void *scope_slot(int slot)
{
	void ***slotsp = SCOPE_G(cur_scope) ? &SCOPE_OBJECT(SCOPE_G(cur_scope))->slots : &SCOPE_G(root_slots);

	if (*slotsp == NULL) {
		*slotsp = ecalloc(scope_slots_n, sizeof(void *));
	}
	if ((*slotsp)[slot] == NULL) {
		(*slotsp)[slot] = emalloc(scope_slot_types[slot].size);
		scope_slot_types[slot].init((*slotsp)[slot]);
	}
	return (*slotsp)[slot];
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
	if (SCOPE_G(superglobals) && SCOPE_G(active)) {
		so->sg = scope_sg_snapshot();
	}
	if (SCOPE_G(locale_timezone) && SCOPE_G(active)) {
		scope_tl_inherit(&so->tl);
	}
	return &so->std;
}

static void scope_free(zend_object *obj)
{
	scope_object *so = SCOPE_OBJECT(obj);
	if (SCOPE_G(root) == obj) {
		SCOPE_G(root) = NULL;   /* freed by the object store after RSHUTDOWN */
	}
	scope_slots_free(so->slots);
	scope_sg_free(so->sg);
	scope_tl_free(&so->tl);
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

/* The root scope object. Inside, the root is NULL in the arrays (and in
 * cur_scope), so root_slots and root_state stay its state; this object is only
 * its public face, the same one for the whole request. */
static zend_object *scope_root(void)
{
	if (SCOPE_G(root) == NULL) {
		zval z;
		object_init_ex(&z, scope_ce);
		/* the root's arrays are the root_sg storage, not this object's */
		scope_sg_free(SCOPE_OBJECT(Z_OBJ(z))->sg);
		SCOPE_OBJECT(Z_OBJ(z))->sg = NULL;
		scope_tl_free(&SCOPE_OBJECT(Z_OBJ(z))->tl);
		SCOPE_G(root) = Z_OBJ(z);
	}
	return SCOPE_G(root);
}

/* Return a scope as stored in an array: the object, or the root for NULL. */
static void scope_return(zval *scope, zval *return_value)
{
	if (scope && Z_TYPE_P(scope) == IS_OBJECT) {
		RETURN_COPY(scope);
	}
	RETURN_OBJ_COPY(scope_root());
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
	if (Z_OBJ_P(ZEND_THIS) == SCOPE_G(root)) {
		ZVAL_NULL(scope_current(a));
	} else {
		ZVAL_OBJ_COPY(scope_current(a), Z_OBJ_P(ZEND_THIS));
	}
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

	scope_return(SCOPE_G(cur) ? scope_current(SCOPE_G(cur)) : NULL, return_value);
}

ZEND_METHOD(Scope, getRootScope)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_OBJ_COPY(scope_root());
}

/* ---- Fiber::getScope() and Generator::getScope() --------------------------- */

static void scope_get_member_scope(INTERNAL_FUNCTION_PARAMETERS)
{
	zend_array *a;

	ZEND_PARSE_PARAMETERS_NONE();

	a = scope_member_array(Z_OBJ_P(ZEND_THIS));
	scope_return(a ? zend_hash_index_find(a, SCOPE_MEMBER) : NULL, return_value);
}

static ZEND_NAMED_FUNCTION(scope_fiber_get_scope)
{
	scope_get_member_scope(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

static ZEND_NAMED_FUNCTION(scope_generator_get_scope)
{
	scope_get_member_scope(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_scope_get_scope, 0, 0, Scope, 0)
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

/* ---- a demonstration slot user (INI scope.slot_demo): a per-scope counter ---- */

static int scope_demo_slot = -1;

static void scope_demo_init(void *state)
{
	*(zend_long *) state = 0;
}

/* scope_demo_counter(): int, the current scope's counter, incremented. */
static ZEND_NAMED_FUNCTION(scope_demo_counter)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG(++*(zend_long *) scope_slot(scope_demo_slot));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scope_demo_counter, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry scope_demo_functions[] = {
#if PHP_VERSION_ID >= 80400
	ZEND_RAW_FENTRY("scope_demo_counter", scope_demo_counter, arginfo_scope_demo_counter, 0, NULL, NULL)
#else
	ZEND_RAW_FENTRY("scope_demo_counter", scope_demo_counter, arginfo_scope_demo_counter, 0)
#endif
	ZEND_FE_END
};

/* ---- module -------------------------------------------------------------- */

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN("scope.isolate_json_errors", "0", PHP_INI_SYSTEM, OnUpdateBool,
		isolate_json_errors, zend_scope_globals, scope_globals)
	STD_PHP_INI_BOOLEAN("scope.superglobals", "1", PHP_INI_SYSTEM, OnUpdateBool,
		superglobals, zend_scope_globals, scope_globals)
	STD_PHP_INI_BOOLEAN("scope.locale_timezone", "1", PHP_INI_SYSTEM, OnUpdateBool,
		locale_timezone, zend_scope_globals, scope_globals)
	STD_PHP_INI_BOOLEAN("scope.slot_demo", "0", PHP_INI_SYSTEM, OnUpdateBool,
		slot_demo, zend_scope_globals, scope_globals)
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
	scope_date_module = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("date"));
	if (scope_date_module == NULL) {
		return FAILURE;
	}
	SCOPE_G(loc_ini) = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("intl.default_locale"));
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
	if (SCOPE_G(slot_demo)) {
		scope_demo_slot = scope_slot_allocate(sizeof(zend_long), scope_demo_init, NULL);
		zend_register_functions(NULL, scope_demo_functions, NULL, MODULE_PERSISTENT);
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
	memset(SCOPE_G(root_sg), 0, sizeof(SCOPE_G(root_sg)));
	memset(&SCOPE_G(root_tl), 0, sizeof(SCOPE_G(root_tl)));
	if (SCOPE_G(locale_timezone)) {
		scope_tl_save(&SCOPE_G(root_tl));
	}
	SCOPE_G(applied) = NULL;
	SCOPE_G(cur_scope) = NULL;
	SCOPE_G(root_slots) = NULL;
	SCOPE_G(root_state) = scope_states_size ? ecalloc(1, scope_states_size) : NULL;
	SCOPE_G(root) = NULL;
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
	scope_tl_free(&SCOPE_G(root_tl));
	scope_slots_free(SCOPE_G(root_slots));
	SCOPE_G(root_slots) = NULL;
	SCOPE_G(active) = false;
	SCOPE_G(cur) = NULL;
	zend_hash_destroy(&SCOPE_G(gen_prev));
	zend_hash_destroy(&SCOPE_G(ctx_owned));
	zend_hash_destroy(&SCOPE_G(ctx_cur));
	zval_ptr_dtor(&SCOPE_G(members));
	zval_ptr_dtor(&SCOPE_G(main));
	if (SCOPE_G(root)) {
		OBJ_RELEASE(SCOPE_G(root));   /* scope_free() clears SCOPE_G(root) if this was the last reference */
		SCOPE_G(root) = NULL;
	}
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

static const zend_module_dep scope_deps[] = {
	ZEND_MOD_REQUIRED("date")
	ZEND_MOD_OPTIONAL("intl")
	ZEND_MOD_END
};

zend_module_entry scope_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL, scope_deps,
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
