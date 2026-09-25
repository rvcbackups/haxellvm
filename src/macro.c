#include <haxellvm/macro.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MacroValue {
  enum { MV_VOID, MV_INT, MV_BOOL, MV_STRING, MV_ARRAY, MV_OBJECT } kind;
  int integer;
  char text[512];
  struct MacroValue *elements;
  unsigned element_count;
  unsigned element_capacity;
  char *keys;
  struct MacroValue *fields;
  unsigned field_count;
  unsigned field_capacity;
} MacroValue;

static void macro_die(const char *message) {
  fprintf(stderr, "haxellvm: macro: %s\n", message);
  exit(1);
}

static MacroValue *macro_alloc(void) {
  MacroValue *value = calloc(1, sizeof(*value));
  if (!value) macro_die("out of memory");
  return value;
}

static MacroValue *macro_int(int integer) {
  MacroValue *value = macro_alloc();
  value->kind = MV_INT;
  value->integer = integer;
  return value;
}

static MacroValue *macro_bool(int integer) {
  MacroValue *value = macro_alloc();
  value->kind = MV_BOOL;
  value->integer = integer ? 1 : 0;
  return value;
}

static MacroValue *macro_string(const char *text) {
  MacroValue *value = macro_alloc();
  value->kind = MV_STRING;
  if (strlen(text) >= sizeof(value->text)) macro_die("string too long");
  strcpy(value->text, text);
  return value;
}

static MacroValue *macro_array(void) {
  MacroValue *value = macro_alloc();
  value->kind = MV_ARRAY;
  return value;
}

static MacroValue *macro_object(void) {
  MacroValue *value = macro_alloc();
  value->kind = MV_OBJECT;
  return value;
}

static void macro_array_push(MacroValue *array, MacroValue *item) {
  if (array->kind != MV_ARRAY) macro_die("push requires an Array");
  if (array->element_count == array->element_capacity) {
    unsigned capacity = array->element_capacity ? array->element_capacity * 2 : 8;
    MacroValue *elements = realloc(array->elements, capacity * sizeof(*elements));
    if (!elements) macro_die("out of memory");
    array->elements = elements;
    array->element_capacity = capacity;
  }
  array->elements[array->element_count++] = *item;
  free(item);
}

static void macro_object_set(MacroValue *object, const char *key, MacroValue *field) {
  if (object->kind != MV_OBJECT) macro_die("object field requires an object");
  if (object->field_count == object->field_capacity) {
    unsigned capacity = object->field_capacity ? object->field_capacity * 2 : 8;
    char (*keys)[64] = (char (*)[64])object->keys;
    keys = realloc(keys, capacity * 64);
    MacroValue *fields = realloc(object->fields, capacity * sizeof(*fields));
    if (!keys || !fields) macro_die("out of memory");
    object->keys = (char *)keys;
    object->fields = fields;
    object->field_capacity = capacity;
  }
  char (*keys)[64] = (char (*)[64])object->keys;
  if (strlen(key) >= 64) macro_die("object key too long");
  strcpy(keys[object->field_count], key);
  object->fields[object->field_count++] = *field;
  free(field);
}

static MacroValue *macro_object_get(MacroValue *object, const char *key) {
  if (!object || object->kind != MV_OBJECT) return NULL;
  char (*keys)[64] = (char (*)[64])object->keys;
  for (unsigned index = 0; index < object->field_count; index++) {
    if (!strcmp(keys[index], key)) {
      MacroValue *copy = macro_alloc();
      *copy = object->fields[index];
      return copy;
    }
  }
  return NULL;
}

typedef struct MacroLocal {
  char name[256];
  MacroValue *value;
  struct MacroLocal *next;
} MacroLocal;

typedef struct {
  MacroLocal *locals;
  Node *functions;
} MacroInterp;

static MacroValue *eval_expr(MacroInterp *interp, Node *item);
static void eval_statement(MacroInterp *interp, Node *item, MacroValue **returned, int *did_return);

static void bind_local(MacroInterp *interp, const char *name, MacroValue *value) {
  MacroLocal *local = calloc(1, sizeof(*local));
  if (!local) macro_die("out of memory");
  strcpy(local->name, name);
  local->value = value;
  local->next = interp->locals;
  interp->locals = local;
}

static MacroValue *find_local(MacroInterp *interp, const char *name) {
  for (MacroLocal *local = interp->locals; local; local = local->next) {
    if (!strcmp(local->name, name)) return local->value;
  }
  return NULL;
}

