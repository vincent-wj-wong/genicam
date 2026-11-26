/* GenICam GenApi 2.1.1 feature access and register mapping subset. */
#include "nodemap.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STRING_LIMIT 4096
#define MAX_EXACT_INTEGER INT64_C(9007199254740991)

struct reg_info {
	uint64_t address;
	size_t length;
	int little_endian;
};

static int read_int(struct gc_nodemap *map, struct node *node, unsigned int depth,
		    int64_t *value);
static int read_float(struct gc_nodemap *map, struct node *node, unsigned int depth,
		      double *value);
static int write_int(struct gc_nodemap *map, struct node *node, unsigned int depth,
		     int64_t value);
static int write_float(struct gc_nodemap *map, struct node *node,
		       unsigned int depth, double value);

static int parse_int(const char *text, int64_t *value)
{
	long long result;
	const char *digits;
	char *end;
	int base;

	if (!text || !*text)
		return GC_PROTOCOL;
	digits = *text == '-' || *text == '+' ? text + 1 : text;
	base = digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10;
	errno = 0;
	result = strtoll(text, &end, base);
	if (errno || *end || end == text)
		return GC_PROTOCOL;
	*value = (int64_t)result;
	return GC_OK;
}

static int parse_double(const char *text, double *value)
{
	double result;
	char *end;

	if (!text || !*text)
		return GC_PROTOCOL;
	errno = 0;
	result = strtod(text, &end);
	if (errno || *end || end == text || !isfinite(result))
		return GC_PROTOCOL;
	*value = result;
	return GC_OK;
}

static int integer_property(struct gc_nodemap *map, const struct node *node,
			    enum property_key literal, enum property_key reference,
			    unsigned int depth, int64_t *value)
{
	const char *name = gc_node_property(node, reference);
	const char *text;

	if (name) {
		struct node *target = gc_node_find(map, name);

		return target ? read_int(map, target, depth + 1, value) : GC_PROTOCOL;
	}
	text = gc_node_property(node, literal);
	return text ? parse_int(text, value) : GC_NOT_FOUND;
}

static int check_access(struct gc_nodemap *map, const struct node *node,
			unsigned int depth, int write)
{
	const char *flags[2];
	const char *name;
	int64_t value;
	unsigned int i;
	int rc;

	if (depth >= GENAPI_DEPTH)
		return GC_PROTOCOL;
	if (!node)
		return GC_NOT_FOUND;

	flags[0] = gc_node_property(node, PROP_ACCESS);
	flags[1] = gc_node_property(node, PROP_IMPOSED_ACCESS);
	for (i = 0; i < 2; i++) {
		if (!flags[i] || !strcmp(flags[i], "RW"))
			continue;
		if ((write && !strcmp(flags[i], "WO")) ||
		    (!write && !strcmp(flags[i], "RO")))
			continue;
		return GC_ACCESS;
	}
	for (i = PROP_IMPLEMENTED; i <= PROP_LOCKED; i++) {
		if (i == PROP_LOCKED && !write)
			break;
		name = gc_node_property(node, i);
		if (!name)
			continue;
		rc = read_int(map, gc_node_find(map, name), depth + 1, &value);
		if (rc)
			return rc;
		if ((i == PROP_LOCKED) == (value != 0))
			return GC_ACCESS;
	}
	return GC_OK;
}

static int get_reg_info(struct gc_nodemap *map, const struct node *node,
			unsigned int depth, struct reg_info *info)
{
	const char *endian = gc_node_property(node, PROP_ENDIAN);
	const char *port = gc_node_property(node, PROP_PORT);
	uint64_t address = 0;
	int64_t value;
	size_t i;
	int rc;

	if (gc_node_property(node, PROP_INDEX) || (port && strcmp(port, "Device")))
		return GC_UNSUPPORTED;
	for (i = 0; i < node->count; i++) {
		const struct property *property = &node->properties[i];

		if (property->key == PROP_ADDRESS)
			rc = parse_int(property->value, &value);
		else if (property->key == PROP_PADDRESS)
			rc = read_int(map, gc_node_find(map, property->value),
				      depth + 1, &value);
		else
			continue;
		if (rc)
			return rc;
		if (value < 0 || (uint64_t)value > UINT64_MAX - address)
			return GC_PROTOCOL;
		address += (uint64_t)value;
	}
	if (!gc_node_property(node, PROP_ADDRESS) &&
	    !gc_node_property(node, PROP_PADDRESS))
		return GC_PROTOCOL;
	rc = integer_property(map, node, PROP_LENGTH, PROP_PLENGTH, depth, &value);
	if (rc)
		return rc;
	if (value < 1 || value > STRING_LIMIT)
		return GC_PROTOCOL;
	info->address = address;
	info->length = (size_t)value;
	info->little_endian = endian && !strcmp(endian, "LittleEndian");
	return GC_OK;
}

