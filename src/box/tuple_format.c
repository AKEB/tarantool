/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "tuple_format.h"

/** Global table of tuple formats */
struct tuple_format **tuple_formats;
static intptr_t recycled_format_ids = FORMAT_ID_NIL;

static uint32_t formats_size = 0, formats_capacity = 0;

static const struct tuple_field tuple_field_default = {
	FIELD_TYPE_ANY, TUPLE_OFFSET_SLOT_NIL, false, ON_CONFLICT_ACTION_DEFAULT
};

/**
 * Extract all available type info from keys and field
 * definitions.
 */
static int
tuple_format_create(struct tuple_format *format, struct key_def * const *keys,
		    uint16_t key_count, const struct field_def *fields,
		    uint32_t field_count)
{
	if (format->field_count == 0) {
		format->field_map_size = 0;
		return 0;
	}
	/* Initialize defined fields */
	for (uint32_t i = 0; i < field_count; ++i) {
		format->fields[i].is_key_part = false;
		format->fields[i].type = fields[i].type;
		format->fields[i].offset_slot = TUPLE_OFFSET_SLOT_NIL;
		format->fields[i].nullable_action = fields[i].nullable_action;
		if (i + 1 > format->min_field_count && !fields[i].is_nullable)
			format->min_field_count = i + 1;
	}
	/* Initialize remaining fields */
	for (uint32_t i = field_count; i < format->field_count; i++)
		format->fields[i] = tuple_field_default;

	int current_slot = 0;

	/* extract field type info */
	for (uint16_t key_no = 0; key_no < key_count; ++key_no) {
		const struct key_def *key_def = keys[key_no];
		bool is_sequential = key_def_is_sequential(key_def);
		const struct key_part *part = key_def->parts;
		const struct key_part *parts_end = part + key_def->part_count;

		for (; part < parts_end; part++) {
			assert(part->fieldno < format->field_count);
			struct tuple_field *field =
				&format->fields[part->fieldno];
			if (part->fieldno >= field_count) {
				field->nullable_action = part->nullable_action;
			} else {
				if (tuple_field_is_nullable(field) !=
				    key_part_is_nullable(part)) {
					diag_set(ClientError,
						 ER_NULLABLE_MISMATCH,
						 part->fieldno +
						 TUPLE_INDEX_BASE,
						 tuple_field_is_nullable(field) ?
						 "nullable" : "not nullable",
						 key_part_is_nullable(part) ?
						 "nullable" : "not nullable");
					return -1;
				}

				if (field->nullable_action == ON_CONFLICT_ACTION_DEFAULT &&
				    !(part->nullable_action == ON_CONFLICT_ACTION_NONE ||
				      part->nullable_action == ON_CONFLICT_ACTION_DEFAULT))
					field->nullable_action = part->nullable_action;
				else {
					if (field->nullable_action != part->nullable_action &&
					    part->nullable_action != ON_CONFLICT_ACTION_DEFAULT) {
						int action_f = field->nullable_action;
						int action_p = part->nullable_action;
						diag_set(ClientError,
							 ER_ACTION_MISMATCH,
							 part->fieldno +
							 TUPLE_INDEX_BASE,
							 on_conflict_action_strs[action_f],
							 on_conflict_action_strs[action_p]);
						return -1;
					}
				}
			}

			/*
			 * Check that there are no conflicts
			 * between index part types and space
			 * fields.
			 */
			if (field->type == FIELD_TYPE_ANY) {
				field->type = part->type;
			} else if (field->type != part->type) {
				const char *name;
				int fieldno = part->fieldno + TUPLE_INDEX_BASE;
				if (part->fieldno >= field_count) {
					name = tt_sprintf("%d", fieldno);
				} else {
					const struct field_def *def =
						&fields[part->fieldno];
					name = tt_sprintf("'%s'", def->name);
				}
				int errcode;
				if (! field->is_key_part)
					errcode = ER_FORMAT_MISMATCH_INDEX_PART;
				else
					errcode = ER_INDEX_PART_TYPE_MISMATCH;
				diag_set(ClientError, errcode, name,
					 field_type_strs[field->type],
					 field_type_strs[part->type]);
				return -1;
			}
			field->is_key_part = true;
			/*
			 * In the tuple, store only offsets necessary
			 * to access fields of non-sequential keys.
			 * First field is always simply accessible,
			 * so we don't store an offset for it.
			 */
			if (field->offset_slot == TUPLE_OFFSET_SLOT_NIL &&
			    is_sequential == false && part->fieldno > 0) {

				field->offset_slot = --current_slot;
			}
		}
	}

	assert(format->fields[0].offset_slot == TUPLE_OFFSET_SLOT_NIL);
	size_t field_map_size = -current_slot * sizeof(uint32_t);
	if (field_map_size + format->extra_size > UINT16_MAX) {
		/** tuple->data_offset is 16 bits */
		diag_set(ClientError, ER_INDEX_FIELD_COUNT_LIMIT,
			 -current_slot);
		return -1;
	}
	format->field_map_size = field_map_size;
	return 0;
}