static Node *find_macro_function(MacroInterp *interp, const char *name) {
  const char *simple = strrchr(name, '.');
  simple = simple ? simple + 1 : name;
  for (Node *function = interp->functions; function; function = function->next) {
    if (function->is_macro && (!strcmp(function->text, name) || !strcmp(function->text, simple))) return function;
  }
  return NULL;
}

static int eval_int(MacroInterp *interp, Node *item) {
  MacroValue *value = eval_expr(interp, item);
  if (!value || (value->kind != MV_INT && value->kind != MV_BOOL)) macro_die("expected Int");
  return value->integer;
}

static MacroValue *eval_expr(MacroInterp *interp, Node *item) {
  if (!item) return macro_alloc();
  if (item->kind == N_INT) return macro_int((int)item->number);
  if (item->kind == N_BOOL) return macro_bool(item->value);
  if (item->kind == N_STR) return macro_string(item->text);
  if (item->kind == N_NULL) return macro_alloc();
  if (item->kind == N_NAME) {
    MacroValue *local = find_local(interp, item->text);
    if (local) {
      MacroValue *copy = macro_alloc();
      *copy = *local;
      return copy;
    }
    if (!strcmp(item->text, "true")) return macro_bool(1);
    if (!strcmp(item->text, "false")) return macro_bool(0);
    if (!strcmp(item->text, "null")) return macro_alloc();
    if (item->text[0] == 'A' && item->text[1] >= 'A' && item->text[1] <= 'Z') return macro_string(item->text);
    macro_die("unknown macro name");
  }
  if (item->kind == N_ARRAY) {
    MacroValue *array = macro_array();
    for (Node *element = item->a; element; element = element->next) macro_array_push(array, eval_expr(interp, element));
    return array;
  }
  if (item->kind == N_RECORD) {
    MacroValue *object = macro_object();
    for (Node *field = item->a; field; field = field->next) macro_object_set(object, field->text, eval_expr(interp, field->a));
    return object;
  }
  if (item->kind == N_UNARY) {
    if (!strcmp(item->text, "!")) return macro_bool(!eval_int(interp, item->a));
    if (!strcmp(item->text, "-")) return macro_int(-eval_int(interp, item->a));
    if (!strcmp(item->text, "~")) return macro_int(~eval_int(interp, item->a));
    macro_die("unsupported macro unary");
  }
  if (item->kind == N_BINARY) {
    if (!strcmp(item->text, "+")) {
      MacroValue *left = eval_expr(interp, item->a);
      MacroValue *right = eval_expr(interp, item->b);
      if (left->kind == MV_STRING || right->kind == MV_STRING) {
        char buffer[512];
        const char *left_text = left->kind == MV_STRING ? left->text : NULL;
        const char *right_text = right->kind == MV_STRING ? right->text : NULL;
        char left_number[32], right_number[32];
        if (!left_text) {
          snprintf(left_number, sizeof(left_number), "%d", left->integer);
          left_text = left_number;
        }
        if (!right_text) {
          snprintf(right_number, sizeof(right_number), "%d", right->integer);
          right_text = right_number;
        }
        if (strlen(left_text) + strlen(right_text) >= sizeof(buffer)) macro_die("concatenated string too long");
        strcpy(buffer, left_text);
        strcat(buffer, right_text);
        return macro_string(buffer);
      }
      return macro_int(left->integer + right->integer);
    }
    int left = eval_int(interp, item->a);
    int right = eval_int(interp, item->b);
    if (!strcmp(item->text, "-")) return macro_int(left - right);
    if (!strcmp(item->text, "*")) return macro_int(left * right);
    if (!strcmp(item->text, "/")) return macro_int(right ? left / right : 0);
    if (!strcmp(item->text, "%")) return macro_int(right ? left % right : 0);
    if (!strcmp(item->text, "&")) return macro_int(left & right);
    if (!strcmp(item->text, "|")) return macro_int(left | right);
    if (!strcmp(item->text, "^")) return macro_int(left ^ right);
    if (!strcmp(item->text, "<<")) return macro_int(left << right);
    if (!strcmp(item->text, ">>")) return macro_int(left >> right);
    if (!strcmp(item->text, "==")) return macro_bool(left == right);
    if (!strcmp(item->text, "!=")) return macro_bool(left != right);
    if (!strcmp(item->text, "<")) return macro_bool(left < right);
    if (!strcmp(item->text, "<=")) return macro_bool(left <= right);
    if (!strcmp(item->text, ">")) return macro_bool(left > right);
    if (!strcmp(item->text, ">=")) return macro_bool(left >= right);
    if (!strcmp(item->text, "&&")) return macro_bool(left && right);
    if (!strcmp(item->text, "||")) return macro_bool(left || right);
    macro_die("unsupported macro binary");
  }
  if (item->kind == N_TERNARY) {
    return eval_int(interp, item->a) ? eval_expr(interp, item->b) : eval_expr(interp, item->c);
  }
  if (item->kind == N_CALL) {
    if (!strcmp(item->text, "push") && item->b && item->b->kind == N_NAME) {
      MacroValue *array = find_local(interp, item->b->text);
      if (!array) macro_die("push target is not a local Array");
      if (!item->a) macro_die("push expects one argument");
      macro_array_push(array, eval_expr(interp, item->a));
      return macro_alloc();
    }
    if (strstr(item->text, ".push") && item->a) {
      char array_name[256];
      size_t length = strlen(item->text) - 5;
      memcpy(array_name, item->text, length);
      array_name[length] = '\0';
      MacroValue *array = find_local(interp, array_name);
      if (!array) macro_die("push target is not a local Array");
      macro_array_push(array, eval_expr(interp, item->a));
      return macro_alloc();
    }
    if (!strcmp(item->text, "FFun") || !strcmp(item->text, "haxe.macro.Expr.FFun")) {
      MacroValue *object = macro_object();
      macro_object_set(object, "kind", macro_string("FFun"));
      Node *argument = item->a;
      if (argument && argument->kind == N_RECORD && !argument->next) {
        MacroValue *record = eval_expr(interp, argument);
        if (record->kind == MV_OBJECT) {
          char (*keys)[64] = (char (*)[64])record->keys;
          for (unsigned index = 0; index < record->field_count; index++) {
            MacroValue *copy = macro_alloc();
            *copy = record->fields[index];
            macro_object_set(object, keys[index], copy);
          }
        }
        return object;
      }
      if (argument) {
        macro_object_set(object, "args", eval_expr(interp, argument));
        argument = argument->next;
      } else macro_object_set(object, "args", macro_array());
      if (argument) {
        macro_object_set(object, "ret", eval_expr(interp, argument));
        argument = argument->next;
      }
      if (argument) macro_object_set(object, "expr", eval_expr(interp, argument));
      return object;
    }
    if (!strcmp(item->text, "EBlock") || !strcmp(item->text, "haxe.macro.Expr.EBlock")) {
      MacroValue *object = macro_object();
      macro_object_set(object, "kind", macro_string("EBlock"));
      macro_object_set(object, "exprs", item->a ? eval_expr(interp, item->a) : macro_array());
      return object;
    }
    if (!strcmp(item->text, "ECall") || !strcmp(item->text, "haxe.macro.Expr.ECall")) {
      MacroValue *object = macro_object();
      macro_object_set(object, "kind", macro_string("ECall"));
      Node *argument = item->a;
      if (argument) {
        macro_object_set(object, "e", eval_expr(interp, argument));
        argument = argument->next;
      }
      if (argument) macro_object_set(object, "params", eval_expr(interp, argument));
      else macro_object_set(object, "params", macro_array());
      return object;
    }
    if (!strcmp(item->text, "EConst") || !strcmp(item->text, "haxe.macro.Expr.EConst")) {
      MacroValue *object = macro_object();
      macro_object_set(object, "kind", macro_string("EConst"));
      if (item->a) macro_object_set(object, "c", eval_expr(interp, item->a));
      return object;
    }
    if (!strcmp(item->text, "CIdent") || !strcmp(item->text, "haxe.macro.Expr.CIdent")) {
      MacroValue *object = macro_object();
      macro_object_set(object, "kind", macro_string("CIdent"));
      if (item->a) macro_object_set(object, "name", eval_expr(interp, item->a));
      return object;
    }
    if (!strcmp(item->text, "CString") || !strcmp(item->text, "haxe.macro.Expr.CString")) {
      MacroValue *object = macro_object();
      macro_object_set(object, "kind", macro_string("CString"));
      if (item->a) macro_object_set(object, "value", eval_expr(interp, item->a));
      return object;
    }
    if (!strcmp(item->text, "macro") && item->a) return eval_expr(interp, item->a);
    Node *callee = find_macro_function(interp, item->text);
    if (callee) {
      MacroLocal *saved = interp->locals;
      Node *parameter = callee->b;
      for (Node *argument = item->a; argument; argument = argument->next, parameter = parameter ? parameter->next : NULL) {
        if (!parameter) macro_die("macro call argument count mismatch");
        bind_local(interp, parameter->text, eval_expr(interp, argument));
      }
      if (parameter) macro_die("macro call argument count mismatch");
      MacroValue *returned = NULL;
      int did_return = 0;
      eval_statement(interp, callee->a, &returned, &did_return);
      interp->locals = saved;
      return returned ? returned : macro_alloc();
    }
    macro_die("unknown macro call");
  }
  if (item->kind == N_FIELD) {
    MacroValue *object = eval_expr(interp, item->a);
    MacroValue *field = macro_object_get(object, item->text);
    if (!field) macro_die("missing object field");
    return field;
  }
  if (item->kind == N_NEW) {
    MacroValue *object = macro_object();
    macro_object_set(object, "kind", macro_string(item->text));
    if (!strcmp(item->text, "FieldType") || strstr(item->text, "Field")) {
      /* new haxe.macro.Expr.Field() style records usually use object literals instead */
    }
    unsigned index = 0;
    const char *keys[] = {"name", "kind", "access", "pos", "doc", "meta", NULL};
    for (Node *argument = item->a; argument && keys[index]; argument = argument->next, index++) {
      macro_object_set(object, keys[index], eval_expr(interp, argument));
    }
    return object;
  }
  macro_die("unsupported macro expression");
  return macro_alloc();
}

