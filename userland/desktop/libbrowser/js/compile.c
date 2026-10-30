/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JavaScript compiler's entry, its functions and its statements
 * (plan/ws074/design.md §12.2): a parsed program becomes a function of the
 * shared bytecode, one code unit for the program and one for each function
 * in it.
 *
 * A function's frame holds its parameters first, then its other bindings
 * that no nested function captures, then the register of its environment
 * (the one it makes when it has captured bindings, or the one of the code
 * around it), then temporary values.  Its code starts by making the
 * environment, moving captured parameters into it, and making the
 * functions it declares; then its statements; then a return of undefined.
 *
 * The first pass covers the core of ES5: var, functions and closures,
 * arguments, this, the operators, object and array literals with
 * accessors, every ES5 statement, and labels.  What later phases
 * bring (let and const, arrow functions, classes, destructuring, spread,
 * templates, generators, async functions, regular expressions and direct
 * eval) arrived in later phases.  Modules now have local bindings, direct
 * export declarations, retained export environments and direct named
 * import links; URL graph loading is still pending.
 */

#include "js/compile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One exported local binding, whose slot is fixed when the compiler places bindings. */
struct js_module_export {
	uint16_t *name;
	size_t length;
	struct js_binding *binding;
	uint32_t slot;
	int lexical;
};

/* One static named import waiting for the host to link its indirect binding. */
struct js_module_import {
	uint16_t *local;
	size_t local_length;
	uint16_t *imported;
	size_t imported_length;
	uint16_t *specifier;
	size_t specifier_length;
	/* The import attributes: the type's value when given, and whether a key other than type was. */
	uint16_t *type;
	size_t type_length;
	int has_type;
	int unknown_key;
	struct js_binding *binding;
	uint32_t slot;
	int linked;
};

/* One compiled module instance, kept rooted by its owner until release. */
struct js_module {
	struct vm_realm *realm;
	struct vm_env *env;
	struct vm_function *function;
	struct js_module_export *exports;
	size_t export_count;
	struct js_module_import *imports;
	size_t import_count;
	int env_rooted;
	int function_rooted;
	int evaluated;
	int eval_status;
};

static void compile_trace(struct vm_heap *heap, void *context);
static void compile_release_all(struct js_compiler *compiler);
static int compile_program(struct vm_realm *realm, struct js_program *program, struct vm_function **function, struct js_module *module, struct js_syntax_error *error);
static void compile_module_exports(struct js_compiler *compiler, struct js_function_info *info, struct js_module *module);
static void compile_module_export_name(struct js_compiler *compiler, struct js_function_info *info, struct js_module *module, struct js_node *name, struct js_node *exported);
static void compile_module_imports(struct js_compiler *compiler, struct js_function_info *info, struct js_module *module);
static struct js_module_import *compile_module_import_entry(struct js_compiler *compiler, struct js_module *module, struct js_node *specifier, struct js_node *local, struct js_node *imported, struct js_node *attributes, struct js_binding *binding);
static void compile_prepare(struct js_function_compiler *fc);
static void compile_place_binding(struct js_function_compiler *fc, struct js_scope *scope, struct js_binding *binding, uint32_t *env_count);
static void compile_prologue(struct js_function_compiler *fc, uint32_t env_count);
static void compile_hoisted(struct js_function_compiler *fc);
static uint32_t compile_flags(const struct js_function_info *info);
static void compile_statement(struct js_function_compiler *fc, struct js_node *node);
static void compile_variables(struct js_function_compiler *fc, struct js_node *node);
static void compile_expression_statement(struct js_function_compiler *fc, struct js_node *node);
static void compile_if(struct js_function_compiler *fc, struct js_node *node);
static struct js_target *compile_target_push(struct js_function_compiler *fc, int loop);
static void compile_target_pop(struct js_function_compiler *fc, struct js_target *target);
static void compile_for(struct js_function_compiler *fc, struct js_node *node);
static void compile_for_in(struct js_function_compiler *fc, struct js_node *node);
static void compile_for_of(struct js_function_compiler *fc, struct js_node *node);
static void compile_while(struct js_function_compiler *fc, struct js_node *node);
static void compile_do_while(struct js_function_compiler *fc, struct js_node *node);
static void compile_jump_statement(struct js_function_compiler *fc, struct js_node *node);
static void compile_jump_out(struct js_function_compiler *fc, uint32_t label, uint32_t finally_depth);
static int compile_has_label(const struct js_target *target, const uint16_t *name, size_t length);
static void compile_return(struct js_function_compiler *fc, struct js_node *node);
static void compile_throw(struct js_function_compiler *fc, struct js_node *node);
static void compile_switch(struct js_function_compiler *fc, struct js_node *node);
static void compile_labeled(struct js_function_compiler *fc, struct js_node *node);
static void compile_try(struct js_function_compiler *fc, struct js_node *node);
static void compile_catch(struct js_function_compiler *fc, struct js_node *node, uint32_t start, uint32_t after);
static void compile_finally_dispatch(struct js_function_compiler *fc, struct js_finally *handler);
static void compile_dispatch_case(struct js_function_compiler *fc, uint32_t kind_register, int code, uint32_t skip);
static void compile_lexicals_empty(struct js_function_compiler *fc, struct js_scope *scope);
static void compile_scope_renew(struct js_function_compiler *fc, struct js_scope *scope, uint32_t outer_env);
static void compile_block(struct js_function_compiler *fc, struct js_node *node);
static void compile_parameters(struct js_function_compiler *fc);
static void compile_class_declaration(struct js_function_compiler *fc, struct js_node *node);
static void compile_with(struct js_function_compiler *fc, struct js_node *node);

/*
 * Compiles a parsed program into a function of a realm (the program's code
 * with no parameters; the caller supplies its top-level this).
 * Returns 0, EINVAL with *error saying what is not supported, or ENOMEM.
 */
int
js_compile(
	struct vm_realm *realm,
	struct js_program *program,
	struct vm_function **function,
	struct js_syntax_error *error)
{
	return compile_program(realm, program, function, NULL, error);
}

/* Compiles either a classic program or a module with a retained environment. */
static int
compile_program(
	struct vm_realm *realm,
	struct js_program *program,
	struct vm_function **function,
	struct js_module *module,
	struct js_syntax_error *error)
{
	struct js_compiler *compiler;
	struct js_function_info *info;
	struct vm_cell *code_root;
	struct vm_function *made;
	struct vm_code *code;
	struct vm_env *env;
	size_t index;
	int jumped;
	int status;

	/* The compiler lives in the program's arena (it must outlive setjmp's frame). */
	*function = NULL;
	memset(error, 0, sizeof(*error));
	compiler = wb_arena_zalloc(&program->arena, sizeof(*compiler));
	if (compiler == NULL)
		return ENOMEM;
	compiler->realm = realm;
	compiler->arena = &program->arena;
	compiler->error = error;
	compiler->module_instance = module != NULL;

	/* The constants of the code being written are the collector's to see. */
	status = vm_heap_add_tracer(realm->heap, compile_trace, compiler);
	if (status != 0)
		return status;

	/* A failure comes back here. */
	jumped = setjmp(compiler->failure);
	if (jumped) {
		compile_release_all(compiler);
		vm_heap_remove_tracer(realm->heap, compile_trace, compiler);
		return compiler->status;
	}

	/* The scopes, then the program's code. */
	info = js_scope_analyze(compiler, program->root);
	if (module != NULL) {
		compile_module_exports(compiler, info, module);
		compile_module_imports(compiler, info, module);
	}
	code = js_compile_function(compiler, NULL, info->node, NULL, 0);
	if (module != NULL) {
		for (index = 0; index < module->export_count; index++) {
			module->exports[index].slot = module->exports[index].binding->location;
			module->exports[index].binding = NULL;
		}
		for (index = 0; index < module->import_count; index++) {
			if (module->imports[index].binding != NULL)
				module->imports[index].slot = module->imports[index].binding->location;
			module->imports[index].binding = NULL;
		}
	}
	vm_heap_remove_tracer(realm->heap, compile_trace, compiler);

	/* A linked module owns an environment that its program and exports share. */
	env = NULL;
	if (module != NULL) {
		code_root = &code->cell;
		status = vm_heap_add_root(realm->heap, &code_root);
		if (status != 0)
			return status;
		env = vm_env_create(realm->heap, NULL, compiler->module_env_count);
		vm_heap_remove_root(realm->heap, &code_root);
		if (env == NULL)
			return ENOMEM;
		module->env = env;
		for (index = 0; index < module->export_count; index++) {
			if (module->exports[index].lexical)
				vm_env_slots(env)[module->exports[index].slot] = VM_VALUE_EMPTY;
		}
	}

	/* The program as a function, retaining the module environment when present. */
	made = vm_closure_create(realm, code, env);
	if (made == NULL)
		return ENOMEM;
	if (module != NULL)
		module->function = made;

	/* Succeeded: the program's function. */
	*function = made;
	return 0;
}

/* Marks direct named exports as retained bindings and records their names. */
static void
compile_module_exports(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_module *module)
{
	struct js_node *item;
	struct js_node *declaration;
	struct js_node *part;

	/* Each supported export declaration contributes one or more direct names. */
	for (item = info->node->first; item != NULL; item = item->next) {
		if (item->kind != JS_NODE_EXPORT)
			continue;

		/* export { local as exported }: names of bindings declared elsewhere in the module. */
		if (item->first != NULL && item->first->kind == JS_NODE_EXPORT_SPECIFIER && item->second == NULL &&
		    (item->flags & (JS_FLAG_DEFAULT | JS_FLAG_ALL)) == 0U) {
			for (part = item->first; part != NULL; part = part->next)
				compile_module_export_name(compiler, info, module, part->first, part->second);
			continue;
		}
		if (!js_export_is_declaration(item))
			js_compile_unsupported(compiler, item, "this kind of module export");

		/* A default export of a named function or class is exported as default. */
		declaration = item->first;
		if ((item->flags & JS_FLAG_DEFAULT) != 0U) {
			compile_module_export_name(compiler, info, module, declaration, NULL);
			continue;
		}

		/* A declaration's name, or each var/let/const declarator's name. */
		if (declaration->kind == JS_NODE_VARIABLES) {
			for (part = declaration->first; part != NULL; part = part->next) {
				if (part->first == NULL || part->first->kind != JS_NODE_IDENTIFIER)
					js_compile_unsupported(compiler, part, "exported binding patterns");
				compile_module_export_name(compiler, info, module, part->first, part->first);
			}
		} else if (declaration->kind == JS_NODE_FUNCTION_DECLARATION ||
		    declaration->kind == JS_NODE_CLASS_DECLARATION) {
			compile_module_export_name(compiler, info, module, declaration, declaration);
		} else {
			js_compile_unsupported(compiler, declaration, "this kind of module export");
		}
	}
}