static uint64_t get_bytes(const uint8_t *bytes, size_t size, int little)
{
	uint64_t result = 0;
	size_t i;

	for (i = 0; i < size; i++)
		result = result << 8 | bytes[little ? size - 1 - i : i];
	return result;
}

static void put_bytes(uint8_t *bytes, size_t size, int little, uint64_t value)
{
	size_t i;

	for (i = 0; i < size; i++) {
		bytes[little ? i : size - 1 - i] = (uint8_t)value;
		value >>= 8;
	}
}

static int reg_field(const struct node *node, const struct reg_info *info,
		     unsigned int *shift, uint64_t *mask)
{
	unsigned int bits = (unsigned int)info->length * 8;
	const char *bit = gc_node_property(node, PROP_BIT);
	int64_t msb;
	int64_t lsb;
	int rc;

	if (bits > 64)
		return GC_UNSUPPORTED;
	*shift = 0;
	*mask = UINT64_MAX >> (64 - bits);
	if (node->type != NODE_MASKED_REG)
		return GC_OK;

	if (bit) {
		rc = parse_int(bit, &lsb);
		if (rc)
			return rc;
		msb = lsb;
	} else {
		rc = parse_int(gc_node_property(node, PROP_LSB), &lsb);
		if (!rc)
			rc = parse_int(gc_node_property(node, PROP_MSB), &msb);
		if (rc)
			return rc;
	}
	if (lsb < 0 || msb < 0 || lsb >= bits || msb >= bits)
		return GC_PROTOCOL;
	if (!info->little_endian) {
		lsb = bits - 1 - lsb;
		msb = bits - 1 - msb;
	}
	if (lsb > msb)
		return GC_PROTOCOL;
	*shift = (unsigned int)lsb;
	*mask = UINT64_MAX >> (63 - (msb - lsb));
	return GC_OK;
}

static int signed_reg(const struct node *node)
{
	const char *sign = gc_node_property(node, PROP_SIGN);

	return sign && !strcmp(sign, "Signed");
}

static int read_int_reg(struct gc_nodemap *map, const struct node *node,
			unsigned int depth, int64_t *value)
{
	struct reg_info info;
	uint8_t data[8];
	uint64_t mask;
	uint64_t raw;
	unsigned int shift;
	int rc;

	rc = get_reg_info(map, node, depth, &info);
	if (!rc)
		rc = reg_field(node, &info, &shift, &mask);
	if (!rc)
		rc = gc_device_read(map->device, info.address, data, info.length);
	if (rc)
		return rc;
	raw = (get_bytes(data, info.length, info.little_endian) >> shift) & mask;
	if (signed_reg(node) && raw > mask / 2) {
		*value = -1 - (int64_t)(mask ^ raw);
		return GC_OK;
	}
	if (raw > INT64_MAX)
		return GC_UNSUPPORTED;
	*value = (int64_t)raw;
	return GC_OK;
}

static int write_int_reg(struct gc_nodemap *map, const struct node *node,
			 unsigned int depth, int64_t value)
{
	struct reg_info info;
	uint8_t data[8];
	uint64_t mask;
	uint64_t raw = 0;
	unsigned int shift;
	int rc;

	rc = get_reg_info(map, node, depth, &info);
	if (!rc)
		rc = reg_field(node, &info, &shift, &mask);
	if (rc)
		return rc;
	if (signed_reg(node)) {
		if (value > (int64_t)(mask / 2) || value < -1 - (int64_t)(mask / 2))
			return GC_INVALID;
	} else if (value < 0 || (uint64_t)value > mask) {
		return GC_INVALID;
	}

	if (node->type == NODE_MASKED_REG) {
		rc = gc_device_read(map->device, info.address, data, info.length);
		if (rc)
			return rc;
		raw = get_bytes(data, info.length, info.little_endian);
		raw &= ~(mask << shift);
	}
	raw |= ((uint64_t)value & mask) << shift;
	put_bytes(data, info.length, info.little_endian, raw);
	return gc_device_write(map->device, info.address, data, info.length);
}

