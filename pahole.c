/*
  Copyright (C) 2006 Mandriva Conectiva S.A.
  Copyright (C) 2006 Arnaldo Carvalho de Melo <acme@mandriva.com>
  Copyright (C) 2007-2008 Arnaldo Carvalho de Melo <acme@redhat.com>

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.
*/

#include <argp.h>
#include <assert.h>
#include <stdio.h>
#include <dwarf.h>
#include <search.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "dwarves_reorganize.h"
#include "dwarves.h"
#include "dutil.h"

static uint8_t class__include_anonymous;
static uint8_t class__include_nested_anonymous;
static uint8_t word_size, original_word_size;

static char *class__exclude_prefix;
static size_t class__exclude_prefix_len;

static char *class__include_prefix;
static size_t class__include_prefix_len;

static char *cu__exclude_prefix;
static size_t cu__exclude_prefix_len;

static char *decl_exclude_prefix;
static size_t decl_exclude_prefix_len;

static uint16_t nr_holes;
static uint16_t nr_bit_holes;
static uint16_t hole_size_ge;
static uint8_t show_packable;
static uint8_t global_verbose;
static uint8_t recursive;
static size_t cacheline_size;
static uint8_t find_containers;
static uint8_t find_pointers_in_structs;
static int reorganize;
static bool defined_in;
static int show_reorg_steps;
static char *class_name;
static char separator = '\t';
static Dwarf_Off class_dwarf_offset;

static struct conf_fprintf conf = {
	.emit_stats = 1,
};

static struct conf_load conf_load;

struct structure {
	struct list_head  node;
	strings_t 	  name;
	uint32_t	  nr_files;
	uint32_t	  nr_methods;
};

static struct structure *structure__new(strings_t name)
{
	struct structure *self = malloc(sizeof(*self));

	if (self != NULL) {
		self->name	 = name;
		self->nr_files   = 1;
		self->nr_methods = 0;
	}

	return self;
}

static void structure__delete(struct structure *self)
{
	free(self);
}

static void *structures__tree;
static LIST_HEAD(structures__list);

static int structure__compare(const void *a, const void *b)
{
	const strings_t *key = a;
	const struct structure *pos = b;

	return strings__cmp(strings, *key, pos->name);
}

static struct structure *structures__add(struct class *class)
{
	struct structure *str;
	strings_t *s = tsearch(&class->type.namespace.name, &structures__tree,
			       structure__compare);

	if (s == NULL)
		return NULL;

	/* Should not be there since we already did a structures__find before
	 * calling structures__add_named */
	assert(*s != class->type.namespace.name);

	str = structure__new(class->type.namespace.name);
	if (str == NULL)
		return NULL;

	/* Insert the new structure */
	*(struct structure **)s = str;

	/* For linear traversals */
	list_add_tail(&str->node, &structures__list);
	return str;
}

static struct structure *structures__find(strings_t name)
{
	struct structure *s = NULL;

	if (name) {
		struct structure **key = tfind(&name, &structures__tree, structure__compare);

		if (key != NULL)
			s = *key;
	}

	return s;
}

void void_structure__delete(void *structure)
{
	structure__delete(structure);
}

void structures__delete(void)
{
	tdestroy(structures__tree, void_structure__delete);
}

static void nr_definitions_formatter(struct structure *self)
{
	printf("%s%c%u\n", strings__ptr(strings, self->name), separator,
	       self->nr_files);
}

static void nr_members_formatter(struct class *self,
				 struct cu *cu __unused, uint16_t id __unused)
{
	printf("%s%c%u\n", class__name(self), separator,
	       class__nr_members(self));
}

static void nr_methods_formatter(struct structure *self)
{
	printf("%s%c%u\n", strings__ptr(strings, self->name), separator,
	       self->nr_methods);
}

static void size_formatter(struct class *self,
			   struct cu *cu __unused, uint16_t id __unused)
{
	printf("%s%c%d%c%u\n", class__name(self), separator,
	       class__size(self), separator, self->nr_holes);
}