static int
tuple_format_register(struct tuple_format *format)
{
	if (recycled_format_ids != FORMAT_ID_NIL) {

		format->id = (uint16_t) recycled_format_ids;
		recycled_format_ids = (intptr_t) tuple_formats[recycled_format_ids];
	} else {
		if (formats_size == formats_capacity) {
			uint32_t new_capacity = formats_capacity ?
						formats_capacity * 2 : 16;
			struct tuple_format **formats;
			formats = (struct tuple_format **)
				realloc(tuple_formats, new_capacity *
						       sizeof(tuple_formats[0]));
			if (formats == NULL) {
				diag_set(OutOfMemory,
					 sizeof(struct tuple_format), "malloc",
					 "tuple_formats");
				return -1;
			}

			formats_capacity = new_capacity;
			tuple_formats = formats;
		}
		if (formats_size == FORMAT_ID_MAX + 1) {
			diag_set(ClientError, ER_TUPLE_FORMAT_LIMIT,
				 (unsigned) formats_capacity);
			return -1;
		}
		format->id = formats_size++;
	}
	tuple_formats[format->id] = format;
	return 0;
}

static void
tuple_format_deregister(struct tuple_format *format)
{
	if (format->id == FORMAT_ID_NIL)
		return;
	tuple_formats[format->id] = (struct tuple_format *) recycled_format_ids;
	recycled_format_ids = format->id;
	format->id = FORMAT_ID_NIL;
}

static struct tuple_format *
tuple_format_alloc(struct key_def * const *keys, uint16_t key_count,
		   uint32_t space_field_count, struct tuple_dictionary *dict)
{
	uint32_t index_field_count = 0;
	/* find max max field no */
	for (uint16_t key_no = 0; key_no < key_count; ++key_no) {
		const struct key_def *key_def = keys[key_no];
		const struct key_part *part = key_def->parts;
		const struct key_part *pend = part + key_def->part_count;
		for (; part < pend; part++) {
			index_field_count = MAX(index_field_count,
						part->fieldno + 1);
		}
	}
	uint32_t field_count = MAX(space_field_count, index_field_count);
	uint32_t total = sizeof(struct tuple_format) +
			 field_count * sizeof(struct tuple_field);

	struct tuple_format *format = (struct tuple_format *) malloc(total);
	if (format == NULL) {
		diag_set(OutOfMemory, sizeof(struct tuple_format), "malloc",
			 "tuple format");
		return NULL;
	}
	if (dict == NULL) {
		assert(space_field_count == 0);
		format->dict = tuple_dictionary_new(NULL, 0);
		if (format->dict == NULL) {
			free(format);
			return NULL;
		}
	} else {
		format->dict = dict;
		tuple_dictionary_ref(dict);
	}
	format->refs = 0;
	format->id = FORMAT_ID_NIL;
	format->field_count = field_count;
	format->index_field_count = index_field_count;
	format->exact_field_count = 0;
	format->min_field_count = index_field_count;
	return format;
}

/** Free tuple format resources, doesn't unregister. */
static inline void
tuple_format_destroy(struct tuple_format *format)
{
	tuple_dictionary_unref(format->dict);
}

void
tuple_format_delete(struct tuple_format *format)
{
	tuple_format_deregister(format);
	tuple_format_destroy(format);
	free(format);
}

struct tuple_format *
tuple_format_new(struct tuple_format_vtab *vtab, struct key_def * const *keys,
		 uint16_t key_count, uint16_t extra_size,
		 const struct field_def *space_fields,
		 uint32_t space_field_count, struct tuple_dictionary *dict)
{
	assert((dict == NULL && space_field_count == 0) ||
	       (dict != NULL && space_field_count == dict->name_count));
	struct tuple_format *format =
		tuple_format_alloc(keys, key_count, space_field_count, dict);
	if (format == NULL)
		return NULL;
	format->vtab = *vtab;
	format->extra_size = extra_size;
	if (tuple_format_register(format) < 0) {
		tuple_format_destroy(format);
		free(format);
		return NULL;
	}
	if (tuple_format_create(format, keys, key_count, space_fields,
				space_field_count) < 0) {
		tuple_format_delete(format);
		return NULL;
	}
	return format;
}

