#define GSC_EXPORTS
#ifdef GSC_EXPORTS

#include "ast.h"
#include "compiler.h"
#include "library.h"
#include <setjmp.h>

#define SMALL_STACK_SIZE (16)

CompiledFile *find_or_create_compiled_file(gsc_Context *state, const char *path)
{
	HashTrieNode *entry = hash_trie_upsert(&state->files, path, &state->allocator, false);
	if(!entry->value)
	{
		CompiledFile *cf = new(&state->perm, CompiledFile, 1);
		cf->state = COMPILE_STATE_NOT_STARTED;
		cf->name = entry->key;
		hash_trie_init(&cf->file_references);
		hash_trie_init(&cf->functions);
		hash_trie_init(&cf->includes);
		entry->value = cf;
	}
	return entry->value;
}

CompiledFile *compile(gsc_Context *state, const char *path, const char *data, int flags)
{
	CompiledFile *cf = find_or_create_compiled_file(state, path);
	if(cf->state != COMPILE_STATE_NOT_STARTED)
		return cf;
	int status = compile_file(path, data, cf, &state->perm, state->temp, &state->strtab, flags);
	cf->state = status == 0 ? COMPILE_STATE_DONE : COMPILE_STATE_FAILED;
	if(cf->state != COMPILE_STATE_DONE)
		return cf;
	// printf("%s %s\n", path, cf->name);
	for(HashTrieNode *it = cf->functions.head; it; it = it->next)
	{
		CompiledFunction *f = it->value;
		// printf("%s (%d instructions)\n", it->key, buf_size(f->instructions));
	}
	for(HashTrieNode *it = cf->file_references.head; it; it = it->next)
	{
		// printf("reference: '%s'\n", it->key);
		find_or_create_compiled_file(state, it->key);
	}
	for(HashTrieNode *it = cf->includes.head; it; it = it->next)
	{
		// printf("include: '%s'\n", it->key);
		find_or_create_compiled_file(state, it->key);
	}
	return cf;
}

static int error(gsc_Context *state, const char *fmt, ...)
{
	state->error = true;
	va_list va;
	va_start(va, fmt);
	vsnprintf(state->error_message, sizeof(state->error_message), fmt, va);
	va_end(va);
	return 1;
}

#define CHECK_OOM(state)          \
	if(setjmp(state->jmp_oom))    \
	{                             \
		return GSC_OUT_OF_MEMORY; \
	}

#define CHECK_ERROR(state) \
	if(state->error)       \
	{                      \
		return GSC_ERROR;  \
	}

static void *gsc_malloc(void *ctx, size_t size)
{
	gsc_Context *state = (gsc_Context*)ctx;
	return new(&state->perm, char, size);
	// return state->options.allocate_memory(state->options.userdata, size);
}

static void gsc_free(void *ctx, void *ptr)
{
	gsc_Context *state = (gsc_Context*)ctx;
	// state->options.free_memory(state->options.userdata, ptr);
}

static CompiledFile *get_file(gsc_Context *state, const char *file)
{
	HashTrieNode *n = hash_trie_upsert(&state->files, file, NULL, false);
	if(!n)
		return NULL;
	CompiledFile *cf = n->value;
	if(cf->state == COMPILE_STATE_FAILED)
		return NULL;
	return cf;
}

static CompiledFunction *get_function(gsc_Context *state, const char *file, const char *function)
{
	CompiledFile *f = get_file(state, file);
	if(!f)
		return NULL;
	HashTrieNode *n = hash_trie_upsert(&f->functions, function, NULL, false);
	if(n && n->value)
		return n->value;
	return NULL;
}

static CompiledFunction *vm_func_lookup(void *ctx, const char *file, const char *function)
{
	gsc_Context *state = (gsc_Context*)ctx;
	return get_function(state, file, function);
}

static int f_endon(gsc_Context *ctx)
{
	VM *vm = ctx->vm;
	Object *self = vm_cast_object(ctx->vm, vm_argv(ctx->vm, -1));
	const char *key = vm_checkstring(vm, 0);
	Thread *thr = vm_thread(vm);
	int idx = vm_string_index(vm, key);
	buf_push(thr->endon, idx);
	return 0;
}

// TODO: wait for animation event / notetracks
// Hack: prefix with anim_ and notify when animation is done / encounters a notetrack