static void class_name_len_formatter(struct class *self,
				     struct cu *cu __unused,
				     uint16_t id __unused)
{
	const char *name = class__name(self);
	printf("%s%c%zd\n", name, separator, strlen(name));
}

static void class_name_formatter(struct class *self,
				 struct cu *cu __unused, uint16_t id __unused)
{
	puts(class__name(self));
}

static void class_formatter(struct class *self, struct cu *cu, uint16_t id)
{
	struct tag *typedef_alias = NULL;
	struct tag *tag = class__tag(self);
	const char *name = class__name(self);

	if (name == NULL) {
		/*
		 * Find the first typedef for this struct, this is enough
		 * as if we optimize the struct all the typedefs will be
		 * affected.
		 */
		typedef_alias = cu__find_first_typedef_of_type(cu, id);
		/*
		 * If there is no typedefs for this anonymous struct it is
		 * found just inside another struct, and in this case it'll
		 * be printed when the type it is in is printed, but if
		 * the user still wants to see its statistics, just use
		 * --nested_anon_include.
		 */
		if (typedef_alias == NULL && !class__include_nested_anonymous)
			return;
	}

	if (typedef_alias != NULL) {
		struct type *tdef = tag__type(typedef_alias);

		conf.prefix = "typedef";
		conf.suffix = type__name(tdef);
	} else
		conf.prefix = conf.suffix = NULL;

	tag__fprintf(tag, cu, &conf, stdout);

	putchar('\n');
}

static void print_packable_info(struct class *c, struct cu *cu, uint16_t id)
{
	const struct tag *t = class__tag(c);
	const size_t orig_size = class__size(c);
	const size_t new_size = class__size(c->priv);
	const size_t savings = orig_size - new_size;
	const char *name = class__name(c);

	/* Anonymous struct? Try finding a typedef */
	if (name == NULL) {
		const struct tag *tdef =
		      cu__find_first_typedef_of_type(cu, id);

		if (tdef != NULL)
			name = class__name(tag__class(tdef));
	}
	if (name != NULL)
		printf("%s%c%zd%c%zd%c%zd\n",
		       name, separator,
		       orig_size, separator,
		       new_size, separator,
		       savings);
	else
		printf("%s(%d)%c%zd%c%zd%c%zd\n",
		       tag__decl_file(t, cu),
		       tag__decl_line(t, cu),
		       separator,
		       orig_size, separator,
		       new_size, separator,
		       savings);
}

static void (*stats_formatter)(struct structure *self) = NULL;

static void print_stats(void)
{
	struct structure *pos;

	list_for_each_entry(pos, &structures__list, node)
		stats_formatter(pos);
}

static struct class *class__filter(struct class *class, struct cu *cu,
				   uint16_t tag_id);

static void (*formatter)(struct class *self,
			 struct cu *cu, uint16_t id) = class_formatter;

static void print_classes(struct cu *cu)
{
	uint16_t id;
	struct class *pos;

	cu__for_each_struct(cu, id, pos) {
		if (pos->type.namespace.name == 0 &&
		    !(class__include_anonymous ||
		      class__include_nested_anonymous))
			continue;

		class__find_holes(pos, cu);

		if (!class__filter(pos, cu, id))
			continue;

		if (show_packable && !global_verbose)
			print_packable_info(pos, cu, id);
		else if (formatter != NULL)
			formatter(pos, cu, id);
		
		if (structures__add(pos) == NULL) {
			fprintf(stderr, "pahole: insufficient memory for "
				"processing %s, skipping it...\n", cu->name);
			return;
		}
	}
}

static struct cu *cu__filter(struct cu *cu)
{
	if (cu__exclude_prefix != NULL &&
	    (cu->name == NULL ||
	     strncmp(cu__exclude_prefix, cu->name,
		     cu__exclude_prefix_len) == 0))
		return NULL;

	return cu;
}