/* Retains one exported name and its local binding for later slot lookup. */
static void
compile_module_export_name(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_module *module,
	struct js_node *name,
	struct js_node *exported)
{
	static const uint16_t default_local[] = { '*', 'd', 'e', 'f', 'a', 'u', 'l', 't', '*' };
	static const uint16_t default_name[] = { 'd', 'e', 'f', 'a', 'u', 'l', 't' };
	struct js_module_export *exports;
	struct js_module_export *entry;
	struct js_binding *binding;
	const uint16_t *exported_text;
	size_t exported_length;
	size_t index;
	int same;

	/* The parser and scope pass must have given the export a binding. */
	if (name->text == NULL)
		js_compile_fail(compiler, name, "An exported declaration needs a name");
	binding = NULL;
	for (binding = info->scope->bindings; binding != NULL; binding = binding->next) {
		same = binding->length == name->text_length;
		if (same)
			same = memcmp(binding->name, name->text, name->text_length * sizeof(uint16_t)) == 0;
		if (same)
			break;
	}
	if (binding == NULL)
		js_compile_fail(compiler, name, "Exported declaration has no local binding");

	/* An imported binding cannot be exported again without an indirect entry. */
	if (binding->kind == JS_BINDING_IMPORT)
		js_compile_unsupported(compiler, name, "re-exporting an import");

	/* The exported name: the local one, another one after as, or default (for a default export and the hidden constant). */
	exported_text = name->text;
	exported_length = name->text_length;
	if (exported != NULL) {
		exported_text = exported->text;
		exported_length = exported->text_length;
	} else {
		exported_text = default_name;
		exported_length = sizeof(default_name) / sizeof(default_name[0]);
	}
	if (name->text_length == sizeof(default_local) / sizeof(default_local[0]) &&
	    memcmp(name->text, default_local, sizeof(default_local)) == 0) {
		exported_text = default_name;
		exported_length = sizeof(default_name) / sizeof(default_name[0]);
	}

	/* One exported name may not appear twice. */
	for (index = 0; index < module->export_count; index++) {
		entry = &module->exports[index];
		same = entry->length == exported_length;
		if (same)
			same = memcmp(entry->name, exported_text, exported_length * sizeof(uint16_t)) == 0;
		if (same)
			js_compile_fail(compiler, name, "Duplicate module export");
	}

	/* The instance keeps a copy beyond the parser arena's lifetime. */
	exports = realloc(module->exports, (module->export_count + 1U) * sizeof(*exports));
	if (exports == NULL)
		js_compile_out_of_memory(compiler);
	module->exports = exports;
	entry = &exports[module->export_count];
	memset(entry, 0, sizeof(*entry));
	entry->name = malloc((exported_length + 1U) * sizeof(uint16_t));
	if (entry->name == NULL)
		js_compile_out_of_memory(compiler);
	memcpy(entry->name, exported_text, exported_length * sizeof(uint16_t));
	entry->name[exported_length] = 0;
	entry->length = exported_length;
	entry->binding = binding;
	entry->lexical = binding->kind == JS_BINDING_LET || binding->kind == JS_BINDING_CONST;
	module->export_count++;

	/* An export lives in the module's retained environment, even without a closure. */
	binding->captured = 1;
	info->has_env = 1;
}

/*
 * Adds one import entry of a module: where it comes from (the specifier and
 * its attributes) and, for a binding, the imported and local names.
 */
static struct js_module_import *
compile_module_import_entry(
	struct js_compiler *compiler,
	struct js_module *module,
	struct js_node *specifier,
	struct js_node *local,
	struct js_node *imported,
	struct js_node *attributes,
	struct js_binding *binding)
{
	static const uint16_t type_key[] = { 't', 'y', 'p', 'e' };
	struct js_module_import *imports;
	struct js_module_import *entry;
	struct js_node *attribute;
	struct js_node *key;
	struct js_node *value;

	/* The entry. */
	imports = realloc(module->imports, (module->import_count + 1U) * sizeof(*imports));
	if (imports == NULL)
		js_compile_out_of_memory(compiler);
	module->imports = imports;
	entry = &imports[module->import_count];
	memset(entry, 0, sizeof(*entry));
	module->import_count++;

	/* The names it owns. */
	entry->specifier = malloc((specifier->text_length + 1U) * sizeof(uint16_t));
	if (entry->specifier == NULL)
		js_compile_out_of_memory(compiler);
	memcpy(entry->specifier, specifier->text, specifier->text_length * sizeof(uint16_t));
	entry->specifier[specifier->text_length] = 0;
	entry->specifier_length = specifier->text_length;
	if (local != NULL) {
		entry->local = malloc((local->text_length + 1U) * sizeof(uint16_t));
		entry->imported = malloc((imported->text_length + 1U) * sizeof(uint16_t));
		if (entry->local == NULL || entry->imported == NULL)
			js_compile_out_of_memory(compiler);
		memcpy(entry->local, local->text, local->text_length * sizeof(uint16_t));
		memcpy(entry->imported, imported->text, imported->text_length * sizeof(uint16_t));
		entry->local[local->text_length] = 0;
		entry->imported[imported->text_length] = 0;
		entry->local_length = local->text_length;
		entry->imported_length = imported->text_length;
	} else {
		/* Nothing to bind: there is nothing for the host to link. */
		entry->linked = 1;
	}
	entry->binding = binding;

	/* The attributes: type is the only key a module knows. */
	for (attribute = attributes; attribute != NULL; attribute = attribute->next) {
		key = attribute->first;
		value = attribute->second;
		if (key == NULL || value == NULL || key->text == NULL ||
		    key->text_length != sizeof(type_key) / sizeof(type_key[0]) ||
		    memcmp(key->text, type_key, sizeof(type_key)) != 0) {
			entry->unknown_key = 1;
			continue;
		}
		entry->has_type = 1;
		entry->type = malloc((value->text_length + 1U) * sizeof(uint16_t));
		if (entry->type == NULL)
			js_compile_out_of_memory(compiler);
		memcpy(entry->type, value->text, value->text_length * sizeof(uint16_t));
		entry->type[value->text_length] = 0;
		entry->type_length = value->text_length;
	}
	return entry;
}

/* Records static named imports and keeps their local slots after evaluation. */
static void
compile_module_imports(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_module *module)
{
	struct js_binding *binding;
	struct js_node *item;
	struct js_node *part;
	struct js_node *local;
	struct js_node *imported;
	struct js_node *specifier;
	struct js_node default_node;
	static const uint16_t default_text[] = { 'd', 'e', 'f', 'a', 'u', 'l', 't' };
	int same;

	/* This finite pass accepts named and default imports from string specifiers. */
	for (item = info->node->first; item != NULL; item = item->next) {
		if (item->kind != JS_NODE_IMPORT)
			continue;
		specifier = item->second;
		if (specifier == NULL || specifier->kind != JS_NODE_STRING)
			js_compile_unsupported(compiler, item, "this kind of module import");

		/* An import with no bindings only loads the module. */
		if (item->first == NULL) {
			compile_module_import_entry(compiler, module, specifier, NULL, NULL, item->third, NULL);
			continue;
		}
		for (part = item->first; part != NULL; part = part->next) {
			imported = part->first;
			local = part->second;
			if ((part->flags & JS_FLAG_ALL) != 0U)
				js_compile_unsupported(compiler, part, "a namespace import");

			/* A default import names the default export. */
			if (imported == NULL && local != NULL && local->kind == JS_NODE_IDENTIFIER) {
				imported = &default_node;
				default_node.kind = JS_NODE_IDENTIFIER;
				default_node.text = default_text;
				default_node.text_length = sizeof(default_text) / sizeof(default_text[0]);
			}
			if (imported == NULL || (imported->kind != JS_NODE_IDENTIFIER && imported->kind != JS_NODE_STRING) ||
			    local == NULL || local->kind != JS_NODE_IDENTIFIER)
				js_compile_unsupported(compiler, part, "this kind of module import");

			/* The scope pass already declared the local name. */
			binding = NULL;
			for (binding = info->scope->bindings; binding != NULL; binding = binding->next) {
				same = binding->length == local->text_length;
				if (same)
					same = memcmp(binding->name, local->text,
					    local->text_length * sizeof(uint16_t)) == 0;
				if (same)
					break;
			}
			if (binding == NULL || binding->kind != JS_BINDING_IMPORT)
				js_compile_fail(compiler, local, "Module import has no local binding");

			/* A stable import entry owns its names beyond the parser arena. */
			compile_module_import_entry(compiler, module, specifier, local, imported, item->third, binding);
			binding->captured = 1;
			info->has_env = 1;
		}
	}
}

/*
 * Parses, compiles and runs a script with the global object as this;
 * stores its completion value (its last expression statement's).  Returns 0, VM_THROWN with
 * the realm's exception set, EINVAL with *error describing a syntax error
 * or what is not supported, or ENOMEM.
 */
int
js_run_script(
	struct vm_realm *realm,
	const uint16_t *source,
	size_t length,
	unsigned how,
	vm_value *result,
	struct js_syntax_error *error)
{
	struct js_program program;
	struct vm_function *function;
	int status;

	/* The tree. */
	*result = VM_VALUE_UNDEFINED;
	status = js_parse(source, length, how, &program, error);
	if (status != 0)
		return status;

	/* The code (the tree is no longer needed after it). */
	status = js_compile(realm, &program, &function, error);
	js_program_release(&program);
	if (status != 0)
		return status;