static void eval_statement(MacroInterp *interp, Node *item, MacroValue **returned, int *did_return) {
  for (; item && !*did_return; item = item->next) {
    if (item->kind == N_BLOCK) {
      eval_statement(interp, item->a, returned, did_return);
      continue;
    }
    if (item->kind == N_VAR) {
      bind_local(interp, item->text, item->a ? eval_expr(interp, item->a) : macro_alloc());
      continue;
    }
    if (item->kind == N_ASSIGN) {
      MacroValue *local = find_local(interp, item->text);
      if (!local) macro_die("assignment to unknown macro local");
      MacroValue *value = eval_expr(interp, item->a);
      *local = *value;
      free(value);
      continue;
    }
    if (item->kind == N_RETURN) {
      *returned = item->a ? eval_expr(interp, item->a) : macro_alloc();
      *did_return = 1;
      return;
    }
    if (item->kind == N_IF) {
      if (eval_int(interp, item->a)) eval_statement(interp, item->b, returned, did_return);
      else if (item->c) eval_statement(interp, item->c, returned, did_return);
      continue;
    }
    if (item->kind == N_FOR) {
      int start = eval_int(interp, item->a);
      int end = eval_int(interp, item->b);
      for (int index = start; index < end && !*did_return; index++) {
        MacroLocal *saved = interp->locals;
        bind_local(interp, item->text, macro_int(index));
        eval_statement(interp, item->c, returned, did_return);
        interp->locals = saved;
      }
      continue;
    }
    if (item->kind == N_WHILE) {
      while (!*did_return && eval_int(interp, item->a)) eval_statement(interp, item->b, returned, did_return);
      continue;
    }
    if (item->kind == N_EXPR && item->a) eval_expr(interp, item->a);
  }
}