static int bool_value(const struct node *node, int on, int64_t *value)
{
	const char *text = gc_node_property(node, on ? PROP_ON : PROP_OFF);

	*value = !!on;
	return text ? parse_int(text, value) : GC_OK;
}

struct formula_context {
	struct gc_nodemap *map;
	struct node *node;
	unsigned int depth;
	const char *argument;
	double input;
};

static int is_float_node(const struct node *node)
{
	return node->type == NODE_FLOAT || node->type == NODE_FLOAT_REG ||
	       node->type == NODE_CONVERTER || node->type == NODE_SWISS_KNIFE;
}

static int integer_formula(const struct node *node)
{
	return node->type == NODE_INT_CONVERTER || node->type == NODE_INT_SWISS_KNIFE;
}

static int read_number(struct gc_nodemap *map, struct node *node, unsigned int depth,
		       double *value)
{
	int64_t number;
	int rc;

	if (!node)
		return GC_PROTOCOL;
	if (is_float_node(node))
		return read_float(map, node, depth, value);
	rc = read_int(map, node, depth, &number);
	if (rc)
		return rc;
	if (number < -MAX_EXACT_INTEGER || number > MAX_EXACT_INTEGER)
		return GC_UNSUPPORTED;
	*value = (double)number;
	return GC_OK;
}

static int resolve_formula(void *context, const char *name, size_t length, double *value)
{
	struct formula_context *formula = context;
	const struct property *prop;
	size_t i;

	if (formula->argument && strlen(formula->argument) == length &&
	    !memcmp(formula->argument, name, length)) {
		*value = formula->input;
		return GC_OK;
	}
	for (i = 0; i < formula->node->count; i++) {
		prop = &formula->node->properties[i];
		if (!prop->name || strlen(prop->name) != length ||
		    memcmp(prop->name, name, length))
			continue;
		if (prop->key == PROP_CONSTANT)
			return parse_double(prop->value, value);
		if (prop->key == PROP_VARIABLE)
			return read_number(formula->map,
					   gc_node_find(formula->map, prop->value),
					   formula->depth + 1, value);
	}
	return GC_UNSUPPORTED;
}

static int read_formula(struct gc_nodemap *map, struct node *node,
			unsigned int depth, double *value)
{
	struct formula_context formula = { .map = map, .node = node, .depth = depth };
	enum property_key key = PROP_FORMULA;
	const char *source;
	int rc;

	if (node->type == NODE_CONVERTER || node->type == NODE_INT_CONVERTER) {
		source = gc_node_property(node, PROP_PVALUE);
		if (!source)
			return GC_PROTOCOL;
		rc = read_number(map, gc_node_find(map, source), depth + 1, &formula.input);
		if (rc)
			return rc;
		formula.argument = "TO";
		key = PROP_FORMULA_FROM;
	}
	return gc_eval_formula(gc_node_property(node, key), integer_formula(node),
			       resolve_formula, &formula, value);
}

static int write_formula(struct gc_nodemap *map, struct node *node,
			 unsigned int depth, double value)
{
	struct formula_context formula = {
		.map = map,
		.node = node,
		.depth = depth,
		.argument = "FROM",
		.input = value,
	};
	const char *name = gc_node_property(node, PROP_PVALUE);
	struct node *target;
	double result;
	int rc;

	if (!name)
		return GC_PROTOCOL;
	target = gc_node_find(map, name);
	if (!target)
		return GC_PROTOCOL;
	rc = gc_eval_formula(gc_node_property(node, PROP_FORMULA_TO),
			     integer_formula(node), resolve_formula, &formula, &result);
	if (rc)
		return rc;
	if (is_float_node(target))
		return write_float(map, target, depth + 1, result);
	if (result < -MAX_EXACT_INTEGER || result > MAX_EXACT_INTEGER)
		return GC_UNSUPPORTED;
	return write_int(map, target, depth + 1, (int64_t)result);
}