	/* The run. */
	/* A module's top-level this is undefined; a classic script receives the global object. */
	if ((how & JS_PARSE_MODULE) != 0U)
		status = vm_call(realm, vm_value_cell(function), VM_VALUE_UNDEFINED, NULL, 0, result);
	else
		status = vm_call(realm, vm_value_cell(function), vm_value_cell(realm->global), NULL, 0, result);
	if (status != 0)
		return status;

	/* Succeeded: the script ran to its end. */
	return 0;
}

/* Parses and compiles one module with an environment its owner can retain. */
int
js_module_create(
	struct vm_realm *realm,
	const uint16_t *source,
	size_t length,
	struct js_module **module,
	struct js_syntax_error *error)
{
	struct js_module *created;
	struct js_program program;
	int status;

	/* The owner and its two stable GC root slots. */
	*module = NULL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->realm = realm;
	status = vm_heap_add_root(realm->heap, (struct vm_cell **)&created->env);
	if (status != 0) {
		free(created);
		return status;
	}
	created->env_rooted = 1;
	status = vm_heap_add_root(realm->heap, (struct vm_cell **)&created->function);
	if (status != 0) {
		js_module_release(created);
		return status;
	}
	created->function_rooted = 1;

	/* The module program, then its retained closure and export slots. */
	status = js_parse(source, length, JS_PARSE_MODULE, &program, error);
	if (status == 0) {
		status = compile_program(realm, &program, &created->function, created, error);
		js_program_release(&program);
	}
	if (status != 0) {
		js_module_release(created);
		return status;
	}

	/* The instance is ready for one evaluation. */
	*module = created;
	return 0;
}

/* Evaluates a module once, preserving its live bindings for subsequent reads. */
int
js_module_evaluate(
	struct js_module *module)
{
	vm_value completion;
	size_t index;
	int status;

	/* A second call cannot repeat a module's side effects. */
	if (module->evaluated)
		return module->eval_status;
	for (index = 0; index < module->import_count; index++) {
		if (!module->imports[index].linked)
			return EAGAIN;
	}
	module->evaluated = 1;
	status = vm_call(module->realm, vm_value_cell(module->function), VM_VALUE_UNDEFINED, NULL, 0, &completion);
	module->eval_status = status;
	return status;
}

/* Reads one direct named export from the instance's stable environment. */
int
js_module_export(
	struct js_module *module,
	const uint16_t *name,
	size_t length,
	vm_value *value)
{
	struct js_module_export *entry;
	size_t index;
	int same;

	/* An unevaluated module has no initialized values for its caller yet. */
	*value = VM_VALUE_UNDEFINED;
	if (!module->evaluated || module->eval_status != 0)
		return EAGAIN;
	for (index = 0; index < module->export_count; index++) {
		entry = &module->exports[index];
		same = entry->length == length;
		if (same)
			same = memcmp(entry->name, name, length * sizeof(uint16_t)) == 0;
		if (!same)
			continue;
		*value = vm_env_slots(module->env)[entry->slot];
		if (*value == VM_VALUE_EMPTY)
			return EAGAIN;
		return 0;
	}
	return ENOENT;
}

/* Returns the stable import names that a graph loader must resolve. */
size_t
js_module_import_count(
	const struct js_module *module)
{
	return module->import_count;
}

/* Borrows one import's specifier and both names until the module is released. */
int
js_module_import_at(
	const struct js_module *module,
	size_t index,
	struct js_module_import_info *info)
{
	const struct js_module_import *entry;

	if (info == NULL)
		return EINVAL;
	if (index >= module->import_count)
		return ENOENT;
	entry = &module->imports[index];
	info->specifier = entry->specifier;
	info->specifier_length = entry->specifier_length;
	info->imported = entry->imported;
	info->imported_length = entry->imported_length;
	info->local = entry->local;
	info->local_length = entry->local_length;
	info->type = entry->type;
	info->type_length = entry->type_length;
	info->has_type = entry->has_type;
	info->unknown_key = entry->unknown_key;
	return 0;
}

/* Links one local named import to the target module's retained export slot. */
int
js_module_link_import(
	struct js_module *module,
	const uint16_t *specifier,
	size_t specifier_length,
	const uint16_t *local,
	size_t local_length,
	struct js_module *target,
	const uint16_t *exported,
	size_t exported_length)
{
	struct js_module_import *import;
	struct js_module_export *entry;
	struct vm_import_binding *binding;
	size_t import_index;
	size_t export_index;
	int same;

	/* Both instances belong to one realm, and linking precedes evaluation. */
	if (module->realm != target->realm || module->evaluated)
		return EINVAL;
	import = NULL;
	for (import_index = 0; import_index < module->import_count; import_index++) {
		import = &module->imports[import_index];
		same = import->local_length == local_length;
		if (same)
			same = memcmp(import->local, local, local_length * sizeof(uint16_t)) == 0;
		if (same)
			break;
	}
	if (import_index == module->import_count)
		return ENOENT;
	if (import->linked)
		return EALREADY;
	if (import->specifier_length != specifier_length ||
	    memcmp(import->specifier, specifier, specifier_length * sizeof(uint16_t)) != 0)
		return EINVAL;
	if (import->imported_length != exported_length ||
	    memcmp(import->imported, exported, exported_length * sizeof(uint16_t)) != 0)
		return EINVAL;

	/* A missing target export cannot become a valid binding by evaluation. */
	entry = NULL;
	for (export_index = 0; export_index < target->export_count; export_index++) {
		entry = &target->exports[export_index];
		same = entry->length == exported_length;
		if (same)
			same = memcmp(entry->name, exported, exported_length * sizeof(uint16_t)) == 0;
		if (same)
			break;
	}
	if (export_index == target->export_count)
		return ENOENT;

	/* The import binding itself keeps the target environment live. */
	binding = vm_import_binding_create(module->realm->heap, target->env, entry->slot);
	if (binding == NULL)
		return ENOMEM;
	vm_env_slots(module->env)[import->slot] = vm_value_cell(binding);
	import->linked = 1;
	return 0;
}

/* Releases the module's GC roots and owned export names before its realm goes away. */
void
js_module_release(
	struct js_module *module)
{
	size_t index;

	if (module == NULL)
		return;
	if (module->function_rooted)
		vm_heap_remove_root(module->realm->heap, (struct vm_cell **)&module->function);
	if (module->env_rooted)
		vm_heap_remove_root(module->realm->heap, (struct vm_cell **)&module->env);
	for (index = 0; index < module->export_count; index++)
		free(module->exports[index].name);
	free(module->exports);
	for (index = 0; index < module->import_count; index++) {
		free(module->imports[index].local);
		free(module->imports[index].imported);
		free(module->imports[index].specifier);
		free(module->imports[index].type);
	}
	free(module->imports);
	free(module);
}

/*
 * Fails the compilation at a node with a message.
 */
void
js_compile_fail(
	struct js_compiler *compiler,
	const struct js_node *node,
	const char *message)
{
	/* The place and the message, then back to js_compile. */
	if (node != NULL) {
		compiler->error->line = node->line;
		compiler->error->column = node->column;
	}

	/* The message and the status, then back to js_compile. */
	snprintf(compiler->error->message, sizeof(compiler->error->message), "%s", message);
	compiler->status = EINVAL;
	longjmp(compiler->failure, 1);
}

/*
 * Fails the compilation at a node that uses what the compiler does not
 * support yet.
 */
void
js_compile_unsupported(
	struct js_compiler *compiler,
	const struct js_node *node,
	const char *what)
{
	char message[160];

	/* The message names the construct; the error says the script is sound. */
	snprintf(message, sizeof(message), "not supported yet: %s", what);
	compiler->error->unsupported = 1;
	js_compile_fail(compiler, node, message);
}

/*
 * Fails the compilation for want of memory.
 */
void
js_compile_out_of_memory(
	struct js_compiler *compiler)
{
	/* The status, then back to js_compile. */
	snprintf(compiler->error->message, sizeof(compiler->error->message), "out of memory");
	compiler->status = ENOMEM;
	longjmp(compiler->failure, 1);
}

/*
 * Compiles a function (or, with no parent, the program) into its code
 * unit; name is its name (NULL for the node's own).
 */
struct vm_code *
js_compile_function(
	struct js_compiler *compiler,
	struct js_function_compiler *parent,
	struct js_node *node,
	const uint16_t *name,
	size_t name_length)
{
	struct js_function_compiler *fc;
	struct js_function_info *info;
	struct vm_code *code;
	uint32_t result;
	uint32_t how;
	uint32_t flags;

	/* What the language does not have in this compiler yet. */
	info = node->scope->function;
	if ((node->flags & JS_FLAG_GENERATOR) != 0U && (node->flags & JS_FLAG_ASYNC) != 0U)
		js_compile_unsupported(compiler, node, "async generators");

	/* The function's state, in the arena, the innermost being compiled. */
	fc = wb_arena_zalloc(compiler->arena, sizeof(*fc));
	if (fc == NULL)
		js_compile_out_of_memory(compiler);
	fc->compiler = compiler;
	fc->parent = parent;
	fc->info = info;
	fc->scope = info->scope;
	js_emit_begin(fc);
	compiler->current = fc;

	/* The registers and the environment, the prologue, then the body. */
	compile_prepare(fc);

	/* A generator's call ends here, after its parameters; the first next goes on from here (ws074-p086). */
	if ((node->flags & JS_FLAG_GENERATOR) != 0U) {
		result = js_temp(fc);
		how = js_temp(fc);
		js_load_value(fc, result, VM_VALUE_UNDEFINED);
		js_emit3(fc, VM_OP_SUSPEND, result, result, how);
	}

	/* The body. */
	if ((node->flags & JS_FLAG_STATIC_INIT) != 0U) {
		/* A class's static function: its static fields and blocks. */
		js_compile_fields(fc, info->class_node, 1);
	} else if (info->program) {
		js_compile_statements(fc, node->first);
	} else if ((node->flags & JS_FLAG_EXPRESSION_BODY) != 0U) {
		/* An arrow function's expression body is its return value. */
		result = js_temp(fc);
		js_compile_expression(fc, node->second, result);
		js_emit1(fc, VM_OP_RETURN, result);
	} else {
		js_compile_statements(fc, node->second);
	}

	/* Falling off the end returns undefined (the program its completion value). */
	if (info->program) {
		js_emit1(fc, VM_OP_RETURN, fc->completion_register);
	} else {
		result = js_temp(fc);
		js_load_value(fc, result, VM_VALUE_UNDEFINED);
		js_emit1(fc, VM_OP_RETURN, result);
	}

	/* The name: the one given, or the node's own. */
	if (name == NULL && node->text != NULL) {
		name = node->text;
		name_length = node->text_length;
	}

	/* The finished unit; the parent is the innermost again. */
	flags = compile_flags(info);
	code = js_emit_finish(fc, name, name_length, flags);
	compiler->current = parent;

	/* Succeeded: the code unit. */
	return code;
}