bool
tuple_format_eq(const struct tuple_format *a, const struct tuple_format *b)
{
	if (a->field_map_size != b->field_map_size ||
	    a->field_count != b->field_count)
		return false;
	for (uint32_t i = 0; i < a->field_count; ++i) {
		if (a->fields[i].type != b->fields[i].type ||
		    a->fields[i].offset_slot != b->fields[i].offset_slot)
			return false;
		if (a->fields[i].is_key_part != b->fields[i].is_key_part)
			return false;
		if (tuple_field_is_nullable(a->fields + i) !=
		    tuple_field_is_nullable(b->fields + i))
			return false;
	}
	return true;
}

struct tuple_format *
tuple_format_dup(struct tuple_format *src)
{
	uint32_t total = sizeof(struct tuple_format) +
			 src->field_count * sizeof(struct tuple_field);
	struct tuple_format *format = (struct tuple_format *) malloc(total);
	if (format == NULL) {
		diag_set(OutOfMemory, total, "malloc", "tuple format");
		return NULL;
	}
	memcpy(format, src, total);
	tuple_dictionary_ref(format->dict);
	format->id = FORMAT_ID_NIL;
	format->refs = 0;
	if (tuple_format_register(format) != 0) {
		tuple_format_destroy(format);
		free(format);
		return NULL;
	}
	return format;
}

/** @sa declaration for details. */
int
tuple_init_field_map(const struct tuple_format *format, uint32_t *field_map,
		     const char *tuple)
{
	if (format->field_count == 0)
		return 0; /* Nothing to initialize */

	const char *pos = tuple;

	/* Check to see if the tuple has a sufficient number of fields. */
	uint32_t field_count = mp_decode_array(&pos);
	if (format->exact_field_count > 0 &&
	    format->exact_field_count != field_count) {
		diag_set(ClientError, ER_EXACT_FIELD_COUNT,
			 (unsigned) field_count,
			 (unsigned) format->exact_field_count);
		return -1;
	}
	if (unlikely(field_count < format->min_field_count)) {
		diag_set(ClientError, ER_MIN_FIELD_COUNT,
			 (unsigned) field_count,
			 (unsigned) format->min_field_count);
		return -1;
	}

	/* first field is simply accessible, so we do not store offset to it */
	enum mp_type mp_type = mp_typeof(*pos);
	const struct tuple_field *field = &format->fields[0];
	if (key_mp_type_validate(field->type, mp_type, ER_FIELD_TYPE,
				 TUPLE_INDEX_BASE, tuple_field_is_nullable(field)))
		return -1;
	mp_next(&pos);
	/* other fields...*/
	++field;
	uint32_t i = 1;
	uint32_t defined_field_count = MIN(field_count, format->field_count);
	for (; i < defined_field_count; ++i, ++field) {
		mp_type = mp_typeof(*pos);
		if (key_mp_type_validate(field->type, mp_type, ER_FIELD_TYPE,
					 i + TUPLE_INDEX_BASE,
					 tuple_field_is_nullable(field)))
			return -1;
		if (field->offset_slot != TUPLE_OFFSET_SLOT_NIL) {
			field_map[field->offset_slot] =
				(uint32_t) (pos - tuple);
		}
		mp_next(&pos);
	}
	return 0;
}

/** Destroy tuple format subsystem and free resourses */
void
tuple_format_free()
{
	/* Clear recycled ids. */
	while (recycled_format_ids != FORMAT_ID_NIL) {
		uint16_t id = (uint16_t) recycled_format_ids;
		recycled_format_ids = (intptr_t) tuple_formats[id];
		tuple_formats[id] = NULL;
	}
	for (struct tuple_format **format = tuple_formats;
	     format < tuple_formats + formats_size; format++) {
		/* Do not unregister. Only free resources. */
		if (*format != NULL) {
			tuple_format_destroy(*format);
			free(*format);
		}
	}
	free(tuple_formats);
}

void
box_tuple_format_ref(box_tuple_format_t *format)
{
	tuple_format_ref(format);
}

void
box_tuple_format_unref(box_tuple_format_t *format)
{
	tuple_format_unref(format);
}