static int f_waittillmatch(gsc_Context *ctx)
{
	VM *vm = ctx->vm;
	Object *self = vm_cast_object(ctx->vm, vm_argv(ctx->vm, -1));
	const char *key = vm_checkstring(vm, 0);
	char fake_key[256];
	snprintf(fake_key, sizeof(fake_key), "$nt_%s", key);
	Thread *thr = vm_thread(vm);
	thr->state = VM_THREAD_WAITING_EVENT;

	for(int i = 1; i < vm_argc(vm); i++)
	{
		Variable arg = *vm_argv(vm, i);
		if(arg.type != VAR_REFERENCE)
		{
			vm_error(vm, "Expected reference for waittillmatch");
		}
		thr->waittill.arguments[i - 1] = arg;
	}
	thr->waittill.numargs = vm_argc(vm) - 1;

	thr->waittill.name = vm_string_index(vm, fake_key);
	thr->waittill.object = self;
	if(thr->waittill.name == -1)
	{
		vm_error(vm, "Key '%s' not found", fake_key);
	}
	return 0;
}

static int f_waittill(gsc_Context *ctx)
{
	VM *vm = ctx->vm;
	Object *self = vm_cast_object(ctx->vm, vm_argv(ctx->vm, -1));
	const char *key = vm_checkstring(vm, 0);
	Thread *thr = vm_thread(vm);
	thr->state = VM_THREAD_WAITING_EVENT;
	// thr->waittill.arguments = NULL;
	for(int i = 1; i < vm_argc(vm); i++)
	{
		Variable arg = *vm_argv(vm, i);
		if(arg.type != VAR_REFERENCE)
		{
			vm_error(vm, "Expected reference for waittill");
		}
		thr->waittill.arguments[i - 1] = arg;
	}
	thr->waittill.numargs = vm_argc(vm) - 1;
	thr->waittill.name = vm_string_index(vm, key);
	thr->waittill.object = self;
	if(thr->waittill.name == -1)
	{
		vm_error(vm, "Key '%s' not found", key);
	}
	// printf("[VM] TODO implement waittill: %s\n", key);
	return 0;
}

static int f_notify(gsc_Context *ctx)
{
	VM *vm = ctx->vm;
	Object *self = vm_cast_object(ctx->vm, vm_argv(ctx->vm, -1));
	const char *key = vm_checkstring(vm, 0);
	Thread *thr = vm_thread(vm);
	// vm_notify(vm, vm_dup(vm, &thr->frame->self), key, vm_argc(vm));
	vm_notify(vm, self, key, vm_argc(vm));
	return 0;
}

int gsc_add_tagged_object(gsc_Context *ctx, const char *tag)
{
	Variable v = vm_create_object(ctx->vm);
	Object *o = v.u.oval;
	o->tag = tag;
	// o->proxy = NULL;
	o->proxy = ctx->default_object_proxy;
	// o->userdata = NULL;
	return vm_pushobject(ctx->vm, o);
}

void *gsc_allocate_object(gsc_Context *ctx)
{
	return (void*)vm_allocate_object(ctx->vm);
}

int gsc_add_object(gsc_Context *ctx)
{
	return gsc_add_tagged_object(ctx, NULL);
}

void gsc_object_set_proxy(gsc_Context *ctx, int obj_index, int proxy_index)
{
	Variable *ov = vm_stack(ctx->vm, obj_index);
	if(ov->type != VAR_OBJECT)
		vm_error(ctx->vm, "'%s' is not a object", variable_type_names[ov->type]);
	Object *o = ov->u.oval;
	Variable *pv = vm_stack(ctx->vm, proxy_index);
	o->proxy = pv->u.oval;
}

int gsc_object_get_proxy(gsc_Context *ctx, int obj_index)
{
	Variable *ov = vm_stack(ctx->vm, obj_index);
	if(ov->type != VAR_OBJECT)
		vm_error(ctx->vm, "'%s' is not a object", variable_type_names[ov->type]);
	Object *o = ov->u.oval;
	if(!o->proxy)
	{
		return 0;
	}
	vm_pushobject(ctx->vm, o->proxy);
	return 1;
}

// void gsc_new_object(gsc_Context *ctx, gsc_ObjectProxy *proxy, void *userdata)
// {
// 	Variable v = vm_create_object(ctx->vm);
// 	Object *o = v.u.oval;
// 	o->proxy = proxy;
// 	o->userdata = userdata;
// 	vm_pushobject(ctx->vm, o);
// }

const char *gsc_string(gsc_Context *ctx, int index)
{
	return string_table_get(ctx->vm->strings, index);
}

