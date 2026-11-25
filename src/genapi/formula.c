#include "nodemap.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EXACT_INTEGER 9007199254740991.0

struct expression {
	const char *text;
	int integer;
	int (*resolve)(void *context, const char *name, size_t length, double *value);
	void *context;
};

enum opcode {
	OP_OR,
	OP_XOR,
	OP_AND,
	OP_SHL,
	OP_SHR,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_MOD,
};

struct operation {
	const char *text;
	enum opcode code;
	unsigned int priority;
};

static const struct operation operators[] = {
	{ "<<", OP_SHL, 4 },
	{ ">>", OP_SHR, 4 },
	{ "|", OP_OR, 1 },
	{ "^", OP_XOR, 2 },
	{ "&", OP_AND, 3 },
	{ "+", OP_ADD, 5 },
	{ "-", OP_SUB, 5 },
	{ "*", OP_MUL, 6 },
	{ "/", OP_DIV, 6 },
	{ "%", OP_MOD, 6 },
};

static int parse_expression(struct expression *exp, unsigned int priority,
			    unsigned int depth, double *value);

static void skip_space(struct expression *exp)
{
	while (isspace((unsigned char)*exp->text))
		exp->text++;
}

static int exact_integer(double value)
{
	return value >= -MAX_EXACT_INTEGER && value <= MAX_EXACT_INTEGER &&
	       (double)(int64_t)value == value;
}

static int apply(enum opcode op, double left, double right,
		 int integer, double *out)
{
	int64_t a;
	int64_t b;
	uint64_t bits;

	if ((op == OP_DIV || op == OP_MOD) && right == 0)
		return GC_INVALID;
	if (op >= OP_ADD) {
		switch (op) {
		case OP_ADD:
			*out = left + right;
			break;
		case OP_SUB:
			*out = left - right;
			break;
		case OP_MUL:
			*out = left * right;
			break;
		case OP_DIV:
			*out = integer ? (double)((int64_t)left / (int64_t)right) :
					left / right;
			break;
		case OP_MOD:
			*out = fmod(left, right);
			break;
		default:
			return GC_PROTOCOL;
		}
		return isfinite(*out) && (!integer || exact_integer(*out)) ?
		       GC_OK : GC_UNSUPPORTED;
	}

	if (!exact_integer(left) || !exact_integer(right))
		return GC_UNSUPPORTED;
	a = (int64_t)left;
	b = (int64_t)right;
	switch (op) {
	case OP_SHR:
	case OP_SHL:
		if (a < 0 || b < 0 || b >= 53)
			return GC_UNSUPPORTED;
		if (op == OP_SHL && (uint64_t)a > ((uint64_t)MAX_EXACT_INTEGER >> b))
			return GC_UNSUPPORTED;
		bits = op == OP_SHL ? (uint64_t)a << b : (uint64_t)a >> b;
		break;
	case OP_AND:
		bits = (uint64_t)a & (uint64_t)b;
		break;
	case OP_OR:
		bits = (uint64_t)a | (uint64_t)b;
		break;
	case OP_XOR:
		bits = (uint64_t)a ^ (uint64_t)b;
		break;
	default:
		return GC_PROTOCOL;
	}
	*out = bits <= INT64_MAX ? (double)bits : (double)(-1 - (int64_t)~bits);
	return exact_integer(*out) ? GC_OK : GC_UNSUPPORTED;
}

static int atom(struct expression *exp, unsigned int depth, double *value)
{
	const char *start;
	char *end;
	char sign;
	int rc;

	if (depth >= GENAPI_DEPTH)
		return GC_PROTOCOL;
	skip_space(exp);
	if (*exp->text == '(') {
		exp->text++;
		rc = parse_expression(exp, 1, depth + 1, value);
		if (rc)
			return rc;
		if (*exp->text != ')')
			return GC_PROTOCOL;
		exp->text++;
		return GC_OK;
	}
	sign = *exp->text;
	if (sign == '+' || sign == '-') {
		exp->text++;
		rc = atom(exp, depth + 1, value);
		if (!rc && sign == '-')
			*value = -*value;
		return rc;
	}

	start = exp->text;
	if (isalpha((unsigned char)*start) || *start == '_') {
		do {
			exp->text++;
		} while (isalnum((unsigned char)*exp->text) || *exp->text == '_');
		rc = exp->resolve(exp->context, start,
				  (size_t)(exp->text - start), value);
		if (rc)
			return rc;
	} else {
		errno = 0;
		if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X'))
			*value = (double)strtoull(start, &end, 16);
		else
			*value = strtod(start, &end);
		if (errno || end == start)
			return GC_PROTOCOL;
		exp->text = end;
	}
	return isfinite(*value) && (!exp->integer || exact_integer(*value)) ?
	       GC_OK : GC_UNSUPPORTED;
}

static const struct operation *next_operation(const char *text)
{
	const struct operation *op;
	size_t i;

	if (!strncmp(text, "&&", 2) || !strncmp(text, "||", 2) ||
	    !strncmp(text, "**", 2))
		return NULL;
	for (i = 0; i < ARRAY_SIZE(operators); i++) {
		op = &operators[i];
		if (!strncmp(text, op->text, strlen(op->text)))
			return op;
	}
	return NULL;
}

static int parse_expression(struct expression *exp, unsigned int priority,
			    unsigned int depth, double *value)
{
	const struct operation *op;
	double right;
	int rc;

	rc = atom(exp, depth, value);
	if (rc)
		return rc;
	for (;;) {
		skip_space(exp);
		op = next_operation(exp->text);
		if (!op || op->priority < priority)
			return GC_OK;
		exp->text += strlen(op->text);
		rc = parse_expression(exp, op->priority + 1, depth + 1, &right);
		if (rc)
			return rc;
		rc = apply(op->code, *value, right, exp->integer, value);
		if (rc)
			return rc;
	}
}

int gc_eval_formula(const char *text, int integer,
		    int (*resolve)(void *context, const char *name, size_t length,
				   double *value),
		    void *context, double *value)
{
	struct expression exp = {
		.text = text,
		.integer = integer,
		.resolve = resolve,
		.context = context,
	};
	int rc;

	if (!text)
		return GC_PROTOCOL;
	rc = parse_expression(&exp, 1, 0, value);
	if (rc)
		return rc;
	return *exp.text ? GC_UNSUPPORTED : GC_OK;
}