static int class__packable(struct class *self, const struct cu *cu)
{
 	struct class *clone;
	size_t savings;

	if (self->nr_holes == 0 && self->nr_bit_holes == 0)
		return 0;

 	clone = class__clone(self, NULL);
 	if (clone == NULL)
		return 0;
 	class__reorganize(clone, cu, 0, stdout);
	savings = class__size(self) - class__size(clone);
	if (savings != 0) {
		self->priv = clone;
		return 1;
	}
	class__delete(clone);
	return 0;
}

static struct class *class__filter(struct class *class, struct cu *cu,
				   uint16_t tag_id)
{
	struct tag *tag = class__tag(class);
	struct structure *str;
	const char *name;
	strings_t stname;

	if (!tag->top_level)
		return NULL;

	name = class__name(class);
	stname = class->type.namespace.name;

	if (class__is_declaration(class))
		return NULL;

	if (!class__include_anonymous && name == NULL)
		return NULL;

	if (class__exclude_prefix != NULL) {
		if (name == NULL) {
			const struct tag *tdef =
				cu__find_first_typedef_of_type(cu, tag_id);
			if (tdef != NULL) {
				struct class *c = tag__class(tdef);

				name = class__name(c);
				stname = c->type.namespace.name;
			}
		}
		if (name != NULL && strncmp(class__exclude_prefix, name,
					    class__exclude_prefix_len) == 0)
			return NULL;
	}

	if (class__include_prefix != NULL) {
		if (name == NULL) {
			const struct tag *tdef =
				cu__find_first_typedef_of_type(cu, tag_id);
			if (tdef != NULL) {
				struct class *c = tag__class(tdef);

				name = class__name(c);
				stname = c->type.namespace.name;
			}
		}
		if (name != NULL && strncmp(class__include_prefix, name,
					    class__include_prefix_len) != 0)
			return NULL;
	}


	if (decl_exclude_prefix != NULL &&
	    (!tag__decl_file(tag, cu) ||
	     strncmp(decl_exclude_prefix, tag__decl_file(tag, cu),
		     decl_exclude_prefix_len) == 0))
		return NULL;

	if (class->nr_holes < nr_holes ||
	    class->nr_bit_holes < nr_bit_holes ||
	    (hole_size_ge != 0 && !class__has_hole_ge(class, hole_size_ge)))
		return NULL;

	str = structures__find(stname);
	if (str != NULL) {
		str->nr_files++;
		return NULL;
	}

	if (show_packable && !class__packable(class, cu))
		return NULL;

	return class;
}

static strings_t long_int_str_t, long_unsigned_int_str_t;

static void union__find_new_size(struct tag *tag, struct cu *cu);

static void class__resize_LP(struct tag *tag, struct cu *cu)
{
	struct tag *tag_pos;
	struct class *self = tag__class(tag);
	size_t word_size_diff;
	size_t orig_size = self->type.size;

	if (tag__type(tag)->resized)
		return;

	tag__type(tag)->resized = 1;

	if (original_word_size > word_size)
		word_size_diff = original_word_size - word_size;
	else
		word_size_diff = word_size - original_word_size;

	type__for_each_tag(tag__type(tag), tag_pos) {
		struct tag *type;
		size_t diff = 0;
		size_t array_multiplier = 1;

		/* we want only data members, i.e. with byte_offset attr */
		if (tag_pos->tag != DW_TAG_member &&
		    tag_pos->tag != DW_TAG_inheritance)
		    	continue;

		type = cu__find_type_by_id(cu, tag_pos->type);
		tag__assert_search_result(type);
		if (type->tag == DW_TAG_array_type) {
			int i;
			for (i = 0; i < tag__array_type(type)->dimensions; ++i)
				array_multiplier *= tag__array_type(type)->nr_entries[i];

			type = cu__find_type_by_id(cu, type->type);
			tag__assert_search_result(type);
		}

		if (tag__is_typedef(type)) {
			type = tag__follow_typedef(type, cu);
			tag__assert_search_result(type);
		}

		switch (type->tag) {
		case DW_TAG_base_type: {
			struct base_type *bt = tag__base_type(type);

			if (bt->name != long_int_str_t &&
			    bt->name != long_unsigned_int_str_t)
				break;
			/* fallthru */
		}
		case DW_TAG_pointer_type:
			diff = word_size_diff;
			break;
		case DW_TAG_structure_type:
		case DW_TAG_union_type:
			if (tag__is_union(type))
				union__find_new_size(type, cu);
			else
				class__resize_LP(type, cu);
			diff = tag__type(type)->size_diff;
			break;
		}

		diff *= array_multiplier;

		if (diff != 0) {
			struct class_member *m = tag__class_member(tag_pos);
			if (original_word_size > word_size) {
				self->type.size -= diff;
				class__subtract_offsets_from(self, cu, m, diff);
			} else {
				self->type.size += diff;
				class__add_offsets_from(self, m, diff);
			}
		}
	}

	if (original_word_size > word_size)
		tag__type(tag)->size_diff = orig_size - self->type.size;
	else
		tag__type(tag)->size_diff = self->type.size - orig_size;

	class__find_holes(self, cu);
	class__fixup_alignment(self, cu);
}