/*
 * Compiles a list of statements.
 */
void
js_compile_statements(
	struct js_function_compiler *fc,
	struct js_node *list)
{
	struct js_node *node;

	/* Each statement in order. */
	for (node = list; node != NULL; node = node->next)
		compile_statement(fc, node);
}

/*
 * Returns a register's value from the function, through the finally
 * blocks of the try statements it leaves.
 */
void
js_emit_return(
	struct js_function_compiler *fc,
	uint32_t value_register)
{
	struct js_finally *handler;

	/* Without a finally block to pass, the return itself. */
	handler = fc->finally;
	if (handler == NULL) {
		js_emit1(fc, VM_OP_RETURN, value_register);
		return;
	}

	/* The value and the completion code go to the finally block, which returns after it. */
	js_emit2(fc, VM_OP_MOV, handler->value_register, value_register);
	js_emit2(fc, VM_OP_LOAD_INT, handler->kind_register, JS_COMPLETION_RETURN);
	js_emit_jump(fc, VM_OP_JUMP, 0, handler->entry_label);
}

/*
 * Enters the scope of a block, a for statement's head or a switch's
 * cases: its environment when it has one (on top of the running one), its
 * let and const not usable yet, and the function declarations it makes.
 * The scope and the environment register before are kept for
 * js_scope_leave.
 */
void
js_scope_enter(
	struct js_function_compiler *fc,
	struct js_scope *scope,
	struct js_scope **saved_scope,
	uint32_t *saved_env)
{
	struct js_hoisted *entry;
	struct vm_code *code;
	uint32_t mark;
	uint32_t closure;
	uint32_t constant;

	/* What the code around it uses. */
	*saved_scope = fc->scope;
	*saved_env = fc->env_register;

	/* The block's environment, made at each entry, holds its captured bindings. */
	if (scope->has_env) {
		js_emit3(fc, VM_OP_NEW_ENV, scope->env_register, fc->env_register, scope->env_count);
		fc->env_register = scope->env_register;
	}

	/* The names resolve from the block now, and its let and const wait for their declarations. */
	fc->scope = scope;
	compile_lexicals_empty(fc, scope);

	/* Each function declared in the block: its closure sees the block, and its var takes it (Annex B). */
	mark = fc->temp_top;
	for (entry = scope->functions; entry != NULL; entry = entry->next) {
		code = js_compile_function(fc->compiler, fc, entry->node, NULL, 0);
		constant = js_constant(fc, vm_value_cell(code));
		closure = js_temp(fc);
		js_emit3(fc, VM_OP_NEW_CLOSURE, closure, constant, fc->env_register);
		js_store_binding(fc, entry->node, entry->node->text, entry->node->text_length, closure);
		fc->temp_top = mark;
	}
}

/* Leaves a scope js_scope_enter entered: the scope and the environment around it again. */
void
js_scope_leave(
	struct js_function_compiler *fc,
	struct js_scope *saved_scope,
	uint32_t saved_env)
{
	/* The code after the block resolves and captures as before it. */
	fc->scope = saved_scope;
	fc->env_register = saved_env;
}

/* Marks the constants of every code unit being written. */
static void
compile_trace(
	struct vm_heap *heap,
	void *context)
{
	struct js_compiler *compiler;
	struct js_function_compiler *fc;
	vm_value *constant;
	size_t index;

	/* Each function being compiled, from the innermost outwards. */
	compiler = context;
	for (fc = compiler->current; fc != NULL; fc = fc->parent) {
		if (fc->released)
			continue;

		/* Each of its constants. */
		for (index = 0; index < fc->constants.count; index++) {
			constant = wb_vector_at(&fc->constants, index);
			vm_heap_mark_value(heap, *constant);
		}
	}
}

/* Frees what the functions being compiled hold, after a failure. */
static void
compile_release_all(
	struct js_compiler *compiler)
{
	struct js_function_compiler *fc;

	/* Each function being compiled. */
	for (fc = compiler->current; fc != NULL; fc = fc->parent)
		js_emit_release(fc);
	compiler->current = NULL;
}

/* Places every binding of a function, counts its registers, and writes its prologue. */
static void
compile_prepare(
	struct js_function_compiler *fc)
{
	struct js_function_info *info;
	struct js_scope *scope;
	struct js_binding *binding;
	struct js_node *parameter;
	uint32_t env_count;

	/* The parameters take the first registers (the program has none). */
	info = fc->info;
	fc->parameter_count = 0;
	parameter = NULL;
	if (!info->program)
		parameter = info->node->first;
	for (;
	     parameter != NULL;
	     parameter = parameter->next) {
		if (parameter->kind != JS_NODE_REST)
			fc->parameter_count++;
	}

	/* The locals start after the arguments' registers. */
	fc->local_count = fc->parameter_count;

	/* Every binding of every scope of the function gets its place, and a block with an environment a register to hold it. */
	env_count = 0;
	for (scope = info->scopes; scope != NULL; scope = scope->next_in_function) {
		for (binding = scope->bindings; binding != NULL; binding = binding->next)
			compile_place_binding(fc, scope, binding, &env_count);
		if (scope->kind == JS_SCOPE_BLOCK && scope->has_env) {
			scope->env_register = fc->local_count;
			fc->local_count++;
		}
	}
	if (fc->compiler->module_instance && info->program)
		fc->compiler->module_env_count = env_count;

	/* A rest parameter reads the arguments object, which needs a register when the code does not name arguments. */
	if (info->rest != NULL && info->arguments == NULL) {
		fc->arguments_register = fc->local_count;
		fc->local_count++;
	}

	/* The program's completion value (the last expression statement's), the environment's register, then the temporaries. */
	fc->completion_register = fc->local_count;
	if (info->program)
		fc->local_count++;
	fc->env_register = fc->local_count;
	fc->local_count++;
	fc->register_count = fc->local_count;
	fc->temp_top = fc->local_count;

	/* The code that sets them up. */
	compile_prologue(fc, env_count);
}

/* Gives one binding its place: a slot of the environment when captured, a register otherwise. */
static void
compile_place_binding(
	struct js_function_compiler *fc,
	struct js_scope *scope,
	struct js_binding *binding,
	uint32_t *env_count)
{
	/* The arguments object always arrives in a register of its own. */
	if (binding->kind == JS_BINDING_ARGUMENTS) {
		fc->arguments_register = fc->local_count;
		fc->local_count++;
	}

	/* A captured binding of a block lives in the block's environment. */
	if (binding->captured && scope->kind == JS_SCOPE_BLOCK) {
		binding->in_env = 1;
		binding->location = scope->env_count;
		scope->env_count++;
		return;
	}

	/* Any other captured binding lives in the function's environment. */
	if (binding->captured) {
		binding->in_env = 1;
		binding->location = *env_count;
		(*env_count)++;
		return;
	}

	/* A parameter keeps its argument's register; arguments its own. */
	binding->in_env = 0;
	if (binding->kind == JS_BINDING_PARAMETER) {
		binding->location = binding->parameter;
		return;
	}

	/* The arguments object keeps its own register. */
	if (binding->kind == JS_BINDING_ARGUMENTS) {
		binding->location = fc->arguments_register;
		return;
	}

	/* Any other binding gets a register. */
	binding->location = fc->local_count;
	fc->local_count++;
}

/*
 * Writes a function's prologue: its environment, the captured parameters
 * and arguments moved into it, its own name for a named function
 * expression, and its function declarations.
 */
static void
compile_prologue(
	struct js_function_compiler *fc,
	uint32_t env_count)
{
	struct js_function_info *info;
	struct js_scope *scope;
	struct js_binding *binding;
	uint32_t this_value;
	uint32_t callee;
	uint32_t opcode;

	/* The environment of the code around it, then its own on top when it has captured bindings. */
	info = fc->info;
	js_emit1(fc, VM_OP_LOAD_CLOSURE_ENV, fc->env_register);
	if (info->has_env && !(fc->compiler->module_instance && info->program))
		js_emit3(fc, VM_OP_NEW_ENV, fc->env_register, fc->env_register, env_count);

	/* The hidden bindings of this and of the home object, which the arrow functions inside read. */
	for (binding = info->scope->bindings; binding != NULL; binding = binding->next) {
		if (binding->kind != JS_BINDING_THIS && binding->kind != JS_BINDING_HOME)
			continue;
		opcode = VM_OP_LOAD_THIS;
		if (binding->kind == JS_BINDING_HOME)
			opcode = VM_OP_LOAD_HOME;

		/* A derived class's constructor has no this yet: its super call stores it (compile_super_this). */
		if (opcode == VM_OP_LOAD_THIS && info->class_node != NULL && info->class_node->first != NULL &&
		    (info->node->flags & JS_FLAG_STATIC_INIT) == 0U)
			continue;
		if (binding->in_env) {
			this_value = js_temp(fc);
			js_emit1(fc, opcode, this_value);
			js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, 0, binding->location, this_value);
			fc->temp_top--;
		} else {
			js_emit1(fc, opcode, binding->location);
		}
	}

	/* The captured parameters and arguments object move into the environment. */
	for (binding = info->scope->bindings; binding != NULL; binding = binding->next) {
		if (!binding->in_env)
			continue;
		if (binding->kind == JS_BINDING_PARAMETER)
			js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, 0, binding->location, binding->parameter);
		if (binding->kind == JS_BINDING_ARGUMENTS)
			js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, 0, binding->location, fc->arguments_register);
	}

	/* A named function expression's own name is the function. */
	for (scope = info->scopes; scope != NULL; scope = scope->next_in_function) {
		if (scope->kind != JS_SCOPE_CALLEE)
			continue;

		/* Its one binding, in the environment or in a register. */
		binding = scope->bindings;
		if (binding->in_env) {
			callee = js_temp(fc);
			js_emit1(fc, VM_OP_LOAD_CALLEE, callee);
			js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, 0, binding->location, callee);
			fc->temp_top--;
		} else {
			js_emit1(fc, VM_OP_LOAD_CALLEE, binding->location);
		}
	}

	/* The let and const of the body cannot be used before their declarations run. */
	compile_lexicals_empty(fc, info->scope);

	/* The parameters that are defaults, patterns or a rest take their arguments. */
	compile_parameters(fc);

	/* A base class's constructor defines the instance fields first (a derived one after its super call). */
	if (info->class_node != NULL && info->class_node->first == NULL && (info->node->flags & JS_FLAG_STATIC_INIT) == 0U)
		js_compile_fields(fc, info->class_node, 0);

	/* The function declarations, made before anything runs. */
	compile_hoisted(fc);
}