static int read_int(struct gc_nodemap *map, struct node *node, unsigned int depth,
		    int64_t *value)
{
	double result;
	int64_t on;
	int64_t off;
	int rc;

	rc = check_access(map, node, depth, 0);
	if (rc)
		return rc;
	switch (node->type) {
	case NODE_INT_REG:
	case NODE_MASKED_REG:
		return read_int_reg(map, node, depth, value);
	case NODE_INT_CONVERTER:
	case NODE_INT_SWISS_KNIFE:
		rc = read_formula(map, node, depth, &result);
		if (!rc)
			*value = (int64_t)result;
		return rc;
	case NODE_INTEGER:
	case NODE_ENUM:
	case NODE_ENTRY:
		return integer_property(map, node, PROP_VALUE, PROP_PVALUE, depth,
					value);
	case NODE_BOOLEAN:
		rc = integer_property(map, node, PROP_VALUE, PROP_PVALUE, depth,
				       value);
		if (!rc)
			rc = bool_value(node, 1, &on);
		if (!rc)
			rc = bool_value(node, 0, &off);
		if (rc)
			return rc;
		if (*value != on && *value != off)
			return GC_PROTOCOL;
		*value = *value == on;
		return GC_OK;
	default:
		return GC_UNSUPPORTED;
	}
}

static int check_int_bounds(struct gc_nodemap *map, const struct node *node,
			    unsigned int depth, int64_t value)
{
	int64_t min = 0;
	int64_t max;
	int64_t inc;
	int rc;

	rc = integer_property(map, node, PROP_MIN, PROP_PMIN, depth, &min);
	if (rc != GC_NOT_FOUND && rc)
		return rc;
	if (!rc && value < min)
		return GC_INVALID;
	rc = integer_property(map, node, PROP_MAX, PROP_PMAX, depth, &max);
	if (rc != GC_NOT_FOUND && rc)
		return rc;
	if (!rc && value > max)
		return GC_INVALID;
	rc = integer_property(map, node, PROP_INC, PROP_PINC, depth, &inc);
	if (rc != GC_NOT_FOUND && rc)
		return rc;
	if (!rc && inc > 1 && ((uint64_t)value - (uint64_t)min) % (uint64_t)inc)
		return GC_INVALID;
	return GC_OK;
}

static struct node *enum_entry(struct gc_nodemap *map, const struct node *node,
			       size_t *index)
{
	size_t parent = (size_t)(node - map->nodes) + 1;
	struct node *entry;
	size_t i;

	for (; *index < map->count; (*index)++) {
		entry = &map->nodes[*index];
		if (entry->parent == parent)
			return entry;
	}
	for (i = *index - map->count; i < node->count; i++, (*index)++) {
		if (node->properties[i].key != PROP_ENUM_ENTRY)
			continue;
		entry = gc_node_find(map, node->properties[i].value);
		if (entry && entry->type == NODE_ENTRY)
			return entry;
	}
	return NULL;
}

static struct node *find_enum(struct gc_nodemap *map, const struct node *node,
			      const char *name, int64_t value, unsigned int depth)
{
	struct node *entry;
	int64_t current;
	size_t index = 0;

	for (; (entry = enum_entry(map, node, &index)); index++) {
		if (read_int(map, entry, depth + 1, &current))
			continue;
		if (name ? !strcmp(name, entry->name) : current == value)
			return entry;
	}
	return NULL;
}

static int write_int(struct gc_nodemap *map, struct node *node, unsigned int depth,
		     int64_t value)
{
	const char *name;
	int rc;

	rc = check_access(map, node, depth, 1);
	if (rc)
		return rc;
	switch (node->type) {
	case NODE_INT_REG:
	case NODE_MASKED_REG:
		return write_int_reg(map, node, depth, value);
	case NODE_INT_CONVERTER:
		if (value < -MAX_EXACT_INTEGER || value > MAX_EXACT_INTEGER)
			return GC_UNSUPPORTED;
		return write_formula(map, node, depth, (double)value);
	case NODE_INTEGER:
		rc = check_int_bounds(map, node, depth, value);
		if (rc)
			return rc;
		break;
	case NODE_ENUM:
		if (!find_enum(map, node, NULL, value, depth))
			return GC_INVALID;
		break;
	case NODE_BOOLEAN:
		if (value != 0 && value != 1)
			return GC_INVALID;
		rc = bool_value(node, value != 0, &value);
		if (rc)
			return rc;
		break;
	default:
		return GC_UNSUPPORTED;
	}
	name = gc_node_property(node, PROP_PVALUE);
	return name ? write_int(map, gc_node_find(map, name), depth + 1, value) :
		      GC_ACCESS;
}

