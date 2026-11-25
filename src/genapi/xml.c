#include "nodemap.h"

#include <ctype.h>
#include <expat.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES	16384
#define MAX_PROPERTIES	64
#define MAX_NODE_NAME	255
#define MAX_FIELD_SIZE	4096

struct xml_state {
	XML_Parser parser;
	struct gc_nodemap *map;
	unsigned int depth;
	unsigned int node_depth;
	unsigned int property_depth;
	size_t current;
	int valid_root;
	int status;
	enum property_key key;
	char property_name[MAX_NODE_NAME + 1];
	char text[MAX_FIELD_SIZE + 1];
	size_t length;
};

static const char *const type_names[] = {
	[NODE_CATEGORY] = "Category",
	[NODE_INTEGER] = "Integer",
	[NODE_FLOAT] = "Float",
	[NODE_BOOLEAN] = "Boolean",
	[NODE_ENUM] = "Enumeration",
	[NODE_ENTRY] = "EnumEntry",
	[NODE_COMMAND] = "Command",
	[NODE_INT_REG] = "IntReg",
	[NODE_MASKED_REG] = "MaskedIntReg",
	[NODE_FLOAT_REG] = "FloatReg",
	[NODE_STRING_REG] = "StringReg",
	[NODE_CONVERTER] = "Converter",
	[NODE_INT_CONVERTER] = "IntConverter",
	[NODE_SWISS_KNIFE] = "SwissKnife",
	[NODE_INT_SWISS_KNIFE] = "IntSwissKnife",
};

static const char *const property_names[] = {
	[PROP_VALUE] = "Value",
	[PROP_PVALUE] = "pValue",
	[PROP_ADDRESS] = "Address",
	[PROP_PADDRESS] = "pAddress",
	[PROP_PORT] = "pPort",
	[PROP_INDEX] = "pIndex",
	[PROP_LENGTH] = "Length",
	[PROP_PLENGTH] = "pLength",
	[PROP_ACCESS] = "AccessMode",
	[PROP_IMPOSED_ACCESS] = "ImposedAccessMode",
	[PROP_ENDIAN] = "Endianess",
	[PROP_SIGN] = "Sign",
	[PROP_MIN] = "Min",
	[PROP_PMIN] = "pMin",
	[PROP_MAX] = "Max",
	[PROP_PMAX] = "pMax",
	[PROP_INC] = "Inc",
	[PROP_PINC] = "pInc",
	[PROP_IMPLEMENTED] = "pIsImplemented",
	[PROP_AVAILABLE] = "pIsAvailable",
	[PROP_LOCKED] = "pIsLocked",
	[PROP_COMMAND] = "CommandValue",
	[PROP_PCOMMAND] = "pCommandValue",
	[PROP_ON] = "OnValue",
	[PROP_OFF] = "OffValue",
	[PROP_ENUM_ENTRY] = "pEnumEntry",
	[PROP_BIT] = "Bit",
	[PROP_MSB] = "MSB",
	[PROP_LSB] = "LSB",
	[PROP_FORMULA] = "Formula",
	[PROP_FORMULA_TO] = "FormulaTo",
	[PROP_FORMULA_FROM] = "FormulaFrom",
	[PROP_VARIABLE] = "pVariable",
	[PROP_CONSTANT] = "Constant",
};

static const char *local_name(const char *name)
{
	const char *separator = strrchr(name, '|');

	return separator ? separator + 1 : name;
}

static unsigned int lookup(const char *name, const char *const *table,
			   size_t count)
{
	unsigned int i;

	for (i = 1; i < count; i++) {
		if (!strcmp(name, table[i]))
			return i;
	}
	return 0;
}

static const char *find_name(const XML_Char **attrs)
{
	while (*attrs) {
		if (!strcmp(local_name(attrs[0]), "Name"))
			return attrs[1];
		attrs += 2;
	}
	return NULL;
}

static void fail_xml(struct xml_state *state, int status)
{
	state->status = status;
	XML_StopParser(state->parser, XML_FALSE);
}

static char *duplicate_text(const char *text, size_t length)
{
	char *value = malloc(length + 1);

	if (value) {
		memcpy(value, text, length);
		value[length] = '\0';
	}
	return value;
}