int gsc_register_string(gsc_Context *ctx, const char *s)
{
	return vm_string_index(ctx->vm, s);
}

static void create_default_object_proxy(gsc_Context *ctx)
{
	ctx->default_object_proxy = NULL;

	int proxy = gsc_add_tagged_object(ctx, "object");
	ctx->default_object_proxy = vm_stack_top(ctx->vm, -1)->u.oval;

	ctx->vm->globals[VAR_GLOB_LEVEL].u.oval->proxy = ctx->default_object_proxy;
	ctx->vm->globals[VAR_GLOB_ANIM].u.oval->proxy = ctx->default_object_proxy;
	ctx->vm->globals[VAR_GLOB_GAME].u.oval->proxy = ctx->default_object_proxy;

	int methods = gsc_add_object(ctx);
		gsc_add_function(ctx, f_waittill);
		gsc_object_set_field(ctx, methods, "waittill");
		gsc_add_function(ctx, f_endon);
		gsc_object_set_field(ctx, methods, "endon");
		gsc_add_function(ctx, f_notify);
		gsc_object_set_field(ctx, methods, "notify");
		gsc_add_function(ctx, f_waittillmatch);
		gsc_object_set_field(ctx, methods, "waittillmatch");
	gsc_object_set_field(ctx, proxy, "__call");

	gsc_set_global(ctx, "object");
}

gsc_Context *gsc_create(gsc_CreateOptions options)
{
	gsc_Context *ctx = options.allocate_memory(options.userdata, sizeof(gsc_Context));
	memset(ctx, 0, sizeof(gsc_Context));
	ctx->options = options;

	if(setjmp(ctx->jmp_oom))
	{
		return NULL;
	}

	ctx->allocator.ctx = ctx;
	ctx->allocator.malloc = gsc_malloc;
	ctx->allocator.free = gsc_free;

	hash_trie_init(&ctx->files);
	// hash_trie_init(&ctx->c_functions);
	// hash_trie_init(&ctx->c_methods);

	// TODO: FIXME
	// #define HEAP_SIZE (512 * 1024 * 1024)
	// #define HEAP_SIZE (28 * 1024 * 1024)
	// #define HEAP_SIZE (85 * 1024 * 1024)
	// #define HEAP_SIZE (83 * 1024 * 1024)
	ctx->heap = options.allocate_memory(options.userdata, options.main_memory_size);
	arena_init(&ctx->perm, ctx->heap, options.main_memory_size);
	ctx->perm.jmp_oom = &ctx->jmp_oom;
	
	// #define TEMP_SIZE (20 * 1024 * 1024)

	arena_init(&ctx->temp, new(&ctx->perm, char, options.temp_memory_size), options.temp_memory_size);
	ctx->temp.jmp_oom = ctx->perm.jmp_oom;

	// #define STRTAB_SIZE (1 * 1024 * 1024)
	Arena strtab_arena;
	arena_init(&strtab_arena, new(&ctx->perm, char, options.string_table_memory_size), options.string_table_memory_size);
	strtab_arena.jmp_oom = ctx->perm.jmp_oom;

	string_table_init(&ctx->strtab, strtab_arena);

	VM *vm = new(&ctx->perm, VM, 1);
	vm_init(vm, &ctx->allocator, &ctx->strtab);
	vm->flags = VM_FLAG_NONE;
	if(options.verbose)
		vm->flags |= VM_FLAG_VERBOSE;
	vm->jmp = &ctx->jmp_oom;
	vm->ctx = ctx;
	vm->func_lookup = vm_func_lookup;

	ctx->vm = vm;
	create_default_object_proxy(ctx);

	void register_dummy_c_functions(VM * vm);
	register_dummy_c_functions(vm);
	return ctx;
}

void gsc_destroy(gsc_Context *state)
{
	if(state)
	{
		vm_cleanup(state->vm);

		gsc_CreateOptions opts = state->options;
		opts.free_memory(opts.userdata, state->heap);
		// opts.free_memory(opts.userdata, state->vm);
		opts.free_memory(opts.userdata, state);
	}
}

void gsc_register_function(gsc_Context *state, const char *namespace, const char *name, gsc_Function callback)
{
	vm_register_callback_function(state->vm, name, (void*)callback, state);
}

void *gsc_object_get_userdata(gsc_Context *ctx, int obj_index)
{
	Variable *ov = vm_stack(ctx->vm, obj_index);
	if(ov->type != VAR_OBJECT)
		vm_error(ctx->vm, "'%s' is not a object", variable_type_names[ov->type]);
	Object *o = ov->u.oval;
	return o->userdata;
}