static void union__find_new_size(struct tag *tag, struct cu *cu)
{
	struct tag *tag_pos;
	struct type *self = tag__type(tag);
	size_t max_size = 0;

	if (self->resized)
		return;

	self->resized = 1;

	type__for_each_tag(self, tag_pos) {
		struct tag *type;
		size_t size;

		/* we want only data members, i.e. with byte_offset attr */
		if (tag_pos->tag != DW_TAG_member &&
		    tag_pos->tag != DW_TAG_inheritance)
		    	continue;

		type = cu__find_type_by_id(cu, tag_pos->type);
		tag__assert_search_result(type);
		if (tag__is_typedef(type))
			type = tag__follow_typedef(type, cu);

		if (tag__is_union(type))
			union__find_new_size(type, cu);
		else if (tag__is_struct(type))
			class__resize_LP(type, cu);

		size = tag__size(type, cu);
		if (size > max_size)
			max_size = size;
	}

	if (max_size > self->size) 
		self->size_diff = max_size - self->size;
	else
		self->size_diff = self->size - max_size;

	self->size = max_size;
}

static int tag_fixup_word_size_iterator(struct tag *tag, struct cu *cu,
					void *cookie)
{
	if (tag__is_struct(tag) || tag__is_union(tag)) {
		struct tag *pos;

		namespace__for_each_tag(tag__namespace(tag), pos)
			tag_fixup_word_size_iterator(pos, cu, cookie);
	}

	switch (tag->tag) {
	case DW_TAG_base_type: {
		struct base_type *bt = tag__base_type(tag);

		/*
		 * This shouldn't happen, but at least on a tcp_ipv6.c
		 * built with GNU C 4.3.0 20080130 (Red Hat 4.3.0-0.7),
		 * one was found, so just bail out.
		 */
		if (!bt->name)
			return 0;

		if (bt->name == long_int_str_t ||
		    bt->name == long_unsigned_int_str_t)
			bt->bit_size = word_size * 8;
	}
		break;
	case DW_TAG_structure_type:
		class__resize_LP(tag, cu);
		break;
	case DW_TAG_union_type:
		union__find_new_size(tag, cu);
		break;
	}

	return 0;
}

static void cu_fixup_word_size_iterator(struct cu *cu)
{
	original_word_size = cu->addr_size;
	cu->addr_size = word_size;
	cu__for_each_tag(cu, tag_fixup_word_size_iterator, NULL, NULL);
}