static int add_node(struct gc_nodemap *map, const char *name,
		    enum node_type type, size_t parent)
{
	struct node *nodes;
	struct node *node;
	size_t capacity;
	size_t length;

	if (map->count == MAX_NODES)
		return GC_BUFFER_SMALL;
	if (!name)
		return GC_PROTOCOL;
	length = strlen(name);
	if (!length || length > MAX_NODE_NAME)
		return GC_PROTOCOL;
	if (map->count == map->capacity) {
		capacity = map->capacity ? map->capacity * 2 : 64;
		nodes = realloc(map->nodes, capacity * sizeof(*nodes));
		if (!nodes)
			return GC_NOMEM;
		map->nodes = nodes;
		map->capacity = capacity;
	}
	node = &map->nodes[map->count];
	memset(node, 0, sizeof(*node));
	node->name = duplicate_text(name, length);
	if (!node->name)
		return GC_NOMEM;
	node->type = type;
	node->parent = parent;
	map->count++;
	return GC_OK;
}

static int add_property(struct node *node, enum property_key key,
			const char *name, const char *value, size_t length)
{
	struct property *properties;
	struct property *entry;

	if (node->count == MAX_PROPERTIES)
		return GC_BUFFER_SMALL;
	properties = realloc(node->properties,
			     (node->count + 1) * sizeof(*properties));
	if (!properties)
		return GC_NOMEM;
	node->properties = properties;
	entry = &properties[node->count];
	entry->value = duplicate_text(value, length);
	if (!entry->value)
		return GC_NOMEM;
	entry->name = NULL;
	if (*name) {
		entry->name = duplicate_text(name, strlen(name));
		if (!entry->name) {
			free(entry->value);
			return GC_NOMEM;
		}
	}
	entry->key = key;
	node->count++;
	return GC_OK;
}

static void XMLCALL start(void *context, const XML_Char *element,
			  const XML_Char **attrs)
{
	struct xml_state *state = context;
	const char *name = local_name(element);
	const char *label;
	size_t length;
	size_t parent = 0;
	enum node_type type;
	int rc;

	if (++state->depth > GENAPI_DEPTH) {
		fail_xml(state, GC_PROTOCOL);
		return;
	}
	if (state->depth == 1) {
		state->valid_root = !strcmp(name, "RegisterDescription");
		if (!state->valid_root)
			fail_xml(state, GC_PROTOCOL);
		return;
	}
	if (state->depth == 2 ||
	    (state->depth == 3 && state->node_depth == 2 &&
	     state->map->nodes[state->current].type == NODE_ENUM &&
	     !strcmp(name, "EnumEntry"))) {
		type = lookup(name, type_names, ARRAY_SIZE(type_names));
		if (!type)
			return;
		if (state->depth == 3)
			parent = state->current + 1;
		rc = add_node(state->map, find_name(attrs), type, parent);
		if (rc) {
			fail_xml(state, rc);
			return;
		}
		state->current = state->map->count - 1;
		state->node_depth = state->depth;
		return;
	}

	if (!state->node_depth || state->depth != state->node_depth + 1)
		return;
	state->key = lookup(name, property_names, ARRAY_SIZE(property_names));
	if (state->key) {
		state->property_name[0] = '\0';
		if (state->key == PROP_VARIABLE || state->key == PROP_CONSTANT) {
			label = find_name(attrs);
			length = label ? strlen(label) : 0;
			if (!length || length > MAX_NODE_NAME) {
				fail_xml(state, GC_PROTOCOL);
				return;
			}
			memcpy(state->property_name, label, length + 1);
		}
		state->length = 0;
		state->property_depth = state->depth;
	}
}

static void XMLCALL text(void *context, const XML_Char *data, int length)
{
	struct xml_state *state = context;

	if (!state->property_depth || state->depth != state->property_depth)
		return;
	if ((size_t)length > MAX_FIELD_SIZE - state->length) {
		fail_xml(state, GC_BUFFER_SMALL);
		return;
	}
	memcpy(state->text + state->length, data, (size_t)length);
	state->length += (size_t)length;
}

