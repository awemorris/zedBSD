/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI namespace: the tree of named objects, name resolution by the
 * rules of ACPI 6.5 section 5.3, and the walks and paths drivers use.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * How deep a path may nest, which bounds the text of a full path.
 */
#define NAMESPACE_DEPTH_MAX 64U

/*
 * One predefined name the namespace starts with.
 */
struct predefined_node {
	const char *name;
	enum drv_acpi_type type;
};

/*
 * The root of the namespace.
 *
 * drv_acpi_ns_init() creates it with the predefined names, and
 * drv_acpi_ns_reset() deletes the whole tree.  The interpreter lock
 * serializes every change to the tree.
 */
static struct drv_acpi_node *namespace_root;

/*
 * The names every namespace has before the first table is loaded, with the
 * types firmware finds them as.
 */
static const struct predefined_node predefined_nodes[] = {
	{ "_GPE", DRV_ACPI_TYPE_SCOPE },
	{ "_PR_", DRV_ACPI_TYPE_SCOPE },
	{ "_SB_", DRV_ACPI_TYPE_DEVICE },
	{ "_SI_", DRV_ACPI_TYPE_SCOPE },
	{ "_TZ_", DRV_ACPI_TYPE_DEVICE },
	{ "_REV", DRV_ACPI_TYPE_INTEGER },
	{ "_OS_", DRV_ACPI_TYPE_STRING },
	{ "_GL_", DRV_ACPI_TYPE_MUTEX },
};

static int predefined_create(const struct predefined_node *definition);
static struct drv_acpi_node *child_find(struct drv_acpi_node *parent, uint32_t segment);
static int walk_node(struct drv_acpi_node *node, unsigned depth, drv_acpi_walk_visitor_t visitor, void *argument);
static void segment_text(uint32_t segment, char *text);
static int ns_search_upward(struct drv_acpi_node *start, const struct drv_acpi_name *name, struct drv_acpi_node **result);

/*
 * Reports the root node of the namespace.
 */
struct drv_acpi_node *
drv_acpi_root(void)
{
	/* The root exists from drv_acpi_ns_init() on. */
	return namespace_root;
}

/*
 * Finds a node for a driver by a path in text, such as "\\_SB.PCI0".
 *
 * A relative path, such as "_STA", starts at scope, or at the root when
 * scope is NULL, and is not searched for toward the root: a driver asking
 * a device for _STA wants the device's own.
 */
int
drv_acpi_lookup(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_node **result)
{
	int error;

	/* Resolves the path without the search rule. */
	error = drv_acpi_lookup_path(scope, path, false, result);
	if (error != 0)
		return error;

	/* Succeeded: result names the node. */
	return 0;
}

/*
 * Finds a node by a path in text.
 *
 * With search set, a single relative name is searched for toward the root
 * as AML references are.
 */
int
drv_acpi_lookup_path(
	struct drv_acpi_node *scope,
	const char *path,
	bool search,
	struct drv_acpi_node **result)
{
	uint8_t segments[NAMESPACE_DEPTH_MAX * 4U];
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	int error;

	/* Refuses a lookup before the namespace exists. */
	if (namespace_root == NULL)
		return ENOENT;

	/* A missing scope means the root. */
	if (scope == NULL)
		scope = namespace_root;

	/* Converts the text into name segments. */
	error = drv_acpi_ns_parse_path(path, segments, sizeof(segments), &name);
	if (error != 0)
		return error;

	/* Resolves the name. */
	error = drv_acpi_ns_lookup(scope, &name, search, &node);
	if (error != 0)
		return error;

	/* Hands over the node; an alias is reported as the node it stands for. */
	*result = drv_acpi_ns_resolve_alias(node);

	/* Succeeded: result names the node. */
	return 0;
}

/*
 * Visits every node below a scope, parents before children.
 */
