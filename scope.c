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
 * fiber switch observer) and when a generator is resumed from outside or yields
 * back (switches compiled into generator bodies only, see "generators" below;
 * there is no fcall observer, so ordinary calls pay nothing).
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "SAPI.h"
#include "main/php_globals.h"
#include "zend_closures.h"
#include "zend_exceptions.h"
#include "zend_extensions.h"
#include "zend_fibers.h"
#include "zend_generators.h"
#include "zend_observer.h"
#include "zend_weakrefs.h"
#include "ext/json/php_json.h"
#include "ext/date/php_date.h"
#include "ext/session/php_session.h"
#include "scope_arginfo.h"

#define PHP_SCOPE_VERSION "0.4.2"

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

#define SCOPE_SG_N 6

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

/* ext/session's per-request state of a scope that is not the applied one (secondary vote A):
 * what ext/session's RINIT/RSHUTDOWN reset. Its settings (the session.* ini entries) are not
 * part of it: they stay request-wide. */
typedef struct {
	zend_string *id;
	php_session_status status;
	const ps_module *mod;
	void *mod_data;
	__typeof__(((php_ps_globals *) 0)->mod_user_names) user_names;
	bool user_implemented;
	bool user_is_open;
	zend_string *user_class_name;
#if PHP_VERSION_ID >= 80600
	bool user_obj_methods;
#endif
	zend_string *vars;
	zval http_vars;
	zval sym;   /* its $_SESSION, as the symbol table held it; UNDEF: no $_SESSION */
	bool send_cookie;
	bool define_sid;
	bool in_save_handler;
	bool set_handler;
#if PHP_VERSION_ID >= 80300
	zend_string *started_file;
	uint32_t started_line;
#endif
} scope_sess;

