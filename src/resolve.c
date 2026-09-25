#include <haxellvm/resolve.h>

#include <haxellvm/diagnostic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *message) {
  fprintf(stderr, "haxellvm: error: %s\n", message);
  exit(1);
}

static void die_at_node(Node *node, const char *message) {
  if (!node->source || !node->source_path) die(message);
  HaxellvmDiagnostic *diagnostic = haxellvm_diagnostic_create(node->source, node->source_path);
  haxellvm_diagnostic_error_at(diagnostic, node->source, node->source_path,
                               node->source_offset, node->source_end_offset, message);
  if (node->reference_source && node->reference_path &&
      strcmp(node->reference_path, node->source_path)) {
    haxellvm_diagnostic_note_at(diagnostic, node->reference_source, node->reference_path,
                                node->reference_offset, node->reference_end_offset, "imported here");
  }
  exit(1);
}

ClassInfo *class_table_find(ClassTable *table, const char *name) {
  for (ClassInfo *info = table->classes; info; info = info->next) {
    if (!strcmp(info->name, name)) return info;
  }
  return NULL;
}

ClassMethod *class_info_find_method(ClassInfo *info, const char *method_name) {
  for (ClassInfo *current = info; current; current = current->parent) {
    for (ClassMethod *method = current->methods; method; method = method->next) {
      if (!strcmp(method->name, method_name)) return method;
    }
  }
  return NULL;
}

ClassField *class_info_find_field(ClassInfo *info, const char *field_name) {
  for (ClassInfo *current = info; current; current = current->parent) {
    for (ClassField *field = current->fields; field; field = field->next) {
      if (!strcmp(field->name, field_name)) return field;
    }
  }
  return NULL;
}

static ClassMethod *find_own_method(ClassInfo *info, const char *method_name) {
  for (ClassMethod *method = info->methods; method; method = method->next) {
    if (!strcmp(method->name, method_name)) return method;
  }
  return NULL;
}

static void mangle_method(char *destination, size_t size, const char *class_name, const char *method_name) {
  if (!strcmp(method_name, "new")) snprintf(destination, size, "%s_new", class_name);
  else snprintf(destination, size, "%s_%s", class_name, method_name);
}

static void append_field(ClassInfo *info, ClassField *field) {
  ClassField **tail = &info->fields;
  while (*tail) tail = &(*tail)->next;
  *tail = field;
}

static void append_method(ClassInfo *info, ClassMethod *method) {
  ClassMethod **tail = &info->methods;
  while (*tail) tail = &(*tail)->next;
  *tail = method;
}

static void collect_class(ClassTable *table, Node *class_node) {
  if (class_table_find(table, class_node->text)) {
    char message[320];
    snprintf(message, sizeof(message), "duplicate class '%s'", class_node->text);
    die_at_node(class_node, message);
  }
  ClassInfo *info = calloc(1, sizeof(*info));
  if (!info) die("out of memory");
  strcpy(info->name, class_node->text);
  strcpy(info->parent_name, class_node->parent_name);
  info->is_abstract = class_node->is_abstract_class;
  info->is_final = class_node->is_final;
  info->declaration = class_node;

  for (Node *field = class_node->a; field; field = field->next) {
    ClassField *item = calloc(1, sizeof(*item));
    if (!item) die("out of memory");
    strcpy(item->name, field->text);
    item->declaration = field;
    append_field(info, item);
  }

  for (Node *method = class_node->b; method; method = method->next) {
    ClassMethod *item = calloc(1, sizeof(*item));
    if (!item) die("out of memory");
    strcpy(item->name, method->text);
    mangle_method(item->mangled, sizeof(item->mangled), method->external_class_name[0] ? method->external_class_name : info->name, method->text);
    item->declaration = method;
    item->is_abstract = method->is_abstract;
    item->is_override = method->is_override;
    item->is_final = method->is_final;
    item->is_constructor = method->is_constructor || !strcmp(method->text, "new");
    item->vtable_index = -1;
    append_method(info, item);
    strcpy(method->class_name, info->name);
  }

  info->next = table->classes;
  table->classes = info;
}

static void link_parents(ClassTable *table) {
  for (ClassInfo *info = table->classes; info; info = info->next) {
    if (!info->parent_name[0]) continue;
    info->parent = class_table_find(table, info->parent_name);
    if (!info->parent) {
      fprintf(stderr, "haxellvm: unknown parent class '%s'\n", info->parent_name);
      exit(1);
    }
    if (info->parent->is_final) {
      fprintf(stderr, "haxellvm: class '%s' cannot extend final class '%s'\n", info->name, info->parent->name);
      exit(1);
    }
  }
}

static void mark_virtuals(ClassInfo *info) {
  for (ClassMethod *method = info->methods; method; method = method->next) {
    if (method->is_constructor) continue;
    if (method->is_abstract || method->is_override) method->is_virtual = 1;
    if (info->parent) {
      ClassMethod *inherited = class_info_find_method(info->parent, method->name);
      if (inherited && inherited->is_final && !method->is_constructor) {
        fprintf(stderr, "haxellvm: method '%s.%s' cannot override final method '%s.%s'\n",
                info->name, method->name, info->parent->name, inherited->name);
        exit(1);
      }
      if (inherited && (inherited->is_virtual || inherited->is_abstract)) {
        method->is_virtual = 1;
        if (!method->is_override && !method->is_abstract) {
          fprintf(stderr, "haxellvm: method '%s.%s' overrides '%s.%s' and must be marked override\n",
                  info->name, method->name, info->parent->name, method->name);
          exit(1);
        }
      }
    }
    if (method->is_override) {
      if (!info->parent) {
        fprintf(stderr, "haxellvm: method '%s.%s' marked override but class has no parent\n", info->name, method->name);
        exit(1);
      }
      ClassMethod *inherited = class_info_find_method(info->parent, method->name);
      if (!inherited) {
        fprintf(stderr, "haxellvm: method '%s.%s' marked override but does not override a parent method\n",
                info->name, method->name);
        exit(1);
      }
      if (inherited->is_final) {
        fprintf(stderr, "haxellvm: method '%s.%s' cannot override final method '%s.%s'\n",
                info->name, method->name, info->parent->name, inherited->name);
        exit(1);
      }
      method->is_virtual = 1;
    }
    if (method->is_virtual) info->has_vtable = 1;
  }
  if (info->parent && info->parent->has_vtable) info->has_vtable = 1;
}