int
drv_acpi_walk(
	struct drv_acpi_node *scope,
	drv_acpi_walk_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_node *child;
	int decision;

	/* A missing scope means the root. */
	if (scope == NULL)
		scope = namespace_root;

	/* An empty namespace has nothing to visit. */
	if (scope == NULL)
		return 0;

	/* Visits each child subtree in creation order. */
	for (child = scope->child;
	     child != NULL;
	     child = child->next) {
		/* Visits one subtree and stops when the visitor asked to. */
		decision = walk_node(child, 0, visitor, argument);
		if (decision < 0)
			return decision;
	}

	/* Succeeded: every node was visited. */
	return 0;
}

/*
 * Writes the full path of a node, such as "\\_SB_.PCI0".
 */
int
drv_acpi_node_path(
	const struct drv_acpi_node *node,
	char *buffer,
	size_t size)
{
	const struct drv_acpi_node *chain[NAMESPACE_DEPTH_MAX];
	const struct drv_acpi_node *walk;
	unsigned depth;
	size_t used;
	char text[5];

	/* Refuses a buffer that cannot hold even the root. */
	if (size < 2)
		return ENOSPC;

	/* Collects the nodes from this one up to below the root. */
	depth = 0;
	for (walk = node;
	     walk != NULL && walk->parent != NULL;
	     walk = walk->parent) {
		/* Refuses a path deeper than the namespace allows. */
		if (depth == NAMESPACE_DEPTH_MAX)
			return ENOSPC;

		/* Records the node one level further up. */
		chain[depth] = walk;
		depth++;
	}

	/* Every path starts at the root. */
	buffer[0] = '\\';
	used = 1;

	/* Appends the segments from the top down, separated by dots. */
	while (depth != 0) {
		/* Refuses a buffer too small for the next segment and terminator. */
		depth--;
		if (used + 6U > size)
			return ENOSPC;

		/* Separates this segment from the one before it. */
		if (used != 1) {
			buffer[used] = '.';
			used++;
		}

		/* Copies the four characters of the segment. */
		segment_text(chain[depth]->name, text);
		kern_memcpy(buffer + used, text, 4);
		used += 4U;
	}

	/* Terminates the path. */
	buffer[used] = '\0';

	/* Succeeded: the buffer holds the full path. */
	return 0;
}

/*
 * Reports the type of the object a node holds.
 */
enum drv_acpi_type
drv_acpi_node_type(
	const struct drv_acpi_node *node)
{
	/* A node without an object is uninitialized. */
	if (node == NULL || node->object == NULL)
		return DRV_ACPI_TYPE_UNINITIALIZED;

	/* Reports the type of the node's object. */
	return (enum drv_acpi_type)node->object->type;
}

/*
 * Creates the root and the predefined names.
 */
int
drv_acpi_ns_init(void)
{
	struct drv_acpi_object *object;
	size_t index;
	int error;

	/* Does nothing when the namespace already exists. */
	if (namespace_root != NULL)
		return 0;

	/* Allocates the root node. */
	namespace_root = drv_acpi_os_alloc(sizeof(*namespace_root));
	if (namespace_root == NULL)
		return ENOMEM;

	/* Names the root, which has no parent and no children yet. */
	kern_memset(namespace_root, 0, sizeof(*namespace_root));
	namespace_root->name = drv_acpi_ns_segment((const uint8_t *)"\\___");

	/*
	 * Makes the root a scope for every name defined at the top level.  A
	 * failure deletes the half-made root, so that the next initialization
	 * starts again instead of finding a namespace that exists.
	 */
	object = drv_acpi_object_new(DRV_ACPI_TYPE_SCOPE);
	if (object == NULL) {
		drv_acpi_ns_reset();
		return ENOMEM;
	}

	/* The root takes its own reference to the scope object. */
	drv_acpi_ns_attach(namespace_root, object);
	drv_acpi_object_release(object);

	/* Creates each predefined name. */
	for (index = 0; index < sizeof(predefined_nodes) / sizeof(predefined_nodes[0]); index++) {
		/* Creates one name and its initial object; a failure deletes the half-made namespace. */
		error = predefined_create(&predefined_nodes[index]);
		if (error != 0) {
			drv_acpi_ns_reset();
			return error;
		}
	}

	/* Succeeded: tables can be loaded now. */
	return 0;
}