/* A Scope object, with its swapped globals and its slots (both allocated on first use). */
typedef struct {
	char *state;
	void **slots;
	zval *sg;   /* the six request superglobals while this scope is not the applied one (NULL: not tracked) */
	scope_tl tl; /* the default timezone and locale while this scope is not the applied one */
	scope_sess *sess; /* its session while this scope is not the applied one (NULL: not tracked) */
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
	bool session;           /* INI scope.session */
	scope_sess root_sess;   /* the root scope's session while another scope is applied */
	HashTable bound;        /* the data of the closures Scope::bind() returned */
	scope_tl root_tl;       /* the root scope's timezone and locale while another scope is applied */
	zend_ini_entry *loc_ini; /* intl.default_locale; NULL when intl is not loaded */
	zend_string *loc_initial; /* the locale a new scope starts with: the ini value, or ICU's default at RINIT if unset */
	zval root_sg[SCOPE_SG_N]; /* the root scope's request superglobals while another scope is applied */
	char *root_state;       /* the globals' own state while a scope's is applied */
	bool active;            /* between RINIT and RSHUTDOWN */
	zend_object *root;      /* Scope::getRootScope(), created on first use; internally the root is NULL */
	zval main;              /* {main}'s array */
	zend_array *cur;        /* the array of the code running now */
	zval members;           /* WeakMap: Fiber|Generator => its array */
	HashTable ctx_cur;      /* (uintptr_t)zend_fiber_context => zend_array* of the code it was running */
	HashTable ctx_owned;    /* arrays of fiber contexts that aren't Fiber objects */
	zend_array *resumer;    /* the array of the code resuming a generator now; NULL: none */
	HashTable ctx_resumer;  /* (uintptr_t)zend_fiber_context => its resumer while it is not running */
	zend_object *gen_last;  /* the generator that switched in last, and its array (a lookup cache) */
	zend_array *gen_last_a;
	Bucket *sym_cache[7];   /* symbol-table buckets of the six superglobals and $_SESSION (see scope_symbol()) */
	zend_string *sym_cache_key[7];   /* the key each of those buckets had when found */
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
static zend_object_handlers scope_generator_handlers;   /* the engine's, with dtor_obj wrapped */
static const zend_object_handlers *scope_generator_handlers_orig;
static void scope_generator_dtor(zend_object *obj);
static void scope_generator_free(zend_object *obj);

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

/* The array of a Fiber or Generator object, or NULL while no Scope exists (all of them
 * are the root's then). The WeakMap is created with the first Scope, so a request that
 * never uses one allocates no object of the extension's and sees the same object ids as
 * without it. A fiber or generator without an entry was created before that: it is the
 * root's, and gets its array now. */
static zend_array *scope_member_array(zend_object *obj)
{
	zval key, rv, *v, null, arr;

	if (!SCOPE_G(active) || Z_ISUNDEF(SCOPE_G(members))) {
		return NULL;
	}
	ZVAL_OBJ(&key, obj);
	v = Z_OBJ_HT(SCOPE_G(members))->read_dimension(Z_OBJ(SCOPE_G(members)), &key, BP_VAR_IS, &rv);
	if (v && Z_TYPE_P(v) == IS_ARRAY) {
		return Z_ARR_P(v);
	}
	ZVAL_NULL(&null);
	ZVAL_ARR(&arr, scope_array_new(&null));
	Z_OBJ_HT(SCOPE_G(members))->write_dimension(Z_OBJ(SCOPE_G(members)), &key, &arr);
	zval_ptr_dtor(&arr);   /* the WeakMap holds it now */
	return Z_ARR(arr);
}

/* A Fiber or Generator belongs to the scope that is current when it is created. */
static void scope_register_member(zend_object *obj)
{
	zval key, arr;

	if (!SCOPE_G(active) || SCOPE_G(cur) == NULL || Z_ISUNDEF(SCOPE_G(members))) {
		return;   /* no Scope yet: a member of the root, recorded when first looked up */
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

	if (scope_generator_handlers_orig == NULL) {
		scope_generator_handlers_orig = obj->handlers;
		scope_generator_handlers = *obj->handlers;
		scope_generator_handlers.dtor_obj = scope_generator_dtor;
		scope_generator_handlers.free_obj = scope_generator_free;
	}
	if (obj->handlers == scope_generator_handlers_orig) {
		obj->handlers = &scope_generator_handlers;
	}
	scope_register_member(obj);
	return obj;
}

/* ---- the request superglobals (secondary vote A) ---------------------------
 *
 * $_GET, $_POST, $_COOKIE, $_FILES, $_REQUEST and $_SERVER follow the scope. Compiled
 * code reads and writes them through their entries in EG(symbol_table); C code (the
 * filter extension, phpinfo) reads PG(http_globals)[TRACK_VARS_*], which holds a
 * second reference to the same array. A switch moves the array of the scope being
 * left out of the symbol table into that scope's storage, moves the incoming one
 * in, and re-points http_globals at it: no array is copied or duplicated.
 * $_REQUEST has no http_globals slot (it is built by the SAPI, not tracked there),
 * so its scope_sg_track entry is -1. */

static const char *const scope_sg_names[SCOPE_SG_N] = { "_GET", "_POST", "_COOKIE", "_FILES", "_REQUEST", "_SERVER" };
static const int scope_sg_track[SCOPE_SG_N] = { TRACK_VARS_GET, TRACK_VARS_POST, TRACK_VARS_COOKIE, TRACK_VARS_FILES, -1, TRACK_VARS_SERVER };

/* The six superglobals' names and $_SESSION's, interned at MINIT so a lookup needs no hashing. */
#define SCOPE_SYM_SESSION SCOPE_SG_N
static zend_string *scope_sym_key[SCOPE_SG_N + 1];

/* A variable's entry in the global symbol table. The bucket found last time is reused while
 * it still holds that variable (inside the table, same key, not deleted); a resize
 * or compaction that moved it falls back to a lookup with the precomputed hash. NULL: absent. */
static zend_always_inline zval *scope_symbol(int i)
{
	HashTable *ht = &EG(symbol_table);
	Bucket *b = SCOPE_G(sym_cache)[i];
	zval *z;

	/* The bucket's key is compared with the key it had when found: by pointer, because the
	 * table's key may be another string than ours (opcache moves interned strings to shared
	 * memory after MINIT, and ext/session adds _SESSION under a key of its own), and by hash,
	 * because a key of its own may be freed and its memory reused for another name. */
	if (b && b >= ht->arData && b < ht->arData + ht->nNumUsed && b->key == SCOPE_G(sym_cache_key)[i]
		&& b->h == ZSTR_H(scope_sym_key[i]) && Z_TYPE(b->val) != IS_UNDEF) {
		z = &b->val;
	} else {
		z = zend_hash_find_known_hash(ht, scope_sym_key[i]);
		b = (Bucket *) z;   /* a bucket starts with its zval */
		SCOPE_G(sym_cache)[i] = b;
		SCOPE_G(sym_cache_key)[i] = b ? b->key : NULL;
	}
	return z;
}

/* The live entry of a superglobal. The auto-globals are created lazily (auto_globals_jit),
 * so the first time we need one, ask the engine to create it. */
static zend_always_inline zval *scope_sg_live(int i)
{
	zval *z = scope_symbol(i);

	if (UNEXPECTED(z == NULL)) {
		zend_is_auto_global(scope_sym_key[i]);
		z = scope_symbol(i);
	}
	if (z != NULL && Z_TYPE_P(z) == IS_INDIRECT) {
		z = Z_INDIRECT_P(z);
	}
	return z;
}

/* A new scope's own arrays: empty, as a server's request starts before it seeds them. The
 * live entries are looked up anyway, so the first scope creation forces the lazily
 * created auto-globals (a later JIT callback would otherwise overwrite a scope's array). */
static zval *scope_sg_new(void)
{
	zval *sg = ecalloc(SCOPE_SG_N, sizeof(zval));

	for (int i = 0; i < SCOPE_SG_N; i++) {
		scope_sg_live(i);
		array_init(&sg[i]);
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
		zval *live = scope_sg_live(i);   /* a cached bucket, checked by its key: a few compares */

		if (live == NULL || Z_TYPE(to[i]) == IS_UNDEF) {
			continue;
		}
		ZVAL_COPY_VALUE(&from[i], live);
		ZVAL_COPY_VALUE(live, &to[i]);
		ZVAL_UNDEF(&to[i]);
		if (scope_sg_track[i] >= 0) {
			/* C code (ext/session's cookie lookup, browscap, zlib, ...) reads PG(http_globals) */
			zval *pg = &PG(http_globals)[scope_sg_track[i]];
			zval old = *pg;

			ZVAL_COPY(pg, live);
			i_zval_ptr_dtor(&old);
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

/* A new scope starts with the ini values: no timezone of its own (the date.timezone ini,
 * or its fallback) and the ini value of intl.default_locale, not its creator's. */
static void scope_tl_init(scope_tl *t)
{
	memset(t, 0, sizeof(*t));
	if (SCOPE_G(loc_initial)) {
		t->loc = zend_string_copy(SCOPE_G(loc_initial));
	}
	t->ready = true;
}

/* RINIT: the locale a new scope starts with. intl.default_locale unset means ICU's process
 * default, which Locale::setDefault() also changes; take its value before any code runs. */
static void scope_tl_startup(void)
{
	zend_ini_entry *e = SCOPE_G(loc_ini);

	SCOPE_G(loc_initial) = NULL;
	if (e == NULL) {
		return;
	}
	if (e->value && ZSTR_LEN(e->value)) {
		SCOPE_G(loc_initial) = zend_string_copy(e->value);
	} else {
		zval fn, rv;
		ZVAL_STRINGL(&fn, "locale_get_default", sizeof("locale_get_default") - 1);
		if (call_user_function(NULL, NULL, &fn, &rv, 0, NULL) == SUCCESS && Z_TYPE(rv) == IS_STRING) {
			SCOPE_G(loc_initial) = zend_string_copy(Z_STR(rv));
		}
		zval_ptr_dtor(&rv);
		zval_ptr_dtor(&fn);
	}
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
		/* Compared by effective locale: an unset ini means ICU's default, which is what a new
		 * scope starts with (SCOPE_G(loc_initial)), so the two need no ini change between them. */
		zend_string *want_eff = want ? want : SCOPE_G(loc_initial);
		zend_string *live_eff = live ? live : SCOPE_G(loc_initial);
		if (want_eff == live_eff || (want_eff && live_eff && zend_string_equals(want_eff, live_eff))) {
			/* the same locale: nothing to apply */
		} else if (want) {
			zend_alter_ini_entry(SCOPE_G(loc_ini)->name, want, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
		} else {
			zend_restore_ini_entry(SCOPE_G(loc_ini)->name, PHP_INI_STAGE_RUNTIME);   /* back to the ini value (unset) */
		}
	}
}

/* ---- the session (secondary vote A) ----------------------------------------
 *
 * ext/session's per-request globals (the PS() fields its RINIT/RSHUTDOWN reset) are moved
 * between the live globals and the scope's storage at every switch, the way the phasync
 * extension's sapi() swaps them per request. ext/session is optional: scope_ps is NULL
 * without it (or on ZTS builds), and nothing is swapped. The session is isolated, its
 * settings are not: the session.* ini entries, and session_name(), session_set_cookie_params()
 * and the others that write them, stay request-wide, as ini_set() does. */

static php_ps_globals *scope_ps;

#define SCOPE_SESS_ON() (SCOPE_G(session) && scope_ps != NULL)

#if PHP_VERSION_ID >= 80600
# define SCOPE_SESS_OBJMETHODS(X) X(user_obj_methods, mod_user_uses_object_methods_as_handlers)
#else
# define SCOPE_SESS_OBJMETHODS(X)
#endif
#if PHP_VERSION_ID >= 80300
# define SCOPE_SESS_STARTED(X) X(started_file, session_started_filename) X(started_line, session_started_lineno)
#else
# define SCOPE_SESS_STARTED(X)
#endif
#define SCOPE_SESS_FIELDS(X) \
	X(id, id) X(status, session_status) X(mod, mod) X(mod_data, mod_data) \
	X(user_names, mod_user_names) X(user_implemented, mod_user_implemented) \
	X(user_is_open, mod_user_is_open) X(user_class_name, mod_user_class_name) SCOPE_SESS_OBJMETHODS(X) \
	X(vars, session_vars) X(http_vars, http_session_vars) X(send_cookie, send_cookie) \
	X(define_sid, define_sid) X(in_save_handler, in_save_handler) X(set_handler, set_handler) \
	SCOPE_SESS_STARTED(X)

/* A new scope's session state: none started, the save handler of the session.save_handler ini. */
static scope_sess *scope_sess_new(void)
{
	scope_sess *ss = ecalloc(1, sizeof(scope_sess));
	zend_ini_entry *h = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("session.save_handler"));
	zend_string *hv = h ? h->value : NULL;   /* the request's current setting: ini_set() of it is request-wide */

	ss->mod = hv ? _php_find_ps_module(ZSTR_VAL(hv)) : NULL;
	ss->status = ss->mod ? php_session_none : php_session_disabled;
	ss->define_sid = true;
	ZVAL_UNDEF(&ss->http_vars);
	ZVAL_UNDEF(&ss->sym);   /* a new request has no $_SESSION until session_start() */
	return ss;
}

/* Release what a stored (not applied) session state owns; its session is already closed. */
static void scope_sess_free(scope_sess *ss)
{
	zval *names = (zval *) &ss->user_names;

	if (ss->id) zend_string_release(ss->id);
	if (ss->vars) zend_string_release(ss->vars);
	if (ss->user_class_name) zend_string_release(ss->user_class_name);
#if PHP_VERSION_ID >= 80300
	if (ss->started_file) zend_string_release(ss->started_file);
#endif
	zval_ptr_dtor(&ss->http_vars);
	zval_ptr_dtor(&ss->sym);
	for (size_t i = 0; i < sizeof(ss->user_names) / sizeof(zval); i++) {
		zval_ptr_dtor(&names[i]);
	}
	memset(ss, 0, sizeof(*ss));
}

/* Move the live session state into from and to's into the live globals (to is left empty). */
static void scope_sess_swap(scope_sess *from, scope_sess *to)
{
	zval *sym, in = to->sym;
	bool indirect;

#define SCOPE_SESS_MOVE(f, g) from->f = scope_ps->g; scope_ps->g = to->f;
	SCOPE_SESS_FIELDS(SCOPE_SESS_MOVE)
#undef SCOPE_SESS_MOVE

	memset(to, 0, sizeof(*to));
	ZVAL_UNDEF(&to->http_vars);

	/* $_SESSION is the scope's own, as the other superglobals are: what the symbol table holds
	 * (session_start() makes it a reference to http_session_vars, and it keeps its array after
	 * session_write_close(), session_abort() or session_destroy(), as in a request) is moved
	 * out, and the incoming scope's put in, or removed if it has none. */
	sym = scope_symbol(SCOPE_SYM_SESSION);
	indirect = sym && Z_TYPE_P(sym) == IS_INDIRECT;   /* a CV slot of {main}: emptied, never removed */
	if (indirect) {
		sym = Z_INDIRECT_P(sym);
	}
	if (sym && !Z_ISUNDEF_P(sym)) {
		ZVAL_COPY_VALUE(&from->sym, sym);
		ZVAL_UNDEF(sym);
	} else {
		ZVAL_UNDEF(&from->sym);
	}
	if (!Z_ISUNDEF(in)) {
		if (sym) {
			ZVAL_COPY_VALUE(sym, &in);
		} else {
			zend_hash_add_new(&EG(symbol_table), scope_sym_key[SCOPE_SYM_SESSION], &in);
		}
	} else if (sym && !indirect) {
		zend_hash_del(&EG(symbol_table), scope_sym_key[SCOPE_SYM_SESSION]);   /* its value is UNDEF: nothing freed */
	}
}

/* The end of a scope's session, as ext/session's RSHUTDOWN ends a request's: write and close
 * an active session, then free the state. The scope must not be the applied one. */
static void scope_sess_end(scope_sess *ss)
{
	scope_sess live;

	if (ss->status == php_session_active || ss->mod_data || ss->user_implemented) {
		scope_sess_swap(&live, ss);
		if (scope_ps->session_status == php_session_active) {
			zend_try {
				php_session_flush(1);
			} zend_end_try();
		}
		zval_ptr_dtor(&scope_ps->http_session_vars);
		ZVAL_UNDEF(&scope_ps->http_session_vars);
		if (scope_ps->mod && (scope_ps->mod_data || scope_ps->mod_user_implemented)) {
			zend_try {
				scope_ps->mod->s_close(&scope_ps->mod_data);
			} zend_end_try();
		}
		scope_ps->mod_data = NULL;
		scope_ps->session_status = php_session_none;
		scope_sess_swap(ss, &live);
	}
	scope_sess_free(ss);
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
	if (SCOPE_G(superglobals) && SCOPE_SESS_ON()) {
		scope_sess *from = SCOPE_G(applied) ? SCOPE_OBJECT(SCOPE_G(applied))->sess : &SCOPE_G(root_sess);
		scope_sess *to = want ? SCOPE_OBJECT(want)->sess : &SCOPE_G(root_sess);

		if (from && to) {
			scope_sess_swap(from, to);
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
	if (SCOPE_G(resumer)) {
		zend_hash_index_update_ptr(&SCOPE_G(ctx_resumer), SCOPE_KEY(from), SCOPE_G(resumer));
	} else if (zend_hash_num_elements(&SCOPE_G(ctx_resumer))) {
		zend_hash_index_del(&SCOPE_G(ctx_resumer), SCOPE_KEY(from));
	}
	SCOPE_G(resumer) = zend_hash_num_elements(&SCOPE_G(ctx_resumer))
		? zend_hash_index_find_ptr(&SCOPE_G(ctx_resumer), SCOPE_KEY(to)) : NULL;
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
	zend_hash_index_del(&SCOPE_G(ctx_resumer), SCOPE_KEY(ctx));
}

/* ---- generators: switches compiled into generator bodies -------------------
 *
 * A generator's body runs in its own array. ZEND_ACC_GENERATOR is known at
 * compile time, so the op_array handler (registered as a zend_extension at MINIT)
 * rewrites generator bodies only, and every other function compiles exactly as
 * without this extension:
 *
 *   - at the start of the body (after GENERATOR_CREATE), after each YIELD and
 *     YIELD_FROM, at the start of each catch block (after its CATCH) and of each
 *     finally block (which a destructor or throw() can jump to directly):
 *     switch in, to the generator's array;
 *   - before each YIELD, YIELD_FROM and return (GENERATOR_RETURN): switch out,
 *     back to the resumer's array.
 *
 * The resumer is whoever resumed the generator from outside: a Generator method,
 * the iterator foreach uses, or the destructor. Each of those is wrapped: it
 * records the array of the code calling it as the resumer (SCOPE_G(resumer),
 * saved per fiber context like SCOPE_G(cur)) and, when the original returns or
 * throws, restores that array; an exception escaping the body passes no switch.
 *
 * yield from: each generator of a delegation chain switches itself in and out,
 * so each body runs in its own scope; between them (and in an iterator that is
 * not a generator, delegated to) the resumer's scope is current. */

static zend_always_inline zend_generator *scope_generator_of(zend_execute_data *ex)
{
	/* A generator's frame holds the generator in return_value. */
	return ex && ex->func && ZEND_USER_CODE(ex->func->type) && (ex->func->op_array.fn_flags & ZEND_ACC_GENERATOR)
		? (zend_generator *) ex->return_value : NULL;
}

static void scope_generator_in(zend_execute_data *ex)
{
	zend_generator *g = scope_generator_of(ex);
	zend_array *a;

	if (g == NULL) {
		return;
	}
	if (&g->std == SCOPE_G(gen_last)) {
		a = SCOPE_G(gen_last_a);
	} else if ((a = scope_member_array(&g->std)) != NULL) {
		SCOPE_G(gen_last) = &g->std;
		SCOPE_G(gen_last_a) = a;
	}
	if (a == NULL || a == SCOPE_G(cur)) {
		return;
	}
	SCOPE_G(cur) = a;
	scope_apply();
}

static void scope_generator_out(zend_execute_data *ex)
{
	zend_array *r = SCOPE_G(resumer);

	if (r == NULL || r == SCOPE_G(cur) || !SCOPE_G(active) || scope_generator_of(ex) == NULL) {
		return;
	}
	SCOPE_G(cur) = r;
	scope_apply();
}

/* The two functions the switches call: frameless on PHP 8.4+ (no frame, and the
 * JIT calls the handler directly), with a frame before. */
ZEND_FUNCTION(scope_generator_switch_in)
{
	scope_generator_in(execute_data->prev_execute_data);
}

ZEND_FUNCTION(scope_generator_switch_out)
{
	scope_generator_out(execute_data->prev_execute_data);
}

#if PHP_VERSION_ID >= 80400
ZEND_FRAMELESS_FUNCTION(scope_generator_switch_in, 0)
{
	scope_generator_in(EG(current_execute_data));
}

ZEND_FRAMELESS_FUNCTION(scope_generator_switch_out, 0)
{
	scope_generator_out(EG(current_execute_data));
}

static const zend_frameless_function_info scope_switch_in_flf[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(scope_generator_switch_in, 0), 0 },
	{ 0 },
};
static const zend_frameless_function_info scope_switch_out_flf[] = {
	{ ZEND_FRAMELESS_FUNCTION_NAME(scope_generator_switch_out, 0), 0 },
	{ 0 },
};
static uint32_t scope_switch_flf[2];
#endif

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_scope_generator_switch, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

/* Internal: they act only when called from a generator's body. */
#define SCOPE_SWITCH_IN_NAME "scope\\internal\\generator_switch_in"
#define SCOPE_SWITCH_OUT_NAME "scope\\internal\\generator_switch_out"
static const zend_function_entry scope_switch_functions[] = {
	{ .fname = SCOPE_SWITCH_IN_NAME, .handler = ZEND_FN(scope_generator_switch_in),
	  .arg_info = arginfo_scope_generator_switch, .num_args = 0, .flags = 0,
#if PHP_VERSION_ID >= 80400
	  .frameless_function_infos = scope_switch_in_flf,
#endif
	},
	{ .fname = SCOPE_SWITCH_OUT_NAME, .handler = ZEND_FN(scope_generator_switch_out),
	  .arg_info = arginfo_scope_generator_switch, .num_args = 0, .flags = 0,
#if PHP_VERSION_ID >= 80400
	  .frameless_function_infos = scope_switch_out_flf,
#endif
	},
	ZEND_FE_END
};
static zend_string *scope_switch_name[2];
static zend_function *scope_switch_func[2];

/* The rewrite runs on the raw op_array at the start of pass_two(), before jump
 * targets become offsets (and before RETURN becomes GENERATOR_RETURN), like
 * ext-phasync's checkpoint injection: every opline number the op_array and the
 * compiler context refer to is moved past the calls inserted before it. */
static void scope_raw_map_targets(zend_op_array *op_array, uint32_t (*map)(uint32_t, void *),
	uint32_t (*map_finally_end)(uint32_t, void *), void *arg)
{
	for (uint32_t i = 0; i < op_array->last; i++) {
		zend_op *op = &op_array->opcodes[i];

		switch (op->opcode) {
			case ZEND_JMP:
				op->op1.opline_num = map(op->op1.opline_num, arg);
				break;
			case ZEND_CATCH:
				if (op->extended_value & ZEND_LAST_CATCH) {
					break;
				}
				ZEND_FALLTHROUGH;
			case ZEND_JMPZ:
			case ZEND_JMPNZ:
			case ZEND_JMPZ_EX:
			case ZEND_JMPNZ_EX:
			case ZEND_JMP_SET:
			case ZEND_COALESCE:
			case ZEND_FE_RESET_R:
			case ZEND_FE_RESET_RW:
			case ZEND_JMP_NULL:
			case ZEND_ASSERT_CHECK:
#ifdef ZEND_BIND_INIT_STATIC_OR_JMP   /* PHP 8.3+ */
			case ZEND_BIND_INIT_STATIC_OR_JMP:
#endif
#if PHP_VERSION_ID >= 80400
			case ZEND_JMP_FRAMELESS:
#endif
				op->op2.opline_num = map(op->op2.opline_num, arg);
				break;
			case ZEND_FE_FETCH_R:
			case ZEND_FE_FETCH_RW:
				op->extended_value = map(op->extended_value, arg);
				break;
			case ZEND_SWITCH_LONG:
			case ZEND_SWITCH_STRING:
			case ZEND_MATCH: {
				zval *zv;

				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(CT_CONSTANT_EX(op_array, op->op2.constant)), zv) {
					Z_LVAL_P(zv) = map((uint32_t) Z_LVAL_P(zv), arg);
				} ZEND_HASH_FOREACH_END();
				op->extended_value = map(op->extended_value, arg);
				break;
			}
		}
	}
	for (int i = 0; i < op_array->last_try_catch; i++) {
		zend_try_catch_element *tc = &op_array->try_catch_array[i];

		tc->try_op = map(tc->try_op, arg);
		tc->catch_op = map(tc->catch_op, arg);
		tc->finally_op = map(tc->finally_op, arg);
		tc->finally_end = map_finally_end(tc->finally_end, arg);
	}
	for (int i = 0; i < CG(context).last_brk_cont; i++) {
		zend_brk_cont_element *bc = &CG(context).brk_cont_array[i];

		if (bc->start >= 0) {
			bc->start = (int) map((uint32_t) bc->start, arg);
		}
		if (bc->cont >= 0) {
			bc->cont = (int) map((uint32_t) bc->cont, arg);
		}
		if (bc->brk >= 0) {
			bc->brk = (int) map((uint32_t) bc->brk, arg);
		}
	}
	if (CG(context).labels) {
		zend_label *label;

		ZEND_HASH_FOREACH_PTR(CG(context).labels, label) {
			label->opline_num = map(label->opline_num, arg);
		} ZEND_HASH_FOREACH_END();
	}
}

typedef struct {
	const uint32_t *at;   /* sorted insertion points, as (opline << 1 | out) */
	uint32_t n;
} scope_shift;

#define SCOPE_SWITCH_OPS 2   /* FRAMELESS_ICALL_0 + FREE, or INIT_FCALL + DO_ICALL */

/* A target moves past the switches inserted before it; one at an insertion point
 * lands on that point's first switch. (0 stays 0: nothing is inserted before the
 * first opline, so an unused try_catch offset keeps its meaning.) */
static uint32_t scope_shift_target(uint32_t t, void *arg)
{
	const scope_shift *s = arg;
	uint32_t lo = 0, hi = s->n;

	while (lo < hi) {   /* how many insertion points are < t */
		uint32_t mid = (lo + hi) / 2;

		if ((s->at[mid] >> 1) < t) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return t + lo * SCOPE_SWITCH_OPS;
}

/* finally_end names the block's FAST_RET itself: the engine reads the finally's fast_call
 * variable from opcodes[finally_end].op1 when an exception or return enters the block. It
 * moves past the switches inserted at it (one is, after a yield that ends the block), not
 * onto them. */
static uint32_t scope_shift_past(uint32_t t, void *arg)
{
	const scope_shift *s = arg;
	uint32_t lo = 0, hi = s->n;

	while (lo < hi) {   /* how many insertion points are <= t */
		uint32_t mid = (lo + hi) / 2;

		if ((s->at[mid] >> 1) <= t) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return t + lo * SCOPE_SWITCH_OPS;
}

static int scope_cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
	return x < y ? -1 : x > y;
}

static uint32_t scope_add_literal(zend_op_array *op_array, zend_string *str)
{
	uint32_t lit = op_array->last_literal;

	if (lit >= CG(context).literals_size) {
		CG(context).literals_size += 16;
		op_array->literals = erealloc(op_array->literals, CG(context).literals_size * sizeof(zval));
	}
	ZVAL_INTERNED_STR(&op_array->literals[lit], str);
	Z_EXTRA(op_array->literals[lit]) = 0;
	op_array->last_literal++;
	return lit;
}

static void scope_op_array_handler(zend_op_array *op_array)
{
	uint32_t *at, n = 0, last = op_array->last, max;
	zend_op *opcodes;
	scope_shift shift;

	if (!(op_array->fn_flags & ZEND_ACC_GENERATOR)) {
		return;
	}
	max = 2 * last + op_array->last_try_catch;
	at = emalloc(max * sizeof(uint32_t));
#define SCOPE_AT(pos, out) (at[n++] = (uint32_t) (pos) << 1 | (out))
	for (uint32_t i = 0; i < last; i++) {
		switch (op_array->opcodes[i].opcode) {
			case ZEND_GENERATOR_CREATE:
			case ZEND_CATCH:
				SCOPE_AT(i + 1, 0);
				break;
			case ZEND_YIELD:
			case ZEND_YIELD_FROM:
				SCOPE_AT(i, 1);
				SCOPE_AT(i + 1, 0);
				break;
			case ZEND_RETURN:
			case ZEND_RETURN_BY_REF:
			case ZEND_GENERATOR_RETURN:
				SCOPE_AT(i, 1);
				break;
		}
	}
	for (int i = 0; i < op_array->last_try_catch; i++) {
		if (op_array->try_catch_array[i].finally_op) {
			SCOPE_AT(op_array->try_catch_array[i].finally_op, 0);
		}
	}
#undef SCOPE_AT
	/* By opline; at one opline, switching in before switching out. */
	qsort(at, n, sizeof(uint32_t), scope_cmp_u32);

	shift = (scope_shift) { at, n };
	scope_raw_map_targets(op_array, scope_shift_target, scope_shift_past, &shift);

	opcodes = emalloc(sizeof(zend_op) * (last + n * SCOPE_SWITCH_OPS));
	for (uint32_t i = 0, j = 0, o = 0; i <= last; i++) {
		while (j < n && (at[j] >> 1) == i) {
			const zend_op *next = &op_array->opcodes[i < last ? i : last - 1];
			uint32_t out = at[j] & 1;
			zend_op *sw = &opcodes[o];

			memset(sw, 0, sizeof(zend_op) * SCOPE_SWITCH_OPS);
			for (uint32_t c = 0; c < SCOPE_SWITCH_OPS; c++) {
				SET_UNUSED(sw[c].op1);
				SET_UNUSED(sw[c].op2);
				SET_UNUSED(sw[c].result);
				sw[c].lineno = next->lineno;
			}
#if PHP_VERSION_ID >= 80400
			sw[0].opcode = ZEND_FRAMELESS_ICALL_0;
			sw[0].extended_value = scope_switch_flf[out];
			sw[0].result_type = IS_TMP_VAR;
			sw[0].result.var = op_array->T;
			sw[1].opcode = ZEND_FREE;
			sw[1].op1_type = IS_TMP_VAR;
			sw[1].op1.var = op_array->T++;
#else
			sw[0].opcode = ZEND_INIT_FCALL;
			sw[0].op1.num = zend_vm_calc_used_stack(0, scope_switch_func[out]);
			sw[0].op2_type = IS_CONST;
			sw[0].op2.constant = scope_add_literal(op_array, scope_switch_name[out]);
			sw[0].result.num = op_array->cache_size;
			op_array->cache_size += sizeof(void *);
			sw[1].opcode = ZEND_DO_ICALL;
#endif
			o += SCOPE_SWITCH_OPS;
			j++;
		}
		if (i < last) {
			opcodes[o++] = op_array->opcodes[i];
		}
	}
	efree(op_array->opcodes);
	op_array->opcodes = opcodes;
	op_array->last = last + n * SCOPE_SWITCH_OPS;
	CG(context).opcodes_size = op_array->last;
	efree(at);
}

static int scope_bound_handle = -1;   /* reserved[] slot of the functions Scope::bind() creates */

static zend_extension scope_zend_extension = {
	.name = "scope",
	.version = PHP_SCOPE_VERSION,
	.author = "fiber-interop",
	.op_array_handler = scope_op_array_handler,
	.resource_number = -1,
};

/* eval() compiles without ZEND_COMPILE_HANDLE_OP_ARRAY (ZEND_COMPILE_DEFAULT_FOR_EVAL
 * is 0), so op_array handlers don't see eval'd code: turn it on while eval'd code
 * compiles. */
static zend_op_array *(*scope_compile_string_orig)(zend_string *source, const char *filename, zend_compile_position position);

static zend_op_array *scope_compile_string(zend_string *source, const char *filename, zend_compile_position position)
{
	uint32_t options = CG(compiler_options);
	zend_op_array *op_array;

	CG(compiler_options) |= ZEND_COMPILE_HANDLE_OP_ARRAY;
	op_array = scope_compile_string_orig(source, filename, position);
	CG(compiler_options) = options;
	return op_array;
}

/* MINIT: the switch functions and the compile hook. A regular extension (and
 * ext-phasync, which bundles this one and calls its MINIT) registers the
 * zend_extension itself; zend_register_extension() also sets the flag that
 * makes pass_two() call op_array handlers. */
static void scope_generator_switches_startup(void)
{
	zend_register_functions(NULL, scope_switch_functions, NULL, MODULE_PERSISTENT);
	scope_switch_name[0] = zend_string_init_interned(ZEND_STRL(SCOPE_SWITCH_IN_NAME), 1);
	scope_switch_name[1] = zend_string_init_interned(ZEND_STRL(SCOPE_SWITCH_OUT_NAME), 1);
	scope_switch_func[0] = zend_hash_find_ptr(CG(function_table), scope_switch_name[0]);
	scope_switch_func[1] = zend_hash_find_ptr(CG(function_table), scope_switch_name[1]);
#if PHP_VERSION_ID >= 80400
	for (uint32_t i = 0; zend_flf_handlers[i]; i++) {
		if (zend_flf_handlers[i] == (void *) ZEND_FRAMELESS_FUNCTION_NAME(scope_generator_switch_in, 0)) {
			scope_switch_flf[0] = i;
		} else if (zend_flf_handlers[i] == (void *) ZEND_FRAMELESS_FUNCTION_NAME(scope_generator_switch_out, 0)) {
			scope_switch_flf[1] = i;
		}
	}
#endif
	zend_register_extension(&scope_zend_extension, NULL);
	scope_bound_handle = zend_get_resource_handle(scope_zend_extension.name);
	scope_compile_string_orig = zend_compile_string;
	zend_compile_string = scope_compile_string;
}

/* The resume entry points: record the caller as the resumer, and restore its
 * array when the original returns or throws. */
typedef struct {
	zend_array *cur, *resumer;
} scope_resume;

static zend_always_inline scope_resume scope_resume_begin(void)
{
	scope_resume r = { SCOPE_G(cur), SCOPE_G(resumer) };

	SCOPE_G(resumer) = r.cur;
	return r;
}

static zend_always_inline void scope_resume_end(scope_resume r)
{
	SCOPE_G(resumer) = r.resumer;
	if (SCOPE_G(cur) != r.cur && SCOPE_G(active)) {
		SCOPE_G(cur) = r.cur;
		scope_apply();
	}
}

/* Generator methods */
#define SCOPE_GENERATOR_METHODS(X) \
	X(rewind) X(valid) X(current) X(key) X(next) X(send) X(throw) X(getReturn)

#define SCOPE_GM_ORIG(name) static zif_handler scope_gm_orig_##name;
SCOPE_GENERATOR_METHODS(SCOPE_GM_ORIG)

#define SCOPE_GM_WRAP(name) \
	static ZEND_NAMED_FUNCTION(scope_gm_##name) \
	{ \
		scope_resume r = scope_resume_begin(); \
		scope_gm_orig_##name(INTERNAL_FUNCTION_PARAM_PASSTHRU); \
		scope_resume_end(r); \
	}
SCOPE_GENERATOR_METHODS(SCOPE_GM_WRAP)

static void scope_generator_methods_swap(bool install)
{
	zend_function *f;

#define SCOPE_GM_SWAP(name) \
	if ((f = zend_hash_str_find_ptr_lc(&zend_ce_generator->function_table, #name, sizeof(#name) - 1)) != NULL) { \
		if (install) { \
			scope_gm_orig_##name = f->internal_function.handler; \
			f->internal_function.handler = scope_gm_##name; \
		} else if (scope_gm_orig_##name) { \
			f->internal_function.handler = scope_gm_orig_##name; \
		} \
	}
	SCOPE_GENERATOR_METHODS(SCOPE_GM_SWAP)
}

/* foreach and the other engine iteration: the generator's iterator */
#if PHP_VERSION_ID < 80400
typedef int scope_valid_result;
#else
typedef zend_result scope_valid_result;
#endif

static zend_object_iterator *(*scope_generator_get_iterator_orig)(zend_class_entry *ce, zval *object, int by_ref);
static zend_object_iterator_funcs scope_gi_funcs;
static const zend_object_iterator_funcs *scope_gi_orig;

static void scope_gi_rewind(zend_object_iterator *it)
{
	scope_resume r = scope_resume_begin();
	scope_gi_orig->rewind(it);
	scope_resume_end(r);
}

static scope_valid_result scope_gi_valid(zend_object_iterator *it)
{
	scope_resume r = scope_resume_begin();
	scope_valid_result v = scope_gi_orig->valid(it);
	scope_resume_end(r);
	return v;
}

static zval *scope_gi_get_current_data(zend_object_iterator *it)
{
	scope_resume r = scope_resume_begin();
	zval *v = scope_gi_orig->get_current_data(it);
	scope_resume_end(r);
	return v;
}

static void scope_gi_get_current_key(zend_object_iterator *it, zval *key)
{
	scope_resume r = scope_resume_begin();
	scope_gi_orig->get_current_key(it, key);
	scope_resume_end(r);
}

static void scope_gi_move_forward(zend_object_iterator *it)
{
	scope_resume r = scope_resume_begin();
	scope_gi_orig->move_forward(it);
	scope_resume_end(r);
}

static zend_object_iterator *scope_generator_get_iterator(zend_class_entry *ce, zval *object, int by_ref)
{
	zend_object_iterator *it = scope_generator_get_iterator_orig(ce, object, by_ref);

	if (it == NULL) {
		return NULL;
	}
	if (scope_gi_orig == NULL) {
		scope_gi_orig = it->funcs;
		scope_gi_funcs = *it->funcs;
		scope_gi_funcs.rewind = scope_gi_rewind;
		scope_gi_funcs.valid = scope_gi_valid;
		scope_gi_funcs.get_current_data = scope_gi_get_current_data;
		scope_gi_funcs.get_current_key = scope_gi_get_current_key;
		scope_gi_funcs.move_forward = scope_gi_move_forward;
	}
	if (it->funcs == scope_gi_orig) {
		it->funcs = &scope_gi_funcs;
	}
	return it;
}

/* The destructor runs pending finally blocks of an unfinished generator. */

static void scope_generator_dtor(zend_object *obj)
{
	scope_resume r = scope_resume_begin();
	scope_generator_handlers_orig->dtor_obj(obj);
	scope_resume_end(r);
}

static void scope_generator_free(zend_object *obj)
{
	if (obj == SCOPE_G(gen_last)) {
		SCOPE_G(gen_last) = NULL;
	}
	scope_generator_handlers_orig->free_obj(obj);
}

/* ---- Scope ---------------------------------------------------------------- */

static zend_object *scope_create(zend_class_entry *ce)
{
	scope_object *so = zend_object_alloc(sizeof(scope_object), ce);
	zend_object_std_init(&so->std, ce);
	object_properties_init(&so->std, ce);
	so->std.handlers = &scope_handlers;
	if (SCOPE_G(active) && Z_ISUNDEF(SCOPE_G(members))) {
		object_init_ex(&SCOPE_G(members), scope_weakmap_ce);   /* the first Scope of the request */
	}
	if (SCOPE_G(superglobals) && SCOPE_G(active)) {
		so->sg = scope_sg_new();
	}
	if (SCOPE_G(locale_timezone) && SCOPE_G(active)) {
		scope_tl_init(&so->tl);
	}
	if (SCOPE_G(superglobals) && SCOPE_SESS_ON() && SCOPE_G(active)) {
		so->sess = scope_sess_new();
	}
	return &so->std;
}

/* A scope destroyed with its session still active writes and closes it here, in the destructor
 * phase, because a save handler may be user code (session_set_save_handler()); free_obj is too
 * late for that, e.g. inside gc_collect_cycles(). The engine calls this when the last reference
 * goes, from the GC before it frees a cycle, and for every live object at request shutdown
 * before RSHUTDOWN. An exception from the handler propagates as one from a destructor would. */
static void scope_dtor(zend_object *obj)
{
	scope_object *so = SCOPE_OBJECT(obj);
	if (so->sess && SCOPE_G(active) && SCOPE_G(applied) != obj) {
		scope_sess *ss = so->sess;
		so->sess = NULL;
		scope_sess_end(ss);
		efree(ss);
	}
	zend_objects_destroy_object(obj);
}

/* What a scope holds while it is not the applied one (the applied scope's values are in the
 * live globals, and its stored ones are UNDEF): its superglobals, its $_SESSION and its user
 * save handler. Reporting them lets the cycle collector free a cycle through them, such as a
 * save handler that refers back to an object holding the scope. */
static HashTable *scope_get_gc(zend_object *obj, zval **table, int *n)
{
	scope_object *so = SCOPE_OBJECT(obj);
	zend_get_gc_buffer *buf = zend_get_gc_buffer_create();

	if (so->sg) {
		for (int i = 0; i < SCOPE_SG_N; i++) {
			zend_get_gc_buffer_add_zval(buf, &so->sg[i]);
		}
	}
	if (so->sess) {
		zval *names = (zval *) &so->sess->user_names;
		for (size_t i = 0; i < sizeof(so->sess->user_names) / sizeof(zval); i++) {
			zend_get_gc_buffer_add_zval(buf, &names[i]);
		}
		zend_get_gc_buffer_add_zval(buf, &so->sess->http_vars);
		zend_get_gc_buffer_add_zval(buf, &so->sess->sym);
	}
	zend_get_gc_buffer_use(buf, table, n);
	return zend_std_get_properties(obj);
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
	if (so->sess) {
		scope_sess_free(so->sess);   /* not ended by scope_dtor(): no user code runs here */
		efree(so->sess);
	}
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
		if (SCOPE_OBJECT(Z_OBJ(z))->sess) {
			scope_sess_free(SCOPE_OBJECT(Z_OBJ(z))->sess);
			efree(SCOPE_OBJECT(Z_OBJ(z))->sess);
			SCOPE_OBJECT(Z_OBJ(z))->sess = NULL;
		}
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

/* Call fci with scope current (NULL or the root object: the root), as run() does: the scope
 * is restored when the call returns, throws or bails out. */
static void scope_call_in(zend_object *scope, zend_fcall_info *fci, zend_fcall_info_cache *fcc)
{
	zend_array *a = SCOPE_G(cur);
	zval *current, saved;
	bool bailout = false;

	if (a == NULL) {
		zend_throw_error(NULL, "Scope::run() is not available during shutdown");
		return;
	}

	/* Push the current scope and make this one current. The array is the running
	 * code's own, so it is the same one again when the callback returns, wherever
	 * the callback suspended in between. */
	current = scope_current(a);
	ZVAL_COPY_VALUE(&saved, current);
	zend_hash_next_index_insert_new(a, &saved);
	if (scope == NULL || scope == SCOPE_G(root)) {
		ZVAL_NULL(scope_current(a));
	} else {
		ZVAL_OBJ_COPY(scope_current(a), scope);
	}
	scope_apply();

	zend_try {
		zend_call_function(fci, fcc);
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

ZEND_METHOD(Scope, run)
{
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	ZEND_PARSE_PARAMETERS_START(1, -1)
		Z_PARAM_FUNC(fci, fcc)
		Z_PARAM_VARIADIC_WITH_NAMED(fci.params, fci.param_count, fci.named_params)
	ZEND_PARSE_PARAMETERS_END();

	fci.retval = return_value;
	scope_call_in(Z_OBJ_P(ZEND_THIS), &fci, &fcc);
}

/* ---- Scope::bind() ----------------------------------------------------------
 *
 * The returned closure is an internal function whose arg_info is the original closure's own
 * (ZEND_ACC_USER_ARG_INFO, as for Closure::__invoke), so its signature, by-reference
 * parameters, defaults and return type are the original's, and the engine passes by-reference
 * arguments as references. Its handler calls the original closure, which keeps its own $this
 * and class scope, through scope_call_in(). The scope and the original closure are held by a
 * scope_bound found through a reserved slot of the function; copies of the closure
 * (Closure::bind(), clone) share it, so it is freed with the last copy that has our handlers
 * when no other copy holds the function name, and otherwise at the end of the request. */

typedef struct {
	zend_object *scope;
	zval closure;
} scope_bound;

static zend_object_handlers scope_bound_handlers;
static void (*scope_closure_free_orig)(zend_object *obj);

static void scope_bound_dtor(zval *zv)
{
	scope_bound *b = Z_PTR_P(zv);

	OBJ_RELEASE(b->scope);
	zval_ptr_dtor(&b->closure);
	efree(b);
}

static void scope_bound_free(zend_object *obj)
{
	const zend_function *f = zend_get_closure_method_def(obj);

	if (SCOPE_G(active) && GC_REFCOUNT(f->common.function_name) == 1) {
		zend_hash_index_del(&SCOPE_G(bound), (zend_ulong) (uintptr_t) f->internal_function.reserved[scope_bound_handle]);
	}
	scope_closure_free_orig(obj);
}

static ZEND_NAMED_FUNCTION(scope_bound_call)
{
	scope_bound *b = EX(func)->internal_function.reserved[scope_bound_handle];
	uint32_t n = ZEND_CALL_NUM_ARGS(execute_data), pos = n;
	zval *args = ZEND_CALL_ARG(execute_data, 1);
	HashTable *named = (ZEND_CALL_INFO(execute_data) & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) ? execute_data->extra_named_params : NULL;
	HashTable *tmp = NULL;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	if (!SCOPE_G(active) || zend_hash_index_find_ptr(&SCOPE_G(bound), (zend_ulong) (uintptr_t) b) != b) {
		zend_throw_error(NULL, "The scope of a closure returned by Scope::bind() is gone");
		return;
	}
	/* Named arguments that skip optional parameters leave holes: pass what follows by name. */
	for (uint32_t i = 0; i < n; i++) {
		if (Z_ISUNDEF(args[i])) {
			pos = i;
			break;
		}
	}
	if (pos < n) {
		const zend_arg_info *ai = (const zend_arg_info *) EX(func)->common.arg_info;

		tmp = zend_new_array(n);
		if (named) {
			zend_hash_copy(tmp, named, zval_add_ref);
		}
		for (uint32_t i = pos; i < n; i++) {
			if (!Z_ISUNDEF(args[i])) {
				Z_TRY_ADDREF(args[i]);
				zend_hash_update(tmp, ai[i].name, &args[i]);
			}
		}
		named = tmp;
	}
	if (zend_fcall_info_init(&b->closure, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
		fci.params = args;
		fci.param_count = pos;
		fci.named_params = named;
		fci.retval = return_value;
		scope_call_in(b->scope, &fci, &fcc);
	}
	if (tmp) {
		zend_array_release(tmp);
	}
}

ZEND_METHOD(Scope, bind)
{
	zval *closure, this_zv;
	zend_object *cobj, *this_obj = NULL;
	zend_class_entry *called_scope = NULL;
	zend_function *fptr = NULL;
	const zend_function *orig;
	zend_internal_function f;
	scope_bound *b;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_OBJECT_OF_CLASS(closure, zend_ce_closure)
	ZEND_PARSE_PARAMETERS_END();

	cobj = Z_OBJ_P(closure);
	orig = zend_get_closure_method_def(cobj);
	/* Binding a closure bind() returned replaces its scope: bind the original one. */
	if (orig->type == ZEND_INTERNAL_FUNCTION && orig->internal_function.reserved[scope_bound_handle]) {
		scope_bound *prev = orig->internal_function.reserved[scope_bound_handle];
		if (zend_hash_index_find_ptr(&SCOPE_G(bound), (zend_ulong) (uintptr_t) prev) == prev) {
			cobj = Z_OBJ(prev->closure);
			orig = zend_get_closure_method_def(cobj);
		}
	}
	cobj->handlers->get_closure(cobj, &called_scope, &fptr, &this_obj, false);

	memset(&f, 0, sizeof(f));
	f.type = ZEND_INTERNAL_FUNCTION;
	memcpy(f.arg_flags, orig->common.arg_flags, sizeof(f.arg_flags));   /* the by-reference send modes */
	f.fn_flags = ZEND_ACC_PUBLIC | ZEND_ACC_USER_ARG_INFO
		| (orig->common.fn_flags & (ZEND_ACC_STATIC | ZEND_ACC_VARIADIC | ZEND_ACC_HAS_RETURN_TYPE));
	f.function_name = zend_string_init(ZSTR_VAL(orig->common.function_name), ZSTR_LEN(orig->common.function_name), 0);
	f.scope = orig->common.scope;
	f.num_args = orig->common.num_args;
	f.required_num_args = orig->common.required_num_args;
	f.arg_info = (zend_internal_arg_info *) orig->common.arg_info;
	f.handler = scope_bound_call;

	b = emalloc(sizeof(scope_bound));
	b->scope = Z_OBJ_P(ZEND_THIS);
	GC_ADDREF(b->scope);
	ZVAL_OBJ_COPY(&b->closure, cobj);
	f.reserved[scope_bound_handle] = b;
	zend_hash_index_add_new_ptr(&SCOPE_G(bound), (zend_ulong) (uintptr_t) b, b);

	if (this_obj) {
		ZVAL_OBJ(&this_zv, this_obj);
	}
	zend_create_closure(return_value, (zend_function *) &f, f.scope, called_scope, this_obj ? &this_zv : NULL);
	zend_string_release(f.function_name);

	if (scope_closure_free_orig == NULL) {
		memcpy(&scope_bound_handlers, Z_OBJ_P(return_value)->handlers, sizeof(zend_object_handlers));
		scope_closure_free_orig = scope_bound_handlers.free_obj;
		scope_bound_handlers.free_obj = scope_bound_free;
	}
	Z_OBJ_P(return_value)->handlers = &scope_bound_handlers;
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
	STD_PHP_INI_BOOLEAN("scope.session", "1", PHP_INI_SYSTEM, OnUpdateBool,
		session, zend_scope_globals, scope_globals)
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
#ifndef ZTS
	{
		zend_module_entry *session = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("session"));
		if (session && session->globals_size == sizeof(php_ps_globals)) {
			scope_ps = session->globals_ptr;
		}
	}
#endif
	for (int i = 0; i < SCOPE_SG_N; i++) {
		scope_sym_key[i] = zend_string_init_interned(scope_sg_names[i], strlen(scope_sg_names[i]), 1);
	}
	scope_sym_key[SCOPE_SYM_SESSION] = zend_string_init_interned("_SESSION", sizeof("_SESSION") - 1, 1);
	scope_ce = register_class_Scope();
	memcpy(&scope_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	scope_handlers.offset = XtOffsetOf(scope_object, std);
	scope_handlers.dtor_obj = scope_dtor;
	scope_handlers.get_gc = scope_get_gc;
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
	scope_generator_get_iterator_orig = zend_ce_generator->get_iterator;
	zend_ce_generator->get_iterator = scope_generator_get_iterator;
	scope_generator_methods_swap(true);
	scope_generator_switches_startup();

	zend_observer_fiber_init_register(scope_fiber_init);
	zend_observer_fiber_switch_register(scope_fiber_switch);
	zend_observer_fiber_destroy_register(scope_fiber_destroy);
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
	zend_ce_generator->get_iterator = scope_generator_get_iterator_orig;
	scope_generator_methods_swap(false);
	zend_compile_string = scope_compile_string_orig;
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
	ZVAL_UNDEF(&SCOPE_G(members));   /* created with the first Scope */
	memset(SCOPE_G(sym_cache), 0, sizeof(SCOPE_G(sym_cache)));
	memset(SCOPE_G(sym_cache_key), 0, sizeof(SCOPE_G(sym_cache_key)));
	zend_hash_init(&SCOPE_G(ctx_cur), 16, NULL, NULL, 0);
	zend_hash_init(&SCOPE_G(ctx_owned), 8, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&SCOPE_G(ctx_resumer), 8, NULL, NULL, 0);
	SCOPE_G(resumer) = NULL;
	SCOPE_G(gen_last) = NULL;
	memset(SCOPE_G(root_sg), 0, sizeof(SCOPE_G(root_sg)));
	memset(&SCOPE_G(root_tl), 0, sizeof(SCOPE_G(root_tl)));
	memset(&SCOPE_G(root_sess), 0, sizeof(SCOPE_G(root_sess)));
	zend_hash_init(&SCOPE_G(bound), 8, NULL, scope_bound_dtor, 0);
	if (SCOPE_G(locale_timezone)) {
		scope_tl_save(&SCOPE_G(root_tl));
		scope_tl_startup();
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
	if (SCOPE_G(superglobals) && SCOPE_SESS_ON()) {
		/* scopes still alive: their sessions end with the request, before ext/session's own RSHUTDOWN */
		for (uint32_t i = 1; i < EG(objects_store).top; i++) {
			zend_object *obj = EG(objects_store).object_buckets[i];
			if (IS_OBJ_VALID(obj) && obj->ce == scope_ce && SCOPE_OBJECT(obj)->sess) {
				/* Detached first: ending it may release the last reference to this scope
				 * (through the save handler or $_SESSION), and scope_free() must not see it. */
				scope_sess *ss = SCOPE_OBJECT(obj)->sess;
				SCOPE_OBJECT(obj)->sess = NULL;
				scope_sess_end(ss);
				efree(ss);
			}
		}
	}
	zend_hash_destroy(&SCOPE_G(bound));
	if (SCOPE_G(root_state)) {
		efree(SCOPE_G(root_state));
		SCOPE_G(root_state) = NULL;
	}
	scope_tl_free(&SCOPE_G(root_tl));
	if (SCOPE_G(loc_initial)) {
		zend_string_release(SCOPE_G(loc_initial));
		SCOPE_G(loc_initial) = NULL;
	}
	scope_slots_free(SCOPE_G(root_slots));
	SCOPE_G(root_slots) = NULL;
	SCOPE_G(active) = false;
	SCOPE_G(cur) = NULL;
	zend_hash_destroy(&SCOPE_G(ctx_owned));
	zend_hash_destroy(&SCOPE_G(ctx_cur));
	zend_hash_destroy(&SCOPE_G(ctx_resumer));
	SCOPE_G(resumer) = NULL;
	SCOPE_G(gen_last) = NULL;
	zval_ptr_dtor(&SCOPE_G(members));
	ZVAL_UNDEF(&SCOPE_G(members));
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
	ZEND_MOD_OPTIONAL("session")
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