/* Makes the function declarations of a function (the program's become globals, its vars too). */
static void
compile_hoisted(
	struct js_function_compiler *fc)
{
	struct js_function_info *info;
	struct js_hoisted *hoisted;
	struct js_global_name *global;
	struct js_global_lexical *lexical;
	struct vm_code *code;
	uint32_t mark;
	uint32_t closure;
	uint32_t constant;
	uint32_t key;

	/* Each declaration (not one its block makes): its closure, bound to its name. */
	info = fc->info;
	mark = fc->temp_top;
	for (hoisted = info->hoisted; hoisted != NULL; hoisted = hoisted->next) {
		if ((hoisted->node->flags & JS_FLAG_BLOCK_FUNCTION) != 0U)
			continue;
		code = js_compile_function(fc->compiler, fc, hoisted->node, NULL, 0);
		constant = js_constant(fc, vm_value_cell(code));
		closure = js_temp(fc);
		js_emit3(fc, VM_OP_NEW_CLOSURE, closure, constant, fc->env_register);

		/* The program's function is a global; a function's is its binding. */
		if (info->program && (info->node->flags & JS_FLAG_MODULE) == 0U) {
			key = js_constant_key(fc, hoisted->node->text, hoisted->node->text_length);
			js_emit2(fc, VM_OP_DEFINE_GLOBAL_FUNCTION, key, closure);
		} else {
			js_store_binding(fc, hoisted->node, hoisted->node->text, hoisted->node->text_length, closure);
		}

		/* The temporaries are free again. */
		fc->temp_top = mark;
	}

	/* The program's vars (a name a function took already keeps it). */
	for (global = info->global_vars; global != NULL; global = global->next) {
		key = js_constant_key(fc, global->name, global->length);
		js_emit1(fc, VM_OP_DEFINE_GLOBAL_VAR, key);
	}

	/* The program's let and const, in the realm's record, not usable before their declarations run. */
	for (lexical = info->global_lexicals; lexical != NULL; lexical = lexical->next) {
		key = js_constant_key(fc, lexical->name, lexical->length);
		js_emit2(fc, VM_OP_DEFINE_GLOBAL_LEXICAL, key, (uint32_t)lexical->is_const);
	}
}

/* Reports the flags of a function's code unit. */
static uint32_t
compile_flags(
	const struct js_function_info *info)
{
	uint32_t flags;

	/* Strict mode code. */
	flags = 0;
	if (info->strict)
		flags |= VM_CODE_STRICT;

	/* A function that uses its arguments object (a rest parameter reads it too). */
	if (info->arguments != NULL || info->rest != NULL)
		flags |= VM_CODE_ARGUMENTS;

	/* A default or a rest makes the length shorter than the parameters. */
	if (!info->simple_parameters)
		flags |= VM_CODE_LENGTH;

	/* A class's constructor (not its static function) is one only new may run, derived when the class extends. */
	if (info->class_node != NULL && (info->node->flags & JS_FLAG_STATIC_INIT) == 0U) {
		flags |= VM_CODE_CLASS | VM_CODE_CONSTRUCTOR;
		if (info->class_node->first != NULL)
			flags |= VM_CODE_DERIVED;
	}

	/* An ordinary function (not the program, not a method, an accessor, an arrow, a generator or an async function) can be called with new. */
	if (!info->program && (info->node->flags & (JS_FLAG_METHOD | JS_FLAG_ARROW | JS_FLAG_GENERATOR | JS_FLAG_ASYNC)) == 0U)
		flags |= VM_CODE_CONSTRUCTOR;

	/* A generator function, or an async function (ws074-p086). */
	if (!info->program && (info->node->flags & JS_FLAG_GENERATOR) != 0U)
		flags |= VM_CODE_GENERATOR;
	if (!info->program && (info->node->flags & JS_FLAG_ASYNC) != 0U)
		flags |= VM_CODE_ASYNC;

	/* Reports the flags. */
	return flags;
}

/* Compiles one statement. */
static void
compile_statement(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	uint32_t saved_line;
	uint32_t saved_column;

	/* The statement's instructions carry its source position. */
	js_emit_at(fc, node, &saved_line, &saved_column);

	/* Each kind of statement. */
	switch (node->kind) {
	case JS_NODE_IMPORT:
		/* A linked module prepared its immutable import slots before evaluation. */
		if (!fc->compiler->module_instance)
			js_compile_unsupported(fc->compiler, node, "modules");
		break;
	case JS_NODE_EXPORT:
		/* A named export declaration runs its declaration in this module's scope. */
		if (node->first != NULL && node->first->kind == JS_NODE_EXPORT_SPECIFIER && node->second == NULL &&
		    (node->flags & (JS_FLAG_DEFAULT | JS_FLAG_ALL)) == 0U)
			break;
		if (!js_export_is_declaration(node))
			js_compile_unsupported(fc->compiler, node, "this kind of module export");
		compile_statement(fc, node->first);
		break;
	case JS_NODE_VARIABLES:
		compile_variables(fc, node);
		break;
	case JS_NODE_FUNCTION_DECLARATION:
	case JS_NODE_EMPTY:
	case JS_NODE_DEBUGGER:
		/* A declaration was made by the prologue; the others do nothing. */
		break;
	case JS_NODE_BLOCK:
		compile_block(fc, node);
		break;
	case JS_NODE_EXPRESSION_STATEMENT:
		compile_expression_statement(fc, node);
		break;
	case JS_NODE_IF:
		compile_if(fc, node);
		break;
	case JS_NODE_FOR:
		compile_for(fc, node);
		break;
	case JS_NODE_FOR_IN:
		compile_for_in(fc, node);
		break;
	case JS_NODE_WHILE:
		compile_while(fc, node);
		break;
	case JS_NODE_DO_WHILE:
		compile_do_while(fc, node);
		break;
	case JS_NODE_CONTINUE:
	case JS_NODE_BREAK:
		compile_jump_statement(fc, node);
		break;
	case JS_NODE_RETURN:
		compile_return(fc, node);
		break;
	case JS_NODE_THROW:
		compile_throw(fc, node);
		break;
	case JS_NODE_SWITCH:
		compile_switch(fc, node);
		break;
	case JS_NODE_LABELED:
		compile_labeled(fc, node);
		break;
	case JS_NODE_TRY:
		compile_try(fc, node);
		break;
	case JS_NODE_CLASS_DECLARATION:
		compile_class_declaration(fc, node);
		break;
	case JS_NODE_FOR_OF:
		compile_for_of(fc, node);
		break;
	case JS_NODE_WITH:
		compile_with(fc, node);
		break;
	default:
		js_compile_unsupported(fc->compiler, node, "modules");
	}

	/* The enclosing statement's later instructions carry its own position again. */
	fc->line = saved_line;
	fc->column = saved_column;
}

/* Compiles sloppy-mode with: names in the body first search the value's properties. */
static void
compile_with(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_with active;
	uint32_t mark;

	/* The object remains in a temporary register for the whole body. */
	mark = fc->temp_top;
	active.object = js_temp(fc);
	js_compile_expression(fc, node->first, active.object);
	js_emit1(fc, VM_OP_CHECK_COERCIBLE, active.object);
	active.outer = fc->with;
	fc->with = &active;
	compile_statement(fc, node->fourth);
	fc->with = active.outer;
	fc->temp_top = mark;
}

/* Compiles a var statement: each declarator with an initializer assigns it. */
static void
compile_variables(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_node *declarator;
	struct js_node *target;
	uint32_t mark;
	uint32_t value;
	int mode;

	/* Each declarator: a name, which a let or const binds (undefined without an initializer), and a var assigns when it has an initializer. */
	mark = fc->temp_top;
	for (declarator = node->first; declarator != NULL; declarator = declarator->next) {
		target = declarator->first;

		/* A pattern takes the parts of its initializer (a var's, a let's or a const's). */
		if (target->kind != JS_NODE_IDENTIFIER) {
			if (declarator->second == NULL)
				continue;
			value = js_temp(fc);
			js_compile_expression(fc, declarator->second, value);
			mode = JS_BIND_VAR;
			if (node->op != JS_P_VAR)
				mode = JS_BIND_INIT;
			js_bind_pattern(fc, target, value, mode);
			fc->temp_top = mark;
			continue;
		}

		/* A let or const: its declaration gives the binding its first value. */
		if (node->op != JS_P_VAR) {
			value = js_temp(fc);
			if (declarator->second != NULL) {
				js_compile_expression_named(fc, declarator->second, value, target->text, target->text_length);
			} else {
				js_load_value(fc, value, VM_VALUE_UNDEFINED);
			}

			/* The binding takes it; the temporary is free again. */
			js_init_binding(fc, target->text, target->text_length, value);
			fc->temp_top = mark;
			continue;
		}

		/* A var without an initializer does nothing here. */
		if (declarator->second == NULL)
			continue;

		/* The initializer (an anonymous function takes the name), then the assignment. */
		value = js_temp(fc);
		js_compile_expression_named(fc, declarator->second, value, target->text, target->text_length);
		js_store_binding(fc, target, target->text, target->text_length, value);
		fc->temp_top = mark;
	}
}