/*
 * Deletes the whole namespace.
 */
void
drv_acpi_ns_reset(void)
{
	struct drv_acpi_node *root;

	/* Nothing is left to delete once the root is gone. */
	if (namespace_root == NULL)
		return;

	/* Forgets the root first so that nothing resolves into the dying tree. */
	root = namespace_root;
	namespace_root = NULL;

	/* Deletes the tree. */
	drv_acpi_ns_delete(root);
}

/*
 * Packs four name characters into a segment value.
 */
uint32_t
drv_acpi_ns_segment(
	const uint8_t *bytes)
{
	uint32_t segment;

	/* The first character is the lowest byte, as in the AML. */
	segment = (uint32_t)bytes[0];
	segment |= (uint32_t)bytes[1] << 8;
	segment |= (uint32_t)bytes[2] << 16;
	segment |= (uint32_t)bytes[3] << 24;

	/* Reports the packed segment. */
	return segment;
}

/*
 * Resolves a NameString relative to a scope.
 *
 * With search set, a single name without prefixes is looked for in the
 * scope and then in each enclosing scope up to the root, as references
 * are.  Otherwise, and for every longer path, each segment must exist
 * below the one before it.
 */
int
drv_acpi_ns_lookup(
	struct drv_acpi_node *scope,
	const struct drv_acpi_name *name,
	bool search,
	struct drv_acpi_node **result)
{
	struct drv_acpi_node *start;
	struct drv_acpi_node *node;
	uint32_t segment;
	uint32_t index;
	int error;

	/* Starts at the root for an absolute path and at the scope otherwise. */
	start = scope;
	if (name->root)
		start = namespace_root;

	/* Climbs once for each parent prefix. */
	for (index = 0; index < name->parents; index++) {
		/* Refuses a parent prefix above the root. */
		if (start->parent == NULL)
			return ENOENT;

		/* Climbs one level. */
		start = start->parent;
	}

	/* A path of prefixes alone names the node they reached. */
	if (name->count == 0) {
		*result = start;
		return 0;
	}

	/* Searches toward the root for a single unprefixed name. */
	if (search &&
	    name->count == 1 &&
	    !name->root &&
	    name->parents == 0) {
		error = ns_search_upward(start, name, result);
		if (error != 0)
			return error;

		/* Succeeded: result names the node an enclosing scope has. */
		return 0;
	}

	/* Walks down the segments one level each. */
	node = start;
	for (index = 0; index < name->count; index++) {
		/* A path through an alias continues below the aliased node. */
		node = drv_acpi_ns_resolve_alias(node);

		/* Steps into the child with the next segment. */
		segment = drv_acpi_ns_segment(name->segments + index * 4U);
		node = child_find(node, segment);
		if (node == NULL)
			return ENOENT;
	}

	/* Hands over the node the last segment named. */
	*result = node;

	/* Succeeded: every segment existed. */
	return 0;
}

/*
 * Creates the node a definition names.
 *
 * Every segment but the last must already exist.  An existing node is
 * reported with EEXIST and returned so that the caller can decide.  A node
 * created while a method runs goes on that invocation's list and is
 * deleted with it.
 */
int
drv_acpi_ns_create(
	struct drv_acpi_eval *eval,
	const struct drv_acpi_name *name,
	struct drv_acpi_node **result)
{
	struct drv_acpi_name parent_name;
	struct drv_acpi_node *parent;
	struct drv_acpi_node *node;
	uint32_t segment;
	uint32_t owner;
	int error;

	/* A definition must name something. */
	if (name->count == 0)
		return EINVAL;