static int read_float(struct gc_nodemap *map, struct node *node, unsigned int depth,
		      double *value)
{
	struct reg_info info;
	uint8_t data[8];
	const char *name;
	uint64_t raw;
	uint32_t single;
	float number;
	int rc;

	rc = check_access(map, node, depth, 0);
	if (rc)
		return rc;
	if (node->type == NODE_CONVERTER || node->type == NODE_SWISS_KNIFE)
		return read_formula(map, node, depth, value);
	if (node->type == NODE_FLOAT) {
		name = gc_node_property(node, PROP_PVALUE);
		if (name)
			return read_float(map, gc_node_find(map, name), depth + 1, value);
		return parse_double(gc_node_property(node, PROP_VALUE), value);
	}
	if (node->type != NODE_FLOAT_REG)
		return GC_UNSUPPORTED;
	rc = get_reg_info(map, node, depth, &info);
	if (rc)
		return rc;
	if (info.length != 4 && info.length != 8)
		return GC_UNSUPPORTED;
	rc = gc_device_read(map->device, info.address, data, info.length);
	if (rc)
		return rc;
	raw = get_bytes(data, info.length, info.little_endian);
	if (info.length == 4) {
		single = (uint32_t)raw;
		memcpy(&number, &single, sizeof(single));
		*value = number;
	} else {
		memcpy(value, &raw, sizeof(raw));
	}
	return isfinite(*value) ? GC_OK : GC_PROTOCOL;
}

static int check_float_bounds(struct gc_nodemap *map, const struct node *node,
			      unsigned int depth, double value)
{
	const char *text;
	const char *name;
	double bound;
	unsigned int i;
	int rc;

	for (i = PROP_MIN; i <= PROP_MAX; i += 2) {
		name = gc_node_property(node, i + 1);
		text = gc_node_property(node, i);
		if (name)
			rc = read_number(map, gc_node_find(map, name), depth + 1, &bound);
		else if (text)
			rc = parse_double(text, &bound);
		else
			continue;
		if (rc)
			return rc;
		if ((i == PROP_MIN && value < bound) ||
		    (i == PROP_MAX && value > bound))
			return GC_INVALID;
	}
	return GC_OK;
}

static int write_float(struct gc_nodemap *map, struct node *node,
		       unsigned int depth, double value)
{
	struct reg_info info;
	uint8_t data[8];
	const char *name;
	uint64_t raw;
	uint32_t single;
	float number;
	int rc;

	rc = check_access(map, node, depth, 1);
	if (rc)
		return rc;
	if (node->type == NODE_CONVERTER)
		return write_formula(map, node, depth, value);
	if (node->type == NODE_FLOAT) {
		rc = check_float_bounds(map, node, depth, value);
		if (rc)
			return rc;
		name = gc_node_property(node, PROP_PVALUE);
		return name ? write_float(map, gc_node_find(map, name), depth + 1,
					  value) : GC_ACCESS;
	}
	if (node->type != NODE_FLOAT_REG)
		return GC_UNSUPPORTED;
	rc = get_reg_info(map, node, depth, &info);
	if (rc)
		return rc;
	if (info.length == 4) {
		number = (float)value;
		if (!isfinite(number))
			return GC_INVALID;
		memcpy(&single, &number, sizeof(single));
		raw = single;
	} else if (info.length == 8) {
		memcpy(&raw, &value, sizeof(raw));
	} else {
		return GC_UNSUPPORTED;
	}
	put_bytes(data, info.length, info.little_endian, raw);
	return gc_device_write(map->device, info.address, data, info.length);
}

int gc_feature_get_integer(struct gc_nodemap *map, const char *name,
			   int64_t *value)
{
	int rc;

	if (!map || !name || !value)
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	rc = read_int(map, gc_node_find(map, name), 0, value);
	osal_mutex_unlock(map->lock);
	return rc;
}

int gc_feature_set_integer(struct gc_nodemap *map, const char *name,
			   int64_t value)
{
	int rc;

	if (!map || !name)
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	rc = write_int(map, gc_node_find(map, name), 0, value);
	osal_mutex_unlock(map->lock);
	return rc;
}

int gc_feature_get_float(struct gc_nodemap *map, const char *name, double *value)
{
	int rc;

	if (!map || !name || !value)
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	rc = read_float(map, gc_node_find(map, name), 0, value);
	osal_mutex_unlock(map->lock);
	return rc;
}

int gc_feature_set_float(struct gc_nodemap *map, const char *name, double value)
{
	int rc;

	if (!map || !name || !isfinite(value))
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	rc = write_float(map, gc_node_find(map, name), 0, value);
	osal_mutex_unlock(map->lock);
	return rc;
}