static void cu__account_nr_methods(struct cu *self)
{
	struct function *pos_function;
	struct structure *str;
	uint32_t id;

	cu__for_each_function(self, id, pos_function) {
		struct class_member *pos;
		list_for_each_entry(pos, &pos_function->proto.parms, tag.node) {
			struct tag *type = cu__find_type_by_id(self, pos->tag.type);

			if (type == NULL || type->tag != DW_TAG_pointer_type)
				continue;

			type = cu__find_type_by_id(self, type->type);
			if (type == NULL || !tag__is_struct(type))
				continue;

			struct type *ctype = tag__type(type);
			if (ctype->namespace.name == 0)
				continue;

			str = structures__find(ctype->namespace.name);
			if (str == NULL) {
				struct class *class = tag__class(type);
				class__find_holes(class, self);

				if (!class__filter(class, self, 0))
					continue;
				
				str = structures__add(class);
				if (str == NULL) {
					fprintf(stderr, "pahole: insufficient memory for "
						"processing %s, skipping it...\n",
						self->name);
					return;
				}
			}
			++str->nr_methods;
		}
	}
}

static char tab[128];

static void print_structs_with_pointer_to(const struct cu *cu, uint16_t type)
{
	struct class *pos;
	struct class_member *pos_member;
	uint16_t id;

	cu__for_each_struct(cu, id, pos) {
		if (pos->type.namespace.name == 0)
			continue;

		type__for_each_member(&pos->type, pos_member) {
			struct tag *ctype = cu__find_type_by_id(cu, pos_member->tag.type);

			tag__assert_search_result(ctype);
			if (ctype->tag != DW_TAG_pointer_type || ctype->type != type)
				continue;

			if (structures__find(pos->type.namespace.name))
				break;

			if (structures__add(pos) == NULL) {
				fprintf(stderr, "pahole: insufficient memory for "
					"processing %s, skipping it...\n",
					cu->name);
				return;
			}
			printf("%s: %s\n", class__name(pos),
			       class_member__name(pos_member));
		}
	}
}

static void print_containers(const struct cu *cu, uint16_t type, int ident)
{
	struct class *pos;
	uint16_t id;

	cu__for_each_struct(cu, id, pos) {
		if (pos->type.namespace.name == 0)
			continue;

		const uint32_t n = type__nr_members_of_type(&pos->type, type);
		if (n == 0)
			continue;

		if (ident == 0) {
			if (structures__find(pos->type.namespace.name))
				continue;

			if (structures__add(pos) == NULL) {
				fprintf(stderr, "pahole: insufficient memory for "
					"processing %s, skipping it...\n",
					cu->name);
				return;
			}
		}

		printf("%.*s%s", ident * 2, tab, class__name(pos));
		if (global_verbose)
			printf(": %u", n);
		putchar('\n');
		if (recursive)
			print_containers(cu, id, ident + 1);
	}
}

/* Name and version of program.  */
ARGP_PROGRAM_VERSION_HOOK_DEF = dwarves_print_version;