	/* Resolves the scope the last segment is created in. */
	parent_name = *name;
	parent_name.count = name->count - 1U;
	error = drv_acpi_ns_lookup(eval->scope, &parent_name, false, &parent);
	if (error != 0)
		return error;

	/* A scope named through an alias is the node the alias stands for. */
	parent = drv_acpi_ns_resolve_alias(parent);

	/* The table that runs the definition owns the node. */
	owner = DRV_ACPI_OWNER_PREDEFINED;
	if (eval->table != NULL)
		owner = eval->table->id;

	/* Creates the child, or reports the one that is there; any other failure leaves no node. */
	node = NULL;
	segment = drv_acpi_ns_segment(name->segments + parent_name.count * 4U);
	error = drv_acpi_ns_create_child(parent, segment, owner, &node);
	if (error == EEXIST) {
		*result = node;
		return error;
	}

	/* Reports a node that could not be created. */
	if (error != 0)
		return error;

	/* A node made by a method invocation lives as long as the invocation. */
	if (eval->frame != NULL) {
		node->temporary_next = eval->frame->created;
		eval->frame->created = node;
	}

	/* Hands over the new node. */
	*result = node;

	/* Succeeded: the caller attaches the object. */
	return 0;
}

/*
 * Creates one child node, or reports the existing one with EEXIST.
 */
int
drv_acpi_ns_create_child(
	struct drv_acpi_node *parent,
	uint32_t segment,
	uint32_t owner,
	struct drv_acpi_node **result)
{
	struct drv_acpi_node *node;

	/* Reports a child that already has the name. */
	node = child_find(parent, segment);
	if (node != NULL) {
		*result = node;
		return EEXIST;
	}

	/* Allocates the node. */
	node = drv_acpi_os_alloc(sizeof(*node));
	if (node == NULL)
		return ENOMEM;

	/* Names the node and links it to its parent and its owning table. */
	kern_memset(node, 0, sizeof(*node));
	node->name = segment;
	node->owner = owner;
	node->parent = parent;

	/* Appends it so that children stay in creation order. */
	if (parent->last_child != NULL) {
		parent->last_child->next = node;
	} else {
		parent->child = node;
	}

	/* The new node is the tail the next child is appended after. */
	parent->last_child = node;

	/* Hands over the new node. */
	*result = node;

	/* Succeeded: the node has no object yet. */
	return 0;
}

/*
 * Deletes a node and everything below it.
 */
void
drv_acpi_ns_delete(
	struct drv_acpi_node *node)
{
	struct drv_acpi_node *parent;
	struct drv_acpi_node *previous;
	struct drv_acpi_node *walk;
	struct drv_acpi_notify *notify;

	/* Deletes the children first, the oldest each time. */
	while (node->child != NULL)
		drv_acpi_ns_delete(node->child);

	/* Unlinks the node from its parent's list of children. */
	parent = node->parent;
	if (parent != NULL) {
		/* Finds the child before the node. */
		previous = NULL;
		for (walk = parent->child;
		     walk != NULL && walk != node;
		     walk = walk->next)
			previous = walk;

		/* Joins the neighbors around the node. */
		if (previous != NULL) {
			previous->next = node->next;
		} else {
			parent->child = node->next;
		}

		/* Moves the tail back when the node was the last child. */
		if (parent->last_child == node)
			parent->last_child = previous;
	}

	/* Frees the notification handlers installed on the node. */
	while (node->notify != NULL) {
		/* Unlinks the first handler and frees it. */
		notify = node->notify;
		node->notify = notify->next;
		drv_acpi_os_free(notify);
	}

	/* Releases the object. */
	drv_acpi_object_release(node->object);

	/* Frees the node. */
	drv_acpi_os_free(node);
}

/*
 * Makes an object the value of a node, replacing any value it had.
 */
void
drv_acpi_ns_attach(
	struct drv_acpi_node *node,
	struct drv_acpi_object *object)
{
	struct drv_acpi_object *old;

	/* The node takes its own reference to the new value. */
	if (object != NULL)
		drv_acpi_object_ref(object);

