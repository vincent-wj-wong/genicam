#ifndef GC_NODEMAP_INTERNAL_H
#define GC_NODEMAP_INTERNAL_H

#include "host/internal.h"
#include "osal/osal.h"

#define GENAPI_DEPTH 48

enum node_type {
	NODE_UNKNOWN,
	NODE_CATEGORY,
	NODE_INTEGER,
	NODE_FLOAT,
	NODE_BOOLEAN,
	NODE_ENUM,
	NODE_ENTRY,
	NODE_COMMAND,
	NODE_INT_REG,
	NODE_MASKED_REG,
	NODE_FLOAT_REG,
	NODE_STRING,
	NODE_STRING_REG,
	NODE_CONVERTER,
	NODE_INT_CONVERTER,
	NODE_SWISS_KNIFE,
	NODE_INT_SWISS_KNIFE,
};

enum property_key {
	PROP_UNKNOWN,
	PROP_VALUE,
	PROP_PVALUE,
	PROP_ADDRESS,
	PROP_PADDRESS,
	PROP_PORT,
	PROP_INDEX,
	PROP_LENGTH,
	PROP_PLENGTH,
	PROP_ACCESS,
	PROP_IMPOSED_ACCESS,
	PROP_ENDIAN,
	PROP_SIGN,
	PROP_MIN,
	PROP_PMIN,
	PROP_MAX,
	PROP_PMAX,
	PROP_INC,
	PROP_PINC,
	PROP_IMPLEMENTED,
	PROP_AVAILABLE,
	PROP_LOCKED,
	PROP_COMMAND,
	PROP_PCOMMAND,
	PROP_ON,
	PROP_OFF,
	PROP_ENUM_ENTRY,
	PROP_BIT,
	PROP_MSB,
	PROP_LSB,
	PROP_FORMULA,
	PROP_FORMULA_TO,
	PROP_FORMULA_FROM,
	PROP_VARIABLE,
	PROP_CONSTANT,
};

struct property {
	enum property_key key;
	char *name;
	char *value;
};

struct node {
	enum node_type type;
	char *name;
	size_t parent;
	struct property *properties;
	size_t count;
};

struct gc_nodemap {
	struct gc_device *device;
	struct osal_mutex *lock;
	struct node *nodes;
	size_t count;
	size_t capacity;
};

const char *gc_node_property(const struct node *node, enum property_key key);
struct node *gc_node_find(struct gc_nodemap *map, const char *name);
int gc_zip_xml(const uint8_t *zip, size_t size, void **xml, size_t *xml_size);
int gc_eval_formula(const char *text, int integer,
		    int (*resolve)(void *context, const char *name, size_t length,
				   double *value),
		    void *context, double *value);

#endif /* GC_NODEMAP_INTERNAL_H */