static const struct argp_option pahole__options[] = {
	{
		.name = "bit_holes",
		.key  = 'B',
		.arg  = "NR_HOLES",
		.doc  = "Show only structs at least NR_HOLES bit holes"
	},
	{
		.name = "cacheline_size",
		.key  = 'c',
		.arg  = "SIZE",
		.doc  = "set cacheline size to SIZE"
	},
	{
		.name = "class_name",
		.key  = 'C',
		.arg  = "CLASS_NAME",
		.doc  = "Show just this class"
	},
	{
		.name = "find_pointers_to",
		.key  = 'f',
		.arg  = "CLASS_NAME",
		.doc  = "Find pointers to CLASS_NAME"
	},
	{
		.name = "contains",
		.key  = 'i',
		.arg  = "CLASS_NAME",
		.doc  = "Show classes that contains CLASS_NAME"
	},
	{
		.name = "show_decl_info",
		.key  = 'I',
		.doc  = "Show the file and line number where the tags were defined"
	},
	{
		.name = "holes",
		.key  = 'H',
		.arg  = "NR_HOLES",
		.doc  = "show only structs with at least NR_HOLES holes",
	},
	{
		.name = "hole_size_ge",
		.key  = 'z',
		.arg  = "HOLE_SIZE",
		.doc  = "show only structs with at least one hole greater "
			"or equal to HOLE_SIZE",
	},
	{
		.name = "packable",
		.key  = 'P',
		.doc  = "show only structs that has holes that can be packed",
	},
	{
		.name = "expand_types",
		.key  = 'E',
		.doc  = "expand class members",
	},
	{
		.name = "nr_members",
		.key  = 'n',
		.doc  = "show number of members",
	},
	{
		.name = "rel_offset",
		.key  = 'r',
		.doc  = "show relative offsets of members in inner structs"
	},
	{
		.name = "recursive",
		.key  = 'd',
		.doc  = "recursive mode, affects several other flags",
	},
	{
		.name = "reorganize",
		.key  = 'R',
		.doc  = "reorg struct trying to kill holes",
	},
	{
		.name = "show_reorg_steps",
		.key  = 'S',
		.doc  = "show the struct layout at each reorganization step",
	},
	{
		.name = "class_name_len",
		.key  = 'N',
		.doc  = "show size of classes",
	},
	{
		.name = "show_first_biggest_size_base_type_member",
		.key  = 'l',
		.doc  = "show first biggest size base_type member",
	},
	{
		.name = "nr_methods",
		.key  = 'm',
		.doc  = "show number of methods",
	},
	{
		.name = "show_only_data_members",
		.key  = 'M',
		.doc  = "show only the members that use space in the class layout",
	},
	{
		.name = "expand_pointers",
		.key  = 'p',
		.doc  = "expand class pointer members",
	},
	{
		.name = "sizes",
		.key  = 's',
		.doc  = "show size of classes",
	},
	{
		.name = "separator",
		.key  = 't',
		.arg  = "SEP",
		.doc  = "use SEP as the field separator",
	},
	{
		.name = "nr_definitions",
		.key  = 'T',
		.doc  = "show how many times struct was defined",
	},
	{
		.name = "dwarf_offset",
		.key  = 'O',
		.arg  = "OFFSET",
		.doc  = "Show tag with DWARF OFFSET",
	},
	{
		.name = "decl_exclude",
		.key  = 'D',
		.arg  = "PREFIX",
		.doc  = "exclude classes declared in files with PREFIX",
	},
	{
		.name = "exclude",
		.key  = 'x',
		.arg  = "PREFIX",
		.doc  = "exclude PREFIXed classes",
	},
	{
		.name = "prefix_filter",
		.key  = 'y',
		.arg  = "PREFIX",
		.doc  = "include PREFIXed classes",
	},
	{
		.name = "cu_exclude",
		.key  = 'X',
		.arg  = "PREFIX",
		.doc  = "exclude PREFIXed compilation units",
	},
	{
		.name = "anon_include",
		.key  = 'a',
		.doc  = "include anonymous classes",
	},
	{
		.name = "nested_anon_include",
		.key  = 'A',
		.doc  = "include nested (inside other structs) anonymous classes",
	},
	{
		.name = "quiet",
		.key  = 'q',
		.doc  = "be quieter",
	},
	{
		.name = "defined_in",
		.key  = 'u',
		.doc  = "show CUs where CLASS_NAME (-C) is defined",
	},
	{
		.name = "verbose",
		.key  = 'V',
		.doc  = "be verbose",
	},
	{
		.name = "word_size",
		.key  = 'w',
		.arg  = "WORD_SIZE",
		.doc  = "change the arch word size to WORD_SIZE"
	},
	{
		.name = NULL,
	}
};