static unsigned count_fields(ClassField *fields) {
  unsigned count = 0;
  for (ClassField *field = fields; field; field = field->next) count++;
  return count;
}

static void layout_class(ClassInfo *info) {
  unsigned base = 0;
  if (info->parent) base = (info->parent->has_vtable ? 1u : 0u) + info->parent->field_count;
  else if (info->has_vtable) base = 1;

  unsigned index = 0;
  for (ClassField *field = info->fields; field; field = field->next) {
    field->struct_index = base + index;
    index++;
  }
  info->field_count = (info->parent ? info->parent->field_count : 0) + count_fields(info->fields);
}

static void build_vtable_layout(ClassInfo *info) {
  info->vtable_slot_count = info->parent ? info->parent->vtable_slot_count : 0;
  if (info->parent) {
    for (ClassMethod *method = info->methods; method; method = method->next) {
      if (!method->is_virtual || method->is_constructor) continue;
      ClassMethod *inherited = class_info_find_method(info->parent, method->name);
      if (inherited && inherited->is_virtual && inherited->vtable_index >= 0) {
        method->vtable_index = inherited->vtable_index;
      }
    }
  }
  for (ClassMethod *method = info->methods; method; method = method->next) {
    if (!method->is_virtual || method->is_constructor) continue;
    if (method->vtable_index >= 0) continue;
    method->vtable_index = (int)info->vtable_slot_count++;
  }
}

static int class_needs_layout(ClassInfo *info) {
  return info->declaration && !info->declaration->value;
}

static void mark_laid_out(ClassInfo *info) {
  if (info->declaration) info->declaration->value = 1;
}

static void validate_abstract(ClassInfo *info) {
  if (info->is_abstract) {
    for (ClassMethod *method = info->methods; method; method = method->next) {
      if (method->is_abstract && method->declaration && method->declaration->a) {
        fprintf(stderr, "haxellvm: abstract method '%s.%s' must not have a body\n", info->name, method->name);
        exit(1);
      }
    }
    return;
  }

  for (ClassMethod *method = info->methods; method; method = method->next) {
    if (method->is_abstract) {
      fprintf(stderr, "haxellvm: concrete class '%s' cannot declare abstract method '%s'\n", info->name, method->name);
      exit(1);
    }
  }

  for (ClassInfo *ancestor = info; ancestor; ancestor = ancestor->parent) {
    for (ClassMethod *method = ancestor->methods; method; method = method->next) {
      if (!method->is_abstract) continue;
      ClassMethod *resolved = class_info_find_method(info, method->name);
      if (!resolved || resolved->is_abstract || !resolved->declaration || !resolved->declaration->a) {
        fprintf(stderr, "haxellvm: concrete class '%s' does not implement abstract method '%s'\n",
                info->name, method->name);
        exit(1);
      }
    }
  }
}

static void check_constructor_super(ClassInfo *info) {
  if (!info->parent) return;
  ClassMethod *parent_ctor = class_info_find_method(info->parent, "new");
  if (!parent_ctor) return;
  ClassMethod *ctor = find_own_method(info, "new");
  if (!ctor || !ctor->declaration || !ctor->declaration->a) return;

  int calls_super = 0;
  for (Node *statement = ctor->declaration->a->a; statement; statement = statement->next) {
    Node *call = NULL;
    if (statement->kind == N_EXPR) call = statement->a;
    else if (statement->kind == N_CALL) call = statement;
    if (call && call->kind == N_CALL && !strcmp(call->text, "super")) {
      calls_super = 1;
      break;
    }
  }
  if (!calls_super) {
    fprintf(stderr, "haxellvm: constructor '%s.new' must call super(...)\n", info->name);
    exit(1);
  }
}

void class_table_build(ClassTable *table, Node *program) {
  memset(table, 0, sizeof(*table));
  for (Node *item = program->next; item; item = item->next) {
    if (item->kind == N_CLASS) {
      item->value = 0;
      collect_class(table, item);
    }
  }
  link_parents(table);

  for (ClassInfo *info = table->classes; info; info = info->next) mark_virtuals(info);

  int progress = 1;
  while (progress) {
    progress = 0;
    for (ClassInfo *info = table->classes; info; info = info->next) {
      if (!class_needs_layout(info)) continue;
      if (info->parent && class_needs_layout(info->parent)) continue;
      layout_class(info);
      build_vtable_layout(info);
      mark_laid_out(info);
      progress = 1;
    }
  }

  for (ClassInfo *info = table->classes; info; info = info->next) {
    if (class_needs_layout(info)) {
      layout_class(info);
      build_vtable_layout(info);
      mark_laid_out(info);
    }
  }

  for (ClassInfo *info = table->classes; info; info = info->next) {
    validate_abstract(info);
    check_constructor_super(info);
  }
}

void class_table_check_abstract(ClassTable *table) {
  for (ClassInfo *info = table->classes; info; info = info->next) validate_abstract(info);
}