/* Compiles an expression statement: the value is computed and dropped. */
static void
compile_expression_statement(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	uint32_t mark;
	uint32_t value;

	/* The expression into a temporary register given back after (the program keeps it as its completion value). */
	mark = fc->temp_top;
	value = js_temp(fc);
	js_compile_expression(fc, node->first, value);
	if (fc->info->program)
		js_emit2(fc, VM_OP_MOV, fc->completion_register, value);
	fc->temp_top = mark;
}

/* Compiles an if statement. */
static void
compile_if(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	uint32_t mark;
	uint32_t test;
	uint32_t otherwise;
	uint32_t end;

	/* The test, which skips the consequent when false. */
	mark = fc->temp_top;
	test = js_temp(fc);
	js_compile_expression(fc, node->first, test);
	fc->temp_top = mark;
	otherwise = js_label_new(fc);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, otherwise);

	/* The consequent, and without an alternate, the end. */
	compile_statement(fc, node->second);
	if (node->third == NULL) {
		js_label_place(fc, otherwise);
		return;
	}

	/* Around the alternate. */
	end = js_label_new(fc);
	js_emit_jump(fc, VM_OP_JUMP, 0, end);

	/* The alternate. */
	js_label_place(fc, otherwise);
	compile_statement(fc, node->third);
	js_label_place(fc, end);
}

/*
 * Pushes the target of a statement break can leave (with continue's label
 * for a loop), taking the labels written before it.
 */
static struct js_target *
compile_target_push(
	struct js_function_compiler *fc,
	int loop)
{
	struct js_target *target;

	/* The target in the arena. */
	target = wb_arena_zalloc(fc->compiler->arena, sizeof(*target));
	if (target == NULL)
		js_compile_out_of_memory(fc->compiler);
	target->outer = fc->targets;
	target->labels = fc->pending_labels;
	target->break_label = js_label_new(fc);
	target->continue_label = JS_LABEL_UNPLACED;
	if (loop)
		target->continue_label = js_label_new(fc);
	target->finally_depth = fc->finally_depth;
	target->unlabeled = 1;

	/* The labels are taken, and the target is the innermost now. */
	fc->pending_labels = NULL;
	fc->targets = target;

	/* Succeeded: the target. */
	return target;
}

/* Pops a target, placing its break label after the statement. */
static void
compile_target_pop(
	struct js_function_compiler *fc,
	struct js_target *target)
{
	/* A break lands after the statement. */
	js_label_place(fc, target->break_label);
	fc->targets = target->outer;
}

/* Compiles a for statement. */
static void
compile_for(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_target *target;
	struct js_scope *saved_scope;
	uint32_t saved_env;
	uint32_t mark;
	uint32_t value;
	uint32_t head;
	int declares;

	/* A let or const in the head has its scope around the whole statement. */
	saved_scope = fc->scope;
	saved_env = fc->env_register;
	if (node->scope != NULL)
		js_scope_enter(fc, node->scope, &saved_scope, &saved_env);

	/* The initialization: a declaration or an expression. */
	mark = fc->temp_top;
	target = compile_target_push(fc, 1);
	declares = 0;
	if (node->first != NULL && node->first->kind == JS_NODE_VARIABLES)
		declares = 1;
	if (declares) {
		compile_variables(fc, node->first);
	} else if (node->first != NULL) {
		value = js_temp(fc);
		js_compile_expression(fc, node->first, value);
		fc->temp_top = mark;
	}

	/* The head: the test leaves the loop when false. */
	head = js_label_new(fc);
	js_label_place(fc, head);
	js_emit0(fc, VM_OP_LOOP_HINT);
	if (node->second != NULL) {
		value = js_temp(fc);
		js_compile_expression(fc, node->second, value);
		js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, value, target->break_label);
		fc->temp_top = mark;
	}

	/* The body; continue goes to the update, which goes back to the head. */
	compile_statement(fc, node->fourth);
	js_label_place(fc, target->continue_label);

	/* Each run of the body has its own let bindings: the next run gets a copy of the environment. */
	if (node->scope != NULL && node->scope->has_env)
		compile_scope_renew(fc, node->scope, saved_env);

	/* The update. */
	if (node->third != NULL) {
		value = js_temp(fc);
		js_compile_expression(fc, node->third, value);
		fc->temp_top = mark;
	}

	/* Back to the head; break lands after, outside the head's scope. */
	js_emit_jump(fc, VM_OP_JUMP, 0, head);
	compile_target_pop(fc, target);
	if (node->scope != NULL)
		js_scope_leave(fc, saved_scope, saved_env);
}

/* Compiles a for-in statement: the body runs once for each enumerable key, assigned to the left side. */
static void
compile_for_in(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_target *target;
	struct js_node *left;
	struct js_node *declarator;
	struct js_scope *saved_scope;
	uint32_t saved_env;
	uint32_t mark;
	uint32_t object;
	uint32_t iterator;
	uint32_t key;
	uint32_t value;
	int lexical;

	/* The left side: a let's or const's name, a var's name (with Annex B's initializer run first), or a target. */
	mark = fc->temp_top;
	left = node->first;
	lexical = 0;
	if (left->kind == JS_NODE_VARIABLES) {
		if (left->op != JS_P_VAR)
			lexical = 1;
		declarator = left->first;
		left = declarator->first;
		if (declarator->second != NULL && !lexical && left->kind == JS_NODE_IDENTIFIER) {
			value = js_temp(fc);
			js_compile_expression(fc, declarator->second, value);
			js_store_binding(fc, left, left->text, left->text_length, value);
			fc->temp_top = mark;
		}
	}

	/* The object, and the iterator over its keys (held for the whole loop). */
	iterator = js_temp(fc);
	key = js_temp(fc);
	object = js_temp(fc);
	js_compile_expression(fc, node->second, object);
	js_emit2(fc, VM_OP_FOR_IN_START, iterator, object);

	/* A let or const on the left has its scope around the body. */
	saved_scope = fc->scope;
	saved_env = fc->env_register;
	if (node->scope != NULL)
		js_scope_enter(fc, node->scope, &saved_scope, &saved_env);

	/* The head: the next key, or out of the loop. */
	target = compile_target_push(fc, 1);
	js_label_place(fc, target->continue_label);
	js_emit0(fc, VM_OP_LOOP_HINT);
	js_emit_for_in_next(fc, key, iterator, target->break_label);

	/* Each run of the body has its own let or const binding: a new environment for it. */
	if (node->scope != NULL && node->scope->has_env)
		js_emit3(fc, VM_OP_NEW_ENV, node->scope->env_register, saved_env, node->scope->env_count);

	/* The key to the left side (the declaration of a let or const, the parts of a pattern), then the body, then the next key. */
	if (lexical) {
		js_bind_pattern(fc, left, key, JS_BIND_INIT);
	} else {
		js_store_target(fc, left, key);
	}

	/* The body, then back for the next key; break lands after, outside the left side's scope. */
	compile_statement(fc, node->fourth);
	js_emit_jump(fc, VM_OP_JUMP, 0, target->continue_label);
	compile_target_pop(fc, target);
	if (node->scope != NULL)
		js_scope_leave(fc, saved_scope, saved_env);
	fc->temp_top = mark;
}

/*
 * Compiles a for-of statement (ws074-p087): the body runs once for each
 * value of the iteration, assigned to the left side.  A way out of the
 * loop before the iteration ends (break, a return, a jump further out, an
 * exception) closes the iteration first: the body is protected like a try
 * statement's block whose finally block closes it.
 */
static void
compile_for_of(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_finally *handler;
	struct js_target *target;
	struct js_node *left;
	struct js_scope *saved_scope;
	uint32_t saved_env;
	uint32_t mark;
	uint32_t iterable;
	uint32_t iterator;
	uint32_t value;
	uint32_t start;
	uint32_t protected_end;
	uint32_t landing;
	uint32_t ended;
	uint32_t loud;
	int lexical;

	/* for await is an async iteration, which is not here yet. */
	if ((node->flags & JS_FLAG_AWAIT) != 0U)
		js_compile_unsupported(fc->compiler, node, "for await");

	/* The left side: a let's or const's name, a var's name, or a target. */
	mark = fc->temp_top;
	left = node->first;
	lexical = 0;
	if (left->kind == JS_NODE_VARIABLES) {
		if (left->op != JS_P_VAR)
			lexical = 1;
		left = left->first->first;
	}

	/* The iteration of the value (held for the whole loop). */
	iterator = js_temp(fc);
	value = js_temp(fc);
	iterable = js_temp(fc);
	js_compile_expression(fc, node->second, iterable);
	js_emit2(fc, VM_OP_ITER_START, iterator, iterable);

	/* A let or const on the left has its scope around the body. */
	saved_scope = fc->scope;
	saved_env = fc->env_register;
	if (node->scope != NULL)
		js_scope_enter(fc, node->scope, &saved_scope, &saved_env);

	/* The body is protected: its ways out go through the close, with a completion code as a finally block's. */
	handler = wb_arena_zalloc(fc->compiler->arena, sizeof(*handler));
	if (handler == NULL)
		js_compile_out_of_memory(fc->compiler);
	handler->outer = fc->finally;
	handler->entry_label = js_label_new(fc);
	handler->kind_register = js_temp(fc);
	handler->value_register = js_temp(fc);
	handler->next_code = JS_COMPLETION_JUMPS;
	fc->finally = handler;
	fc->finally_depth++;

	/* The head: the next value, or out of the loop when the iteration has ended. */
	ended = js_label_new(fc);
	target = compile_target_push(fc, 1);
	start = js_here(fc);
	js_label_place(fc, target->continue_label);
	js_emit0(fc, VM_OP_LOOP_HINT);
	js_emit_for_of_next(fc, value, iterator, ended);

	/* Each run of the body has its own let or const binding: a new environment for it. */
	if (node->scope != NULL && node->scope->has_env)
		js_emit3(fc, VM_OP_NEW_ENV, node->scope->env_register, saved_env, node->scope->env_count);

	/* The value to the left side, then the body, then the next value. */
	if (lexical) {
		js_bind_pattern(fc, left, value, JS_BIND_INIT);
	} else {
		js_store_target(fc, left, value);
	}

	/* The body, then back for the next value. */
	compile_statement(fc, node->fourth);
	js_emit_jump(fc, VM_OP_JUMP, 0, target->continue_label);
	protected_end = js_here(fc);

	/* The close is outside what it protects. */
	fc->finally = handler->outer;
	fc->finally_depth--;

	/* break of this loop closes the iteration and leaves. */
	compile_target_pop(fc, target);
	js_emit2(fc, VM_OP_ITER_CLOSE, iterator, 0);
	js_emit_jump(fc, VM_OP_JUMP, 0, ended);

	/* An exception from the body arrives with its value, completing by a throw. */
	landing = js_label_new(fc);
	js_label_place(fc, landing);
	js_emit_handler(fc, start, protected_end, landing, handler->value_register);
	js_emit2(fc, VM_OP_LOAD_INT, handler->kind_register, JS_COMPLETION_THROW);

	/* The close: quiet for an exception (it keeps its own), then where the completion goes. */
	js_label_place(fc, handler->entry_label);
	loud = js_label_new(fc);
	compile_dispatch_case(fc, handler->kind_register, JS_COMPLETION_THROW, loud);
	js_emit2(fc, VM_OP_ITER_CLOSE, iterator, 1);
	js_emit1(fc, VM_OP_THROW, handler->value_register);
	js_label_place(fc, loud);
	js_emit2(fc, VM_OP_ITER_CLOSE, iterator, 0);
	compile_finally_dispatch(fc, handler);

	/* The loop's end, outside the left side's scope. */
	js_label_place(fc, ended);
	if (node->scope != NULL)
		js_scope_leave(fc, saved_scope, saved_env);
	fc->temp_top = mark;
}