	/* Replaces the value. */
	old = node->object;
	node->object = object;

	/* Lets go of the old value. */
	drv_acpi_object_release(old);
}

/*
 * Follows an alias to the node it stands for.
 */
struct drv_acpi_node *
drv_acpi_ns_resolve_alias(
	struct drv_acpi_node *node)
{
	unsigned hops;

	/* Follows a bounded chain of aliases, which may point at aliases. */
	for (hops = 0; hops < NAMESPACE_DEPTH_MAX; hops++) {
		/* Stops at the first node that is not an alias. */
		if (node == NULL || node->object == NULL)
			return node;
		if (node->object->type != DRV_ACPI_TYPE_ALIAS)
			return node;

		/* Follows the alias one step. */
		node = node->object->value.alias.target;
	}

	/* Reports the node reached when the chain was too long. */
	return node;
}

/*
 * Converts a path in text into a NameString.
 *
 * The segments are written into the caller's storage; segments shorter
 * than four characters are padded with underscores.
 */
int
drv_acpi_ns_parse_path(
	const char *text,
	uint8_t *segments,
	size_t capacity,
	struct drv_acpi_name *name)
{
	size_t position;
	size_t length;
	uint8_t *segment;

	/* Starts with an empty relative name. */
	kern_memset(name, 0, sizeof(*name));
	name->segments = segments;

	/* Reads the root prefix or the parent prefixes. */
	position = 0;
	if (text[0] == '\\') {
		name->root = true;
		position = 1;
	} else {
		/* Counts each parent prefix. */
		while (text[position] == '^') {
			name->parents++;
			position++;
		}
	}

	/* Reads the dot-separated segments. */
	while (text[position] != '\0') {
		/* Refuses more segments than the storage holds. */
		if ((size_t)(name->count + 1U) * 4U > capacity)
			return ENOSPC;

		/* Finds the storage of the next segment. */
		segment = segments + name->count * 4U;

		/* Copies up to four characters of the segment. */
		length = 0;
		while (text[position] != '\0' && text[position] != '.') {
			/* Refuses a segment longer than four characters. */
			if (length == 4)
				return EINVAL;

			/* Copies one character. */
			segment[length] = (uint8_t)text[position];
			length++;
			position++;
		}

		/* Refuses an empty segment, as in "a..b". */
		if (length == 0)
			return EINVAL;

		/* Pads a short segment with underscores. */
		while (length < 4) {
			segment[length] = '_';
			length++;
		}

		/* Counts the finished segment. */
		name->count++;

		/* Steps over the separator before the next segment. */
		if (text[position] == '.')
			position++;
	}

	/* Succeeded: the name refers into the caller's storage. */
	return 0;
}

/*
 * Writes a NameString in text for messages.
 */
int
drv_acpi_ns_name_text(
	const struct drv_acpi_name *name,
	char *buffer,
	size_t size)
{
	size_t used;
	uint32_t index;

	/* Refuses a buffer too small for the prefixes and the terminator. */
	if (size < (size_t)name->parents + 2U)
		return ENOSPC;

	/* Writes the root prefix. */
	used = 0;
	if (name->root) {
		buffer[used] = '\\';
		used++;
	}

	/* Writes the parent prefixes. */
	for (index = 0; index < name->parents; index++) {
		buffer[used] = '^';
		used++;
	}

	/* Writes each segment, separated by dots. */
	for (index = 0; index < name->count; index++) {
		/* Refuses a buffer too small for the next segment. */
		if (used + 6U > size)
			return ENOSPC;

		/* Separates the segment from the one before it. */
		if (index != 0) {
			buffer[used] = '.';
			used++;
		}

		/* Copies the four characters. */
		kern_memcpy(buffer + used, name->segments + index * 4U, 4);
		used += 4U;
	}

	/* Terminates the text. */
	buffer[used] = '\0';

	/* Succeeded: the buffer holds the name. */
	return 0;
}