static Node *make_ident(const char *name) {
  Node *name_node = node_new(N_NAME);
  strcpy(name_node->text, name);
  return name_node;
}

static Node *make_string_node(const char *text) {
  Node *node = node_new(N_STR);
  if (strlen(text) >= sizeof(node->text)) macro_die("asm template too long");
  strcpy(node->text, text);
  return node;
}

static Node *expr_from_value(MacroValue *value);

static Node *call_from_value(MacroValue *value) {
  MacroValue *kind = macro_object_get(value, "kind");
  if (kind && kind->kind == MV_STRING && !strcmp(kind->text, "ECall")) {
    Node *call = node_new(N_CALL);
    MacroValue *callee = macro_object_get(value, "e");
    if (callee && callee->kind == MV_OBJECT) {
      MacroValue *ident = macro_object_get(callee, "kind");
      if (ident && !strcmp(ident->text, "CIdent")) {
        MacroValue *name = macro_object_get(callee, "name");
        if (name && name->kind == MV_STRING) strcpy(call->text, name->text);
      } else if (ident && !strcmp(ident->text, "EConst")) {
        MacroValue *constant = macro_object_get(callee, "c");
        if (constant) {
          MacroValue *name = macro_object_get(constant, "name");
          if (name && name->kind == MV_STRING) strcpy(call->text, name->text);
        }
      }
    } else if (callee && callee->kind == MV_STRING) strcpy(call->text, callee->text);
    MacroValue *params = macro_object_get(value, "params");
    Node **tail = &call->a;
    if (params && params->kind == MV_ARRAY) {
      for (unsigned index = 0; index < params->element_count; index++) {
        *tail = expr_from_value(&params->elements[index]);
        tail = &(*tail)->next;
      }
    }
    return call;
  }
  if (kind && kind->kind == MV_STRING && !strcmp(kind->text, "EConst")) {
    MacroValue *constant = macro_object_get(value, "c");
    if (constant && constant->kind == MV_OBJECT) {
      MacroValue *constant_kind = macro_object_get(constant, "kind");
      if (constant_kind && !strcmp(constant_kind->text, "CString")) {
        MacroValue *text = macro_object_get(constant, "value");
        return make_string_node(text && text->kind == MV_STRING ? text->text : "");
      }
      if (constant_kind && !strcmp(constant_kind->text, "CIdent")) {
        MacroValue *name = macro_object_get(constant, "name");
        return make_ident(name && name->kind == MV_STRING ? name->text : "");
      }
    }
    if (constant && constant->kind == MV_STRING) return make_string_node(constant->text);
  }
  if (kind && kind->kind == MV_STRING && !strcmp(kind->text, "CString")) {
    MacroValue *text = macro_object_get(value, "value");
    return make_string_node(text && text->kind == MV_STRING ? text->text : "");
  }
  if (value->kind == MV_STRING) return make_string_node(value->text);
  macro_die("unsupported generated expression");
  return make_ident("asm");
}