static int copy_string(char *out, size_t capacity, const char *data, size_t length)
{
	if (length >= capacity)
		return GC_BUFFER_SMALL;
	memcpy(out, data, length);
	out[length] = '\0';
	return GC_OK;
}

static int string_target(struct gc_nodemap *map, const char *name, int write,
			  struct node **target, unsigned int *depth)
{
	struct node *node;
	int rc;

	for (*depth = 0; ; (*depth)++) {
		node = gc_node_find(map, name);
		rc = check_access(map, node, *depth, write);
		if (rc)
			return rc;
		if (node->type != NODE_STRING)
			break;
		name = gc_node_property(node, PROP_PVALUE);
		if (!name)
			break;
	}
	*target = node;
	return GC_OK;
}

int gc_feature_get_string(struct gc_nodemap *map, const char *name, char *value,
			  size_t capacity)
{
	char data[STRING_LIMIT];
	struct reg_info info;
	struct node *node;
	struct node *entry;
	const char *end;
	int64_t number;
	unsigned int depth;
	int rc;

	if (!map || !name || !value || !capacity)
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	rc = string_target(map, name, 0, &node, &depth);
	if (rc)
		goto out;
	if (node->type == NODE_ENUM) {
		rc = read_int(map, node, depth, &number);
		if (rc)
			goto out;
		entry = find_enum(map, node, NULL, number, depth);
		rc = entry ? copy_string(value, capacity, entry->name,
					 strlen(entry->name)) : GC_NOT_FOUND;
	} else if (node->type == NODE_STRING_REG) {
		rc = get_reg_info(map, node, depth, &info);
		if (!rc)
			rc = gc_device_read(map->device, info.address, data, info.length);
		if (rc)
			goto out;
		end = memchr(data, 0, info.length);
		rc = copy_string(value, capacity, data,
				 end ? (size_t)(end - data) : info.length);
	} else if (node->type == NODE_STRING) {
		end = gc_node_property(node, PROP_VALUE);
		rc = end ? copy_string(value, capacity, end, strlen(end)) : GC_PROTOCOL;
	} else {
		rc = GC_UNSUPPORTED;
	}
out:
	osal_mutex_unlock(map->lock);
	return rc;
}

int gc_feature_set_string(struct gc_nodemap *map, const char *name,
			  const char *value)
{
	char data[STRING_LIMIT] = { 0 };
	struct reg_info info;
	struct node *node;
	struct node *entry;
	int64_t number;
	size_t length;
	unsigned int depth;
	int rc;

	if (!map || !name || !value)
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	rc = string_target(map, name, 1, &node, &depth);
	if (rc)
		goto out;
	if (node->type == NODE_ENUM) {
		entry = find_enum(map, node, value, 0, depth);
		if (!entry) {
			rc = GC_NOT_FOUND;
			goto out;
		}
		rc = read_int(map, entry, depth + 1, &number);
		if (!rc)
			rc = write_int(map, node, depth, number);
	} else if (node->type == NODE_STRING_REG) {
		rc = get_reg_info(map, node, depth, &info);
		if (rc)
			goto out;
		length = strlen(value);
		if (length >= info.length) {
			rc = GC_BUFFER_SMALL;
			goto out;
		}
		memcpy(data, value, length);
		rc = gc_device_write(map->device, info.address, data, info.length);
	} else if (node->type == NODE_STRING) {
		rc = GC_ACCESS;
	} else {
		rc = GC_UNSUPPORTED;
	}
out:
	osal_mutex_unlock(map->lock);
	return rc;
}

int gc_feature_execute(struct gc_nodemap *map, const char *name)
{
	const char *ref;
	struct node *node;
	int64_t value;
	int rc;

	if (!map || !name)
		return GC_INVALID;
	osal_mutex_lock(map->lock);
	node = gc_node_find(map, name);
	rc = check_access(map, node, 0, 1);
	if (rc)
		goto out;
	if (node->type != NODE_COMMAND) {
		rc = GC_UNSUPPORTED;
		goto out;
	}
	ref = gc_node_property(node, PROP_PVALUE);
	if (!ref) {
		rc = GC_PROTOCOL;
		goto out;
	}
	rc = integer_property(map, node, PROP_COMMAND, PROP_PCOMMAND, 0, &value);
	if (!rc)
		rc = write_int(map, gc_node_find(map, ref), 1, value);
out:
	osal_mutex_unlock(map->lock);
	return rc;
}