/* Creates one predefined name with its initial value. */
static int
predefined_create(
	const struct predefined_node *definition)
{
	struct drv_acpi_object *object;
	struct drv_acpi_node *node;
	uint32_t segment;
	int error;

	/* Creates the node below the root. */
	segment = drv_acpi_ns_segment((const uint8_t *)definition->name);
	error = drv_acpi_ns_create_child(namespace_root, segment, DRV_ACPI_OWNER_PREDEFINED, &node);
	if (error != 0)
		return error;

	/* Chooses the initial value by the name's type. */
	switch (definition->type) {
	case DRV_ACPI_TYPE_INTEGER:
		/* _REV reports the ACPI revision firmware expects: 2. */
		object = drv_acpi_object_integer_new(2);
		break;
	case DRV_ACPI_TYPE_STRING:
		/* _OS_ names the operating system firmware tests for. */
		object = drv_acpi_object_string_new("Microsoft Windows NT");
		break;
	default:
		/* Every other predefined name starts as an empty object of its type. */
		object = drv_acpi_object_new(definition->type);
		break;
	}

	/* Reports a value that could not be allocated. */
	if (object == NULL)
		return ENOMEM;

	/* The node takes the value. */
	drv_acpi_ns_attach(node, object);
	drv_acpi_object_release(object);

	/* Succeeded: the predefined name exists. */
	return 0;
}

/* Finds the child of a node with one segment. */
static struct drv_acpi_node *
child_find(
	struct drv_acpi_node *parent,
	uint32_t segment)
{
	struct drv_acpi_node *child;

	/* Compares each child's segment in turn. */
	for (child = parent->child;
	     child != NULL;
	     child = child->next) {
		/* Reports the child with the segment. */
		if (child->name == segment)
			return child;
	}

	/* Reports that no child has the segment. */
	return NULL;
}

/* Visits one node and, unless the visitor declined, its subtree. */
static int
walk_node(
	struct drv_acpi_node *node,
	unsigned depth,
	drv_acpi_walk_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_node *child;
	struct drv_acpi_node *next;
	int decision;

	/* Lets the visitor see the node first. */
	decision = visitor(node, depth, argument);
	if (decision != 0)
		return decision;

	/* Visits the children; the next one is read first so a visitor may delete. */
	for (child = node->child;
	     child != NULL;
	     child = next) {
		/* Visits one subtree and stops when the visitor asked to. */
		next = child->next;
		decision = walk_node(child, depth + 1U, visitor, argument);
		if (decision < 0)
			return decision;
	}

	/* Succeeded: the subtree was visited. */
	return 0;
}

/* Unpacks a segment into four characters and a terminator. */
static void
segment_text(
	uint32_t segment,
	char *text)
{
	/* Takes the characters from the lowest byte up. */
	text[0] = (char)(segment & 0xffU);
	text[1] = (char)((segment >> 8) & 0xffU);
	text[2] = (char)((segment >> 16) & 0xffU);
	text[3] = (char)((segment >> 24) & 0xffU);
	text[4] = '\0';
}

/* Searches a scope and then each enclosing scope for a single name. */
static int
ns_search_upward(
	struct drv_acpi_node *start,
	const struct drv_acpi_name *name,
	struct drv_acpi_node **result)
{
	struct drv_acpi_node *node;
	struct drv_acpi_node *found;
	uint32_t segment;

	/* Tries the scope and then each enclosing scope, stopping at the first that has the name. */
	segment = drv_acpi_ns_segment(name->segments);
	found = NULL;
	for (node = start;
	     node != NULL;
	     node = node->parent) {
		/* Stops at the first scope that has the name. */
		found = child_find(node, segment);
		if (found != NULL)
			break;
	}

	/* Reports a name no enclosing scope has. */
	if (found == NULL)
		return ENOENT;

	/* Hands over the node. */
	*result = found;

	/* Succeeded: result names the nearest node with the name. */
	return 0;
}