/* Compiles a while statement. */
static void
compile_while(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_target *target;
	uint32_t mark;
	uint32_t test;

	/* The head: the test leaves the loop when false; continue comes back here. */
	mark = fc->temp_top;
	target = compile_target_push(fc, 1);
	js_label_place(fc, target->continue_label);
	js_emit0(fc, VM_OP_LOOP_HINT);
	test = js_temp(fc);
	js_compile_expression(fc, node->first, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, target->break_label);
	fc->temp_top = mark;

	/* The body, then back to the head. */
	compile_statement(fc, node->fourth);
	js_emit_jump(fc, VM_OP_JUMP, 0, target->continue_label);
	compile_target_pop(fc, target);
}

/* Compiles a do-while statement. */
static void
compile_do_while(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_target *target;
	uint32_t mark;
	uint32_t test;
	uint32_t head;

	/* The body first. */
	mark = fc->temp_top;
	target = compile_target_push(fc, 1);
	head = js_label_new(fc);
	js_label_place(fc, head);
	js_emit0(fc, VM_OP_LOOP_HINT);
	compile_statement(fc, node->fourth);

	/* The test (continue comes here) goes back to the body when true. */
	js_label_place(fc, target->continue_label);
	test = js_temp(fc);
	js_compile_expression(fc, node->first, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_TRUE, test, head);
	fc->temp_top = mark;
	compile_target_pop(fc, target);
}

/* Compiles break and continue: a jump to the target the label (or the innermost loop or switch) names. */
static void
compile_jump_statement(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_target *target;
	int is_continue;
	int labeled;

	/* The target, from the innermost outwards. */
	is_continue = 0;
	if (node->kind == JS_NODE_CONTINUE)
		is_continue = 1;
	for (target = fc->targets; target != NULL; target = target->outer) {
		/* continue needs a loop. */
		if (is_continue && target->continue_label == JS_LABEL_UNPLACED)
			continue;

		/* A label names its statement; without one, the innermost loop or switch. */
		if (node->text != NULL) {
			labeled = compile_has_label(target, node->text, node->text_length);
			if (labeled)
				break;
		} else if (target->unlabeled) {
			break;
		}
	}

	/* The parser saw that the target exists. */
	if (target == NULL)
		js_compile_fail(fc->compiler, node, "internal error: no target for break or continue");

	/* The jump, through the finally blocks it leaves. */
	if (is_continue) {
		compile_jump_out(fc, target->continue_label, target->finally_depth);
	} else {
		compile_jump_out(fc, target->break_label, target->finally_depth);
	}
}

/* Jumps to a label outside the try statements whose finally blocks must run first. */
static void
compile_jump_out(
	struct js_function_compiler *fc,
	uint32_t label,
	uint32_t finally_depth)
{
	struct js_finally *handler;
	struct js_exit *exit;

	/* No finally block in between: the jump itself. */
	if (fc->finally_depth <= finally_depth) {
		js_emit_jump(fc, VM_OP_JUMP, 0, label);
		return;
	}

	/* The innermost finally block runs first, with the jump's completion code (one code for each place). */
	handler = fc->finally;
	for (exit = handler->exits; exit != NULL; exit = exit->next) {
		if (exit->label == label)
			break;
	}

	/* A place not seen before gets the next code. */
	if (exit == NULL) {
		exit = wb_arena_zalloc(fc->compiler->arena, sizeof(*exit));
		if (exit == NULL)
			js_compile_out_of_memory(fc->compiler);
		exit->code = handler->next_code;
		handler->next_code++;
		exit->label = label;
		exit->finally_depth = finally_depth;
		exit->next = handler->exits;
		handler->exits = exit;
	}

	/* The code, then the finally block. */
	js_emit2(fc, VM_OP_LOAD_INT, handler->kind_register, (uint32_t)exit->code);
	js_emit_jump(fc, VM_OP_JUMP, 0, handler->entry_label);
}

/* Tells whether a target has a label. */
static int
compile_has_label(
	const struct js_target *target,
	const uint16_t *name,
	size_t length)
{
	const struct js_label_name *label;
	int same;

	/* Each label written before the statement. */
	for (label = target->labels; label != NULL; label = label->next) {
		same = js_text_equal(label->name, label->length, name, length);
		if (same)
			return 1;
	}

	/* Not one of its labels. */
	return 0;
}

/* Compiles a return statement. */
static void
compile_return(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	uint32_t mark;
	uint32_t value;

	/* The value (undefined without one), then the return. */
	mark = fc->temp_top;
	value = js_temp(fc);
	if (node->first != NULL) {
		js_compile_expression(fc, node->first, value);
	} else {
		js_load_value(fc, value, VM_VALUE_UNDEFINED);
	}

	/* The return, through the finally blocks it leaves. */
	js_emit_return(fc, value);
	fc->temp_top = mark;
}

/* Compiles a throw statement. */
static void
compile_throw(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	uint32_t mark;
	uint32_t value;

	/* The value, thrown. */
	mark = fc->temp_top;
	value = js_temp(fc);
	js_compile_expression(fc, node->first, value);
	js_emit1(fc, VM_OP_THROW, value);
	fc->temp_top = mark;
}

/* Compiles a switch statement: the cases' tests in order, then the bodies, which fall through. */
static void
compile_switch(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_target *target;
	struct js_node *clause;
	struct js_scope *saved_scope;
	uint32_t saved_env;
	uint32_t mark;
	uint32_t discriminant;
	uint32_t test;
	uint32_t first_label;
	uint32_t label;
	uint32_t default_label;

	/* The discriminant, then the cases' scope when they declare a let or const. */
	mark = fc->temp_top;
	discriminant = js_temp(fc);
	js_compile_expression(fc, node->first, discriminant);
	saved_scope = fc->scope;
	saved_env = fc->env_register;
	if (node->scope != NULL)
		js_scope_enter(fc, node->scope, &saved_scope, &saved_env);
	target = compile_target_push(fc, 0);

	/* A label for each clause's body, made in order (their numbers follow each other). */
	first_label = JS_LABEL_UNPLACED;
	for (clause = node->second; clause != NULL; clause = clause->next) {
		label = js_label_new(fc);
		if (first_label == JS_LABEL_UNPLACED)
			first_label = label;
	}

	/* Each test compares strictly with the discriminant and jumps to its body. */
	test = js_temp(fc);
	default_label = target->break_label;
	label = first_label;
	for (clause = node->second; clause != NULL; clause = clause->next) {
		if (clause->first == NULL) {
			default_label = label;
		} else {
			js_compile_expression(fc, clause->first, test);
			js_emit3(fc, VM_OP_STRICT_EQ, test, discriminant, test);
			js_emit_jump(fc, VM_OP_JUMP_IF_TRUE, test, label);
		}

		/* The next clause's label. */
		label++;
	}

	/* No case matched: the default's body, or out. */
	js_emit_jump(fc, VM_OP_JUMP, 0, default_label);

	/* The bodies in order. */
	label = first_label;
	for (clause = node->second; clause != NULL; clause = clause->next) {
		js_label_place(fc, label);
		js_compile_statements(fc, clause->second);
		label++;
	}

	/* The end, where break lands, outside the cases' scope. */
	compile_target_pop(fc, target);
	if (node->scope != NULL)
		js_scope_leave(fc, saved_scope, saved_env);
	fc->temp_top = mark;
}

/* Compiles a labeled statement: a loop takes the labels; anything else is a target break can leave. */
static void
compile_labeled(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_label_name *label;
	struct js_target *target;
	struct js_node *body;

	/* The label joins those waiting for the statement. */
	label = wb_arena_zalloc(fc->compiler->arena, sizeof(*label));
	if (label == NULL)
		js_compile_out_of_memory(fc->compiler);
	label->name = node->text;
	label->length = node->text_length;
	label->next = fc->pending_labels;
	fc->pending_labels = label;

	/* A loop (or another label) takes them. */
	body = node->fourth;
	if (body->kind == JS_NODE_LABELED ||
	    body->kind == JS_NODE_FOR ||
	    body->kind == JS_NODE_FOR_IN ||
	    body->kind == JS_NODE_FOR_OF ||
	    body->kind == JS_NODE_WHILE ||
	    body->kind == JS_NODE_DO_WHILE) {
		compile_statement(fc, body);
		return;
	}

	/* Any other statement is a target only a labeled break leaves. */
	target = compile_target_push(fc, 0);
	target->unlabeled = 0;
	compile_statement(fc, body);
	compile_target_pop(fc, target);
}