static error_t pahole__options_parser(int key, char *arg,
				      struct argp_state *state)
{
	switch (key) {
	case ARGP_KEY_INIT:
		if (state->child_inputs != NULL)
			state->child_inputs[0] = state->input;
		break;
	case 'A': class__include_nested_anonymous = 1;	break;
	case 'a': class__include_anonymous = 1;		break;
	case 'B': nr_bit_holes = atoi(arg);		break;
	case 'C': class_name = arg;			break;
	case 'c': cacheline_size = atoi(arg);		break;
	case 'D': decl_exclude_prefix = arg;
		  decl_exclude_prefix_len = strlen(decl_exclude_prefix);
		  conf_load.extra_dbg_info = 1;		break;
	case 'd': recursive = 1;			break;
	case 'E': conf.expand_types = 1;		break;
	case 'f': find_pointers_in_structs = 1;
		  class_name = arg;			break;
	case 'H': nr_holes = atoi(arg);			break;
	case 'I': conf.show_decl_info = 1;
		  conf_load.extra_dbg_info = 1;		break;
	case 'i': find_containers = 1;
		  class_name = arg;			break;
	case 'l': conf.show_first_biggest_size_base_type_member = 1;	break;
	case 'M': conf.show_only_data_members = 1;	break;
	case 'm': stats_formatter = nr_methods_formatter; break;
	case 'N': formatter = class_name_len_formatter;	break;
	case 'n': formatter = nr_members_formatter;	break;
	case 'O': class_dwarf_offset = strtoul(arg, NULL, 0);
		  conf_load.extra_dbg_info = 1;		break;
	case 'P': show_packable	= 1;
		  conf_load.extra_dbg_info = 1;		break;
	case 'p': conf.expand_pointers = 1;		break;
	case 'q': conf.emit_stats = 0;
		  conf.suppress_comments = 1;
		  conf.suppress_offset_comment = 1;	break;
	case 'R': reorganize = 1;			break;
	case 'r': conf.rel_offset = 1;			break;
	case 'S': show_reorg_steps = 1;			break;
	case 's': formatter = size_formatter;		break;
	case 'T': stats_formatter = nr_definitions_formatter;
		  formatter = NULL;			break;
	case 't': separator = arg[0];			break;
	case 'u': defined_in = 1;			break;
	case 'V': global_verbose = 1;			break;
	case 'w': word_size = atoi(arg);		break;
	case 'X': cu__exclude_prefix = arg;
		  cu__exclude_prefix_len = strlen(cu__exclude_prefix);
							break;
	case 'x': class__exclude_prefix = arg;
		  class__exclude_prefix_len = strlen(class__exclude_prefix);
							break;
	case 'y': class__include_prefix = arg;
		  class__include_prefix_len = strlen(class__include_prefix);
							break;
	case 'z':
		hole_size_ge = atoi(arg);
		if (!global_verbose)
			formatter = class_name_formatter;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const char pahole__args_doc[] = "FILE";

static struct argp pahole__argp = {
	.options  = pahole__options,
	.parser	  = pahole__options_parser,
	.args_doc = pahole__args_doc,
};

static strings_t class_sname;
static struct tag *class;
static uint16_t class_id;

static enum load_steal_kind class_stealer(struct cu *cu)
{
	if (class_sname == 0) {
		class_sname = strings__find(strings, class_name);
		if (class_sname == 0)
			return LSK__STOLEN;
	}

	int include_decls = find_pointers_in_structs != 0 ||
			    stats_formatter == nr_methods_formatter;
	class = cu__find_struct_by_sname(cu, class_sname,
					 include_decls, &class_id);
	if (class == NULL)
		return LSK__STOLEN;

	class__find_holes(tag__class(class), cu);
	return LSK__STOP_LOADING;
}

static enum load_steal_kind pahole_stealer(struct cu *cu,
					   struct conf_load *conf_load __unused)
{
	if (!cu__filter(cu))
		goto dump_it;

	if (defined_in) {
		if (class_sname == 0)
			class_sname = strings__find(strings, class_name);
		
		if (cu__find_struct_by_sname(cu, class_sname, 0, NULL))
			puts(cu->name);

		goto dump_it;
	}

	if (class_name != NULL && class_stealer(cu) == LSK__STOLEN)
		goto dump_it;

	if (stats_formatter == nr_methods_formatter) {
		cu__account_nr_methods(cu);
		goto dump_it;
	}

	if (word_size != 0) {
		if (long_int_str_t == 0 || long_unsigned_int_str_t == 0) {
			long_int_str_t = strings__find(strings, "long int"),
			long_unsigned_int_str_t =
					 strings__find(strings, "long unsigned int");

			if (long_int_str_t == 0 || long_unsigned_int_str_t == 0) {
				fputs("pahole: couldn't find one of \"long int\" or "
				      "\"long unsigned int\" types", stderr);
				exit(EXIT_FAILURE);
			}
		}

		cu_fixup_word_size_iterator(cu);
	}

	if (class_dwarf_offset != 0) {
		struct tag *tag = cu__find_tag_by_id(cu, class_dwarf_offset);
		if (tag == NULL) {
			fprintf(stderr, "id %llx not found!\n",
				(unsigned long long)class_dwarf_offset);
			return EXIT_FAILURE;
		}

 		tag__fprintf(tag, cu, &conf, stdout);
		putchar('\n');
		cu__delete(cu);
		return LSK__STOP_LOADING;
	}

	memset(tab, ' ', sizeof(tab) - 1);

	if (class == NULL) {
		print_classes(cu);
		goto dump_it;
	}

 	if (reorganize) {
		size_t savings;
		const uint8_t reorg_verbose =
				show_reorg_steps ? 2 : global_verbose;
		struct class *clone = class__clone(tag__class(class), NULL);
		if (clone == NULL) {
			fprintf(stderr, "pahole: out of memory!\n");
			exit(EXIT_FAILURE);
		}
		class__reorganize(clone, cu, reorg_verbose, stdout);
		savings = class__size(tag__class(class)) - class__size(clone);
		if (savings != 0 && reorg_verbose) {
			putchar('\n');
			if (show_reorg_steps)
				puts("/* Final reorganized struct: */");
		}
		tag__fprintf(class__tag(clone), cu, &conf, stdout);
		if (savings != 0) {
			const size_t cacheline_savings =
			      (tag__nr_cachelines(class, cu) -
			       tag__nr_cachelines(class__tag(clone), cu));

			printf("   /* saved %zd byte%s", savings,
			       savings != 1 ? "s" : "");
			if (cacheline_savings != 0)
				printf(" and %zu cacheline%s",
				       cacheline_savings,
				       cacheline_savings != 1 ?
						"s" : "");
			puts("! */");
		}
		class__delete(clone);
	} else if (find_containers) {
		print_containers(cu, class_id, 0);
		goto dump_it;
	} else if (find_pointers_in_structs) {
		print_structs_with_pointer_to(cu, class_id);
		goto dump_it;
 	} else {
		/*
		 * We don't need to print it for every compile unit
		 * but the previous options need
		 */
		tag__fprintf(class, cu, &conf, stdout);
		putchar('\n');
	}

	cu__delete(cu);
	return LSK__STOP_LOADING;
dump_it:
	cu__delete(cu);
	return LSK__STOLEN;
}

int main(int argc, char *argv[])
{
	int err, remaining, rc = EXIT_FAILURE;
	struct cus *cus;

	if (argp_parse(&pahole__argp, argc, argv, 0, &remaining, NULL) ||
	    remaining == argc) {
		argp_help(&pahole__argp, stderr, ARGP_HELP_SEE, argv[0]);
		goto out;
	}

	cus = cus__new();
	if (dwarves__init(cacheline_size) || cus == NULL) {
		fputs("pahole: insufficient memory\n", stderr);
		goto out;
	}

	conf_load.steal = pahole_stealer;

	err = cus__load_files(cus, &conf_load, argv + remaining);
	if (err != 0) {
		fputs("pahole: No debugging information found\n", stderr);
		goto out;
	}

	if (stats_formatter != NULL)
		print_stats();
	rc = EXIT_SUCCESS;
out:
	cus__delete(cus);
	structures__delete();
	dwarves__exit();
	return rc;
}