static Node *expr_from_value(MacroValue *value) {
  if (value->kind == MV_STRING) return make_string_node(value->text);
  if (value->kind == MV_OBJECT) return call_from_value(value);
  macro_die("unsupported generated expression value");
  return make_ident("asm");
}

static Node *block_from_value(MacroValue *value) {
  Node *block = node_new(N_BLOCK);
  Node **tail = &block->a;
  MacroValue *exprs = NULL;
  if (value && value->kind == MV_OBJECT) {
    MacroValue *kind = macro_object_get(value, "kind");
    if (kind && !strcmp(kind->text, "EBlock")) exprs = macro_object_get(value, "exprs");
    else {
      Node *statement = node_new(N_EXPR);
      statement->a = expr_from_value(value);
      *tail = statement;
      return block;
    }
  } else if (value && value->kind == MV_ARRAY) exprs = value;
  if (exprs && exprs->kind == MV_ARRAY) {
    for (unsigned index = 0; index < exprs->element_count; index++) {
      Node *statement = node_new(N_EXPR);
      statement->a = expr_from_value(&exprs->elements[index]);
      *tail = statement;
      tail = &(*tail)->next;
    }
  }
  return block;
}

static int field_has_access(MacroValue *field, const char *name) {
  MacroValue *access = macro_object_get(field, "access");
  if (!access) return 0;
  if (access->kind == MV_ARRAY) {
    for (unsigned index = 0; index < access->element_count; index++) {
      if (access->elements[index].kind == MV_STRING && !strcmp(access->elements[index].text, name)) return 1;
      if (access->elements[index].kind == MV_OBJECT) {
        MacroValue *kind = macro_object_get(&access->elements[index], "kind");
        if (kind && kind->kind == MV_STRING && strstr(kind->text, name)) return 1;
      }
    }
  }
  if (access->kind == MV_STRING && strstr(access->text, name)) return 1;
  return 0;
}

static int exception_has_error_code(unsigned vector) {
  if (vector >= 32) return 0;
  return ((1u << vector) & 0x403F7DFFu) != 0;
}

static Node *make_naked_isr(const char *name, unsigned vector, int has_error_code) {
  Node *function = node_new(N_FUNCTION);
  strcpy(function->text, name);
  strcpy(function->type_name, "Void");
  function->is_static = 1;
  function->retain_function_name = 1;
  function->is_naked = 1;
  Node *call = node_new(N_CALL);
  strcpy(call->text, "asm");
  char template[256];
  if (has_error_code) snprintf(template, sizeof(template), "push %u\npush fs\njmp idtCommonHandler", vector);
  else snprintf(template, sizeof(template), "push 0\npush %u\npush fs\njmp idtCommonHandler", vector);
  call->a = make_string_node(template);
  Node *statement = node_new(N_EXPR);
  statement->a = call;
  Node *body = node_new(N_BLOCK);
  body->a = statement;
  function->a = body;
  return function;
}