/*
 * Compiles a try statement.  A catch clause is a handler of the protected
 * block; a finally block is a handler of both, and every way out of them
 * (falling through, an exception, a return, a jump) goes through it with a
 * completion code saying where to go after.
 */
static void
compile_try(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_finally *handler;
	uint32_t mark;
	uint32_t start;
	uint32_t protected_end;
	uint32_t after_catch;
	uint32_t finally_handler;

	/* With a finally block, the registers of its completion and the state of the jumps through it. */
	mark = fc->temp_top;
	handler = NULL;
	if (node->fourth != NULL) {
		handler = wb_arena_zalloc(fc->compiler->arena, sizeof(*handler));
		if (handler == NULL)
			js_compile_out_of_memory(fc->compiler);
		handler->outer = fc->finally;
		handler->entry_label = js_label_new(fc);
		handler->kind_register = js_temp(fc);
		handler->value_register = js_temp(fc);
		handler->next_code = JS_COMPLETION_JUMPS;
		fc->finally = handler;
		fc->finally_depth++;
	}

	/* The protected block. */
	start = js_here(fc);
	compile_statement(fc, node->first);

	/* The catch clause, around which the protected block's normal end jumps. */
	after_catch = js_label_new(fc);
	if (node->third != NULL)
		compile_catch(fc, node, start, after_catch);
	js_label_place(fc, after_catch);
	protected_end = js_here(fc);

	/* Without a finally block, that is all. */
	if (handler == NULL) {
		fc->temp_top = mark;
		return;
	}

	/* The finally block is outside what it protects. */
	fc->finally = handler->outer;
	fc->finally_depth--;

	/* The normal completion goes to the finally block. */
	js_emit2(fc, VM_OP_LOAD_INT, handler->kind_register, JS_COMPLETION_NORMAL);
	js_emit_jump(fc, VM_OP_JUMP, 0, handler->entry_label);

	/* An exception from the blocks it protects arrives with its value, completing by a throw. */
	finally_handler = js_label_new(fc);
	js_label_place(fc, finally_handler);
	js_emit_handler(fc, start, protected_end, finally_handler, handler->value_register);
	js_emit2(fc, VM_OP_LOAD_INT, handler->kind_register, JS_COMPLETION_THROW);

	/* The finally block, then where the completion goes. */
	js_label_place(fc, handler->entry_label);
	compile_statement(fc, node->fourth);
	compile_finally_dispatch(fc, handler);
	fc->temp_top = mark;
}

/* Compiles a catch clause: the handler of the protected block, which binds the exception and runs the clause. */
static void
compile_catch(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t start,
	uint32_t after)
{
	struct js_scope *outer;
	struct js_node *parameter;
	uint32_t end;
	uint32_t caught;
	uint32_t landing;
	uint32_t mark;

	/* The protected block's normal end skips the clause. */
	end = js_here(fc);
	js_emit_jump(fc, VM_OP_JUMP, 0, after);

	/* The handler of the protected block lands here with the exception in a register. */
	mark = fc->temp_top;
	caught = js_temp(fc);
	landing = js_label_new(fc);
	js_label_place(fc, landing);
	js_emit_handler(fc, start, end, landing, caught);

	/* The clause's scope, with its parameter bound to the exception. */
	outer = fc->scope;
	fc->scope = node->scope;
	parameter = node->second;
	if (parameter != NULL && parameter->kind != JS_NODE_IDENTIFIER)
		js_bind_pattern(fc, parameter, caught, JS_BIND_VAR);
	if (parameter != NULL && parameter->kind == JS_NODE_IDENTIFIER)
		js_store_binding(fc, parameter, parameter->text, parameter->text_length, caught);
	fc->temp_top = mark;

	/* The clause's block, then the scope around it again. */
	compile_statement(fc, node->third);
	fc->scope = outer;
}

/* Writes what follows a finally block: a throw, a return, or a jump by the completion code; the normal completion falls through. */
static void
compile_finally_dispatch(
	struct js_function_compiler *fc,
	struct js_finally *handler)
{
	struct js_exit *exit;
	uint32_t skip;

	/* A throw goes on with the exception. */
	skip = js_label_new(fc);
	compile_dispatch_case(fc, handler->kind_register, JS_COMPLETION_THROW, skip);
	js_emit1(fc, VM_OP_THROW, handler->value_register);
	js_label_place(fc, skip);

	/* A return goes on with the value (through the finally blocks further out). */
	skip = js_label_new(fc);
	compile_dispatch_case(fc, handler->kind_register, JS_COMPLETION_RETURN, skip);
	js_emit_return(fc, handler->value_register);
	js_label_place(fc, skip);

	/* Each jump goes on to its place. */
	for (exit = handler->exits; exit != NULL; exit = exit->next) {
		skip = js_label_new(fc);
		compile_dispatch_case(fc, handler->kind_register, exit->code, skip);
		compile_jump_out(fc, exit->label, exit->finally_depth);
		js_label_place(fc, skip);
	}
}

/* Writes the test of one completion code: the code that follows runs only for it. */
static void
compile_dispatch_case(
	struct js_function_compiler *fc,
	uint32_t kind_register,
	int code,
	uint32_t skip)
{
	uint32_t mark;
	uint32_t test;

	/* The completion code compared with the one wanted. */
	mark = fc->temp_top;
	test = js_temp(fc);
	js_emit2(fc, VM_OP_LOAD_INT, test, (uint32_t)code);
	js_emit3(fc, VM_OP_STRICT_EQ, test, kind_register, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, skip);
	fc->temp_top = mark;
}



/* Marks each let and const of a scope as not usable before its declaration runs (the empty value). */
static void
compile_lexicals_empty(
	struct js_function_compiler *fc,
	struct js_scope *scope)
{
	struct js_binding *binding;
	uint32_t mark;
	uint32_t empty;

	/* Each binding of the scope that is a let or const. */
	mark = fc->temp_top;
	for (binding = scope->bindings; binding != NULL; binding = binding->next) {
		if (binding->kind != JS_BINDING_LET && binding->kind != JS_BINDING_CONST)
			continue;

		/* The empty value in its slot of the scope's environment (the running one), or in its register. */
		if (binding->in_env) {
			empty = js_temp(fc);
			js_emit1(fc, VM_OP_LOAD_EMPTY, empty);
			js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, 0, binding->location, empty);
			fc->temp_top = mark;
		} else {
			js_emit1(fc, VM_OP_LOAD_EMPTY, binding->location);
		}
	}
}

/*
 * Gives the next run of a loop's body its own copy of the head's
 * environment (a for statement's let bindings, each run's own for the
 * closures made in it).
 */
static void
compile_scope_renew(
	struct js_function_compiler *fc,
	struct js_scope *scope,
	uint32_t outer_env)
{
	uint32_t mark;
	uint32_t copy;
	uint32_t value;
	uint32_t slot;

	/* A new environment on the one around the loop. */
	mark = fc->temp_top;
	copy = js_temp(fc);
	value = js_temp(fc);
	js_emit3(fc, VM_OP_NEW_ENV, copy, outer_env, scope->env_count);

	/* Each slot's value copied into it. */
	for (slot = 0; slot < scope->env_count; slot++) {
		js_emit4(fc, VM_OP_GET_ENV, value, scope->env_register, 0, slot);
		js_emit4(fc, VM_OP_PUT_ENV, copy, 0, slot, value);
	}

	/* The copy is the head's environment from now on. */
	js_emit2(fc, VM_OP_MOV, scope->env_register, copy);
	fc->temp_top = mark;
}

/* Compiles a block: its statements, in a scope of its own when it declares a let or const. */
static void
compile_block(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	struct js_scope *saved_scope;
	uint32_t saved_env;

	/* A block without a scope is its statements. */
	if (node->scope == NULL) {
		js_compile_statements(fc, node->first);
		return;
	}

	/* The scope around the statements. */
	js_scope_enter(fc, node->scope, &saved_scope, &saved_env);
	js_compile_statements(fc, node->first);
	js_scope_leave(fc, saved_scope, saved_env);
}

/*
 * Binds the parameters that are not plain names: a default or a pattern
 * from its argument's register, a rest from the arguments object.
 */
static void
compile_parameters(
	struct js_function_compiler *fc)
{
	struct js_function_info *info;
	struct js_node *parameter;
	uint32_t mark;
	uint32_t rest;
	uint32_t index;

	/* A list of plain names needs nothing. */
	info = fc->info;
	if (info->program || info->simple_parameters)
		return;

	/* Each parameter in order, the n-th argument in register n. */
	index = 0;
	mark = fc->temp_top;
	for (parameter = info->node->first; parameter != NULL; parameter = parameter->next) {
		/* A rest: the arguments from its place on. */
		if (parameter->kind == JS_NODE_REST) {
			rest = js_temp(fc);
			js_emit3(fc, VM_OP_ARGS_REST, rest, fc->arguments_register, index);
			js_bind_pattern(fc, parameter->first, rest, JS_BIND_VAR);
			fc->temp_top = mark;
			continue;
		}

		/* A default or a pattern takes its argument. */
		if (parameter->kind != JS_NODE_IDENTIFIER)
			js_bind_pattern(fc, parameter, index, JS_BIND_VAR);
		index++;
	}
}

/* Compiles a class declaration: the class, then its name's binding takes it. */
static void
compile_class_declaration(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	uint32_t mark;
	uint32_t value;

	/* The class, named after the declaration. */
	mark = fc->temp_top;
	value = js_temp(fc);
	js_compile_class(fc, node, value, node->text, node->text_length);

	/* The declaration of its name runs. */
	js_init_binding(fc, node->text, node->text_length, value);
	fc->temp_top = mark;
}