void gsc_object_set_userdata(gsc_Context *ctx, int obj_index, void *userdata)
{
	Variable *ov = vm_stack(ctx->vm, obj_index);
	if(ov->type != VAR_OBJECT)
		vm_error(ctx->vm, "'%s' is not a object", variable_type_names[ov->type]);
	Object *o = ov->u.oval;
	o->userdata = userdata;
}

// void gsc_register_method(gsc_Context *state, const char *namespace, const char *name, gsc_Method callback)
// {
// 	vm_register_callback_method(state->vm, name, (void*)callback, state);
// }

// void *gsc_object_userdata(gsc_Object *ptr)
// {
// 	Object *object = (Object*)ptr;
//     return object->userdata;
// }

// gsc_ObjectProxy *gsc_object_get_proxy(gsc_Object *ptr)
// {
// 	Object *object = (Object *)ptr;
// 	return object->proxy;
// }

// void gsc_object_set_proxy(gsc_Context *ctx, gsc_Object *ptr, gsc_ObjectProxy *proxy)
// {
// 	Object *object = (Object*)ptr;
//     object->proxy = proxy;
// }

// void gsc_object_set_userdata(gsc_Context *ctx, gsc_Object *ptr, void *userdata)
// {
// 	Object *object = (Object*)ptr;
//     object->userdata = userdata;
// }

void gsc_object_set_field(gsc_Context *state, int obj_index, const char *name)
{
	// Variable value = vm_pop(state->vm);
	vm_set_object_field(state->vm, obj_index, name);
	// vm_set_object_field(state->vm, (Object *)object, name, &value);
}

const char *gsc_object_get_tag(gsc_Context *ctx, int obj_index)
{
	Variable *ov = vm_stack(ctx->vm, obj_index);
	if(ov->type != VAR_OBJECT)
		vm_error(ctx->vm, "'%s' is not an object", variable_type_names[ov->type]);
	return ov->u.oval->tag;
}

void gsc_object_get_field(gsc_Context *state, int obj_index, const char *name)
{
	vm_get_object_field(state->vm, obj_index, name);
}

void gsc_set_global(gsc_Context *ctx, const char *name)
{
	void set_object_field(VM *vm, Variable *ov, const char *key);
	set_object_field(ctx->vm, &ctx->vm->global_object, name);
	// ObjectField *entry = vm_object_upsert(ctx->vm, &ctx->vm->global_object, string(ctx->vm, name));
	// *entry->value = vm_pop(ctx->vm);
}

int gsc_get_global(gsc_Context *ctx, const char *name)
{
	void get_object_field(VM *vm, Variable *ov, const char *key);
	get_object_field(ctx->vm, &ctx->vm->global_object, name);
	// ObjectField *entry = vm_object_upsert(NULL, &ctx->vm->global_object, string(ctx->vm, name));
	// if(!entry)
	// {
	// 	vm_pushundefined(ctx->vm);
	// } else
	// {
	// 	vm_pushvar(ctx->vm, entry->value);
	// }
	return gsc_top(ctx) - 1;
}

int gsc_link(gsc_Context *state)
{
	CHECK_OOM(state);
	Allocator perm_allocator = arena_allocator(&state->perm);
	for(HashTrieNode *it = state->files.head; it; it = it->next)
	{
		CompiledFile *cf = it->value;
		if(cf->state != COMPILE_STATE_DONE)
			continue;
		// Go through all includes
		for(HashTrieNode *include = cf->includes.head; include; include = include->next)
		{
			CompiledFile *included = get_file(state, include->key);
			// If included file failed to compile for any reason, skip
			if(!included)
			{
				continue;
			}
			// printf("include->key:%s, include:%x\n", include->key, included);
			for(HashTrieNode *include_func_it = included->functions.head; include_func_it;
				include_func_it = include_func_it->next)
			{
				// If the file that's including already has a function with this name, don't overwrite it
				HashTrieNode *fnd = hash_trie_upsert(&cf->functions, include_func_it->key, &perm_allocator, false);
				if(!fnd->value)
				{
					fnd->value = include_func_it->value;
				}
			}
		}
	}
	return GSC_OK;
}