static Node *function_from_field(MacroValue *field) {
  MacroValue *name_value = macro_object_get(field, "name");
  if (!name_value || name_value->kind != MV_STRING) macro_die("@:build field is missing a name");
  MacroValue *kind = macro_object_get(field, "kind");
  Node *function = node_new(N_FUNCTION);
  strcpy(function->text, name_value->text);
  strcpy(function->type_name, "Void");
  function->is_static = field_has_access(field, "AStatic") || field_has_access(field, "static") || 1;
  function->retain_function_name = 1;
  function->is_naked = field_has_access(field, "ANaked") || field_has_access(field, "naked");
  if (kind && kind->kind == MV_OBJECT) {
    MacroValue *ret = macro_object_get(kind, "ret");
    if (ret && ret->kind == MV_STRING) strcpy(function->type_name, ret->text);
    MacroValue *expr = macro_object_get(kind, "expr");
    function->a = block_from_value(expr);
    if (!function->is_naked) {
      /* ISR stubs generated as FFun+asm are still interrupt entries. */
      if (!strncmp(function->text, "isr", 3)) function->is_naked = 1;
    }
  } else function->a = node_new(N_BLOCK);
  return function;
}

static Node *lookup_macro_function(Node *program, const char *path) {
  const char *simple = strrchr(path, '.');
  simple = simple ? simple + 1 : path;
  char class_name[256] = "";
  char method_name[256] = "";
  const char *separator = strrchr(path, '.');
  if (separator) {
    size_t class_length = (size_t)(separator - path);
    if (class_length >= sizeof(class_name)) macro_die("@:build path too long");
    memcpy(class_name, path, class_length);
    class_name[class_length] = '\0';
    strcpy(method_name, separator + 1);
    const char *class_simple = strrchr(class_name, '.');
    if (class_simple) memmove(class_name, class_simple + 1, strlen(class_simple + 1) + 1);
  } else strcpy(method_name, path);
  for (Node *function = program->c; function; function = function->next) {
    if (!function->is_macro) continue;
    if (!strcmp(function->text, method_name) && (!class_name[0] || !strcmp(function->class_name, class_name))) return function;
    if (!strcmp(function->text, simple)) return function;
  }
  return NULL;
}

static void append_function(Node *program, Node *function) {
  Node **tail = &program->c;
  while (*tail) tail = &(*tail)->next;
  *tail = function;
}

static void apply_build_fields(Node *program, Node *class_node, MacroValue *fields) {
  if (!fields || fields->kind != MV_ARRAY) macro_die("@:build must return Array<Field>");
  for (unsigned index = 0; index < fields->element_count; index++) {
    Node *function = function_from_field(&fields->elements[index]);
    strcpy(function->class_name, class_node->text);
    if (!strncmp(function->text, "isr", 3)) {
      unsigned vector = (unsigned)strtoul(function->text + 3, NULL, 10);
      if (!function->is_naked) function->is_naked = 1;
      if (!function->a || !function->a->a) {
        Node *generated = make_naked_isr(function->text, vector, exception_has_error_code(vector) && vector < 32);
        generated->next = NULL;
        strcpy(generated->class_name, class_node->text);
        append_function(program, generated);
        continue;
      }
    }
    append_function(program, function);
  }
}

void haxellvm_expand_macros(Node *program) {
  if (!program) return;
  MacroInterp interp = {0};
  interp.functions = program->c;
  for (Node *class_node = program->next; class_node; class_node = class_node->next) {
    if (class_node->kind != N_CLASS && class_node->kind != N_INTERFACE) continue;
    if (!class_node->build_macro[0]) continue;
    Node *macro = lookup_macro_function(program, class_node->build_macro);
    if (!macro) {
      char message[320];
      snprintf(message, sizeof(message), "cannot resolve @:build macro '%s'", class_node->build_macro);
      macro_die(message);
    }
    MacroLocal *saved = interp.locals;
    MacroValue *returned = NULL;
    int did_return = 0;
    eval_statement(&interp, macro->a, &returned, &did_return);
    interp.locals = saved;
    apply_build_fields(program, class_node, returned);
  }
}