static void XMLCALL end(void *context, const XML_Char *name)
{
	struct xml_state *state = context;
	struct node *node;
	const char *begin;
	const char *finish;
	int rc;

	(void)name;
	if (state->depth == state->property_depth) {
		begin = state->text;
		finish = begin + state->length;
		while (begin < finish && isspace((unsigned char)*begin))
			begin++;
		while (finish > begin && isspace((unsigned char)finish[-1]))
			finish--;
		rc = add_property(&state->map->nodes[state->current], state->key,
				  state->property_name, begin, (size_t)(finish - begin));
		state->property_depth = 0;
		if (rc)
			fail_xml(state, rc);
	}
	if (state->depth == state->node_depth) {
		node = &state->map->nodes[state->current];
		if (node->parent) {
			state->current = node->parent - 1;
			state->node_depth = 2;
		} else {
			state->node_depth = 0;
		}
	}
	state->depth--;
}

static void XMLCALL doctype(void *context, const XML_Char *name,
			   const XML_Char *sysid, const XML_Char *pubid,
			   int internal)
{
	(void)name;
	(void)sysid;
	(void)pubid;
	(void)internal;
	fail_xml(context, GC_PROTOCOL);
}

static int parse_xml(struct gc_nodemap *map, const void *xml, size_t size)
{
	struct xml_state state = { .map = map };
	int rc;

	state.parser = XML_ParserCreateNS(NULL, '|');
	if (!state.parser)
		return GC_NOMEM;
	XML_SetUserData(state.parser, &state);
	XML_SetElementHandler(state.parser, start, end);
	XML_SetCharacterDataHandler(state.parser, text);
	XML_SetStartDoctypeDeclHandler(state.parser, doctype);
	rc = XML_Parse(state.parser, xml, (int)size, XML_TRUE);
	XML_ParserFree(state.parser);
	if (state.status)
		return state.status;
	return rc == XML_STATUS_OK && state.valid_root ? GC_OK : GC_PROTOCOL;
}

const char *gc_node_property(const struct node *node, enum property_key key)
{
	size_t i;

	for (i = 0; i < node->count; i++) {
		if (node->properties[i].key == key)
			return node->properties[i].value;
	}
	return NULL;
}

struct node *gc_node_find(struct gc_nodemap *map, const char *name)
{
	size_t i;

	for (i = 0; i < map->count; i++) {
		if (!map->nodes[i].parent && !strcmp(map->nodes[i].name, name))
			return &map->nodes[i];
	}
	return NULL;
}

size_t gc_nodemap_count(const struct gc_nodemap *map)
{
	return map ? map->count : 0;
}

const char *gc_nodemap_name(const struct gc_nodemap *map, size_t index)
{
	return map && index < map->count ? map->nodes[index].name : NULL;
}

void gc_nodemap_close(struct gc_nodemap *map)
{
	size_t i;
	size_t j;

	if (!map)
		return;
	for (i = 0; i < map->count; i++) {
		free(map->nodes[i].name);
		for (j = 0; j < map->nodes[i].count; j++) {
			free(map->nodes[i].properties[j].name);
			free(map->nodes[i].properties[j].value);
		}
		free(map->nodes[i].properties);
	}
	free(map->nodes);
	osal_mutex_destroy(map->lock);
	free(map);
}

int gc_nodemap_open(struct gc_device *device, struct gc_nodemap **result)
{
	struct gc_nodemap *map;
	void *xml = NULL;
	void *bytes = NULL;
	size_t size = 0;
	int rc;

	if (!device || !result)
		return GC_INVALID;
	*result = NULL;
	map = calloc(1, sizeof(*map));
	if (!map)
		return GC_NOMEM;
	map->device = device;
	map->lock = osal_mutex_create();
	if (!map->lock) {
		free(map);
		return GC_NOMEM;
	}

	rc = device->ops->xml(device->context, &bytes, &size);
	if (rc)
		goto out;
	if (size >= 4 && !memcmp(bytes, "PK\003\004", 4)) {
		rc = gc_zip_xml(bytes, size, &xml, &size);
		if (rc)
			goto out;
	} else {
		xml = bytes;
		bytes = NULL;
	}
	while (size && !((const char *)xml)[size - 1])
		size--;
	rc = parse_xml(map, xml, size);
out:
	free(bytes);
	free(xml);
	if (rc)
		gc_nodemap_close(map);
	else
		*result = map;
	return rc;
}