int gsc_compile_source(gsc_Context *state, const char *filename, const char *source, int flags)
{
	char basename[256];
	char *sep = strrchr(filename, '.');
	if(sep)
	{
		snprintf(basename, sizeof(basename), "%.*s", (int)(sep - filename), filename);
	}
	CHECK_OOM(state);
	CompiledFile *cf = compile(state, sep ? basename : filename, source, flags);
	switch(cf->state)
	{
		case COMPILE_STATE_DONE: return GSC_OK;
		case COMPILE_STATE_FAILED: return GSC_ERROR;
		case COMPILE_STATE_NOT_STARTED: return GSC_YIELD;
	}
	// while(1)
	// {
	// 	bool done = true;
	// 	for(HashTrieNode *it = files.head; it; it = it->next)
	// 	{
	// 		CompiledFile *cf = it->value;
	// 		if(cf->state != COMPILE_STATE_NOT_STARTED)
	// 			continue;
	// 		done = false;
	// 		snprintf(path, sizeof(path), "%s/%s.gsc", base_path, it->key);
	// 		compile(path, it->key);
	// 	}
	// 	if(done)
	// 		break;
	// }
	return GSC_OK;
}

int gsc_compile(gsc_Context *state, const char *filename, int flags)
{
	int status = GSC_OK;
	const char *source = state->options.read_file(state->options.userdata, filename, &status);
	if(status != GSC_OK)
		return status;
	return gsc_compile_source(state, filename, source, flags);
}

void *gsc_temp_alloc(gsc_Context *ctx, int size)
{
	return new(&ctx->vm->c_function_arena, char, size);
}

const char *gsc_next_compile_dependency(gsc_Context *state)
{
	for(HashTrieNode *it = state->files.head; it; it = it->next)
	{
		CompiledFile *cf = it->value;
		if(cf->state == COMPILE_STATE_NOT_STARTED)
			return it->key;
	}
	return NULL;
}

static void info(gsc_Context *state)
{
	// printf("[INFO] heap %.2f/%.2f MB available\n",
	// 	   arena_available_mib(&state->perm),
	// 	   (float)(HEAP_SIZE - TEMP_SIZE - STRTAB_SIZE) / 1024.f / 1024.f);
	// printf("[INFO] temp %.2f MB\n",
	// 	   arena_available_mib(&state->temp));
	// printf("[INFO] strings text usage:%.2f, entry usage:%.2f / %.2f MB available\n",
	// 	   arena_available_mib(&state->strtab.begin),
	// 	   arena_available_mib(&state->strtab.end),
	// 	   (float)STRTAB_SIZE / 1024.f / 1024.f);
	// int thread_count(VM * vm);
	// printf("[INFO] %d threads\n", thread_count(state->vm));
}

int gsc_update(gsc_Context *state, float dt) // TODO: FIXME mode INFINITE, TIME SLOTTED?
// int gsc_update(gsc_Context *state, int delta_time)
{
	// const char **states[] = { [COMPILE_STATE_NOT_STARTED] = "not started",
	// 						  [COMPILE_STATE_DONE] = "done",
	// 						  [COMPILE_STATE_FAILED] = "failed" };
	// for(HashTrieNode *it = state->files.head; it; it = it->next)
	// {
	// 	CompiledFile *cf = it->value;
	// 	printf("file: %s %s, state: %s, %x\n", it->key, cf->name, states[cf->state], it->value);
	// 	for(HashTrieNode *fit = cf->functions.head; fit; fit = fit->next)
	// 	{
	// 		printf("\t%s %s %x\n", it->key, ((CompiledFunction*)fit->value)->name, fit->value);
	// 	}
	// 	// getchar();
	// }
	// // getchar();
	CHECK_ERROR(state);
	CHECK_OOM(state);
	if(!vm_run_threads(state->vm, dt))
		return GSC_OK;
	// static bool once = false;
	// if(!once)
	// {
	// 	info(state);
	// 	once = true;
	// }
	return GSC_YIELD;
}

static const char *intern_string(gsc_Context *ctx, const char *s)
{
	return gsc_string(ctx, gsc_register_string(ctx, s));
}

void gsc_object_set_debug_info(gsc_Context *ctx, void *object, const char *file, const char *function, int line)
{
	Object *o = (Object*)object;
	o->debug_info.file = intern_string(ctx, file);
	o->debug_info.function = intern_string(ctx, function);
	o->debug_info.line = line;
}

int gsc_call_method(gsc_Context *ctx, const char *namespace, const char *function, int nargs)
{
	CHECK_ERROR(ctx);
	//TODO: handle args
	CHECK_OOM(ctx);
	Variable self = vm_pop(ctx->vm);
	vm_call_function_thread(ctx->vm, namespace, function, nargs, &self);
	return GSC_OK; // TODO: FIXME
}

int gsc_call(gsc_Context *state, const char *namespace, const char *function, int nargs)
{
	CHECK_ERROR(state);
	//TODO: handle args
	CHECK_OOM(state);
	vm_call_function_thread(state->vm, namespace, function, nargs, NULL);
	return GSC_OK; // TODO: FIXME
}

int gsc_push_object(gsc_Context *state, void *object)
{
	return vm_pushobject(state->vm, object);
}

void gsc_push(gsc_Context *state, void *value)
{
	vm_pushvar(state->vm, value);
	// if(state->sp >= SMALL_STACK_SIZE)
	// {
	// 	error(state, "Stack pointer >= SMALL_STACK_SIZE");
	// 	return;
	// }
	// state->small_stack[state->sp++] = *(Variable*)value;
}

int gsc_top(gsc_Context *ctx)
{
	return ctx->vm->thread->sp;
}

void gsc_error(gsc_Context *ctx, const char *fmt, ...)
{
	char message[2048];
	va_list va;
	va_start(va, fmt);
	vsnprintf(message, sizeof(message), fmt, va);
	va_end(va);
	vm_error(ctx->vm, "%s", message);
}

int gsc_type(gsc_Context *ctx, int index)
{
	return vm_stack_top(ctx->vm, index)->type;
}

int gsc_get_type(gsc_Context *ctx, int index)
{
	return vm_argv(ctx->vm, index)->type;
}

void gsc_pop(gsc_Context *state, int count)
{
	for(int i = 0; i < count; ++i)
		vm_pop(state->vm);
}

void gsc_add_int(gsc_Context *state, int value)
{
	vm_pushinteger(state->vm, value);

}
void gsc_add_vec3(gsc_Context *ctx, /*const*/ float *value)
{
	vm_pushvector(ctx->vm, value);
}

void gsc_add_function(gsc_Context *ctx, gsc_Function value)
{
	Variable v;
	v.type = VAR_FUNCTION;
	v.u.funval.is_native = true;
	v.u.funval.native_function = value;
	vm_pushvar(ctx->vm, &v);
}

void gsc_add_bool(gsc_Context *ctx, int cond)
{
	vm_pushbool(ctx->vm, cond);
}

void gsc_add_float(gsc_Context *state, float value)
{
	vm_pushfloat(state->vm, value);
}

void gsc_add_string(gsc_Context *state, const char *value)
{
	vm_pushstring(state->vm, value);
}

int64_t gsc_get_int(gsc_Context *state, int index)
{
	return vm_checkinteger(state->vm, index);
}

void* gsc_get_ptr(gsc_Context *ctx, int index)
{
	Variable *v = vm_argv(ctx->vm, index);
	if(v->type == VAR_OBJECT)
		return v->u.oval;
	return NULL;
}

int gsc_get_bool(gsc_Context *state, int index)
{
	return vm_checkbool(state->vm, index);
}

void gsc_get_vec3(gsc_Context *ctx, int index, float *v)
{
	vm_checkvector(ctx->vm, index, v);
}

int gsc_get_object(gsc_Context *state, int index)
{
	return vm_checkobject(state->vm, index);
}

int gsc_arg(gsc_Context *ctx, int index)
{
	return ctx->vm->fsp - 3 - index;	
}

int gsc_numargs(gsc_Context *ctx)
{
	return ctx->vm->nargs;
}

float gsc_get_float(gsc_Context *state, int index)
{
	return vm_checkfloat(state->vm, index);
}

int gsc_to_int(gsc_Context *ctx, int index)
{
	return vm_cast_int(ctx->vm, vm_stack_top(ctx->vm, index));
}

float gsc_to_float(gsc_Context *ctx, int index)
{
	return vm_cast_float(ctx->vm, vm_stack_top(ctx->vm, index));
}

const char *gsc_to_string(gsc_Context *ctx, int index)
{
	return vm_cast_string(ctx->vm, vm_stack_top(ctx->vm, index));
}

// Only valid in callback of functions added with gsc_register_function
const char *gsc_get_string(gsc_Context *state, int index)
{
	return vm_checkstring(state->vm, index);
}

GSC_API void *gsc_get_internal_pointer(gsc_Context *state, const char *tag)
{
	if(!strcmp(tag, "vm"))
	{
		return state->vm;
	}
	return NULL;
}

#endif