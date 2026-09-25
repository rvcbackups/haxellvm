#include <haxellvm/codegen.h>

#include <haxellvm/diagnostic.h>
#include <haxellvm/resolve.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct Local {
  char name[256];
  LLVMValueRef address;
  LLVMTypeRef type;
  LLVMValueRef string_length;
  int integer_unsigned;
  LLVMTypeRef fixed_element_type;
  unsigned fixed_array_length;
  int fixed_element_unsigned;
  LLVMTypeRef pointer_element_type;
  int pointer_element_unsigned;
  LLVMTypeRef dynamic_element_type;
  int dynamic_element_unsigned;
  int is_growable_array;
  unsigned growable_capacity;
  unsigned alignment;
  char class_name[256];
  struct Local *next;
} Local;

typedef struct Function {
  Node *declaration;
  LLVMValueRef value;
  LLVMTypeRef return_type;
  struct Function *next;
} Function;

typedef struct CatchFrame {
  LLVMBasicBlockRef handler;
  LLVMBasicBlockRef join;
  char catch_name[256];
  LLVMTypeRef value_type;
  struct CatchFrame *parent;
} CatchFrame;

typedef struct {
  LLVMContextRef context;
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMTypeRef i1, i8, i16, i32, i64, word_type, void_type, pointer_type, printf_type, puts_type;
  LLVMTypeRef f32, f64;
  unsigned word_bits;
  int uses_float;
  int has_usings;
  Node *usings;
  CatchFrame *catch_stack;
  LLVMValueRef exception_slot;
  LLVMValueRef printf_fn, puts_fn, main_fn;
  Local *locals;
  Node *trace_handler;
  Node *functions;
  Node *typedefs;
  Node *globals;
  Node *class_initializers;
  Function *function_values;
  LLVMTypeRef current_return_type;
  LLVMTypeRef last_pointer_element_type;
  int last_pointer_element_unsigned;
  LLVMValueRef string_length_function;
  LLVMValueRef i128_udivmod_fn;
  LLVMBasicBlockRef break_target;
  LLVMBasicBlockRef continue_target;
  ClassTable classes;
  LLVMValueRef arena_storage;
  LLVMValueRef arena_cursor;
  HaxellvmDiagnostic *diagnostic;
  Node *tree;
  LLVMValueRef string_scratch_buffers[128];
  unsigned string_scratch_count;
  char current_class_name[256];
  int object_format; /* ObjectFormat */
  int target_abi; /* TargetAbi */
  int is_x86_64;
  int emit_debug;
  LLVMDIBuilderRef di_builder;
  LLVMMetadataRef di_file;
  LLVMMetadataRef di_compile_unit;
  LLVMMetadataRef di_scope;
  const char *debug_source;
  const char *debug_path;
} Codegen;

typedef enum { OBJECT_FORMAT_ELF, OBJECT_FORMAT_PE, OBJECT_FORMAT_MACHO } ObjectFormat;
typedef enum { TARGET_ABI_SYSV, TARGET_ABI_WIN64, TARGET_ABI_DEFAULT } TargetAbi;

typedef struct {
  const char *name;
  uint64_t value;
} ConstantBinding;

static void debug_offset_to_line_column(const char *source, size_t offset, unsigned *line, unsigned *column) {
  unsigned current_line = 1;
  unsigned current_column = 1;
  if (!source) {
    *line = 1;
    *column = 1;
    return;
  }
  for (size_t index = 0; source[index] && index < offset; index++) {
    if (source[index] == '\n') {
      current_line++;
      current_column = 1;
    } else current_column++;
  }
  *line = current_line;
  *column = current_column;
}

static int declaration_is_typed_pointer(const Node *declaration);

static void debug_split_path(const char *path, char *directory, size_t directory_size, char *filename, size_t filename_size) {
  const char *slash = path ? strrchr(path, '/') : NULL;
  if (!slash) {
    snprintf(directory, directory_size, ".");
    snprintf(filename, filename_size, "%s", path ? path : "input.hx");
    return;
  }
  size_t directory_length = (size_t)(slash - path);
  if (directory_length >= directory_size) directory_length = directory_size - 1;
  memcpy(directory, path, directory_length);
  directory[directory_length] = '\0';
  snprintf(filename, filename_size, "%s", slash + 1);
}

static void debug_init(Codegen *codegen, const char *path) {
  if (!codegen->emit_debug) return;
  codegen->debug_source = codegen->tree ? codegen->tree->source : NULL;
  codegen->debug_path = (codegen->tree && codegen->tree->source_path && codegen->tree->source_path[0]) ? codegen->tree->source_path : path;
  codegen->di_builder = LLVMCreateDIBuilder(codegen->module);
  char directory[512];
  char filename[256];
  debug_split_path(codegen->debug_path, directory, sizeof(directory), filename, sizeof(filename));
  codegen->di_file = LLVMDIBuilderCreateFile(codegen->di_builder, filename, strlen(filename), directory, strlen(directory));
  const char *producer = "haxellvm";
  codegen->di_compile_unit = LLVMDIBuilderCreateCompileUnit(
      codegen->di_builder, LLVMDWARFSourceLanguageC_plus_plus, codegen->di_file,
      producer, strlen(producer), 0, "", 0, 0, "", 0, LLVMDWARFEmissionFull, 0, 0, 0, "", 0, "", 0);
  codegen->di_scope = codegen->di_compile_unit;
  LLVMValueRef debug_version = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 3, 0);
  LLVMAddModuleFlag(codegen->module, LLVMModuleFlagBehaviorWarning, "Debug Info Version", strlen("Debug Info Version"), LLVMValueAsMetadata(debug_version));
  if (codegen->object_format == OBJECT_FORMAT_PE) {
    LLVMValueRef codeview = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 1, 0);
    LLVMAddModuleFlag(codegen->module, LLVMModuleFlagBehaviorWarning, "CodeView", strlen("CodeView"), LLVMValueAsMetadata(codeview));
  }
}

static void debug_set_location(Codegen *codegen, Node *item) {
  if (!codegen->emit_debug || !codegen->di_builder || !codegen->di_scope) return;
  unsigned line = 1;
  unsigned column = 1;
  if (item && codegen->debug_source) debug_offset_to_line_column(codegen->debug_source, item->source_offset, &line, &column);
  LLVMMetadataRef location = LLVMDIBuilderCreateDebugLocation(codegen->context, line, column, codegen->di_scope, NULL);
  LLVMSetCurrentDebugLocation2(codegen->builder, location);
}

static void debug_attach_subprogram(Codegen *codegen, LLVMValueRef function, Node *declaration) {
  if (!codegen->emit_debug || !codegen->di_builder || !function) return;
  const char *name = LLVMGetValueName(function);
  if (!name || !name[0]) name = declaration && declaration->text[0] ? declaration->text : "fn";
  unsigned line = 1;
  unsigned column = 1;
  if (declaration && codegen->debug_source) debug_offset_to_line_column(codegen->debug_source, declaration->source_offset, &line, &column);
  (void)column;
  LLVMMetadataRef subroutine = LLVMDIBuilderCreateSubroutineType(codegen->di_builder, codegen->di_file, NULL, 0, LLVMDIFlagZero);
  LLVMMetadataRef subprogram = LLVMDIBuilderCreateFunction(
      codegen->di_builder, codegen->di_file, name, strlen(name), name, strlen(name),
      codegen->di_file, line, subroutine, 0, 1, line, LLVMDIFlagPrototyped, 0);
  LLVMSetSubprogram(function, subprogram);
  codegen->di_scope = subprogram;
}

static void debug_finalize(Codegen *codegen) {
  if (!codegen->emit_debug || !codegen->di_builder) return;
  LLVMDIBuilderFinalize(codegen->di_builder);
  LLVMDisposeDIBuilder(codegen->di_builder);
  codegen->di_builder = NULL;
}

static LLVMValueRef object_field_address(Codegen *codegen, Local *object, const char *field_name, LLVMTypeRef *field_type);
static LLVMValueRef object_array_element_address(Codegen *codegen, Node *item, LLVMTypeRef *element_type_out);
static LLVMValueRef local_field_address(Codegen *codegen, const char *name, LLVMTypeRef *field_type);
static LLVMValueRef global_field_address(Codegen *codegen, const char *name, LLVMTypeRef *field_type);
static int split_receiver_method(const char *qualified, char *receiver, size_t receiver_size, char *method, size_t method_size);
static LLVMValueRef emit_constant(Codegen *codegen, Node *item, const char *parameter, uint64_t argument, LLVMTypeRef *type);
static Node *expand_constant_array_initializer(Codegen *codegen, Node *item);
static LLVMValueRef emit_constant_bindings(Codegen *codegen, Node *item, ConstantBinding *bindings, unsigned binding_count, LLVMTypeRef *type);
static int type_is_any(const char *name);
static void designate_any_class(Node *global, const char *class_name);
static ClassInfo *class_from_typed_declaration(Codegen *codegen, Node *declaration);
static void stamp_class_name_from_declaration(Codegen *codegen, Local *local, Node *declaration, Node *initializer);
static ClassInfo *find_unique_class_with_member(Codegen *codegen, const char *member, int want_method);
static ClassInfo *resolve_any_class(Codegen *codegen, Node *global, Local *local, const char *member, int want_method);
static void refine_any_globals(Codegen *codegen);

static HaxellvmDiagnostic *active_diagnostic;
static Node *active_diagnostic_node;
static void die(const char *message) {
  if (active_diagnostic) {
    size_t start_offset = active_diagnostic_node ? active_diagnostic_node->source_offset : 0;
    size_t end_offset = active_diagnostic_node ? active_diagnostic_node->source_end_offset : 0;
    haxellvm_diagnostic_error(active_diagnostic, start_offset, end_offset, message);
  } else fprintf(stderr, "haxellvm: error: %s\n", message);
  exit(1);
}
static void die_at(Codegen *codegen, Node *item, const char *message) {
  if (item && item->source && item->source_path) {
    haxellvm_diagnostic_error_at(codegen->diagnostic, item->source, item->source_path,
                                 item->source_offset, item->source_end_offset, message);
    if (item->reference_source && item->reference_path &&
        (!item->source_path || strcmp(item->reference_path, item->source_path))) {
      haxellvm_diagnostic_note_at(codegen->diagnostic, item->reference_source, item->reference_path,
                                  item->reference_offset, item->reference_end_offset, "imported here");
    } else {
      const char *primary = haxellvm_diagnostic_primary_path(codegen->diagnostic);
      if (primary && primary[0] && item->source_path && strcmp(primary, item->source_path) &&
          codegen->tree && codegen->tree->source && codegen->tree->source_path) {
        haxellvm_diagnostic_note_at(codegen->diagnostic, codegen->tree->source, codegen->tree->source_path,
                                    0, 0, "while compiling this file");
      }
    }
  } else {
    size_t start_offset = item ? item->source_offset : 0;
    size_t end_offset = item ? item->source_end_offset : 0;
    haxellvm_diagnostic_error(codegen->diagnostic, start_offset, end_offset, message);
  }
  exit(1);
}
static void die_call(Codegen *codegen, Node *item) {
  char message[384];
  snprintf(message, sizeof(message), "call to undeclared function '%s'; declare it with 'function' or import the module that exports it", item->text);
  die_at(codegen, item, message);
}

static ObjectFormat object_format_from_triple(const char *triple) {
  char *normalized = LLVMNormalizeTargetTriple(triple);
  const char *value = normalized ? normalized : triple;
  ObjectFormat format = OBJECT_FORMAT_ELF;
  /* UEFI firmware uses PE/COFF on all supported arches. */
  if (strstr(value, "windows") || strstr(value, "win32") || strstr(value, "mingw") || strstr(value, "cygwin") ||
      strstr(value, "uefi")) {
    format = OBJECT_FORMAT_PE;
  } else if (strstr(value, "darwin") || strstr(value, "macos") || strstr(value, "ios")
             || strstr(value, "tvos") || strstr(value, "watchos") || strstr(value, "xros")) {
    format = OBJECT_FORMAT_MACHO;
  }
  if (normalized) LLVMDisposeMessage(normalized);
  return format;
}

static int triple_is_x86_64(const char *triple) {
  char *normalized = LLVMNormalizeTargetTriple(triple);
  const char *value = normalized ? normalized : triple;
  int is_x86_64 = !strncmp(value, "x86_64", 6) || strstr(value, "amd64") != NULL;
  if (normalized) LLVMDisposeMessage(normalized);
  return is_x86_64;
}

static TargetAbi target_abi_from_triple(const char *triple) {
  char *normalized = LLVMNormalizeTargetTriple(triple);
  const char *value = normalized ? normalized : triple;
  TargetAbi abi = TARGET_ABI_DEFAULT;
  int windows_like = strstr(value, "windows") || strstr(value, "win32") || strstr(value, "mingw") ||
                     strstr(value, "cygwin") || strstr(value, "uefi") || strstr(value, "msvc");
  if (triple_is_x86_64(value)) {
    /* x86_64 Windows / UEFI use the Microsoft ABI, not SysV. */
    abi = windows_like ? TARGET_ABI_WIN64 : TARGET_ABI_SYSV;
  }
  if (normalized) LLVMDisposeMessage(normalized);
  return abi;
}

/* Haxe Int/UInt are native register width for this freestanding target. */
static unsigned word_bits_from_triple(const char *triple) {
  char *normalized = LLVMNormalizeTargetTriple(triple);
  const char *value = normalized ? normalized : triple;
  /* Default to 64-bit word — this is the AMD64 / AArch64 / Alpha / MIPS64 era.
     Only known 32-bit (and narrower) targets opt into i32. Explicit UIntSize<8/16/32> stays narrow. */
  unsigned bits = 64;
  if (strstr(value, "x86_64") || strstr(value, "amd64") || strstr(value, "aarch64") || strstr(value, "arm64") ||
      strstr(value, "riscv64") || strstr(value, "powerpc64") || strstr(value, "ppc64") || strstr(value, "mips64") ||
      strstr(value, "sparcv9") || strstr(value, "sparc64") || strstr(value, "wasm64") || strstr(value, "loongarch64") ||
      strstr(value, "s390x") || strstr(value, "alpha") || strstr(value, "ia64") || strstr(value, "hppa64")) {
    bits = 64;
  } else if (strstr(value, "i386") || strstr(value, "i486") || strstr(value, "i586") || strstr(value, "i686") ||
             strstr(value, "arm") || strstr(value, "thumb") || strstr(value, "riscv32") || strstr(value, "wasm32") ||
             strstr(value, "mips") || strstr(value, "powerpc") || strstr(value, "ppc") || strstr(value, "sparc") ||
             strstr(value, "loongarch32") || strstr(value, "hexagon") || strstr(value, "hppa")) {
    bits = 32;
  }
  if (normalized) LLVMDisposeMessage(normalized);
  return bits;
}

static int type_is_word_integer(const char *name) {
  return name && (!strcmp(name, "Int") || !strcmp(name, "UInt"));
}

/* Fills buffer when Mach-O needs segment,section composition. Returns NULL if no section. */
static const char *resolve_section(Node *item, ObjectFormat format, char *buffer, size_t buffer_size) {
  if (format == OBJECT_FORMAT_ELF && item->section_elf[0]) return item->section_elf;
  if (format == OBJECT_FORMAT_PE && item->section_pe[0]) return item->section_pe;
  if (format == OBJECT_FORMAT_MACHO) {
    if (item->macho_segment[0] && !item->section_macho[0]) die("machoSegment requires machoSection");
    if (item->macho_segment[0] && item->section_macho[0]) {
      if (snprintf(buffer, buffer_size, "%s,%s", item->macho_segment, item->section_macho) >= (int)buffer_size) die("mach-o section name too long");
      return buffer;
    }
    if (item->section_macho[0]) return item->section_macho;
  }
  if (item->section[0]) return item->section;
  return NULL;
}

static void apply_global_section(Codegen *codegen, Node *item, LLVMValueRef global) {
  char composed[512];
  const char *section = resolve_section(item, (ObjectFormat)codegen->object_format, composed, sizeof(composed));
  if (section) LLVMSetSection(global, section);
}

static int global_has_section(Node *item, ObjectFormat format) {
  char composed[512];
  return resolve_section(item, format, composed, sizeof(composed)) != NULL;
}

#define GROWABLE_ARRAY_CAPACITY 256u

static Local *find_local(Codegen *codegen, const char *name);
static Node *global_named(Codegen *codegen, const char *name);
static LLVMTypeRef dynamic_element_type(Codegen *codegen, Node *item);

static int is_empty_array_literal(Node *item) {
  return item && item->kind == N_ARRAY && !item->a;
}

static int is_growable_array_declaration(Node *item) {
  return item && item->is_dynamic_array && is_empty_array_literal(item->a);
}

#define CONSTANT_ARRAY_CAPACITY 1024u

static int evaluate_constant_integer(Node *item, uint64_t *value) {
  if (!item) return 0;
  if (item->kind == N_INT) {
    *value = item->number;
    return 1;
  }
  if (item->kind == N_UNARY && !strcmp(item->text, "-")) {
    uint64_t operand;
    if (!evaluate_constant_integer(item->a, &operand)) return 0;
    *value = (uint64_t)(-(int64_t)operand);
    return 1;
  }
  if (item->kind == N_UNARY && !strcmp(item->text, "~")) {
    uint64_t operand;
    if (!evaluate_constant_integer(item->a, &operand)) return 0;
    *value = ~operand;
    return 1;
  }
  if (item->kind == N_BINARY) {
    uint64_t left, right;
    if (!evaluate_constant_integer(item->a, &left) || !evaluate_constant_integer(item->b, &right)) return 0;
    if (!strcmp(item->text, "+")) { *value = left + right; return 1; }
    if (!strcmp(item->text, "-")) { *value = left - right; return 1; }
    if (!strcmp(item->text, "*")) { *value = left * right; return 1; }
  }
  return 0;
}

static Node *integer_node(uint64_t value) {
  Node *item = calloc(1, sizeof(*item));
  if (!item) die("out of memory");
  item->kind = N_INT;
  item->number = value;
  item->value = (int)value;
  return item;
}

static Node *expand_constant_array(Node *item) {
  if (!item) return NULL;
  if (item->kind == N_ARRAY) return item;
  if (item->kind == N_CALL && !strcmp(item->text, "Array.create")) {
    if (!item->a || !item->a->next || item->a->next->next) die("Array.create expects (length, value)");
    uint64_t length, fill;
    if (!evaluate_constant_integer(item->a, &length) || !evaluate_constant_integer(item->a->next, &fill)) die("Array.create requires constant integer arguments");
    if (length > CONSTANT_ARRAY_CAPACITY) die("Array.create length is too large");
    Node *array = calloc(1, sizeof(*array));
    if (!array) die("out of memory");
    array->kind = N_ARRAY;
    Node **tail = &array->a;
    for (uint64_t index = 0; index < length; index++) {
      *tail = integer_node(fill);
      tail = &(*tail)->next;
    }
    return array;
  }
  if (item->kind == N_CALL && !strcmp(item->text, "concat")) {
    if (!item->b || !item->a || item->a->next) die("concat expects one array argument");
    Node *left = expand_constant_array(item->b);
    Node *right = expand_constant_array(item->a);
    if (!left || !right) die("concat requires constant array operands");
    Node *array = calloc(1, sizeof(*array));
    if (!array) die("out of memory");
    array->kind = N_ARRAY;
    Node **tail = &array->a;
    unsigned count = 0;
    for (Node *value = left->a; value; value = value->next) {
      if (count++ == CONSTANT_ARRAY_CAPACITY) die("concat result is too large");
      uint64_t number;
      if (!evaluate_constant_integer(value, &number)) die("concat requires integer array elements");
      *tail = integer_node(number);
      tail = &(*tail)->next;
    }
    for (Node *value = right->a; value; value = value->next) {
      if (count++ == CONSTANT_ARRAY_CAPACITY) die("concat result is too large");
      uint64_t number;
      if (!evaluate_constant_integer(value, &number)) die("concat requires integer array elements");
      *tail = integer_node(number);
      tail = &(*tail)->next;
    }
    return array;
  }
  return NULL;
}

static LLVMTypeRef growable_array_type(Codegen *codegen, LLVMTypeRef element_type) {
  LLVMTypeRef fields[2];
  fields[0] = codegen->i32;
  fields[1] = LLVMArrayType2(element_type, GROWABLE_ARRAY_CAPACITY);
  return LLVMStructTypeInContext(codegen->context, fields, 2, 0);
}

static LLVMValueRef growable_array_length_address(Codegen *codegen, LLVMValueRef array_address, LLVMTypeRef array_type) {
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
  return LLVMBuildGEP2(codegen->builder, array_type, array_address, indices, 2, "array.length");
}

static LLVMValueRef growable_array_data_address(Codegen *codegen, LLVMValueRef array_address, LLVMTypeRef array_type) {
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 1, 0), LLVMConstInt(codegen->i32, 0, 0) };
  return LLVMBuildGEP2(codegen->builder, array_type, array_address, indices, 3, "array.data");
}

static LLVMValueRef growable_array_element_address(Codegen *codegen, LLVMValueRef array_address, LLVMTypeRef array_type, LLVMValueRef index) {
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 1, 0), index };
  return LLVMBuildGEP2(codegen->builder, array_type, array_address, indices, 3, "array.element");
}

static LLVMValueRef growable_array_zero_element(Codegen *codegen, LLVMTypeRef element_type) {
  if (element_type == codegen->pointer_type) return LLVMConstPointerNull(codegen->pointer_type);
  if (LLVMGetTypeKind(element_type) == LLVMIntegerTypeKind) return LLVMConstInt(element_type, 0, 0);
  return LLVMConstNull(element_type);
}

static int call_method_name(const char *qualified, const char *method, char *receiver, size_t receiver_size) {
  size_t method_length = strlen(method);
  size_t length = strlen(qualified);
  if (length <= method_length + 1) return 0;
  if (qualified[length - method_length - 1] != '.') return 0;
  if (strcmp(qualified + length - method_length, method)) return 0;
  size_t receiver_length = length - method_length - 1;
  if (receiver_length >= receiver_size) return 0;
  memcpy(receiver, qualified, receiver_length);
  receiver[receiver_length] = '\0';
  return 1;
}

static int resolve_growable_array(Codegen *codegen, const char *name, LLVMValueRef *address_out, LLVMTypeRef *array_type_out, LLVMTypeRef *element_type_out) {
  Local *item_local = find_local(codegen, name);
  if (item_local && item_local->is_growable_array) {
    *address_out = item_local->address;
    *array_type_out = item_local->type;
    *element_type_out = item_local->dynamic_element_type;
    return 1;
  }
  char receiver[256], field_name[256];
  if (split_receiver_method(name, receiver, sizeof(receiver), field_name, sizeof(field_name))) {
    Local *object = find_local(codegen, receiver);
    ClassInfo *info = object && object->class_name[0] ? class_table_find(&codegen->classes, object->class_name) : NULL;
    ClassField *field = info ? class_info_find_field(info, field_name) : NULL;
    if (field && field->declaration->is_dynamic_array && !field->declaration->a) {
      LLVMTypeRef field_type;
      LLVMValueRef field_address = object_field_address(codegen, object, field_name, &field_type);
      LLVMTypeRef element_type = dynamic_element_type(codegen, field->declaration);
      if (!field_address || !element_type) return 0;
      *address_out = field_address;
      *array_type_out = field_type;
      *element_type_out = element_type;
      return 1;
    }
  }
  Node *global = global_named(codegen, name);
  if (global && is_growable_array_declaration(global)) {
    LLVMValueRef global_value = LLVMGetNamedGlobal(codegen->module, global->text);
    LLVMTypeRef element_type = dynamic_element_type(codegen, global);
    if (!global_value || !element_type) return 0;
    *address_out = global_value;
    *array_type_out = LLVMGlobalGetValueType(global_value);
    *element_type_out = element_type;
    return 1;
  }
  return 0;
}

static Local *find_local(Codegen *codegen, const char *name) { for (Local *item = codegen->locals; item; item = item->next) if (!strcmp(item->name, name)) return item; return NULL; }
static Local *local(Codegen *codegen, const char *name, Node *origin) {
  Local *item = find_local(codegen, name);
  if (item) return item;
  char message[320];
  snprintf(message, sizeof(message), "unknown variable '%s'", name);
  die_at(codegen, origin, message);
  return NULL;
}

/* SysV x86-64 integer ABI alignment (Clang/GCC): i8→1 … i64→8, i128→16. */
static unsigned sysv_integer_abi_alignment(unsigned width_bits) {
  unsigned bytes = (width_bits + 7) / 8;
  if (bytes <= 1) return 1;
  if (bytes <= 2) return 2;
  if (bytes <= 4) return 4;
  if (bytes <= 8) return 8;
  return 16;
}

/* Host/SysV testing: prefer module DataLayout ABI align; fall back for bare integers. */
static unsigned sysv_type_abi_alignment(Codegen *codegen, LLVMTypeRef type) {
  if (codegen->target_abi != TARGET_ABI_SYSV) return 0;
  LLVMTargetDataRef target_data = LLVMGetModuleDataLayout(codegen->module);
  if (target_data) {
    unsigned alignment = LLVMABIAlignmentOfType(target_data, type);
    if (alignment) return alignment;
  }
  if (LLVMGetTypeKind(type) == LLVMIntegerTypeKind) return sysv_integer_abi_alignment(LLVMGetIntTypeWidth(type));
  return 0;
}

/* Stamp Clang-like x86_64 SysV layout early so i128 gets ABI align 16 under lli/host tests. */
static void apply_sysv_host_data_layout(Codegen *codegen) {
  if (codegen->target_abi != TARGET_ABI_SYSV || !codegen->is_x86_64) return;
  LLVMSetDataLayout(codegen->module,
                    "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128");
}

/* Allocas must live in the entry block. An alloca inside a loop body becomes a
   dynamic stack adjustment every iteration and never pops — classic kernel stack
   smash once RSP walks off the mapped page. */
static LLVMValueRef build_entry_alloca(Codegen *codegen, LLVMTypeRef type, const char *name, unsigned alignment) {
  LLVMBasicBlockRef current = LLVMGetInsertBlock(codegen->builder);
  LLVMValueRef function = codegen->main_fn;
  if (!function && current) function = LLVMGetBasicBlockParent(current);
  if (!function) die("alloca requires an active function");
  LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(function);
  LLVMValueRef first = LLVMGetFirstInstruction(entry);
  if (first) LLVMPositionBuilderBefore(codegen->builder, first);
  else LLVMPositionBuilderAtEnd(codegen->builder, entry);
  LLVMValueRef address = LLVMBuildAlloca(codegen->builder, type, name);
  if (alignment) LLVMSetAlignment(address, alignment);
  if (current) LLVMPositionBuilderAtEnd(codegen->builder, current);
  return address;
}

static Local *add_local_aligned(Codegen *codegen, const char *name, LLVMTypeRef type, LLVMValueRef value, LLVMValueRef string_length, int integer_unsigned, unsigned alignment) {
  Local *item = calloc(1, sizeof(*item));
  if (!item) die("out of memory");
  strcpy(item->name, name);
  item->type = type;
  item->string_length = string_length;
  item->integer_unsigned = integer_unsigned;
  if (!alignment) alignment = sysv_type_abi_alignment(codegen, type);
  item->alignment = alignment;
  item->address = build_entry_alloca(codegen, type, name, alignment);
  LLVMValueRef store = LLVMBuildStore(codegen->builder, value, item->address);
  if (alignment) LLVMSetAlignment(store, alignment);
  item->next = codegen->locals;
  codegen->locals = item;
  return item;
}

static Local *add_local(Codegen *codegen, const char *name, LLVMTypeRef type, LLVMValueRef value, LLVMValueRef string_length, int integer_unsigned) {
  return add_local_aligned(codegen, name, type, value, string_length, integer_unsigned, 0);
}
static LLVMValueRef as_boolean(Codegen *codegen, LLVMValueRef value, LLVMTypeRef type) {
  if (type == codegen->i1) return value;
  if (type == codegen->f32) return LLVMBuildFCmp(codegen->builder, LLVMRealONE, value, LLVMConstReal(codegen->f32, 0.0), "ftest");
  if (type == codegen->f64) return LLVMBuildFCmp(codegen->builder, LLVMRealONE, value, LLVMConstReal(codegen->f64, 0.0), "ftest");
  if (type == codegen->pointer_type) return LLVMBuildICmp(codegen->builder, LLVMIntNE, value, LLVMConstNull(type), "ptest");
  return LLVMBuildICmp(codegen->builder, LLVMIntNE, value, LLVMConstInt(type, 0, 0), "test");
}
/* Integer scratch width follows the target word (i64 on amd64 long mode), not a hard-coded i32. */
static LLVMValueRef as_integer(Codegen *codegen, LLVMValueRef value, LLVMTypeRef type) {
  if (type == codegen->word_type) return value;
  if (type == codegen->f32 || type == codegen->f64) {
    return LLVMBuildFPToSI(codegen->builder, value, codegen->word_type, "fptosi");
  }
  if (LLVMGetTypeKind(type) != LLVMIntegerTypeKind) {
    die("as_integer requires an integer or float value");
    return value;
  }
  unsigned width = LLVMGetIntTypeWidth(type);
  if (width > codegen->word_bits) return LLVMBuildTrunc(codegen->builder, value, codegen->word_type, "truncate");
  if (width < codegen->word_bits) return LLVMBuildZExt(codegen->builder, value, codegen->word_type, "extend");
  return value;
}
static int expression_integer_unsigned(Codegen *codegen, Node *item) {
  if (!item) return 1;
  if (item->kind == N_CAST || item->kind == N_VAR || item->kind == N_GLOBAL) return item->integer_unsigned;
  if (item->kind == N_INT || item->kind == N_NULL) return 1;
  if (item->kind == N_UNARY && item->a) return expression_integer_unsigned(codegen, item->a);
  if (item->kind == N_BINARY && item->a && item->b) {
    return expression_integer_unsigned(codegen, item->a) && expression_integer_unsigned(codegen, item->b);
  }
  if (item->kind == N_NAME) {
    Local *item_local = find_local(codegen, item->text);
    if (item_local) return item_local->integer_unsigned;
    char receiver[256], field_name[256];
    if (split_receiver_method(item->text, receiver, sizeof(receiver), field_name, sizeof(field_name))) {
      Local *object = find_local(codegen, receiver);
      ClassInfo *info = object && object->class_name[0] ? class_table_find(&codegen->classes, object->class_name) : NULL;
      if (!info) {
        Node *global = global_named(codegen, receiver);
        if (global) info = class_from_typed_declaration(codegen, global);
      }
      ClassField *field = info ? class_info_find_field(info, field_name) : NULL;
      if (field) return field->declaration->integer_unsigned;
    }
    Node *global = global_named(codegen, item->text);
    if (global) return global->integer_unsigned;
  }
  if (item->kind == N_FIELD && item->a && item->a->kind == N_NAME) {
    Local *object = find_local(codegen, item->a->text);
    ClassInfo *info = object && object->class_name[0] ? class_table_find(&codegen->classes, object->class_name) : NULL;
    ClassField *field = info ? class_info_find_field(info, item->text) : NULL;
    if (field) return field->declaration->integer_unsigned;
  }
  /* Untyped / unknown — treat as unsigned (UInt / addresses are the common case). */
  return 1;
}
static int type_is_float(Codegen *codegen, LLVMTypeRef type) {
  return type == codegen->f32 || type == codegen->f64;
}
static void note_float_use(Codegen *codegen) { codegen->uses_float = 1; }
/* Ptr<*> loads/stores are treated as volatile so MMIO spin-waits are not LICM'd into empty loops. */
static LLVMValueRef load_volatile(Codegen *codegen, LLVMTypeRef type, LLVMValueRef pointer, const char *name) {
  LLVMValueRef load = LLVMBuildLoad2(codegen->builder, type, pointer, name);
  LLVMSetVolatile(load, 1);
  return load;
}
static void store_volatile(Codegen *codegen, LLVMValueRef value, LLVMValueRef pointer) {
  LLVMSetVolatile(LLVMBuildStore(codegen->builder, value, pointer), 1);
}
static LLVMValueRef cast_to_float(Codegen *codegen, LLVMValueRef value, LLVMTypeRef from, LLVMTypeRef to) {
  if (from == to) return value;
  if (LLVMGetTypeKind(from) == LLVMIntegerTypeKind) return LLVMBuildSIToFP(codegen->builder, value, to, "sitofp");
  if (from == codegen->f32 && to == codegen->f64) return LLVMBuildFPExt(codegen->builder, value, to, "fpext");
  if (from == codegen->f64 && to == codegen->f32) return LLVMBuildFPTrunc(codegen->builder, value, to, "fptrunc");
  die("unsupported float conversion");
  return value;
}
static LLVMValueRef convert_integer(Codegen *codegen, LLVMValueRef value, LLVMTypeRef source_type, LLVMTypeRef destination_type, int destination_unsigned) {
  unsigned source_width = LLVMGetIntTypeWidth(source_type);
  unsigned destination_width = LLVMGetIntTypeWidth(destination_type);
  if (source_width == destination_width) return value;
  if (source_width > destination_width) return LLVMBuildTrunc(codegen->builder, value, destination_type, "narrow");
  return destination_unsigned ? LLVMBuildZExt(codegen->builder, value, destination_type, "zextend") : LLVMBuildSExt(codegen->builder, value, destination_type, "sextend");
}
static LLVMValueRef coerce_value(Codegen *codegen, Node *origin, LLVMValueRef value, LLVMTypeRef from, LLVMTypeRef to, int to_unsigned, const char *mismatch) {
  if (from == to) return value;
  if (from == codegen->pointer_type && LLVMGetTypeKind(to) == LLVMIntegerTypeKind) {
    return LLVMBuildPtrToInt(codegen->builder, value, to, "ptrtoint");
  }
  if (LLVMGetTypeKind(from) == LLVMIntegerTypeKind && to == codegen->pointer_type) {
    return LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, value, from, codegen->i64, 1), to, "inttoptr");
  }
  if (type_is_float(codegen, from) || type_is_float(codegen, to)) {
    note_float_use(codegen);
    if (type_is_float(codegen, to) && (type_is_float(codegen, from) || LLVMGetTypeKind(from) == LLVMIntegerTypeKind))
      return cast_to_float(codegen, value, from, to);
    if (type_is_float(codegen, from) && LLVMGetTypeKind(to) == LLVMIntegerTypeKind)
      return to_unsigned ? LLVMBuildFPToUI(codegen->builder, value, to, "fptoui") : LLVMBuildFPToSI(codegen->builder, value, to, "fptosi");
  }
  if (LLVMGetTypeKind(from) != LLVMIntegerTypeKind || LLVMGetTypeKind(to) != LLVMIntegerTypeKind) die_at(codegen, origin, mismatch);
  return convert_integer(codegen, value, from, to, to_unsigned);
}
/* null and raw integer addresses have no pointee. Deref them as bytes, same as Ptr<Void>. */
static int raw_address_as_byte_pointer(Codegen *codegen, LLVMValueRef *pointer, LLVMTypeRef pointer_type) {
  if (pointer_type == codegen->pointer_type) {
    codegen->last_pointer_element_type = codegen->i8;
    codegen->last_pointer_element_unsigned = 1;
    return 1;
  }
  if (LLVMGetTypeKind(pointer_type) != LLVMIntegerTypeKind) return 0;
  *pointer = LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, *pointer, pointer_type, codegen->i64, 1), codegen->pointer_type, "inttoptr");
  codegen->last_pointer_element_type = codegen->i8;
  codegen->last_pointer_element_unsigned = 1;
  return 1;
}
static LLVMTypeRef declared_integer_type(Codegen *codegen, Node *item) {
  if (item->integer_width) return LLVMIntTypeInContext(codegen->context, item->integer_width);
  if (type_is_word_integer(item->type_name)) return codegen->word_type;
  return NULL;
}
static Node *typedef_named(Codegen *codegen, const char *name);
static LLVMTypeRef fixed_array_element_type(Codegen *codegen, Node *item) {
  if (item->fixed_element_is_pointer) return codegen->pointer_type;
  if (item->fixed_element_width) return LLVMIntTypeInContext(codegen->context, item->fixed_element_width);
  if (type_is_word_integer(item->fixed_element_type_name)) return codegen->word_type;
  Node *typedef_declaration = typedef_named(codegen, item->fixed_element_type_name);
  return typedef_declaration ? LLVMGetTypeByName2(codegen->context, typedef_declaration->text) : NULL;
}
static int is_fixed_array_decl(const Node *item) {
  return item && (item->fixed_array_length || item->is_flexible_array);
}
static LLVMTypeRef fixed_array_type(Codegen *codegen, Node *item) {
  LLVMTypeRef element_type = fixed_array_element_type(codegen, item);
  return element_type ? LLVMArrayType2(element_type, item->is_flexible_array ? 0 : item->fixed_array_length) : NULL;
}
static const char *unqualified_name(const char *name) { const char *separator = strrchr(name, '.'); return separator ? separator + 1 : name; }
static Node *typedef_named(Codegen *codegen, const char *name) { const char *simple_name = unqualified_name(name); for (Node *item = codegen->typedefs; item; item = item->next) if (!strcmp(item->text, simple_name)) return item; return NULL; }
static Node *enum_named(Codegen *codegen, const char *name) {
  const char *simple_name = unqualified_name(name);
  for (Node *item = codegen->typedefs; item; item = item->next) if (item->kind == N_ENUM && !strcmp(item->text, simple_name)) return item;
  return NULL;
}
static int enum_has_payload(Node *enumeration) {
  if (!enumeration || enumeration->kind != N_ENUM) return 0;
  for (Node *variant = enumeration->a; variant; variant = variant->next) if (variant->b) return 1;
  return 0;
}
static Node *enum_variant_in(Node *enumeration, const char *name) {
  const char *simple_name = unqualified_name(name);
  if (!enumeration) return NULL;
  for (Node *variant = enumeration->a; variant; variant = variant->next) if (!strcmp(variant->text, simple_name)) return variant;
  return NULL;
}
static Node *enum_for_variant(Codegen *codegen, const char *name, Node **variant_out) {
  const char *simple_name = unqualified_name(name);
  for (Node *enumeration = codegen->typedefs; enumeration; enumeration = enumeration->next) {
    if (enumeration->kind != N_ENUM) continue;
    Node *variant = enum_variant_in(enumeration, simple_name);
    if (variant) {
      if (variant_out) *variant_out = variant;
      return enumeration;
    }
  }
  return NULL;
}
static LLVMTypeRef enum_value_type(Codegen *codegen, Node *enumeration) {
  if (!enum_has_payload(enumeration)) return codegen->i32;
  LLVMTypeRef existing = LLVMGetTypeByName2(codegen->context, enumeration->text);
  if (existing) return existing;
  LLVMTypeRef type = LLVMStructCreateNamed(codegen->context, enumeration->text);
  LLVMTypeRef fields[2] = { codegen->i32, codegen->pointer_type };
  LLVMStructSetBody(type, fields, 2, 0);
  return type;
}
static LLVMValueRef emit_enum_value(Codegen *codegen, Node *enumeration, Node *variant, LLVMValueRef payload, LLVMTypeRef *type) {
  *type = enum_value_type(codegen, enumeration);
  if (!enum_has_payload(enumeration)) return LLVMConstInt(codegen->i32, variant->number, 0);
  LLVMValueRef values[2];
  values[0] = LLVMConstInt(codegen->i32, variant->number, 0);
  values[1] = payload ? payload : LLVMConstNull(codegen->pointer_type);
  if (LLVMIsConstant(values[0]) && LLVMIsConstant(values[1])) return LLVMConstNamedStruct(*type, values, 2);
  LLVMValueRef result = LLVMGetUndef(*type);
  result = LLVMBuildInsertValue(codegen->builder, result, values[0], 0, "enum.tag");
  return LLVMBuildInsertValue(codegen->builder, result, values[1], 1, "enum.payload");
}
static int enum_pattern_info(Codegen *codegen, Node *pattern, Node **enumeration_out, Node **variant_out) {
  if (!pattern) return 0;
  if (pattern->kind == N_NAME) {
    Node *variant = NULL;
    Node *enumeration = enum_for_variant(codegen, pattern->text, &variant);
    if (!enumeration) return 0;
    if (enumeration_out) *enumeration_out = enumeration;
    if (variant_out) *variant_out = variant;
    return 1;
  }
  if (pattern->kind == N_FIELD && pattern->a && pattern->a->kind == N_NAME) {
    Node *enumeration = enum_named(codegen, pattern->a->text);
    Node *variant = enumeration ? enum_variant_in(enumeration, pattern->text) : NULL;
    if (!variant) return 0;
    if (enumeration_out) *enumeration_out = enumeration;
    if (variant_out) *variant_out = variant;
    return 1;
  }
  if (pattern->kind == N_CALL) {
    Node *enumeration = NULL;
    Node *variant = NULL;
    if (pattern->b && pattern->b->kind == N_NAME) {
      enumeration = enum_named(codegen, pattern->b->text);
      variant = enumeration ? enum_variant_in(enumeration, pattern->text) : NULL;
    } else {
      enumeration = enum_for_variant(codegen, pattern->text, &variant);
    }
    if (!variant) return 0;
    if (enumeration_out) *enumeration_out = enumeration;
    if (variant_out) *variant_out = variant;
    return 1;
  }
  return 0;
}
static LLVMTypeRef pointer_element_type(Codegen *codegen, Node *item) {
  if (item->pointer_element_width) return LLVMIntTypeInContext(codegen->context, item->pointer_element_width);
  if (!item->pointer_type_name[0]) return NULL;
  if (type_is_word_integer(item->pointer_type_name)) return codegen->word_type;
  Node *typedef_declaration = typedef_named(codegen, item->pointer_type_name);
  return typedef_declaration ? LLVMGetTypeByName2(codegen->context, typedef_declaration->text) : NULL;
}

/* Field loads of Ptr<T> must stamp the pointee so *this.field / *record.ptr work. */
static void remember_pointer_element_from_decl(Codegen *codegen, Node *item) {
  LLVMTypeRef element;
  if (!item || !(item->pointer_element_width || item->pointer_type_name[0])) return;
  if (item->pointer_element_width) {
    codegen->last_pointer_element_type = LLVMIntTypeInContext(codegen->context, item->pointer_element_width);
    codegen->last_pointer_element_unsigned = item->pointer_element_unsigned;
    return;
  }
  if (!strcmp(item->pointer_type_name, "Void") || type_is_any(item->pointer_type_name) ||
      !strcmp(item->pointer_type_name, "Dynamic") || !strcmp(item->pointer_type_name, "String")) {
    codegen->last_pointer_element_type = codegen->i8;
    codegen->last_pointer_element_unsigned = 1;
    return;
  }
  {
    ClassInfo *class_info = class_table_find(&codegen->classes, item->pointer_type_name);
    if (class_info && class_info->struct_type) {
      codegen->last_pointer_element_type = class_info->struct_type;
      codegen->last_pointer_element_unsigned = 0;
      return;
    }
  }
  element = pointer_element_type(codegen, item);
  if (!element) return;
  codegen->last_pointer_element_type = element;
  codegen->last_pointer_element_unsigned = 0;
}
static LLVMTypeRef dynamic_element_type(Codegen *codegen, Node *item) {
  if (!item->is_dynamic_array) return NULL;
  if (item->dynamic_element_is_pointer) return codegen->pointer_type;
  if (item->dynamic_element_width) return LLVMIntTypeInContext(codegen->context, item->dynamic_element_width);
  if (!item->dynamic_element_type_name[0]) return NULL;
  if (type_is_word_integer(item->dynamic_element_type_name)) return codegen->word_type;
  if (!strcmp(item->dynamic_element_type_name, "String") || !strcmp(item->dynamic_element_type_name, "Dynamic") ||
      !strcmp(item->dynamic_element_type_name, "Any") || !strcmp(item->dynamic_element_type_name, "Void")) {
    return codegen->pointer_type;
  }
  if (class_table_find(&codegen->classes, item->dynamic_element_type_name)) return codegen->pointer_type;
  Node *enumeration = enum_named(codegen, item->dynamic_element_type_name);
  if (enumeration) return enum_value_type(codegen, enumeration);
  Node *typedef_declaration = typedef_named(codegen, item->dynamic_element_type_name);
  return typedef_declaration ? LLVMGetTypeByName2(codegen->context, typedef_declaration->text) : NULL;
}
static Node *global_named(Codegen *codegen, const char *name) {
  const char *simple_name = unqualified_name(name);
  int is_qualified = simple_name != name;
  for (Node *item = codegen->globals; item; item = item->next) {
    if (is_qualified && !item->is_imported) continue;
    if (!is_qualified && item->is_imported) continue;
    if (!strcmp(item->text, simple_name)) return item;
  }
  if (!is_qualified) {
    for (Node *item = codegen->globals; item; item = item->next) {
      if (item->is_imported && !strcmp(item->text, simple_name)) return item;
    }
  }
  return NULL;
}
static int same_field_name(const char *left, const char *right) { while (*left && *right) { if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return 0; } return !*left && !*right; }
static LLVMTypeRef declaration_type(Codegen *codegen, Node *item) {
  if (item->kind == N_TYPEDEF) return LLVMGetTypeByName2(codegen->context, item->text);
  if (is_fixed_array_decl(item)) return fixed_array_type(codegen, item);
  if (item->is_dynamic_array) return codegen->pointer_type;
  if (item->is_function_pointer) return codegen->pointer_type;
  if (!item->type_name[0] || !strcmp(item->type_name, "Dynamic") || !strcmp(item->type_name, "Any") || !strcmp(item->type_name, "String")) return codegen->pointer_type;
  if (!strcmp(item->type_name, "Int") || !strcmp(item->type_name, "UInt")) return codegen->word_type;
  if (!strcmp(item->type_name, "Float") || !strcmp(item->type_name, "Single")) {
    note_float_use(codegen);
    return !strcmp(item->type_name, "Single") ? codegen->f32 : codegen->f64;
  }
  if (item->integer_width) return declared_integer_type(codegen, item);
  if (item->pointer_element_width || item->pointer_type_name[0]) return codegen->pointer_type;
  if (!strcmp(item->type_name, "Bool")) return codegen->i1;
  if (!strcmp(item->type_name, "Void")) return codegen->pointer_type;
  if (enum_named(codegen, item->type_name)) return enum_value_type(codegen, enum_named(codegen, item->type_name));
  if (class_table_find(&codegen->classes, item->type_name)) return codegen->pointer_type;
  LLVMTypeRef type = LLVMGetTypeByName2(codegen->context, item->type_name);
  if (type) return type;
  Node *typedef_declaration = typedef_named(codegen, item->type_name);
  if (typedef_declaration) {
    type = LLVMGetTypeByName2(codegen->context, typedef_declaration->text);
    return type ? type : LLVMStructCreateNamed(codegen->context, typedef_declaration->text);
  }
  return NULL;
}
static LLVMTypeRef class_field_type(Codegen *codegen, Node *item) {
  if (item->is_dynamic_array && !item->a) {
    LLVMTypeRef element_type = dynamic_element_type(codegen, item);
    if (!element_type) return NULL;
    return growable_array_type(codegen, element_type);
  }
  return declaration_type(codegen, item);
}
static LLVMTypeRef function_return_type(Codegen *codegen, Node *function) {
  if (!strcmp(function->type_name, "Void")) return codegen->void_type;
  LLVMTypeRef type = declaration_type(codegen, function);
  return type ? type : codegen->i32;
}
static LLVMTypeRef cast_target_type(Codegen *codegen, Node *cast) {
  if (cast->integer_width) return LLVMIntTypeInContext(codegen->context, cast->integer_width);
  if (!strcmp(cast->type_name, "Int") || !strcmp(cast->type_name, "UInt")) return codegen->word_type;
  if (!strcmp(cast->type_name, "Float")) { note_float_use(codegen); return codegen->f64; }
  if (!strcmp(cast->type_name, "Single")) { note_float_use(codegen); return codegen->f32; }
  if (!strcmp(cast->type_name, "Bool")) return codegen->i1;
  if (!strcmp(cast->type_name, "Ptr") || !strcmp(cast->type_name, "Any") || !strcmp(cast->type_name, "Dynamic") || !strcmp(cast->type_name, "String") || !strcmp(cast->type_name, "Void")) return codegen->pointer_type;
  Node *typedef_declaration = typedef_named(codegen, cast->type_name);
  return typedef_declaration ? LLVMGetTypeByName2(codegen->context, typedef_declaration->text) : NULL;
}
static Function *function_value(Codegen *codegen, const char *name) {
  const char *simple_name = unqualified_name(name);
  for (Function *item = codegen->function_values; item; item = item->next) if (!strcmp(item->declaration->text, simple_name)) return item;
  return NULL;
}
static Node *typedef_field_named(Node *typedef_declaration, const char *name, unsigned *index) {
  unsigned field_index = 0;
  for (Node *field = typedef_declaration->a; field; field = field->next, field_index++) {
    if (same_field_name(field->text, name)) {
      if (index) *index = field_index;
      return field;
    }
  }
  return NULL;
}
static Node *typedef_for_type(Codegen *codegen, LLVMTypeRef type) {
  for (Node *item = codegen->typedefs; item; item = item->next) {
    if (LLVMGetTypeByName2(codegen->context, item->text) == type) return item;
  }
  return NULL;
}
static void emit_typedefs(Codegen *codegen) {
  for (Node *item = codegen->typedefs; item; item = item->next) {
    if (item->kind == N_ENUM && enum_has_payload(item)) enum_value_type(codegen, item);
    if (item->kind != N_TYPEDEF) continue;
    if (!LLVMGetTypeByName2(codegen->context, item->text)) LLVMStructCreateNamed(codegen->context, item->text);
  }
  for (Node *item = codegen->typedefs; item; item = item->next) {
    if (item->kind != N_TYPEDEF) continue;
    LLVMTypeRef fields[256];
    unsigned count = 0;
    for (Node *field = item->a; field; field = field->next) {
      if (field->is_flexible_array && field->next) die("flexible FixedArray must be the last field of a typedef");
      if (count == 256 || !(fields[count] = declaration_type(codegen, field))) die("unsupported typedef field type");
      count++;
    }
    LLVMStructSetBody(LLVMGetTypeByName2(codegen->context, item->text), fields, count, item->is_packed);
  }
}
static void emit_statement(Codegen *codegen, Node *item);
static LLVMValueRef emit_expression(Codegen *codegen, Node *item, LLVMTypeRef *type);
static LLVMValueRef emit_switch_expression(Codegen *codegen, Node *item, LLVMTypeRef *type);
static LLVMValueRef string_value_length(Codegen *codegen, LLVMValueRef value);

#define HAXELLVM_STRING_SCRATCH_SIZE 1024

static void remember_string_scratch(Codegen *codegen, LLVMValueRef buffer) {
  if (codegen->string_scratch_count == sizeof(codegen->string_scratch_buffers) / sizeof(*codegen->string_scratch_buffers)) die("too many string scratch buffers in one function");
  codegen->string_scratch_buffers[codegen->string_scratch_count++] = buffer;
}

static int is_string_scratch(Codegen *codegen, LLVMValueRef value) {
  for (unsigned index = 0; index < codegen->string_scratch_count; index++) {
    if (codegen->string_scratch_buffers[index] == value) return 1;
  }
  return 0;
}

static LLVMValueRef format_integer_string(Codegen *codegen, LLVMValueRef value, LLVMTypeRef value_type, unsigned base) {
  if (base < 2 || base > 16) die("toString base must be between 2 and 16");
  LLVMTypeRef buffer_type = LLVMArrayType2(codegen->i8, 66);
  LLVMValueRef buffer = build_entry_alloca(codegen, buffer_type, "integer.string", 0);
  LLVMValueRef zero = LLVMConstInt(codegen->i32, 0, 0);
  LLVMValueRef end_index = LLVMConstInt(codegen->i32, 65, 0);
  LLVMValueRef end = LLVMBuildGEP2(codegen->builder, buffer_type, buffer, (LLVMValueRef[]){ zero, end_index }, 2, "integer.string.end");
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i8, 0, 0), end);
  LLVMValueRef number = LLVMGetIntTypeWidth(value_type) < 64 ? LLVMBuildZExt(codegen->builder, value, codegen->i64, "integer.extend") : value;
  LLVMValueRef number_slot = build_entry_alloca(codegen, codegen->i64, "integer.value", 0);
  LLVMValueRef index_slot = build_entry_alloca(codegen, codegen->i32, "integer.index", 0);
  LLVMBuildStore(codegen->builder, number, number_slot);
  LLVMBuildStore(codegen->builder, end_index, index_slot);
  LLVMBasicBlockRef zero_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "integer.zero");
  LLVMBasicBlockRef digit_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "integer.digit");
  LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "integer.done");
  LLVMBuildCondBr(codegen->builder, LLVMBuildICmp(codegen->builder, LLVMIntEQ, number, LLVMConstInt(codegen->i64, 0, 0), "integer.is_zero"), zero_block, digit_block);
  LLVMPositionBuilderAtEnd(codegen->builder, zero_block);
  LLVMValueRef zero_index = LLVMConstInt(codegen->i32, 64, 0);
  LLVMValueRef zero_character = LLVMBuildGEP2(codegen->builder, buffer_type, buffer, (LLVMValueRef[]){ zero, zero_index }, 2, "integer.zero.character");
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i8, '0', 0), zero_character);
  LLVMBuildStore(codegen->builder, zero_index, index_slot);
  LLVMBuildBr(codegen->builder, done_block);
  LLVMPositionBuilderAtEnd(codegen->builder, digit_block);
  LLVMValueRef current = LLVMBuildLoad2(codegen->builder, codegen->i64, number_slot, "integer.current");
  LLVMValueRef remainder = LLVMBuildURem(codegen->builder, current, LLVMConstInt(codegen->i64, base, 0), "integer.remainder");
  LLVMValueRef index = LLVMBuildSub(codegen->builder, LLVMBuildLoad2(codegen->builder, codegen->i32, index_slot, "integer.index"), LLVMConstInt(codegen->i32, 1, 0), "integer.index.next");
  LLVMBuildStore(codegen->builder, index, index_slot);
  LLVMValueRef character = LLVMBuildGEP2(codegen->builder, buffer_type, buffer, (LLVMValueRef[]){ zero, index }, 2, "integer.character");
  LLVMValueRef digit = LLVMBuildTrunc(codegen->builder, remainder, codegen->i8, "integer.digit");
  LLVMValueRef is_decimal = LLVMBuildICmp(codegen->builder, LLVMIntULT, remainder, LLVMConstInt(codegen->i64, 10, 0), "integer.decimal");
  LLVMValueRef digit_character = LLVMBuildSelect(codegen->builder, is_decimal, LLVMBuildAdd(codegen->builder, digit, LLVMConstInt(codegen->i8, '0', 0), "integer.decimal.character"), LLVMBuildAdd(codegen->builder, digit, LLVMConstInt(codegen->i8, 'a' - 10, 0), "integer.hex.character"), "integer.character.value");
  LLVMBuildStore(codegen->builder, digit_character, character);
  LLVMValueRef next = LLVMBuildUDiv(codegen->builder, current, LLVMConstInt(codegen->i64, base, 0), "integer.next");
  LLVMBuildStore(codegen->builder, next, number_slot);
  LLVMBuildCondBr(codegen->builder, LLVMBuildICmp(codegen->builder, LLVMIntNE, next, LLVMConstInt(codegen->i64, 0, 0), "integer.more"), digit_block, done_block);
  LLVMPositionBuilderAtEnd(codegen->builder, done_block);
  LLVMValueRef start = LLVMBuildLoad2(codegen->builder, codegen->i32, index_slot, "integer.start");
  return LLVMBuildGEP2(codegen->builder, buffer_type, buffer, (LLVMValueRef[]){ zero, start }, 2, "integer.string.start");
}

/* Freestanding float→decimal: SSE cvttsd2si / fptosi then digit walk. No libc dtoa. */
static LLVMValueRef format_float_string(Codegen *codegen, LLVMValueRef value, LLVMTypeRef value_type) {
  note_float_use(codegen);
  if (value_type == codegen->f32) value = LLVMBuildFPExt(codegen->builder, value, codegen->f64, "float.promote");
  LLVMValueRef as_int = LLVMBuildFPToSI(codegen->builder, value, codegen->i64, "float.trunc");
  return format_integer_string(codegen, as_int, codegen->i64, 10);
}

/* Byte FixedArray → NUL-terminated scratch C string (ACPI sigs / OEM IDs). */
static LLVMValueRef fixed_array_as_cstring(Codegen *codegen, LLVMValueRef array, LLVMTypeRef array_type) {
  unsigned length = (unsigned)LLVMGetArrayLength2(array_type);
  LLVMTypeRef element_type = LLVMGetElementType(array_type);
  LLVMTypeRef scratch_type = LLVMArrayType2(codegen->i8, length + 1);
  LLVMValueRef scratch = build_entry_alloca(codegen, scratch_type, "fixedarray.string", 0);
  LLVMValueRef zero = LLVMConstInt(codegen->i32, 0, 0);
  LLVMValueRef data = LLVMBuildGEP2(codegen->builder, scratch_type, scratch, (LLVMValueRef[]){ zero, zero }, 2, "fixedarray.string.data");
  remember_string_scratch(codegen, data);
  for (unsigned index = 0; index < length; index++) {
    LLVMValueRef element = LLVMBuildExtractValue(codegen->builder, array, index, "fixedarray.byte");
    LLVMValueRef byte = convert_integer(codegen, element, element_type, codegen->i8, 1);
    LLVMValueRef slot = LLVMBuildGEP2(codegen->builder, scratch_type, scratch, (LLVMValueRef[]){ zero, LLVMConstInt(codegen->i32, index, 0) }, 2, "fixedarray.slot");
    LLVMBuildStore(codegen->builder, byte, slot);
  }
  LLVMValueRef terminator = LLVMBuildGEP2(codegen->builder, scratch_type, scratch, (LLVMValueRef[]){ zero, LLVMConstInt(codegen->i32, length, 0) }, 2, "fixedarray.nul");
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i8, 0, 0), terminator);
  return data;
}

static void emit_memcpy(Codegen *codegen, LLVMValueRef destination, unsigned dest_align, LLVMValueRef source, unsigned source_align, LLVMValueRef length) {
  if (!codegen->is_x86_64) {
    LLVMBuildMemCpy(codegen->builder, destination, dest_align, source, source_align, length);
    return;
  }
  /* Freestanding x86_64: REP MOVSB — no llvm.memcpy → SSE before the FPU is online. */
  LLVMTypeRef length_type = LLVMTypeOf(length);
  if (LLVMGetTypeKind(length_type) != LLVMIntegerTypeKind) die("memcpy length must be an integer");
  length = convert_integer(codegen, length, length_type, codegen->i64, 1);
  LLVMTypeRef param_types[] = { codegen->pointer_type, codegen->pointer_type, codegen->i64 };
  LLVMTypeRef asm_type = LLVMFunctionType(codegen->void_type, param_types, 3, 0);
  const char *assembly = "cld; rep movsb";
  const char *constraints = "{rdi},{rsi},{rcx},~{dirflag},~{fpsr},~{flags},~{rdi},~{rsi},~{rcx},~{memory}";
  LLVMValueRef inline_asm = LLVMGetInlineAsm(asm_type, assembly, strlen(assembly), constraints, strlen(constraints), 1, 0, LLVMInlineAsmDialectIntel, 0);
  LLVMValueRef arguments[] = { destination, source, length };
  LLVMBuildCall2(codegen->builder, asm_type, inline_asm, arguments, 3, "");
}

static void emit_memset(Codegen *codegen, LLVMValueRef destination, LLVMValueRef fill_byte, LLVMValueRef length, unsigned alignment) {
  if (!codegen->is_x86_64) {
    LLVMBuildMemSet(codegen->builder, destination, fill_byte, length, alignment);
    return;
  }
  /* Freestanding x86_64: REP STOSB — scalar fill, Intel syntax. */
  LLVMTypeRef byte_type = LLVMTypeOf(fill_byte);
  if (LLVMGetTypeKind(byte_type) != LLVMIntegerTypeKind) die("memset fill must be an integer");
  fill_byte = convert_integer(codegen, fill_byte, byte_type, codegen->i8, 1);
  LLVMTypeRef length_type = LLVMTypeOf(length);
  if (LLVMGetTypeKind(length_type) != LLVMIntegerTypeKind) die("memset length must be an integer");
  length = convert_integer(codegen, length, length_type, codegen->i64, 1);
  LLVMTypeRef param_types[] = { codegen->pointer_type, codegen->i8, codegen->i64 };
  LLVMTypeRef asm_type = LLVMFunctionType(codegen->void_type, param_types, 3, 0);
  const char *assembly = "cld; rep stosb";
  const char *constraints = "{rdi},{al},{rcx},~{dirflag},~{fpsr},~{flags},~{rdi},~{rcx},~{memory}";
  LLVMValueRef inline_asm = LLVMGetInlineAsm(asm_type, assembly, strlen(assembly), constraints, strlen(constraints), 1, 0, LLVMInlineAsmDialectIntel, 0);
  LLVMValueRef arguments[] = { destination, fill_byte, length };
  LLVMBuildCall2(codegen->builder, asm_type, inline_asm, arguments, 3, "");
}

static LLVMValueRef concat_strings(Codegen *codegen, LLVMValueRef left, LLVMValueRef right) {
  LLVMTypeRef buffer_type = LLVMArrayType2(codegen->i8, HAXELLVM_STRING_SCRATCH_SIZE);
  LLVMValueRef zero = LLVMConstInt(codegen->i32, 0, 0);
  LLVMValueRef destination;
  if (is_string_scratch(codegen, left)) destination = left;
  else {
    LLVMValueRef buffer = build_entry_alloca(codegen, buffer_type, "string.concat", 0);
    destination = LLVMBuildGEP2(codegen->builder, buffer_type, buffer, (LLVMValueRef[]){ zero, zero }, 2, "string.concat.data");
    remember_string_scratch(codegen, destination);
  }
  LLVMValueRef left_length = string_value_length(codegen, left);
  LLVMValueRef right_length = string_value_length(codegen, right);
  LLVMValueRef total_length = LLVMBuildAdd(codegen->builder, left_length, right_length, "string.concat.length");
  if (LLVMIsAConstantInt(total_length) && LLVMConstIntGetZExtValue(total_length) >= HAXELLVM_STRING_SCRATCH_SIZE) die("string concatenation exceeds scratch buffer");
  emit_memcpy(codegen, destination, 1, left, 1, left_length);
  LLVMValueRef right_destination = LLVMBuildGEP2(codegen->builder, codegen->i8, destination, &left_length, 1, "string.concat.right");
  emit_memcpy(codegen, right_destination, 1, right, 1, right_length);
  LLVMValueRef terminator = LLVMBuildGEP2(codegen->builder, codegen->i8, destination, &total_length, 1, "string.concat.terminator");
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i8, 0, 0), terminator);
  return destination;
}

static LLVMValueRef pad_string(Codegen *codegen, LLVMValueRef text, LLVMValueRef width, LLVMValueRef pad, int left) {
  LLVMTypeRef buffer_type = LLVMArrayType2(codegen->i8, HAXELLVM_STRING_SCRATCH_SIZE);
  LLVMValueRef buffer = build_entry_alloca(codegen, buffer_type, left ? "string.lpad" : "string.rpad", 0);
  LLVMValueRef zero = LLVMConstInt(codegen->i32, 0, 0);
  LLVMValueRef destination = LLVMBuildGEP2(codegen->builder, buffer_type, buffer, (LLVMValueRef[]){ zero, zero }, 2, "string.pad.data");
  remember_string_scratch(codegen, destination);
  LLVMValueRef text_length = string_value_length(codegen, text);
  /* width comes from as_integer (word-sized); keep length on the same width. */
  width = convert_integer(codegen, width, LLVMTypeOf(width), codegen->word_type, 1);
  text_length = convert_integer(codegen, text_length, LLVMTypeOf(text_length), codegen->word_type, 1);
  LLVMValueRef output_length = LLVMBuildSelect(codegen->builder, LLVMBuildICmp(codegen->builder, LLVMIntUGT, width, text_length, "string.pad.needed"), width, text_length, "string.pad.length");
  if (LLVMIsAConstantInt(output_length) && LLVMConstIntGetZExtValue(output_length) >= HAXELLVM_STRING_SCRATCH_SIZE) die("string padding exceeds scratch buffer");
  LLVMValueRef pad_length = LLVMBuildSub(codegen->builder, output_length, text_length, "string.pad.count");
  LLVMValueRef pad_byte = LLVMBuildLoad2(codegen->builder, codegen->i8, pad, "string.pad.byte");
  LLVMValueRef pad_destination = left ? destination : LLVMBuildGEP2(codegen->builder, codegen->i8, destination, &text_length, 1, "string.pad.start");
  emit_memset(codegen, pad_destination, pad_byte, pad_length, 1);
  LLVMValueRef text_destination = left ? LLVMBuildGEP2(codegen->builder, codegen->i8, destination, &pad_length, 1, "string.pad.text") : destination;
  emit_memcpy(codegen, text_destination, 1, text, 1, text_length);
  LLVMValueRef terminator = LLVMBuildGEP2(codegen->builder, codegen->i8, destination, &output_length, 1, "string.pad.terminator");
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i8, 0, 0), terminator);
  return destination;
}
static LLVMValueRef emit_constant(Codegen *codegen, Node *item, const char *parameter, uint64_t argument, LLVMTypeRef *type);
static LLVMValueRef global_field_value(Codegen *codegen, const char *name, LLVMTypeRef *type, Node **final_field) {
  const char *separator = strchr(name, '.');
  if (!separator) return NULL;
  char global_name[256];
  size_t global_name_length = (size_t)(separator - name);
  if (global_name_length >= sizeof(global_name)) return NULL;
  memcpy(global_name, name, global_name_length);
  global_name[global_name_length] = '\0';
  Node *global_declaration = global_named(codegen, global_name);
  if (!global_declaration) return NULL;
  LLVMValueRef address = LLVMGetNamedGlobal(codegen->module, global_declaration->text);
  if (!address) return NULL;
  LLVMTypeRef struct_type;
  Node *typedef_declaration;
  /* Ptr<Typedef> globals store a pointer — load it, then walk the pointee record. */
  if (global_declaration->pointer_element_width || global_declaration->pointer_type_name[0]) {
    struct_type = pointer_element_type(codegen, global_declaration);
    if (!struct_type) return NULL;
    address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, global_name);
    typedef_declaration = typedef_named(codegen, global_declaration->pointer_type_name);
  } else {
    struct_type = declaration_type(codegen, global_declaration);
    typedef_declaration = typedef_for_type(codegen, struct_type);
  }
  const char *field_name = separator + 1;
  while (typedef_declaration) {
    const char *next_separator = strchr(field_name, '.');
    char field_buffer[256];
    size_t field_name_length = next_separator ? (size_t)(next_separator - field_name) : strlen(field_name);
    if (field_name_length >= sizeof(field_buffer)) return NULL;
    memcpy(field_buffer, field_name, field_name_length);
    field_buffer[field_name_length] = '\0';
    unsigned field_index;
    Node *field = typedef_field_named(typedef_declaration, field_buffer, &field_index);
    if (!field) return NULL;
    LLVMTypeRef field_type = declaration_type(codegen, field);
    address = LLVMBuildStructGEP2(codegen->builder, struct_type, address, field_index, "field");
    if (!next_separator) {
      *type = field_type;
      if (final_field) *final_field = field;
      remember_pointer_element_from_decl(codegen, field);
      return LLVMBuildLoad2(codegen->builder, field_type, address, "field.load");
    }
    if (field->pointer_element_width || field->pointer_type_name[0]) {
      struct_type = pointer_element_type(codegen, field);
      if (!struct_type) return NULL;
      address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "field.pointer");
      typedef_declaration = typedef_named(codegen, field->pointer_type_name);
    } else {
      struct_type = field_type;
      typedef_declaration = typedef_named(codegen, field->type_name);
    }
    if (!typedef_declaration) return NULL;
    field_name = next_separator + 1;
  }
  return NULL;
}
static LLVMValueRef object_field_value(Codegen *codegen, const char *name, LLVMTypeRef *type) {
  const char *separator = strchr(name, '.');
  if (!separator || (size_t)(separator - name) >= 256 || strlen(separator + 1) >= 256) return NULL;
  char receiver[256], field_name[256];
  memcpy(receiver, name, (size_t)(separator - name));
  receiver[separator - name] = '\0';
  strcpy(field_name, separator + 1);
  Local *object = find_local(codegen, receiver);
  ClassInfo *receiver_info = NULL;
  LLVMValueRef object_pointer = NULL;
  if (object && object->class_name[0]) {
    receiver_info = class_table_find(&codegen->classes, object->class_name);
    object_pointer = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, object->address, receiver);
  } else if (object && object->type == codegen->pointer_type) {
    receiver_info = find_unique_class_with_member(codegen, field_name, 0);
    if (receiver_info) {
      strcpy(object->class_name, receiver_info->name);
      object_pointer = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, object->address, receiver);
    }
  } else {
    Node *global = global_named(codegen, receiver);
    if (global) {
      if (type_is_any(global->type_name)) {
        receiver_info = resolve_any_class(codegen, global, NULL, field_name, 0);
        if (receiver_info) designate_any_class(global, receiver_info->name);
      } else receiver_info = class_from_typed_declaration(codegen, global);
      if (!receiver_info && global->class_name[0]) receiver_info = class_table_find(&codegen->classes, global->class_name);
      LLVMValueRef global_value = receiver_info ? LLVMGetNamedGlobal(codegen->module, global->text) : NULL;
      if (global_value) object_pointer = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, global_value, receiver);
    }
  }
  if (!receiver_info || !object_pointer) return NULL;
  const char *field_cursor = field_name;
  const char *next_separator = strchr(field_cursor, '.');
  char field_buffer[256];
  size_t field_name_length = next_separator ? (size_t)(next_separator - field_cursor) : strlen(field_cursor);
  if (field_name_length >= sizeof(field_buffer)) return NULL;
  memcpy(field_buffer, field_cursor, field_name_length);
  field_buffer[field_name_length] = '\0';
  ClassField *first_field = class_info_find_field(receiver_info, field_buffer);
  if (!first_field) return NULL;
  LLVMTypeRef record_type = class_field_type(codegen, first_field->declaration);
  LLVMValueRef address = LLVMBuildStructGEP2(codegen->builder, receiver_info->struct_type, object_pointer, first_field->struct_index, "object.field");
  if (!next_separator) {
    LLVMValueRef value = LLVMBuildLoad2(codegen->builder, record_type, address, "object.field.load");
    if (LLVMGetTypeKind(record_type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(record_type) < 32) {
      *type = codegen->i32;
      return first_field->declaration->integer_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
    }
    *type = record_type;
    remember_pointer_element_from_decl(codegen, first_field->declaration);
    return value;
  }
  ClassField *outer_field = first_field;
  ClassInfo *nested_info = outer_field ? class_table_find(&codegen->classes, outer_field->declaration->type_name) : NULL;
  if (nested_info && !strchr(next_separator + 1, '.')) {
    ClassField *nested_field = class_info_find_field(nested_info, next_separator + 1);
    if (!nested_field) return NULL;
    LLVMValueRef nested_object = LLVMBuildLoad2(codegen->builder, record_type, address, "object.field.receiver");
    LLVMTypeRef nested_type = class_field_type(codegen, nested_field->declaration);
    LLVMValueRef nested_address = LLVMBuildStructGEP2(codegen->builder, nested_info->struct_type, nested_object, nested_field->struct_index, "object.field");
    LLVMValueRef value = LLVMBuildLoad2(codegen->builder, nested_type, nested_address, "object.field.load");
    if (LLVMGetTypeKind(nested_type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(nested_type) < 32) {
      *type = codegen->i32;
      return nested_field->declaration->integer_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
    }
    *type = nested_type;
    remember_pointer_element_from_decl(codegen, nested_field->declaration);
    return value;
  }
  Node *typedef_declaration = NULL;
  /* this.ptrField.member — class field is Ptr<Typedef>, not an embedded record. */
  if (declaration_is_typed_pointer(first_field->declaration)) {
    record_type = pointer_element_type(codegen, first_field->declaration);
    if (!record_type || !first_field->declaration->pointer_type_name[0]) return NULL;
    typedef_declaration = typedef_named(codegen, first_field->declaration->pointer_type_name);
    if (!typedef_declaration) return NULL;
    address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "object.field.pointer");
  } else {
    typedef_declaration = typedef_for_type(codegen, record_type);
    if (!typedef_declaration && first_field->declaration->type_name[0])
      typedef_declaration = typedef_named(codegen, first_field->declaration->type_name);
  }
  if (!typedef_declaration) return NULL;
  field_cursor = next_separator + 1;
  while (typedef_declaration) {
    next_separator = strchr(field_cursor, '.');
    field_name_length = next_separator ? (size_t)(next_separator - field_cursor) : strlen(field_cursor);
    if (field_name_length >= sizeof(field_buffer)) return NULL;
    memcpy(field_buffer, field_cursor, field_name_length);
    field_buffer[field_name_length] = '\0';
    unsigned field_index;
    Node *field = typedef_field_named(typedef_declaration, field_buffer, &field_index);
    if (!field) return NULL;
    LLVMTypeRef field_type = declaration_type(codegen, field);
    address = LLVMBuildStructGEP2(codegen->builder, record_type, address, field_index, "field");
    if (!next_separator) {
      if (is_fixed_array_decl(field)) {
        /* Keep array-by-value so FixedArray.toString / sig compares work. */
        *type = field_type;
        return LLVMBuildLoad2(codegen->builder, field_type, address, "field.load");
      }
      *type = field_type;
      remember_pointer_element_from_decl(codegen, field);
      LLVMValueRef value = LLVMBuildLoad2(codegen->builder, field_type, address, "field.load");
      if (LLVMGetTypeKind(field_type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(field_type) < 32) {
        *type = codegen->i32;
        return field->integer_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
      }
      return value;
    }
    if (field->pointer_element_width || field->pointer_type_name[0]) {
      record_type = pointer_element_type(codegen, field);
      if (!record_type) return NULL;
      address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "field.pointer");
      typedef_declaration = typedef_named(codegen, field->pointer_type_name);
    } else {
      record_type = field_type;
      typedef_declaration = typedef_named(codegen, field->type_name);
    }
    if (!typedef_declaration) return NULL;
    field_cursor = next_separator + 1;
  }
  return NULL;
}

static LLVMValueRef local_field_value(Codegen *codegen, const char *name, LLVMTypeRef *type) {
  LLVMValueRef object_value = object_field_value(codegen, name, type);
  if (object_value) return object_value;
  const char *separator = strchr(name, '.');
  if (!separator) return NULL;
  char local_name[256];
  size_t local_name_length = (size_t)(separator - name);
  if (local_name_length >= sizeof(local_name)) return NULL;
  memcpy(local_name, name, local_name_length);
  local_name[local_name_length] = '\0';
  Local *item_local = find_local(codegen, local_name);
  if (!item_local) return NULL;
  if (item_local->class_name[0] && !strchr(separator + 1, '.')) {
    LLVMTypeRef field_type;
    LLVMValueRef field_address = object_field_address(codegen, item_local, separator + 1, &field_type);
    if (field_address) {
      ClassField *class_field = class_info_find_field(class_table_find(&codegen->classes, item_local->class_name), separator + 1);
      LLVMValueRef value = LLVMBuildLoad2(codegen->builder, field_type, field_address, "object.field.load");
      if (LLVMGetTypeKind(field_type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(field_type) < 32) {
        *type = codegen->i32;
        return LLVMBuildSExt(codegen->builder, value, codegen->i32, "sext");
      }
      *type = field_type;
      if (class_field) remember_pointer_element_from_decl(codegen, class_field->declaration);
      return value;
    }
    Node *static_field = NULL;
    for (Node *global = codegen->globals; global; global = global->next) {
      if (strcmp(global->text, separator + 1)) continue;
      if (global->class_name[0] && strcmp(global->class_name, item_local->class_name)) continue;
      static_field = global;
      break;
    }
    if (!static_field) static_field = global_named(codegen, separator + 1);
    if (static_field) {
      LLVMValueRef global = LLVMGetNamedGlobal(codegen->module, static_field->text);
      if (global && static_field->is_dynamic_array) {
        LLVMTypeRef storage_type = LLVMGlobalGetValueType(global);
        LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
        *type = codegen->pointer_type;
        return LLVMBuildGEP2(codegen->builder, storage_type, global, indices, 2, "array.data");
      }
      if (global) {
        *type = declaration_type(codegen, static_field);
        return LLVMBuildLoad2(codegen->builder, *type, global, static_field->text);
      }
    }
  }
  LLVMTypeRef struct_type = item_local->type;
  LLVMValueRef address = item_local->address;
  if (item_local->pointer_element_type) {
    struct_type = item_local->pointer_element_type;
    address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, local_name);
  }
  Node *typedef_declaration = typedef_for_type(codegen, struct_type);
  if (!typedef_declaration) return NULL;
  const char *field_name = separator + 1;
  while (address) {
    const char *next_separator = strchr(field_name, '.');
    char field_buffer[256];
    size_t field_name_length = next_separator ? (size_t)(next_separator - field_name) : strlen(field_name);
    if (field_name_length >= sizeof(field_buffer)) return NULL;
    memcpy(field_buffer, field_name, field_name_length);
    field_buffer[field_name_length] = '\0';
    unsigned field_index;
    Node *field = typedef_field_named(typedef_declaration, field_buffer, &field_index);
    if (!field) return NULL;
    LLVMTypeRef field_type = declaration_type(codegen, field);
    address = LLVMBuildStructGEP2(codegen->builder, struct_type, address, field_index, "field");
    if (!next_separator) {
      *type = field_type;
      return LLVMBuildLoad2(codegen->builder, field_type, address, "field.load");
    }
    if (field->pointer_element_width || field->pointer_type_name[0]) {
      struct_type = pointer_element_type(codegen, field);
      if (!struct_type) return NULL;
      address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "field.pointer");
      typedef_declaration = typedef_named(codegen, field->pointer_type_name);
    } else {
      typedef_declaration = typedef_named(codegen, field->type_name);
      struct_type = field_type;
    }
    if (!typedef_declaration) return NULL;
    field_name = next_separator + 1;
  }
  return NULL;
}
static LLVMValueRef emit_record_constant(Codegen *codegen, Node *record, Node *typedef_declaration, const char *parameter, uint64_t argument, LLVMTypeRef *type) {
  LLVMTypeRef struct_type = LLVMGetTypeByName2(codegen->context, typedef_declaration->text);
  LLVMValueRef values[256];
  unsigned count = 0;
  for (Node *field = typedef_declaration->a; field; field = field->next, count++) {
    if (count == 256) die("typedef has too many fields");
    LLVMTypeRef field_type = declaration_type(codegen, field);
    values[count] = LLVMConstNull(field_type);
  }
  for (Node *value_field = record->a; value_field; value_field = value_field->next) {
    unsigned index;
    Node *field = typedef_field_named(typedef_declaration, value_field->text, &index);
    if (!field) die("record initializer contains an unknown field");
    LLVMTypeRef field_type = declaration_type(codegen, field);
    LLVMValueRef value;
    if ((value_field->a->kind == N_NAME && !strcmp(value_field->a->text, "null")) || value_field->a->kind == N_NULL) {
      if (field_type != codegen->pointer_type) die("null record field requires a pointer type");
      value = LLVMConstNull(field_type);
    } else {
      LLVMTypeRef value_type;
      value = emit_constant(codegen, value_field->a, parameter, argument, &value_type);
      if (value_type != field_type) {
        if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(field_type) != LLVMIntegerTypeKind || !LLVMIsAConstantInt(value)) die("record field initializer type mismatch");
        value = LLVMConstInt(field_type, LLVMConstIntGetZExtValue(value), 0);
      }
    }
    values[index] = value;
  }
  *type = struct_type;
  return LLVMConstNamedStruct(struct_type, values, count);
}

static LLVMValueRef emit_record_value(Codegen *codegen, Node *record, Node *typedef_declaration, LLVMTypeRef *type) {
  LLVMTypeRef struct_type = LLVMGetTypeByName2(codegen->context, typedef_declaration->text);
  LLVMValueRef value = LLVMGetUndef(struct_type);
  for (Node *value_field = record->a; value_field; value_field = value_field->next) {
    unsigned index;
    Node *field = typedef_field_named(typedef_declaration, value_field->text, &index);
    if (!field) die("record initializer contains an unknown field");
    LLVMTypeRef field_type = declaration_type(codegen, field);
    LLVMTypeRef value_type;
    LLVMValueRef field_value = emit_expression(codegen, value_field->a, &value_type);
    if (value_type != field_type) {
      if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(field_type) != LLVMIntegerTypeKind) die("record field initializer type mismatch");
      field_value = convert_integer(codegen, field_value, value_type, field_type, field->integer_unsigned);
    }
    value = LLVMBuildInsertValue(codegen->builder, value, field_value, index, "record");
  }
  *type = struct_type;
  return value;
}
static int is_string_expression(Node *item) {
  return item->kind == N_STR
      || (item->kind == N_CALL && !strcmp(item->text, "Std.string") && item->a && !item->a->next)
      || (item->kind == N_CALL && !strcmp(item->text, "String.fromCharCodes") && item->a && !item->a->next);
}

static int declaration_is_typed_pointer(const Node *declaration) {
  return declaration && (declaration->pointer_element_width || declaration->pointer_type_name[0]);
}

/* True for C strings / String values. False for Ptr<T> address values. */
static int expression_is_cstring(Codegen *codegen, Node *item, LLVMValueRef value) {
  if (!item) return is_string_scratch(codegen, value);
  if (item->kind == N_STR || is_string_expression(item)) return 1;
  if (is_string_scratch(codegen, value)) return 1;
  if (item->kind == N_UNARY && !strcmp(item->text, "&")) return 0;
  if (item->kind == N_CAST && declaration_is_typed_pointer(item)) return 0;
  if (item->kind == N_CALL && (strstr(item->text, "toString") || !strncmp(item->text, "StringTools.", 12))) return 1;
  /* Nested field this.ptrField — not a string just because it's a pointer-typed load. */
  if (item->kind == N_FIELD && item->a) {
    if (item->a->kind == N_NAME) {
      char dotted[512];
      snprintf(dotted, sizeof(dotted), "%s.%s", item->a->text, item->text);
      Local *object = find_local(codegen, item->a->text);
      ClassInfo *info = object && object->class_name[0] ? class_table_find(&codegen->classes, object->class_name) : NULL;
      ClassField *field = info ? class_info_find_field(info, item->text) : NULL;
      if (field && declaration_is_typed_pointer(field->declaration)) return 0;
      if (field && (!strcmp(field->declaration->type_name, "String") || !strcmp(field->declaration->type_name, "Dynamic"))) return 1;
      (void)dotted;
    }
    return expression_is_cstring(codegen, item->a, value);
  }
  if (item->kind == N_BINARY && !strcmp(item->text, "+")) {
    /* Only treat as string if this + was actually concat, not ptr arithmetic. */
    return 1;
  }
  if (item->kind == N_NAME) {
    Local *item_local = find_local(codegen, item->text);
    if (item_local) {
      if (item_local->pointer_element_type) return 0;
      if (item_local->string_length) return 1;
    }
    char receiver[256], field_name[256];
    if (split_receiver_method(item->text, receiver, sizeof(receiver), field_name, sizeof(field_name))) {
      Local *object = find_local(codegen, receiver);
      ClassInfo *info = object && object->class_name[0] ? class_table_find(&codegen->classes, object->class_name) : NULL;
      if (!info) {
        Node *global = global_named(codegen, receiver);
        if (global) info = class_from_typed_declaration(codegen, global);
      }
      ClassField *field = info ? class_info_find_field(info, field_name) : NULL;
      if (field && declaration_is_typed_pointer(field->declaration)) return 0;
      if (field && !strcmp(field->declaration->type_name, "Any")) return 0;
      if (field && (!strcmp(field->declaration->type_name, "String") || !strcmp(field->declaration->type_name, "Dynamic"))) return 1;
    }
    Node *global = global_named(codegen, item->text);
    if (global && declaration_is_typed_pointer(global)) return 0;
    if (global && !strcmp(global->type_name, "Any")) return 0;
    if (global && (!strcmp(global->type_name, "String") || !strcmp(global->type_name, "Dynamic"))) return 1;
  }
  return 1;
}

static LLVMValueRef pointer_as_address_string(Codegen *codegen, LLVMValueRef pointer) {
  LLVMValueRef address = LLVMBuildPtrToInt(codegen->builder, pointer, codegen->i64, "ptrtoint");
  return format_integer_string(codegen, address, codegen->i64, 16);
}

/* Freestanding i128 div/rem: no libgcc/compiler-rt __divti3/__modti3.
   Operands sit in align-16 slots (XMM-friendly); math is add/sub/shift/icmp only. */
static LLVMTypeRef i128_type(Codegen *codegen) {
  return LLVMIntTypeInContext(codegen->context, 128);
}

static void i128_div0_trap(Codegen *codegen) {
  LLVMTypeRef asm_type = LLVMFunctionType(codegen->void_type, NULL, 0, 0);
  LLVMValueRef hlt = LLVMGetInlineAsm(asm_type, "hlt", 3, "", 0, 0, 0, LLVMInlineAsmDialectIntel, 0);
  LLVMBuildCall2(codegen->builder, asm_type, hlt, NULL, 0, "");
}

static LLVMValueRef ensure_i128_udivmod_function(Codegen *codegen) {
  if (codegen->i128_udivmod_fn) return codegen->i128_udivmod_fn;
  LLVMBasicBlockRef caller = LLVMGetInsertBlock(codegen->builder);
  LLVMTypeRef i128 = i128_type(codegen);
  LLVMTypeRef params[4] = { codegen->pointer_type, codegen->pointer_type, codegen->pointer_type, codegen->pointer_type };
  LLVMValueRef fn = LLVMAddFunction(codegen->module, "__haxellvm_i128_udivmod",
                                    LLVMFunctionType(codegen->void_type, params, 4, 0));
  LLVMSetLinkage(fn, LLVMInternalLinkage);

  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(codegen->context, fn, "entry");
  LLVMBasicBlockRef div0 = LLVMAppendBasicBlockInContext(codegen->context, fn, "div0");
  LLVMBasicBlockRef check = LLVMAppendBasicBlockInContext(codegen->context, fn, "check");
  LLVMBasicBlockRef early = LLVMAppendBasicBlockInContext(codegen->context, fn, "early");
  LLVMBasicBlockRef init = LLVMAppendBasicBlockInContext(codegen->context, fn, "init");
  LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(codegen->context, fn, "body");
  LLVMBasicBlockRef take = LLVMAppendBasicBlockInContext(codegen->context, fn, "take");
  LLVMBasicBlockRef skip = LLVMAppendBasicBlockInContext(codegen->context, fn, "skip");
  LLVMBasicBlockRef step = LLVMAppendBasicBlockInContext(codegen->context, fn, "step");
  LLVMBasicBlockRef finish = LLVMAppendBasicBlockInContext(codegen->context, fn, "finish");

  LLVMPositionBuilderAtEnd(codegen->builder, entry);
  LLVMValueRef quot_out = LLVMGetParam(fn, 0);
  LLVMValueRef rem_out = LLVMGetParam(fn, 1);
  LLVMValueRef num_p = LLVMGetParam(fn, 2);
  LLVMValueRef den_p = LLVMGetParam(fn, 3);
  LLVMValueRef num = LLVMBuildLoad2(codegen->builder, i128, num_p, "num");
  LLVMSetAlignment(num, 16);
  LLVMValueRef den = LLVMBuildLoad2(codegen->builder, i128, den_p, "den");
  LLVMSetAlignment(den, 16);
  LLVMValueRef rem_s = LLVMBuildAlloca(codegen->builder, i128, "rem");
  LLVMSetAlignment(rem_s, 16);
  LLVMValueRef quot_s = LLVMBuildAlloca(codegen->builder, i128, "quot");
  LLVMSetAlignment(quot_s, 16);
  LLVMValueRef bit_s = LLVMBuildAlloca(codegen->builder, codegen->i32, "bit");
  LLVMBuildCondBr(codegen->builder,
                  LLVMBuildICmp(codegen->builder, LLVMIntEQ, den, LLVMConstInt(i128, 0, 0), "den0"),
                  div0, check);

  LLVMPositionBuilderAtEnd(codegen->builder, div0);
  i128_div0_trap(codegen);
  LLVMBuildBr(codegen->builder, div0);

  LLVMPositionBuilderAtEnd(codegen->builder, check);
  LLVMBuildStore(codegen->builder, LLVMConstInt(i128, 0, 0), quot_s);
  LLVMBuildCondBr(codegen->builder,
                  LLVMBuildICmp(codegen->builder, LLVMIntULT, num, den, "num.lt"),
                  early, init);

  LLVMPositionBuilderAtEnd(codegen->builder, early);
  LLVMBuildStore(codegen->builder, num, rem_s);
  LLVMBuildBr(codegen->builder, finish);

  LLVMPositionBuilderAtEnd(codegen->builder, init);
  LLVMBuildStore(codegen->builder, LLVMConstInt(i128, 0, 0), rem_s);
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i32, 127, 0), bit_s);
  LLVMBuildBr(codegen->builder, body);

  LLVMPositionBuilderAtEnd(codegen->builder, body);
  LLVMValueRef bit = LLVMBuildLoad2(codegen->builder, codegen->i32, bit_s, "bit");
  LLVMValueRef rem = LLVMBuildLoad2(codegen->builder, i128, rem_s, "rem");
  LLVMValueRef quot = LLVMBuildLoad2(codegen->builder, i128, quot_s, "quot");
  LLVMValueRef bit128 = LLVMBuildZExt(codegen->builder, bit, i128, "bit.z");
  LLVMValueRef rem_shl = LLVMBuildShl(codegen->builder, rem, LLVMConstInt(i128, 1, 0), "rem.shl");
  LLVMValueRef num_bit = LLVMBuildAnd(codegen->builder,
                                      LLVMBuildLShr(codegen->builder, num, bit128, "num.shr"),
                                      LLVMConstInt(i128, 1, 0), "num.bit");
  LLVMValueRef rem_n = LLVMBuildOr(codegen->builder, rem_shl, num_bit, "rem.n");
  LLVMBuildCondBr(codegen->builder,
                  LLVMBuildICmp(codegen->builder, LLVMIntUGE, rem_n, den, "ge"),
                  take, skip);

  LLVMPositionBuilderAtEnd(codegen->builder, take);
  LLVMBuildStore(codegen->builder, LLVMBuildSub(codegen->builder, rem_n, den, "rem.sub"), rem_s);
  LLVMBuildStore(codegen->builder,
                 LLVMBuildOr(codegen->builder, quot,
                             LLVMBuildShl(codegen->builder, LLVMConstInt(i128, 1, 0), bit128, "q.bit"), "q.or"),
                 quot_s);
  LLVMBuildBr(codegen->builder, step);

  LLVMPositionBuilderAtEnd(codegen->builder, skip);
  LLVMBuildStore(codegen->builder, rem_n, rem_s);
  LLVMBuildBr(codegen->builder, step);

  LLVMPositionBuilderAtEnd(codegen->builder, step);
  LLVMValueRef bit_next = LLVMBuildSub(codegen->builder, bit, LLVMConstInt(codegen->i32, 1, 0), "bit.next");
  LLVMBuildStore(codegen->builder, bit_next, bit_s);
  LLVMBuildCondBr(codegen->builder,
                  LLVMBuildICmp(codegen->builder, LLVMIntSGE, bit_next, LLVMConstInt(codegen->i32, 0, 0), "more"),
                  body, finish);

  LLVMPositionBuilderAtEnd(codegen->builder, finish);
  LLVMValueRef qstore = LLVMBuildStore(codegen->builder, LLVMBuildLoad2(codegen->builder, i128, quot_s, "q"), quot_out);
  LLVMSetAlignment(qstore, 16);
  LLVMValueRef rstore = LLVMBuildStore(codegen->builder, LLVMBuildLoad2(codegen->builder, i128, rem_s, "r"), rem_out);
  LLVMSetAlignment(rstore, 16);
  LLVMBuildRetVoid(codegen->builder);

  codegen->i128_udivmod_fn = fn;
  LLVMPositionBuilderAtEnd(codegen->builder, caller);
  return fn;
}

static LLVMValueRef soft_i128_div_or_rem(Codegen *codegen, LLVMValueRef left, LLVMValueRef right,
                                        LLVMTypeRef type, int want_rem, int is_unsigned) {
  LLVMTypeRef i128 = i128_type(codegen);
  LLVMValueRef fn = ensure_i128_udivmod_function(codegen);
  LLVMValueRef num_s = build_entry_alloca(codegen, i128, "i128.num", 16);
  LLVMValueRef den_s = build_entry_alloca(codegen, i128, "i128.den", 16);
  LLVMValueRef quot_s = build_entry_alloca(codegen, i128, "i128.quot", 16);
  LLVMValueRef rem_s = build_entry_alloca(codegen, i128, "i128.rem", 16);
  left = convert_integer(codegen, left, type, i128, is_unsigned);
  right = convert_integer(codegen, right, type, i128, is_unsigned);

  if (is_unsigned) {
    LLVMValueRef s0 = LLVMBuildStore(codegen->builder, left, num_s);
    LLVMSetAlignment(s0, 16);
    LLVMValueRef s1 = LLVMBuildStore(codegen->builder, right, den_s);
    LLVMSetAlignment(s1, 16);
  } else {
    LLVMValueRef sign_a = LLVMBuildAShr(codegen->builder, left, LLVMConstInt(i128, 127, 0), "sa");
    LLVMValueRef sign_b = LLVMBuildAShr(codegen->builder, right, LLVMConstInt(i128, 127, 0), "sb");
    LLVMValueRef abs_a = LLVMBuildSub(codegen->builder, LLVMBuildXor(codegen->builder, left, sign_a, "xa"), sign_a, "aa");
    LLVMValueRef abs_b = LLVMBuildSub(codegen->builder, LLVMBuildXor(codegen->builder, right, sign_b, "xb"), sign_b, "ab");
    LLVMValueRef s0 = LLVMBuildStore(codegen->builder, abs_a, num_s);
    LLVMSetAlignment(s0, 16);
    LLVMValueRef s1 = LLVMBuildStore(codegen->builder, abs_b, den_s);
    LLVMSetAlignment(s1, 16);
    LLVMValueRef args[4] = { quot_s, rem_s, num_s, den_s };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 4, "");
    LLVMValueRef quot = LLVMBuildLoad2(codegen->builder, i128, quot_s, "q");
    LLVMSetAlignment(quot, 16);
    LLVMValueRef rem = LLVMBuildLoad2(codegen->builder, i128, rem_s, "r");
    LLVMSetAlignment(rem, 16);
    LLVMValueRef sign_q = LLVMBuildXor(codegen->builder, sign_a, sign_b, "sq");
    quot = LLVMBuildSub(codegen->builder, LLVMBuildXor(codegen->builder, quot, sign_q, "xq"), sign_q, "qs");
    rem = LLVMBuildSub(codegen->builder, LLVMBuildXor(codegen->builder, rem, sign_a, "xr"), sign_a, "rs");
    LLVMValueRef result = want_rem ? rem : quot;
    return convert_integer(codegen, result, i128, type, is_unsigned);
  }

  LLVMValueRef args[4] = { quot_s, rem_s, num_s, den_s };
  LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 4, "");
  LLVMValueRef result = LLVMBuildLoad2(codegen->builder, i128, want_rem ? rem_s : quot_s, want_rem ? "rem" : "quot");
  LLVMSetAlignment(result, 16);
  return convert_integer(codegen, result, i128, type, is_unsigned);
}

static LLVMValueRef ensure_string_length_function(Codegen *codegen) {
  if (codegen->string_length_function) return codegen->string_length_function;
  LLVMBasicBlockRef caller_block = LLVMGetInsertBlock(codegen->builder);
  /* Length is target-word sized — i64 on amd64/arm64, i32 only on actual 32-bit targets. */
  LLVMTypeRef function_type = LLVMFunctionType(codegen->word_type, &codegen->pointer_type, 1, 0);
  LLVMValueRef function = LLVMAddFunction(codegen->module, "haxellvm_string_length", function_type);
  LLVMSetLinkage(function, LLVMInternalLinkage);
  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(codegen->context, function, "entry");
  LLVMBasicBlockRef condition = LLVMAppendBasicBlockInContext(codegen->context, function, "condition");
  LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(codegen->context, function, "body");
  LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(codegen->context, function, "done");
  LLVMPositionBuilderAtEnd(codegen->builder, entry);
  LLVMValueRef index = LLVMBuildAlloca(codegen->builder, codegen->word_type, "index");
  LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->word_type, 0, 0), index);
  LLVMBuildBr(codegen->builder, condition);
  LLVMPositionBuilderAtEnd(codegen->builder, condition);
  LLVMValueRef current = LLVMBuildLoad2(codegen->builder, codegen->word_type, index, "index");
  LLVMValueRef pointer = LLVMBuildGEP2(codegen->builder, codegen->i8, LLVMGetParam(function, 0), &current, 1, "character");
  LLVMValueRef character = LLVMBuildLoad2(codegen->builder, codegen->i8, pointer, "character");
  LLVMValueRef is_end = LLVMBuildICmp(codegen->builder, LLVMIntEQ, character, LLVMConstInt(codegen->i8, 0, 0), "is_end");
  LLVMBuildCondBr(codegen->builder, is_end, done, body);
  LLVMPositionBuilderAtEnd(codegen->builder, body);
  LLVMValueRef next = LLVMBuildAdd(codegen->builder, current, LLVMConstInt(codegen->word_type, 1, 0), "next");
  LLVMBuildStore(codegen->builder, next, index);
  LLVMBuildBr(codegen->builder, condition);
  LLVMPositionBuilderAtEnd(codegen->builder, done);
  LLVMBuildRet(codegen->builder, current);
  codegen->string_length_function = function;
  LLVMPositionBuilderAtEnd(codegen->builder, caller_block);
  return function;
}
static LLVMValueRef string_value_length(Codegen *codegen, LLVMValueRef value) {
  LLVMValueRef function = ensure_string_length_function(codegen);
  return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(function), function, &value, 1, "string.length");
}
static LLVMValueRef string_length(Codegen *codegen, Node *item) {
  if (item->kind == N_STR) return LLVMConstInt(codegen->word_type, strlen(item->text), 0);
  if (item->kind == N_NAME) {
    Local *item_local = local(codegen, item->text, item);
    if (item_local->string_length) {
      LLVMTypeRef length_type = LLVMTypeOf(item_local->string_length);
      if (length_type == codegen->word_type) return item_local->string_length;
      if (LLVMGetTypeKind(length_type) == LLVMIntegerTypeKind)
        return convert_integer(codegen, item_local->string_length, length_type, codegen->word_type, 1);
      return item_local->string_length;
    }
    if (item_local->type == codegen->pointer_type) {
      LLVMValueRef value = LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, item->text);
      LLVMValueRef function = ensure_string_length_function(codegen);
      return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(function), function, &value, 1, "string.length");
    }
  }
  if (item->kind == N_CALL && !strcmp(item->text, "Std.string") && item->a && !item->a->next) return string_length(codegen, item->a);
  if (item->kind == N_CALL && !strcmp(item->text, "String.fromCharCodes") && item->a && !item->a->next) {
    if (item->a->kind != N_NAME) die("String.fromCharCodes expects an array name");
    LLVMValueRef array_address;
    LLVMTypeRef array_type, element_type;
    if (resolve_growable_array(codegen, item->a->text, &array_address, &array_type, &element_type)) {
      LLVMValueRef length = LLVMBuildLoad2(codegen->builder, codegen->i32, growable_array_length_address(codegen, array_address, array_type), "string.length");
      return convert_integer(codegen, length, codegen->i32, codegen->word_type, 1);
    }
    Node *global = global_named(codegen, item->a->text);
    if (global && global->is_dynamic_array && global->a && global->a->kind == N_ARRAY) {
      unsigned count = 0;
      for (Node *value = global->a->a; value; value = value->next) count++;
      return LLVMConstInt(codegen->word_type, count, 0);
    }
    die("String.fromCharCodes expects an Array");
  }
  die("length requires a string value");
  return NULL;
}

static LLVMValueRef expression_string_length(Codegen *codegen, Node *item) {
  if (is_string_expression(item)) return string_length(codegen, item);
  if (item->kind == N_NAME) {
    Local *item_local = find_local(codegen, item->text);
    if (item_local && item_local->string_length) return item_local->string_length;
  }
  return NULL;
}

static void emit_trace_handler(Codegen *codegen, Node *argument);

static void ensure_host_trace_io(Codegen *codegen) {
  if (codegen->puts_fn) return;
  codegen->printf_type = LLVMFunctionType(codegen->i32, &codegen->pointer_type, 1, 1);
  codegen->puts_type = LLVMFunctionType(codegen->i32, &codegen->pointer_type, 1, 0);
  codegen->printf_fn = LLVMAddFunction(codegen->module, "printf", codegen->printf_type);
  codegen->puts_fn = LLVMAddFunction(codegen->module, "puts", codegen->puts_type);
}
static int asm_constraint_is_memory(const char *constraints, unsigned operand_index);

typedef enum { ASM_CONSTRAINT_INPUT, ASM_CONSTRAINT_OUTPUT, ASM_CONSTRAINT_CLOBBER } AsmConstraintKind;

typedef struct {
  char text[128];
  AsmConstraintKind kind;
} AsmConstraint;

typedef struct {
  AsmConstraint items[256];
  unsigned count;
  unsigned inputs;
  unsigned outputs;
} AsmConstraintList;

static void parse_asm_constraints(Codegen *codegen, Node *call, const char *text, AsmConstraintList *list) {
  const char *cursor = text;
  while (*cursor) {
    while (*cursor == ' ') cursor++;
    const char *start = cursor;
    while (*cursor && *cursor != ',') cursor++;
    const char *end = cursor;
    while (end > start && end[-1] == ' ') end--;
    if (start == end || list->count == sizeof(list->items) / sizeof(*list->items)) die_at(codegen, call, "invalid inline asm constraint list");
    AsmConstraint *item = &list->items[list->count++];
    size_t length = (size_t)(end - start);
    if (length >= sizeof(item->text)) die_at(codegen, call, "inline asm constraint is too long");
    memcpy(item->text, start, length);
    item->text[length] = '\0';
    if (item->text[0] == '~') item->kind = ASM_CONSTRAINT_CLOBBER;
    else if (item->text[0] == '=' || item->text[0] == '+') {
      item->kind = ASM_CONSTRAINT_OUTPUT;
      if (list->inputs) die_at(codegen, call, "inline asm output constraints must precede inputs");
      list->outputs++;
    } else {
      item->kind = ASM_CONSTRAINT_INPUT;
      list->inputs++;
    }
    if (*cursor) cursor++;
  }
}

static unsigned asm_operand_count(Node *operands) {
  unsigned count = 0;
  for (; operands; operands = operands->next) count++;
  return count;
}

static void inline_asm_text(const char *source, const char *constraints, char *destination, size_t size) {
  size_t output = 0;
  for (size_t input = 0; source[input] && output + 1 < size; input++) {
    if (source[input] == '[' && source[input + 1] == '$') {
      char *end;
      unsigned operand_index = (unsigned)strtoul(source + input + 2, &end, 10);
      if (end != source + input + 2 && *end == ']' && asm_constraint_is_memory(constraints, operand_index)) continue;
    }
    if (source[input] == ']' && input && source[input - 1] >= '0' && source[input - 1] <= '9') {
      size_t digit = input;
      while (digit && source[digit - 1] >= '0' && source[digit - 1] <= '9') digit--;
      if (digit && source[digit - 1] == '$' && digit > 1 && source[digit - 2] == '[') {
        unsigned operand_index = (unsigned)strtoul(source + digit, NULL, 10);
        if (asm_constraint_is_memory(constraints, operand_index)) continue;
      }
    }
    destination[output++] = source[input] == '%' ? '$' : source[input];
  }
  destination[output] = '\0';
}

static int asm_constraint_is_memory(const char *constraints, unsigned operand_index) {
  const char *cursor = constraints;
  for (unsigned index = 0; index < operand_index; index++) {
    cursor = strchr(cursor, ',');
    if (!cursor) return 0;
    cursor++;
  }
  while (*cursor == ' ' || *cursor == '*') cursor++;
  return *cursor == 'm';
}

static int asm_constraint_has_register(const char *constraints, unsigned operand_index, const char *register_name) {
  const char *cursor = constraints;
  for (unsigned index = 0; index < operand_index; index++) {
    cursor = strchr(cursor, ',');
    if (!cursor) return 0;
    cursor++;
  }
  size_t register_length = strlen(register_name);
  while (*cursor && *cursor != ',') {
    if (*cursor == '{' && !strncmp(cursor + 1, register_name, register_length) && cursor[register_length + 1] == '}') return 1;
    cursor++;
  }
  return 0;
}

static unsigned asm_constraint_integer_width(const char *constraints, unsigned operand_index) {
  const char *byte_registers[] = { "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh", "sil", "dil", "bpl", "spl", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };
  const char *word_registers[] = { "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
  const char *dword_registers[] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
  const char *qword_registers[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
  for (size_t index = 0; index < sizeof(byte_registers) / sizeof(*byte_registers); index++) if (asm_constraint_has_register(constraints, operand_index, byte_registers[index])) return 8;
  for (size_t index = 0; index < sizeof(word_registers) / sizeof(*word_registers); index++) if (asm_constraint_has_register(constraints, operand_index, word_registers[index])) return 16;
  for (size_t index = 0; index < sizeof(dword_registers) / sizeof(*dword_registers); index++) if (asm_constraint_has_register(constraints, operand_index, dword_registers[index])) return 32;
  for (size_t index = 0; index < sizeof(qword_registers) / sizeof(*qword_registers); index++) if (asm_constraint_has_register(constraints, operand_index, qword_registers[index])) return 64;
  return 0;
}

static void llvm_asm_constraints(const char *source, char *destination, size_t size) {
  size_t output = 0;
  int at_constraint_start = 1;
  for (size_t input = 0; source[input]; input++) {
    if (output + 2 >= size) die("asm constraint string too long");
    if (at_constraint_start && source[input] == 'm') destination[output++] = '*';
    destination[output++] = source[input];
    at_constraint_start = source[input] == ',';
  }
  destination[output] = '\0';
}

static LLVMValueRef emit_asm_memory_operand(Codegen *codegen, Node *operand, LLVMTypeRef *type, LLVMTypeRef *element_type) {
  *type = codegen->pointer_type;
  Node *name = operand;
  if (operand->kind == N_UNARY && !strcmp(operand->text, "&")) {
    if (operand->a->kind != N_NAME) die("asm memory address must name a local, global, or field");
    name = operand->a;
  } else if (operand->kind != N_NAME) {
    die("asm memory operand requires a local, global, or address expression");
  }
  Local *item_local = find_local(codegen, name->text);
  if (item_local) { *element_type = item_local->type; return item_local->address; }
  Node *global = global_named(codegen, name->text);
  LLVMValueRef address = global ? LLVMGetNamedGlobal(codegen->module, global->text) : NULL;
  if (address) { *element_type = declaration_type(codegen, global); return address; }
  address = local_field_address(codegen, name->text, element_type);
  if (address) return address;
  address = global_field_address(codegen, name->text, element_type);
  if (address) return address;
  die("asm memory operand requires a local, global, or address expression");
  return NULL;
}

static LLVMValueRef emit_inline_asm(Codegen *codegen, Node *call) {
  Node *template_node = call->a;
  Node *constraints_node = template_node ? template_node->next : NULL;
  Node *operand_nodes = constraints_node ? constraints_node->next : NULL;
  AsmConstraintList constraints_list = {0};
  if (constraints_node) {
    parse_asm_constraints(codegen, call, constraints_node->text, &constraints_list);
    unsigned expected_operands = constraints_list.inputs + constraints_list.outputs;
    if (asm_operand_count(operand_nodes) != expected_operands) {
      char message[160];
      snprintf(message, sizeof(message), "inline asm constraint list expects %u operand%s but call supplies %u", expected_operands, expected_operands == 1 ? "" : "s", asm_operand_count(operand_nodes));
      die_at(codegen, call, message);
    }
  }
  if (constraints_node && asm_constraint_has_register(constraints_node->text, 0, "ip")) {
    die_at(codegen, call, "inline asm cannot use {ip} as an input register; instruction pointers are not allocatable registers");
  }

  Local *output_locals[256];
  LLVMTypeRef output_types[256];
  unsigned output_count = 0;
  LLVMValueRef input_operands[256];
  LLVMTypeRef input_types[256];
  LLVMTypeRef memory_element_types[256] = {0};
  unsigned input_count = 0;
  Node *operand = operand_nodes;
  unsigned constraint_index = 0;

  if (constraints_node) {
    const char *cursor = constraints_node->text;
    while (*cursor) {
      while (*cursor == ' ') cursor++;
      const char *start = cursor;
      while (*cursor && *cursor != ',') cursor++;
      char constraint[256];
      size_t length = (size_t)(cursor - start);
      while (length && start[length - 1] == ' ') length--;
      if (length >= sizeof(constraint)) die_at(codegen, call, "inline asm constraint is too long");
      memcpy(constraint, start, length);
      constraint[length] = '\0';

      if (constraint[0] == '~') {
        /* clobber — no operand */
      } else if (constraint[0] == '=' || constraint[0] == '+') {
        if (!operand || operand->kind != N_NAME) die_at(codegen, call, "asm output operand must be a local variable name");
        if (output_count == 256) die_at(codegen, call, "asm supports at most 256 outputs");
        Local *output_local = find_local(codegen, operand->text);
        if (!output_local) die_at(codegen, operand, "asm output operand must be a local variable");
        LLVMTypeRef output_type = output_local->type;
        unsigned output_width = asm_constraint_integer_width(constraints_node->text, constraint_index);
        if (output_width) {
          if (LLVMGetTypeKind(output_type) != LLVMIntegerTypeKind || LLVMGetIntTypeWidth(output_type) != output_width) {
            die_at(codegen, operand, "asm output variable type does not match fixed-register width");
          }
        }
        output_locals[output_count] = output_local;
        output_types[output_count] = output_type;
        /* Read-write (+) also feeds the current value as an input tied to this output. */
        if (constraint[0] == '+') {
          if (input_count == 256) die("asm supports at most 256 operands");
          input_operands[input_count] = LLVMBuildLoad2(codegen->builder, output_type, output_local->address, output_local->name);
          input_types[input_count] = output_type;
          memory_element_types[input_count] = NULL;
          input_count++;
        }
        output_count++;
        operand = operand->next;
      } else {
        if (!operand) die_at(codegen, call, "asm input operand missing");
        if (input_count == 256) die("asm supports at most 256 operands");
        if (asm_constraint_is_memory(constraints_node->text, constraint_index)) {
          input_operands[input_count] = emit_asm_memory_operand(codegen, operand, &input_types[input_count], &memory_element_types[input_count]);
        } else {
          input_operands[input_count] = emit_expression(codegen, operand, &input_types[input_count]);
          unsigned operand_width = asm_constraint_integer_width(constraints_node->text, constraint_index);
          if (operand_width) {
            if (input_types[input_count] == codegen->pointer_type) {
              if (operand_width != 64) die_at(codegen, call, "pointer asm operand requires a 64-bit fixed register such as {rax}");
            } else {
              if (LLVMGetTypeKind(input_types[input_count]) != LLVMIntegerTypeKind) die("fixed-register asm operand requires an integer or pointer value");
              LLVMTypeRef narrowed_type = LLVMIntTypeInContext(codegen->context, operand_width);
              input_operands[input_count] = convert_integer(codegen, input_operands[input_count], input_types[input_count], narrowed_type, 1);
              input_types[input_count] = narrowed_type;
            }
          }
          memory_element_types[input_count] = NULL;
        }
        input_count++;
        operand = operand->next;
      }
      constraint_index++;
      if (*cursor) cursor++;
    }
  }

  LLVMTypeRef result_type = codegen->void_type;
  if (output_count == 1) result_type = output_types[0];
  else if (output_count > 1) result_type = LLVMStructTypeInContext(codegen->context, output_types, output_count, 0);

  LLVMTypeRef function_type = LLVMFunctionType(result_type, input_types, input_count, 0);
  LLVMInlineAsmDialect dialect = LLVMInlineAsmDialectIntel;
  char assembly[512];
  char constraints[512];
  inline_asm_text(template_node->text, constraints_node ? constraints_node->text : "", assembly, sizeof(assembly));
  llvm_asm_constraints(constraints_node ? constraints_node->text : "", constraints, sizeof(constraints));
  LLVMValueRef inline_asm = LLVMGetInlineAsm(function_type, assembly, strlen(assembly), constraints, strlen(constraints), 1, 0, dialect, 0);
  LLVMValueRef asm_call = LLVMBuildCall2(codegen->builder, function_type, inline_asm, input_operands, input_count, output_count ? "asm.out" : "");
  unsigned elementtype_kind = LLVMGetEnumAttributeKindForName("elementtype", strlen("elementtype"));
  for (unsigned index = 0; index < input_count; index++) {
    if (memory_element_types[index]) LLVMAddCallSiteAttribute(asm_call, index + 1, LLVMCreateTypeAttribute(codegen->context, elementtype_kind, memory_element_types[index]));
  }
  if (output_count == 1) {
    LLVMBuildStore(codegen->builder, asm_call, output_locals[0]->address);
  } else if (output_count > 1) {
    for (unsigned index = 0; index < output_count; index++) {
      LLVMValueRef piece = LLVMBuildExtractValue(codegen->builder, asm_call, index, "asm.extract");
      LLVMBuildStore(codegen->builder, piece, output_locals[index]->address);
    }
  }
  return LLVMConstInt(codegen->i32, 0, 0);
}

static LLVMValueRef fixed_array_element(Codegen *codegen, Node *item, Local **array_local) {
  if (!item->a || item->a->kind != N_NAME) die("fixed array indexing requires an array name");
  Local *local_array = local(codegen, item->a->text, item->a);
  if (!local_array->fixed_element_type) die("indexing requires a FixedArray");
  LLVMTypeRef index_type;
  LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), index };
  if (array_local) *array_local = local_array;
  return LLVMBuildGEP2(codegen->builder, local_array->type, local_array->address, indices, 2, "element");
}

static LLVMValueRef global_fixed_array_element(Codegen *codegen, Node *item, LLVMTypeRef *element_type) {
  if (!item->a || item->a->kind != N_NAME) return NULL;
  const char *name = item->a->text;
  if (!strncmp(name, "this.", 5)) name += 5;
  Node *array = global_named(codegen, name);
  if (!array || !array->fixed_array_length) return NULL;
  LLVMTypeRef array_type = declaration_type(codegen, array);
  LLVMTypeRef index_type;
  LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), index };
  *element_type = fixed_array_element_type(codegen, array);
  return LLVMBuildGEP2(codegen->builder, array_type, LLVMGetNamedGlobal(codegen->module, array->text), indices, 2, "element");
}

static LLVMValueRef fixed_array_argument_address(Codegen *codegen, Node *argument) {
  if (argument->kind != N_NAME) die_at(codegen, argument, "FixedArray arguments must be named storage");
  Local *local_array = find_local(codegen, argument->text);
  if (local_array && local_array->fixed_element_type) return local_array->address;
  LLVMTypeRef field_type;
  LLVMValueRef field_address = local_field_address(codegen, argument->text, &field_type);
  if (field_address && LLVMGetTypeKind(field_type) == LLVMArrayTypeKind) return field_address;
  field_address = global_field_address(codegen, argument->text, &field_type);
  if (field_address && LLVMGetTypeKind(field_type) == LLVMArrayTypeKind) return field_address;
  Node *global = global_named(codegen, argument->text);
  if (global && is_fixed_array_decl(global)) return LLVMGetNamedGlobal(codegen->module, global->text);
  die_at(codegen, argument, "FixedArray argument must be a local, field, or global FixedArray");
  return NULL;
}

static LLVMValueRef typedef_field_fixed_array_element(Codegen *codegen, Node *item, LLVMTypeRef *element_type_out) {
  if (!item || item->kind != N_INDEX || !item->a || item->a->kind != N_NAME || !strchr(item->a->text, '.')) return NULL;
  LLVMTypeRef field_type;
  LLVMValueRef field_address = local_field_address(codegen, item->a->text, &field_type);
  if (!field_address || LLVMGetTypeKind(field_type) != LLVMArrayTypeKind) return NULL;
  LLVMTypeRef index_type;
  LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), index };
  *element_type_out = LLVMGetElementType(field_type);
  return LLVMBuildGEP2(codegen->builder, field_type, field_address, indices, 2, "element");
}

static LLVMValueRef record_field_address(Codegen *codegen, Node *item, LLVMTypeRef *field_type) {
  if (!item->a) return NULL;
  LLVMTypeRef record_type;
  LLVMValueRef record;
  if (item->a->kind == N_INDEX) {
    record = object_array_element_address(codegen, item->a, &record_type);
    if (!record) record = global_fixed_array_element(codegen, item->a, &record_type);
    if (!record && item->a->a && item->a->a->kind == N_NAME) {
      Local *pointer_local = find_local(codegen, item->a->a->text);
      if (pointer_local && pointer_local->pointer_element_type) {
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->a->b, &index_type), index_type);
        LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, pointer_local->type, pointer_local->address, item->a->a->text);
        record_type = pointer_local->pointer_element_type;
        record = LLVMBuildGEP2(codegen->builder, record_type, pointer, &index, 1, "element");
      }
    }
    /* Array<Class> element slots hold pointers; load then index into the class layout. */
    if (record && record_type == codegen->pointer_type && item->a->a && item->a->a->kind == N_NAME) {
      char receiver[256], array_field_name[256];
      ClassInfo *element_info = NULL;
      if (split_receiver_method(item->a->a->text, receiver, sizeof(receiver), array_field_name, sizeof(array_field_name))) {
        Local *object = find_local(codegen, receiver);
        ClassInfo *info = object && object->class_name[0] ? class_table_find(&codegen->classes, object->class_name) : NULL;
        ClassField *array_field = info ? class_info_find_field(info, array_field_name) : NULL;
        if (array_field && array_field->declaration->dynamic_element_type_name[0])
          element_info = class_table_find(&codegen->classes, array_field->declaration->dynamic_element_type_name);
      } else {
        Local *array_local = find_local(codegen, item->a->a->text);
        if (array_local && array_local->is_growable_array) {
          /* Locals only keep the LLVM element type; class name is recovered from a matching Array<T> decl if unique. */
        }
        Node *global = global_named(codegen, item->a->a->text);
        if (global && global->dynamic_element_type_name[0])
          element_info = class_table_find(&codegen->classes, global->dynamic_element_type_name);
      }
      if (element_info) {
        ClassField *class_field = class_info_find_field(element_info, item->text);
        if (!class_field) return NULL;
        LLVMValueRef object = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, record, "array.object");
        *field_type = class_field_type(codegen, class_field->declaration);
        return LLVMBuildStructGEP2(codegen->builder, element_info->struct_type, object, class_field->struct_index, "field");
      }
    }
  } else if (item->a->kind == N_NAME) {
    Node *global = global_named(codegen, item->a->text);
    if (!global) return NULL;
    record_type = declaration_type(codegen, global);
    record = LLVMGetNamedGlobal(codegen->module, global->text);
  } else return NULL;
  if (!record) return NULL;
  Node *typedef_declaration = typedef_for_type(codegen, record_type);
  unsigned field_index;
  Node *field = typedef_declaration ? typedef_field_named(typedef_declaration, item->text, &field_index) : NULL;
  if (!field) return NULL;
  *field_type = declaration_type(codegen, field);
  return LLVMBuildStructGEP2(codegen->builder, record_type, record, field_index, "field");
}

static LLVMValueRef global_field_address(Codegen *codegen, const char *name, LLVMTypeRef *field_type) {
  const char *separator = strchr(name, '.');
  if (!separator) return NULL;
  char global_name[256];
  size_t global_name_length = (size_t)(separator - name);
  if (global_name_length >= sizeof(global_name)) return NULL;
  memcpy(global_name, name, global_name_length);
  global_name[global_name_length] = '\0';
  Node *global = global_named(codegen, global_name);
  if (!global) return NULL;
  LLVMValueRef address = LLVMGetNamedGlobal(codegen->module, global->text);
  if (!address) return NULL;
  LLVMTypeRef record_type;
  Node *typedef_declaration;
  if (global->pointer_element_width || global->pointer_type_name[0]) {
    record_type = pointer_element_type(codegen, global);
    if (!record_type) return NULL;
    address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, global_name);
    typedef_declaration = typedef_named(codegen, global->pointer_type_name);
  } else {
    record_type = declaration_type(codegen, global);
    typedef_declaration = typedef_for_type(codegen, record_type);
  }
  const char *field_name = separator + 1;
  while (typedef_declaration) {
    const char *next_separator = strchr(field_name, '.');
    char field_buffer[256];
    size_t field_name_length = next_separator ? (size_t)(next_separator - field_name) : strlen(field_name);
    if (field_name_length >= sizeof(field_buffer)) return NULL;
    memcpy(field_buffer, field_name, field_name_length);
    field_buffer[field_name_length] = '\0';
    unsigned field_index;
    Node *field = typedef_field_named(typedef_declaration, field_buffer, &field_index);
    if (!field) return NULL;
    LLVMTypeRef next_type = declaration_type(codegen, field);
    address = LLVMBuildStructGEP2(codegen->builder, record_type, address, field_index, "field");
    if (!next_separator) {
      *field_type = next_type;
      return address;
    }
    if (field->pointer_element_width || field->pointer_type_name[0]) {
      record_type = pointer_element_type(codegen, field);
      if (!record_type) return NULL;
      address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "field.pointer");
      typedef_declaration = typedef_named(codegen, field->pointer_type_name);
    } else {
      record_type = next_type;
      typedef_declaration = typedef_named(codegen, field->type_name);
    }
    if (!typedef_declaration) return NULL;
    field_name = next_separator + 1;
  }
  return NULL;
}

#define HAXELLVM_ARENA_SIZE 65536u

static unsigned class_struct_element_count(ClassInfo *info) {
  return (info->has_vtable ? 1u : 0u) + info->field_count;
}

static void emit_class_types(Codegen *codegen) {
  for (ClassInfo *info = codegen->classes.classes; info; info = info->next) {
    char type_name[288];
    snprintf(type_name, sizeof(type_name), "class.%s", info->name);
    if (!LLVMGetTypeByName2(codegen->context, type_name)) info->struct_type = LLVMStructCreateNamed(codegen->context, type_name);
    else info->struct_type = LLVMGetTypeByName2(codegen->context, type_name);
  }
  int progress = 1;
  while (progress) {
    progress = 0;
    for (ClassInfo *info = codegen->classes.classes; info; info = info->next) {
      if (LLVMIsOpaqueStruct(info->struct_type) == 0) continue;
      if (info->parent && LLVMIsOpaqueStruct(info->parent->struct_type)) continue;
      LLVMTypeRef elements[256];
      unsigned count = 0;
      if (info->has_vtable) {
        if (count == 256) die("class layout too large");
        elements[count++] = codegen->pointer_type;
      }
      for (ClassInfo *ancestor = info; ancestor; ancestor = ancestor->parent) {
        /* collect later in order via field_count walk using struct_index sort instead */
      }
      /* Fill by absolute index. */
      unsigned total = class_struct_element_count(info);
      if (total > 256) die("class layout too large");
      for (unsigned index = 0; index < total; index++) elements[index] = NULL;
      if (info->has_vtable) elements[0] = codegen->pointer_type;
      for (ClassInfo *current = info; current; current = current->parent) {
        for (ClassField *field = current->fields; field; field = field->next) {
          LLVMTypeRef field_type = class_field_type(codegen, field->declaration);
          if (!field_type) die_at(codegen, field->declaration, "unsupported class field type");
          if (field->struct_index >= 256) die("class layout too large");
          elements[field->struct_index] = field_type;
        }
      }
      for (unsigned index = 0; index < total; index++) {
        if (!elements[index]) die("incomplete class layout");
      }
      LLVMStructSetBody(info->struct_type, elements, total, 0);
      progress = 1;
    }
  }
  for (ClassInfo *info = codegen->classes.classes; info; info = info->next) {
    if (!info->has_vtable || !info->vtable_slot_count) continue;
    LLVMTypeRef slot = codegen->pointer_type;
    info->vtable_type = LLVMArrayType2(slot, info->vtable_slot_count);
  }
}

static ClassMethod *method_for_vtable_slot(ClassInfo *info, int slot) {
  for (ClassInfo *current = info; current; current = current->parent) {
    for (ClassMethod *method = current->methods; method; method = method->next) {
      if (method->is_virtual && method->vtable_index == slot && !method->is_abstract) return method;
    }
  }
  return NULL;
}

static void emit_vtables(Codegen *codegen) {
  for (ClassInfo *info = codegen->classes.classes; info; info = info->next) {
    if (info->is_abstract || !info->has_vtable || !info->vtable_slot_count) continue;
    char name[288];
    snprintf(name, sizeof(name), "vtable.%s", info->name);
    LLVMValueRef global = LLVMAddGlobal(codegen->module, info->vtable_type, name);
    LLVMSetLinkage(global, LLVMInternalLinkage);
    LLVMSetGlobalConstant(global, 1);
    LLVMValueRef slots[256];
    for (unsigned index = 0; index < info->vtable_slot_count; index++) {
      ClassMethod *method = method_for_vtable_slot(info, (int)index);
      if (!method) die("missing vtable method");
      Function *function = function_value(codegen, method->mangled);
      if (!function) die("vtable method was not declared");
      slots[index] = function->value;
    }
    LLVMSetInitializer(global, LLVMConstArray2(codegen->pointer_type, slots, info->vtable_slot_count));
    info->vtable_global = global;
  }
}

static void ensure_arena(Codegen *codegen) {
  if (codegen->arena_storage) return;
  LLVMTypeRef arena_type = LLVMArrayType2(codegen->i8, HAXELLVM_ARENA_SIZE);
  codegen->arena_storage = LLVMAddGlobal(codegen->module, arena_type, "__haxellvm_arena");
  LLVMSetLinkage(codegen->arena_storage, LLVMInternalLinkage);
  LLVMSetInitializer(codegen->arena_storage, LLVMConstNull(arena_type));
  codegen->arena_cursor = LLVMAddGlobal(codegen->module, codegen->i64, "__haxellvm_arena_cursor");
  LLVMSetLinkage(codegen->arena_cursor, LLVMInternalLinkage);
  LLVMSetInitializer(codegen->arena_cursor, LLVMConstInt(codegen->i64, 0, 0));
}

static LLVMValueRef arena_alloc(Codegen *codegen, LLVMTypeRef object_type, const char *name) {
  ensure_arena(codegen);
  unsigned align_bytes = 8;
  unsigned sysv_align = sysv_type_abi_alignment(codegen, object_type);
  if (sysv_align > align_bytes) align_bytes = sysv_align;
  uint64_t align_mask = (uint64_t)align_bytes - 1;
  LLVMValueRef size = LLVMSizeOf(object_type);
  size = LLVMBuildAdd(codegen->builder, size, LLVMConstInt(codegen->i64, align_mask, 0), "arena.pad");
  size = LLVMBuildAnd(codegen->builder, size, LLVMConstInt(codegen->i64, ~align_mask, 0), "arena.align");
  LLVMValueRef cursor = LLVMBuildLoad2(codegen->builder, codegen->i64, codegen->arena_cursor, "arena.cursor");
  /* Bump cursor up to the object's ABI alignment (SysV i128 objects want 16). */
  cursor = LLVMBuildAdd(codegen->builder, cursor, LLVMConstInt(codegen->i64, align_mask, 0), "arena.cursor.pad");
  cursor = LLVMBuildAnd(codegen->builder, cursor, LLVMConstInt(codegen->i64, ~align_mask, 0), "arena.cursor.align");
  LLVMValueRef next = LLVMBuildAdd(codegen->builder, cursor, size, "arena.next");
  LLVMValueRef limit = LLVMConstInt(codegen->i64, HAXELLVM_ARENA_SIZE, 0);
  LLVMValueRef ok = LLVMBuildICmp(codegen->builder, LLVMIntULE, next, limit, "arena.ok");
  LLVMBasicBlockRef cont = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "arena.ok");
  LLVMBasicBlockRef oom = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "arena.oom");
  LLVMBuildCondBr(codegen->builder, ok, cont, oom);
  LLVMPositionBuilderAtEnd(codegen->builder, oom);
  LLVMTypeRef asm_type = LLVMFunctionType(codegen->void_type, NULL, 0, 0);
  LLVMValueRef hlt = LLVMGetInlineAsm(asm_type, "hlt", 3, "", 0, 0, 0, LLVMInlineAsmDialectATT, 0);
  LLVMBuildCall2(codegen->builder, asm_type, hlt, NULL, 0, "");
  LLVMBuildBr(codegen->builder, oom);
  LLVMPositionBuilderAtEnd(codegen->builder, cont);
  LLVMBuildStore(codegen->builder, next, codegen->arena_cursor);
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), cursor };
  LLVMTypeRef arena_type = LLVMArrayType2(codegen->i8, HAXELLVM_ARENA_SIZE);
  LLVMValueRef raw = LLVMBuildGEP2(codegen->builder, arena_type, codegen->arena_storage, indices, 2, name);
  emit_memset(codegen, raw, LLVMConstInt(codegen->i8, 0, 0), size, align_bytes);
  return raw;
}

static LLVMValueRef object_field_address(Codegen *codegen, Local *object, const char *field_name, LLVMTypeRef *field_type) {
  if (!object || !object->class_name[0]) return NULL;
  ClassInfo *info = class_table_find(&codegen->classes, object->class_name);
  if (!info) return NULL;
  ClassField *field = class_info_find_field(info, field_name);
  if (!field) return NULL;
  LLVMValueRef object_pointer = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, object->address, object->name);
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, field->struct_index, 0) };
  *field_type = class_field_type(codegen, field->declaration);
  return LLVMBuildGEP2(codegen->builder, info->struct_type, object_pointer, indices, 2, "object.field");
}

static LLVMValueRef ensure_array_field_global(Codegen *codegen, ClassInfo *owner, ClassField *field) {
  Node *item = field->declaration;
  char name[320];
  snprintf(name, sizeof(name), "%s__%s", owner->name, field->name);
  LLVMValueRef existing = LLVMGetNamedGlobal(codegen->module, name);
  if (existing) return existing;
  if (!item->is_dynamic_array || !item->a) die("array field initializer required");
  {
    Node *array_literal = expand_constant_array(item->a);
    if (array_literal) item->a = array_literal;
  }
  if (item->a->kind != N_ARRAY || is_empty_array_literal(item->a)) die("array field initializer required");
  LLVMTypeRef element_type = dynamic_element_type(codegen, item);
  if (!element_type) die("dynamic array element type is unsupported");
  LLVMValueRef values[CONSTANT_ARRAY_CAPACITY];
  unsigned count = 0;
  for (Node *value_node = item->a->a; value_node; value_node = value_node->next) {
    LLVMTypeRef value_type;
    LLVMValueRef value = emit_constant(codegen, value_node, NULL, 0, &value_type);
    if (count == CONSTANT_ARRAY_CAPACITY || !LLVMIsAConstantInt(value) || LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind) die("dynamic array initializer must contain integer constants");
    values[count++] = LLVMConstInt(element_type, LLVMConstIntGetZExtValue(value), item->dynamic_element_unsigned);
  }
  LLVMTypeRef type = LLVMArrayType2(element_type, count);
  LLVMValueRef global = LLVMAddGlobal(codegen->module, type, name);
  LLVMSetInitializer(global, LLVMConstArray2(element_type, values, count));
  LLVMSetLinkage(global, LLVMInternalLinkage);
  LLVMSetGlobalConstant(global, 1);
  return global;
}

static void store_object_array_field_initializers(Codegen *codegen, ClassInfo *info, LLVMValueRef object) {
  for (ClassInfo *current = info; current; current = current->parent) {
    for (ClassField *field = current->fields; field; field = field->next) {
      Node *declaration = field->declaration;
      if (!declaration->is_dynamic_array || !declaration->a) continue;
      {
        Node *array_literal = expand_constant_array(declaration->a);
        if (array_literal) declaration->a = array_literal;
      }
      if (declaration->a->kind != N_ARRAY || is_empty_array_literal(declaration->a)) continue;
      LLVMValueRef global = ensure_array_field_global(codegen, current, field);
      LLVMTypeRef storage_type = LLVMGlobalGetValueType(global);
      LLVMValueRef data_indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
      LLVMValueRef data = LLVMBuildGEP2(codegen->builder, storage_type, global, data_indices, 2, "array.data");
      LLVMValueRef field_indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, field->struct_index, 0) };
      LLVMValueRef field_address = LLVMBuildGEP2(codegen->builder, info->struct_type, object, field_indices, 2, "object.field");
      LLVMBuildStore(codegen->builder, data, field_address);
    }
  }
}

static void store_object_field_initializers(Codegen *codegen, ClassInfo *info, LLVMValueRef object) {
  for (ClassInfo *current = info; current; current = current->parent) {
    for (ClassField *field = current->fields; field; field = field->next) {
      Node *declaration = field->declaration;
      if (declaration->is_dynamic_array || !declaration->a) continue;
      LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, field->struct_index, 0) };
      LLVMValueRef field_address = LLVMBuildGEP2(codegen->builder, info->struct_type, object, indices, 2, "object.field");
      if (declaration->fixed_array_length) {
        Node *array_literal = expand_constant_array_initializer(codegen, declaration->a);
        if (array_literal) declaration->a = array_literal;
        if (declaration->a->kind != N_ARRAY) die_at(codegen, declaration, "FixedArray requires an array literal or constant helper initializer");
        LLVMTypeRef array_type = fixed_array_type(codegen, declaration);
        LLVMTypeRef element_type = fixed_array_element_type(codegen, declaration);
        unsigned index = 0;
        for (Node *value_node = declaration->a->a; value_node; value_node = value_node->next) {
          if (index == declaration->fixed_array_length) die_at(codegen, value_node, "FixedArray initializer is too long");
          LLVMTypeRef value_type;
          LLVMValueRef value = emit_expression(codegen, value_node, &value_type);
          if (LLVMGetTypeKind(element_type) == LLVMIntegerTypeKind) value = convert_integer(codegen, value, value_type, element_type, declaration->fixed_element_unsigned);
          else if (value_type != element_type) die_at(codegen, value_node, "FixedArray element initializer type mismatch");
          LLVMValueRef element_indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, index++, 0) };
          LLVMBuildStore(codegen->builder, value, LLVMBuildGEP2(codegen->builder, array_type, field_address, element_indices, 2, "element"));
        }
        continue;
      }
      LLVMTypeRef field_type = declaration_type(codegen, declaration);
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_expression(codegen, declaration->a, &value_type);
      if (value_type != field_type) {
        if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(field_type) != LLVMIntegerTypeKind) die_at(codegen, declaration, "instance field initializer type mismatch");
        value = convert_integer(codegen, value, value_type, field_type, declaration->integer_unsigned);
      }
      LLVMBuildStore(codegen->builder, value, field_address);
    }
  }
}

static LLVMValueRef object_array_element_address(Codegen *codegen, Node *item, LLVMTypeRef *element_type_out) {
  if (!item || item->kind != N_INDEX || !item->a || item->a->kind != N_NAME || !strchr(item->a->text, '.')) return NULL;
  char receiver[256], field_name[256];
  if (!split_receiver_method(item->a->text, receiver, sizeof(receiver), field_name, sizeof(field_name))) return NULL;
  Local *object = find_local(codegen, receiver);
  if (!object || !object->class_name[0]) return NULL;
  ClassInfo *info = class_table_find(&codegen->classes, object->class_name);
  if (!info) return NULL;
  ClassField *field = class_info_find_field(info, field_name);
  if (field && (field->declaration->pointer_element_width || field->declaration->pointer_type_name[0])) {
    LLVMTypeRef field_type;
    LLVMValueRef field_address = object_field_address(codegen, object, field_name, &field_type);
    LLVMTypeRef element_type = pointer_element_type(codegen, field->declaration);
    if (!field_address || !element_type) return NULL;
    LLVMTypeRef index_type;
    LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
    LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, field_type, field_address, "object.pointer");
    *element_type_out = element_type;
    return LLVMBuildGEP2(codegen->builder, element_type, pointer, &index, 1, "element");
  }
  if (field && is_fixed_array_decl(field->declaration)) {
    LLVMTypeRef field_type;
    LLVMValueRef field_address = object_field_address(codegen, object, field_name, &field_type);
    if (!field_address) return NULL;
    LLVMTypeRef index_type;
    LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
    LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), index };
    *element_type_out = fixed_array_element_type(codegen, field->declaration);
    return LLVMBuildGEP2(codegen->builder, field_type, field_address, indices, 2, "element");
  }
  if (field && field->declaration->is_dynamic_array) {
    LLVMTypeRef field_type;
    LLVMValueRef field_address = object_field_address(codegen, object, field_name, &field_type);
    if (!field_address) return NULL;
    LLVMTypeRef element_type = dynamic_element_type(codegen, field->declaration);
    if (!element_type) die("dynamic array element type is unsupported");
    LLVMTypeRef index_type;
    LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
    *element_type_out = element_type;
    if (!field->declaration->a) return growable_array_element_address(codegen, field_address, field_type, index);
    LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, field_type, field_address, "object.array");
    return LLVMBuildGEP2(codegen->builder, element_type, pointer, &index, 1, "element");
  }
  /* Haxe allows this.staticField — resolve class statics (including final arrays). */
  Node *static_field = NULL;
  for (Node *global = codegen->globals; global; global = global->next) {
    if (strcmp(global->text, field_name)) continue;
    if (global->class_name[0] && strcmp(global->class_name, object->class_name)) continue;
    static_field = global;
    break;
  }
  if (!static_field) static_field = global_named(codegen, field_name);
  if (!static_field || !static_field->is_dynamic_array) return NULL;
  LLVMTypeRef element_type = dynamic_element_type(codegen, static_field);
  if (!element_type) die("dynamic array element type is unsupported");
  LLVMTypeRef index_type;
  LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
  LLVMValueRef global = LLVMGetNamedGlobal(codegen->module, static_field->text);
  if (!global) return NULL;
  LLVMTypeRef storage_type = LLVMGlobalGetValueType(global);
  LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), index };
  *element_type_out = element_type;
  return LLVMBuildGEP2(codegen->builder, storage_type, global, indices, 2, "element");
}

static int split_receiver_method(const char *qualified, char *receiver, size_t receiver_size, char *method, size_t method_size) {
  const char *separator = strrchr(qualified, '.');
  if (!separator) return 0;
  size_t receiver_length = (size_t)(separator - qualified);
  if (receiver_length >= receiver_size) return 0;
  memcpy(receiver, qualified, receiver_length);
  receiver[receiver_length] = '\0';
  if (strlen(separator + 1) >= method_size) return 0;
  strcpy(method, separator + 1);
  return 1;
}

static int type_is_any(const char *name) {
  return name && (!strcmp(name, "Any") || !strcmp(name, "Dynamic"));
}

static int class_is_ancestor(ClassInfo *ancestor, ClassInfo *descendant) {
  for (ClassInfo *parent = descendant ? descendant->parent : NULL; parent; parent = parent->parent) {
    if (parent == ancestor) return 1;
  }
  return 0;
}

/* Any is opaque until a concrete class is designated (assignment) or uniquely implied by a member. */
static ClassInfo *find_unique_class_with_member(Codegen *codegen, const char *member, int want_method) {
  ClassInfo *found = NULL;
  int ambiguous = 0;
  for (ClassInfo *info = codegen->classes.classes; info; info = info->next) {
    int has = want_method ? class_info_find_method(info, member) != NULL : class_info_find_field(info, member) != NULL;
    if (!has) continue;
    if (!found) {
      found = info;
      continue;
    }
    if (class_is_ancestor(found, info)) found = info;
    else if (class_is_ancestor(info, found)) continue;
    else ambiguous = 1;
  }
  return ambiguous ? NULL : found;
}

static void designate_any_class(Node *global, const char *class_name) {
  if (!global || !type_is_any(global->type_name) || !class_name || !class_name[0]) return;
  if (!global->class_name[0]) strcpy(global->class_name, class_name);
}

static void refine_any_from_statement(Codegen *codegen, Node *item) {
  for (; item; item = item->next) {
    if (item->kind == N_ASSIGN && item->text[0] && !strchr(item->text, '.') && item->a) {
      Node *global = global_named(codegen, item->text);
      if (global && type_is_any(global->type_name)) {
        if (item->a->kind == N_NEW) designate_any_class(global, item->a->text);
        else if (item->a->kind == N_NAME) {
          Node *source = global_named(codegen, item->a->text);
          if (source) {
            if (source->class_name[0]) designate_any_class(global, source->class_name);
            else if (source->type_name[0] && !type_is_any(source->type_name) && class_table_find(&codegen->classes, source->type_name))
              designate_any_class(global, source->type_name);
          }
        }
      }
    }
    if (item->a) refine_any_from_statement(codegen, item->a);
    if (item->b) refine_any_from_statement(codegen, item->b);
    if (item->c) refine_any_from_statement(codegen, item->c);
    if (item->d) refine_any_from_statement(codegen, item->d);
  }
}

static void refine_any_globals(Codegen *codegen) {
  for (Node *function = codegen->functions; function; function = function->next) {
    if (function->a) refine_any_from_statement(codegen, function->a);
  }
  if (codegen->tree && codegen->tree->a) refine_any_from_statement(codegen, codegen->tree->a);
  if (codegen->class_initializers) refine_any_from_statement(codegen, codegen->class_initializers);
}

static ClassInfo *resolve_any_class(Codegen *codegen, Node *global, Local *local, const char *member, int want_method) {
  if (local && local->class_name[0]) {
    ClassInfo *info = class_table_find(&codegen->classes, local->class_name);
    if (info) return info;
  }
  if (global) {
    if (global->class_name[0]) {
      ClassInfo *info = class_table_find(&codegen->classes, global->class_name);
      if (info) return info;
    }
    if (!type_is_any(global->type_name)) return NULL;
  } else if (!local) return NULL;
  return find_unique_class_with_member(codegen, member, want_method);
}

static ClassInfo *class_from_typed_declaration(Codegen *codegen, Node *declaration) {
  if (!declaration) return NULL;
  if (declaration->class_name[0]) {
    ClassInfo *info = class_table_find(&codegen->classes, declaration->class_name);
    if (info) return info;
  }
  if (declaration->pointer_type_name[0]) {
    ClassInfo *info = class_table_find(&codegen->classes, declaration->pointer_type_name);
    if (info) return info;
  }
  if (declaration->type_name[0] && !type_is_any(declaration->type_name) && strcmp(declaration->type_name, "Ptr")) {
    ClassInfo *info = class_table_find(&codegen->classes, declaration->type_name);
    if (info) return info;
  }
  return NULL;
}

static void stamp_class_name_from_declaration(Codegen *codegen, Local *local, Node *declaration, Node *initializer) {
  if (!local || local->class_name[0]) return;
  ClassInfo *info = class_from_typed_declaration(codegen, declaration);
  if (info) {
    strcpy(local->class_name, info->name);
    return;
  }
  if (initializer && initializer->kind == N_NEW) strcpy(local->class_name, initializer->text);
}

static ClassInfo *resolve_object_receiver(Codegen *codegen, const char *receiver, LLVMValueRef *value) {
  Local *object = find_local(codegen, receiver);
  if (object && object->class_name[0]) {
    *value = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, object->address, receiver);
    return class_table_find(&codegen->classes, object->class_name);
  }
  Node *global = global_named(codegen, receiver);
  if (global) {
    ClassInfo *info = class_from_typed_declaration(codegen, global);
    if (!info && type_is_any(global->type_name) && global->class_name[0]) info = class_table_find(&codegen->classes, global->class_name);
    if (info) {
      LLVMValueRef global_value = LLVMGetNamedGlobal(codegen->module, global->text);
      if (!global_value) return NULL;
      *value = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, global_value, receiver);
      return info;
    }
  }
  const char *separator = strchr(receiver, '.');
  if (!separator || strchr(separator + 1, '.')) return NULL;
  char outer_name[256];
  size_t outer_length = (size_t)(separator - receiver);
  if (outer_length >= sizeof(outer_name)) return NULL;
  memcpy(outer_name, receiver, outer_length);
  outer_name[outer_length] = '\0';
  object = find_local(codegen, outer_name);
  if (object && object->class_name[0]) {
    ClassInfo *outer_info = class_table_find(&codegen->classes, object->class_name);
    ClassField *field = outer_info ? class_info_find_field(outer_info, separator + 1) : NULL;
    ClassInfo *field_info = field ? class_from_typed_declaration(codegen, field->declaration) : NULL;
    if (!field_info) return NULL;
    LLVMTypeRef field_type;
    LLVMValueRef field_address = object_field_address(codegen, object, separator + 1, &field_type);
    if (!field_address || field_type != codegen->pointer_type) return NULL;
    *value = LLVMBuildLoad2(codegen->builder, field_type, field_address, "object.field.receiver");
    return field_info;
  }
  /* Global object field as receiver: global.nested.method() */
  Node *outer_global = global_named(codegen, outer_name);
  if (!outer_global) return NULL;
  ClassInfo *outer_info = class_from_typed_declaration(codegen, outer_global);
  if (!outer_info && type_is_any(outer_global->type_name) && outer_global->class_name[0])
    outer_info = class_table_find(&codegen->classes, outer_global->class_name);
  ClassField *field = outer_info ? class_info_find_field(outer_info, separator + 1) : NULL;
  ClassInfo *field_info = field ? class_from_typed_declaration(codegen, field->declaration) : NULL;
  if (!field_info) return NULL;
  LLVMValueRef global_value = LLVMGetNamedGlobal(codegen->module, outer_global->text);
  if (!global_value) return NULL;
  LLVMValueRef object_pointer = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, global_value, outer_name);
  LLVMValueRef field_address = LLVMBuildStructGEP2(codegen->builder, outer_info->struct_type, object_pointer, field->struct_index, "object.field");
  LLVMTypeRef field_type = class_field_type(codegen, field->declaration);
  if (field_type != codegen->pointer_type) return NULL;
  *value = LLVMBuildLoad2(codegen->builder, field_type, field_address, "object.field.receiver");
  return field_info;
}

static LLVMValueRef emit_new_expression(Codegen *codegen, Node *item, LLVMTypeRef *type) {
  ClassInfo *info = class_table_find(&codegen->classes, item->text);
  if (!info) die_at(codegen, item, "new requires a known class type");
  if (info->is_abstract) die("cannot instantiate an abstract class");
  if (!info->struct_type) die("class type was not emitted");
  LLVMValueRef object = arena_alloc(codegen, info->struct_type, "object");
  if (info->has_vtable) {
    if (!info->vtable_global) die("concrete class is missing a vtable");
    LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
    LLVMValueRef vptr = LLVMBuildGEP2(codegen->builder, info->struct_type, object, indices, 2, "object.vptr");
    LLVMBuildStore(codegen->builder, info->vtable_global, vptr);
  }
  store_object_array_field_initializers(codegen, info, object);
  store_object_field_initializers(codegen, info, object);
  ClassMethod *ctor = class_info_find_method(info, "new");
  if (ctor && !ctor->is_abstract) {
    Function *function = function_value(codegen, ctor->mangled);
    if (!function) die("constructor was not declared");
    LLVMValueRef arguments[256];
    unsigned count = 0;
    arguments[count++] = object;
    Node *parameter = function->declaration->b ? function->declaration->b->next : NULL;
    for (Node *argument = item->a; argument; argument = argument->next, parameter = parameter ? parameter->next : NULL) {
      if (!parameter || count == 256) die("constructor argument count mismatch");
      LLVMTypeRef argument_type;
      LLVMTypeRef parameter_type = is_fixed_array_decl(parameter) ? codegen->pointer_type : declaration_type(codegen, parameter);
      if (is_fixed_array_decl(parameter)) {
        arguments[count] = fixed_array_argument_address(codegen, argument);
        argument_type = codegen->pointer_type;
      } else arguments[count] = emit_expression(codegen, argument, &argument_type);
      if (argument_type != parameter_type) {
        arguments[count] = coerce_value(codegen, argument, arguments[count], argument_type, parameter_type, parameter->integer_unsigned, "constructor argument type mismatch");
      }
      count++;
    }
    if (parameter) die("constructor argument count mismatch");
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(function->value), function->value, arguments, count, "");
  }
  codegen->last_pointer_element_type = info->struct_type;
  codegen->last_pointer_element_unsigned = 0;
  *type = codegen->pointer_type;
  return object;
}

static LLVMValueRef emit_method_call(Codegen *codegen, Node *item, LLVMTypeRef *type) {
  char receiver[256], method_name[256];
  if (!strcmp(item->text, "super") || !strncmp(item->text, "super.", 6)) {
    if (!codegen->current_class_name[0]) die("super requires an instance method");
    ClassInfo *info = class_table_find(&codegen->classes, codegen->current_class_name);
    if (!info || !info->parent) die("super requires a parent class");
    Local *this_local = find_local(codegen, "this");
    if (!this_local) die("super requires this");
    LLVMValueRef this_value = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, this_local->address, "this");
    const char *target_name = !strcmp(item->text, "super") ? "new" : item->text + 6;
    ClassMethod *method = class_info_find_method(info->parent, target_name);
    if (!method) die("unknown super method");
    Function *function = function_value(codegen, method->mangled);
    if (!function) {
      /* Prefer the parent's own mangled implementation. */
      char mangled[256];
      if (!strcmp(target_name, "new")) snprintf(mangled, sizeof(mangled), "%s_new", info->parent->name);
      else snprintf(mangled, sizeof(mangled), "%s_%s", info->parent->name, target_name);
      function = function_value(codegen, mangled);
    }
    if (!function) die("super method was not declared");
    LLVMValueRef arguments[256];
    unsigned count = 0;
    arguments[count++] = this_value;
    Node *parameter = function->declaration->b ? function->declaration->b->next : NULL;
    for (Node *argument = item->a; argument; argument = argument->next, parameter = parameter ? parameter->next : NULL) {
      if (!parameter || count == 256) die("super call argument count mismatch");
      LLVMTypeRef argument_type;
      arguments[count] = emit_expression(codegen, argument, &argument_type);
      LLVMTypeRef parameter_type = declaration_type(codegen, parameter);
      if (argument_type != parameter_type) {
        arguments[count] = coerce_value(codegen, argument, arguments[count], argument_type, parameter_type, parameter->integer_unsigned, "super call argument type mismatch");
      }
      count++;
    }
    if (parameter) die("super call argument count mismatch");
    *type = function->return_type;
    LLVMValueRef call = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(function->value), function->value, arguments, count, function->return_type == codegen->void_type ? "" : "super.call");
    return *type == codegen->void_type ? LLVMConstInt(codegen->i32, 0, 0) : call;
  }
  if (!split_receiver_method(item->text, receiver, sizeof(receiver), method_name, sizeof(method_name))) return NULL;
  LLVMValueRef this_value;
  ClassInfo *info = resolve_object_receiver(codegen, receiver, &this_value);
  if (!info) {
    Node *global = global_named(codegen, receiver);
    if (!global || !type_is_any(global->type_name)) return NULL;
    info = resolve_any_class(codegen, global, NULL, method_name, 1);
    if (!info) return NULL;
    designate_any_class(global, info->name);
    LLVMValueRef global_value = LLVMGetNamedGlobal(codegen->module, global->text);
    if (!global_value) return NULL;
    this_value = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, global_value, receiver);
  }
  ClassMethod *method = class_info_find_method(info, method_name);
  if (!method) return NULL;
  LLVMValueRef callee = NULL;
  LLVMTypeRef callee_type = NULL;
  if (method->is_virtual) {
    if (method->vtable_index < 0) die("virtual method is missing a vtable slot");
    LLVMValueRef indices0[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
    LLVMValueRef vptr_address = LLVMBuildGEP2(codegen->builder, info->struct_type, this_value, indices0, 2, "object.vptr");
    LLVMValueRef vtable = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, vptr_address, "vtable");
    LLVMValueRef indices1[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, (unsigned)method->vtable_index, 0) };
    LLVMTypeRef vtable_type = info->vtable_type ? info->vtable_type : (info->parent ? info->parent->vtable_type : NULL);
    if (!vtable_type) die("missing vtable type");
    LLVMValueRef slot = LLVMBuildGEP2(codegen->builder, vtable_type, vtable, indices1, 2, "vtable.slot");
    callee = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, slot, "method");
    /* Build the callee type from the method declaration (abstract bases have no symbol). */
    LLVMTypeRef parameter_types[256];
    unsigned parameter_count = 0;
    parameter_types[parameter_count++] = codegen->pointer_type;
    for (Node *parameter = method->declaration->b; parameter; parameter = parameter->next) {
      if (parameter_count == 256) die("method has too many parameters");
      parameter_types[parameter_count++] = declaration_type(codegen, parameter);
    }
    *type = function_return_type(codegen, method->declaration);
    callee_type = LLVMFunctionType(*type, parameter_types, parameter_count, 0);
  } else {
    Function *function = function_value(codegen, method->mangled);
    if (!function) die("instance method was not declared");
    callee = function->value;
    callee_type = LLVMGlobalGetValueType(function->value);
    *type = function->return_type;
  }
  LLVMValueRef arguments[256];
  unsigned count = 0;
  arguments[count++] = this_value;
  Node *parameter = method->declaration->b;
  for (Node *argument = item->a; argument; argument = argument->next, parameter = parameter ? parameter->next : NULL) {
    if (!parameter || count == 256) die("method call argument count mismatch");
    LLVMTypeRef argument_type;
    arguments[count] = emit_expression(codegen, argument, &argument_type);
    LLVMTypeRef parameter_type = declaration_type(codegen, parameter);
    if (argument_type != parameter_type) {
      arguments[count] = coerce_value(codegen, argument, arguments[count], argument_type, parameter_type, parameter->integer_unsigned, "method call argument type mismatch");
    }
    count++;
  }
  if (parameter) die("method call argument count mismatch");
  LLVMValueRef call = LLVMBuildCall2(codegen->builder, callee_type, callee, arguments, count, *type == codegen->void_type ? "" : "method.call");
  return *type == codegen->void_type ? LLVMConstInt(codegen->i32, 0, 0) : call;
}



static LLVMValueRef walk_typedef_field_address(Codegen *codegen, LLVMValueRef address, LLVMTypeRef record_type,
                                               Node *typedef_declaration, const char *field_name, LLVMTypeRef *field_type) {
  while (typedef_declaration) {
    const char *next_separator = strchr(field_name, '.');
    char field_buffer[256];
    size_t field_name_length = next_separator ? (size_t)(next_separator - field_name) : strlen(field_name);
    if (field_name_length >= sizeof(field_buffer)) return NULL;
    memcpy(field_buffer, field_name, field_name_length);
    field_buffer[field_name_length] = '\0';
    unsigned field_index;
    Node *field = typedef_field_named(typedef_declaration, field_buffer, &field_index);
    if (!field) return NULL;
    LLVMTypeRef next_type = declaration_type(codegen, field);
    address = LLVMBuildStructGEP2(codegen->builder, record_type, address, field_index, "field");
    if (!next_separator) {
      *field_type = next_type;
      return address;
    }
    if (field->pointer_element_width || field->pointer_type_name[0]) {
      record_type = pointer_element_type(codegen, field);
      if (!record_type) return NULL;
      address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "field.pointer");
      typedef_declaration = typedef_named(codegen, field->pointer_type_name);
    } else {
      record_type = next_type;
      typedef_declaration = typedef_named(codegen, field->type_name);
    }
    if (!typedef_declaration) return NULL;
    field_name = next_separator + 1;
  }
  return NULL;
}

static LLVMValueRef local_field_address(Codegen *codegen, const char *name, LLVMTypeRef *field_type) {
  const char *separator = strchr(name, '.');
  if (!separator) return NULL;
  char local_name[256];
  size_t local_name_length = (size_t)(separator - name);
  if (local_name_length >= sizeof(local_name)) return NULL;
  memcpy(local_name, name, local_name_length);
  local_name[local_name_length] = '\0';
  Local *item_local = find_local(codegen, local_name);
  if (!item_local) return NULL;
  if (item_local->class_name[0] && !strchr(separator + 1, '.')) {
    LLVMValueRef object_address = object_field_address(codegen, item_local, separator + 1, field_type);
    if (object_address) return object_address;
  }
  /* this.ptrField.flexArray — class field is Ptr<Typedef>, remaining path is the record. */
  if (item_local->class_name[0] && strchr(separator + 1, '.')) {
    const char *rest = separator + 1;
    const char *next = strchr(rest, '.');
    char first[256];
    size_t first_length = (size_t)(next - rest);
    if (first_length >= sizeof(first)) return NULL;
    memcpy(first, rest, first_length);
    first[first_length] = '\0';
    ClassInfo *info = class_table_find(&codegen->classes, item_local->class_name);
    ClassField *class_field = info ? class_info_find_field(info, first) : NULL;
    if (class_field) {
      LLVMTypeRef class_field_type;
      LLVMValueRef address = object_field_address(codegen, item_local, first, &class_field_type);
      if (!address) return NULL;
      LLVMTypeRef record_type;
      Node *typedef_declaration = NULL;
      if (declaration_is_typed_pointer(class_field->declaration)) {
        record_type = pointer_element_type(codegen, class_field->declaration);
        typedef_declaration = typedef_named(codegen, class_field->declaration->pointer_type_name);
        address = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, address, "object.field.pointer");
      } else {
        record_type = class_field_type;
        typedef_declaration = typedef_for_type(codegen, record_type);
        if (!typedef_declaration && class_field->declaration->type_name[0])
          typedef_declaration = typedef_named(codegen, class_field->declaration->type_name);
      }
      if (record_type && typedef_declaration)
        return walk_typedef_field_address(codegen, address, record_type, typedef_declaration, next + 1, field_type);
    }
  }
  LLVMTypeRef record_type = item_local->pointer_element_type ? item_local->pointer_element_type : item_local->type;
  LLVMValueRef address = item_local->pointer_element_type ? LLVMBuildLoad2(codegen->builder, codegen->pointer_type, item_local->address, local_name) : item_local->address;
  Node *typedef_declaration = typedef_for_type(codegen, record_type);
  if (!typedef_declaration) return NULL;
  return walk_typedef_field_address(codegen, address, record_type, typedef_declaration, separator + 1, field_type);
}


static LLVMValueRef emit_expression(Codegen *codegen, Node *item, LLVMTypeRef *type) {
  active_diagnostic_node = item;
  if (item->kind == N_NEW) return emit_new_expression(codegen, item, type);
  if (item->kind == N_SWITCH) return emit_switch_expression(codegen, item, type);
  if (item->kind == N_BLOCK && item->is_block_expression) {
    Node *child = item->a;
    if (!child) {
      *type = codegen->i32;
      return LLVMConstInt(codegen->i32, 0, 0);
    }
    while (child->next) {
      emit_statement(codegen, child);
      child = child->next;
    }
    if (child->kind == N_EXPR && child->a) return emit_expression(codegen, child->a, type);
    emit_statement(codegen, child);
    *type = codegen->i32;
    return LLVMConstInt(codegen->i32, 0, 0);
  }
  if (item->kind == N_INT) {
    /* Prefer the full parsed magnitude so hex literals like 0xFFFFFFFF80010C44 survive. */
    if (item->number > 0xffffffffull) {
      *type = codegen->i64;
      return LLVMConstInt(codegen->i64, item->number, 0);
    }
    *type = codegen->i32;
    return LLVMConstInt(codegen->i32, (unsigned)item->number, 0);
  }
  if (item->kind == N_FLOAT) {
    note_float_use(codegen);
    double value = strtod(item->text, NULL);
    if (item->integer_width == 32) {
      *type = codegen->f32;
      return LLVMConstReal(codegen->f32, value);
    }
    *type = codegen->f64;
    return LLVMConstReal(codegen->f64, value);
  }
  if (item->kind == N_NULL) {
    *type = codegen->pointer_type;
    codegen->last_pointer_element_type = NULL;
    return LLVMConstNull(*type);
  }
  if (item->kind == N_BOOL) { *type = codegen->i1; return LLVMConstInt(codegen->i1, item->value, 0); }
  if (item->kind == N_STR) { *type = codegen->pointer_type; return LLVMBuildGlobalStringPtr(codegen->builder, item->text, "string"); }
  if (item->kind == N_TERNARY) {
    LLVMTypeRef cond_type, then_type, else_type;
    LLVMValueRef condition = emit_expression(codegen, item->a, &cond_type);
    LLVMBasicBlockRef then_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "tern.then");
    LLVMBasicBlockRef else_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "tern.else");
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "tern.end");
    LLVMBuildCondBr(codegen->builder, as_boolean(codegen, condition, cond_type), then_block, else_block);
    LLVMPositionBuilderAtEnd(codegen->builder, then_block);
    LLVMValueRef then_value = emit_expression(codegen, item->b, &then_type);
    LLVMBasicBlockRef then_end = LLVMGetInsertBlock(codegen->builder);
    if (!LLVMGetBasicBlockTerminator(then_end)) LLVMBuildBr(codegen->builder, end_block);
    LLVMPositionBuilderAtEnd(codegen->builder, else_block);
    LLVMValueRef else_value = emit_expression(codegen, item->c, &else_type);
    LLVMBasicBlockRef else_end = LLVMGetInsertBlock(codegen->builder);
    if (!LLVMGetBasicBlockTerminator(else_end)) LLVMBuildBr(codegen->builder, end_block);
    LLVMPositionBuilderAtEnd(codegen->builder, end_block);
    if (then_type != else_type) {
      if (LLVMGetTypeKind(then_type) == LLVMIntegerTypeKind && LLVMGetTypeKind(else_type) == LLVMIntegerTypeKind) {
        unsigned width = LLVMGetIntTypeWidth(then_type);
        if (LLVMGetIntTypeWidth(else_type) > width) width = LLVMGetIntTypeWidth(else_type);
        if (width < 32) width = 32;
        *type = LLVMIntTypeInContext(codegen->context, width);
        then_value = convert_integer(codegen, then_value, then_type, *type, 1);
        else_value = convert_integer(codegen, else_value, else_type, *type, 1);
      } else if (type_is_float(codegen, then_type) || type_is_float(codegen, else_type)) {
        note_float_use(codegen);
        *type = (then_type == codegen->f32 && else_type == codegen->f32) ? codegen->f32 : codegen->f64;
        then_value = cast_to_float(codegen, then_value, then_type, *type);
        else_value = cast_to_float(codegen, else_value, else_type, *type);
      } else die_at(codegen, item, "ternary branches must share a type");
    } else *type = then_type;
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, *type, "tern");
    LLVMAddIncoming(phi, &then_value, &then_end, 1);
    LLVMAddIncoming(phi, &else_value, &else_end, 1);
    return phi;
  }
  if (item->kind == N_THROW) {
    LLVMTypeRef thrown_type;
    LLVMValueRef thrown = emit_expression(codegen, item->a, &thrown_type);
    if (codegen->catch_stack) {
      if (!codegen->exception_slot) die_at(codegen, item, "internal: missing exception slot");
      LLVMValueRef boxed = thrown;
      if (thrown_type != codegen->pointer_type) {
        if (LLVMGetTypeKind(thrown_type) == LLVMIntegerTypeKind)
          boxed = LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, thrown, thrown_type, codegen->i64, 1), codegen->pointer_type, "throw.box");
        else die_at(codegen, item, "throw value must be an integer, pointer, or class instance");
      }
      LLVMBuildStore(codegen->builder, boxed, codegen->exception_slot);
      LLVMBuildBr(codegen->builder, codegen->catch_stack->handler);
    } else {
      /* Uncaught: park the CPU Intel-style. RISC-V fans can write their own handler. */
      LLVMTypeRef asm_type = LLVMFunctionType(codegen->void_type, NULL, 0, 0);
      LLVMValueRef hlt = LLVMGetInlineAsm(asm_type, "hlt", 3, "", 0, 1, 0, LLVMInlineAsmDialectIntel, 0);
      LLVMBuildCall2(codegen->builder, asm_type, hlt, NULL, 0, "");
      LLVMBuildUnreachable(codegen->builder);
    }
    *type = codegen->i32;
    return LLVMConstInt(codegen->i32, 0, 0);
  }
  if (item->kind == N_TRY) {
    if (!codegen->exception_slot) {
      LLVMBasicBlockRef current = LLVMGetInsertBlock(codegen->builder);
      LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(codegen->main_fn);
      LLVMValueRef first = LLVMGetFirstInstruction(entry);
      if (first) LLVMPositionBuilderBefore(codegen->builder, first);
      else LLVMPositionBuilderAtEnd(codegen->builder, entry);
      codegen->exception_slot = LLVMBuildAlloca(codegen->builder, codegen->pointer_type, "exception");
      LLVMBuildStore(codegen->builder, LLVMConstNull(codegen->pointer_type), codegen->exception_slot);
      LLVMPositionBuilderAtEnd(codegen->builder, current);
    }
    Node *catch_clause = item->b;
    if (!catch_clause) die_at(codegen, item, "try requires a catch");
    CatchFrame frame = {0};
    frame.handler = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "catch");
    frame.join = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "try.join");
    strcpy(frame.catch_name, catch_clause->text);
    frame.value_type = codegen->pointer_type;
    frame.parent = codegen->catch_stack;
    codegen->catch_stack = &frame;
    emit_statement(codegen, item->a);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, frame.join);
    codegen->catch_stack = frame.parent;
    LLVMPositionBuilderAtEnd(codegen->builder, frame.handler);
    Local *saved_locals = codegen->locals;
    LLVMValueRef caught = LLVMBuildLoad2(codegen->builder, codegen->pointer_type, codegen->exception_slot, "caught");
    LLVMBuildStore(codegen->builder, LLVMConstNull(codegen->pointer_type), codegen->exception_slot);
    add_local(codegen, frame.catch_name, codegen->pointer_type, caught, NULL, 1);
    emit_statement(codegen, catch_clause->a);
    codegen->locals = saved_locals;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, frame.join);
    /* Additional catch clauses: Haxe allows several; bind only the first for now, chain the rest. */
    for (Node *extra = catch_clause->next; extra; extra = extra->next) {
      /* Unreachable fallback catches kept for syntax completeness. */
    }
    LLVMPositionBuilderAtEnd(codegen->builder, frame.join);
    *type = codegen->i32;
    return LLVMConstInt(codegen->i32, 0, 0);
  }
  if (item->kind == N_IS) {
    *type = codegen->i1;
    const char *type_name = item->b ? item->b->text : "";
    LLVMTypeRef value_type;
    LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
    if (!strcmp(type_name, "Int") || !strcmp(type_name, "UInt"))
      return LLVMConstInt(codegen->i1, LLVMGetTypeKind(value_type) == LLVMIntegerTypeKind, 0);
    if (!strcmp(type_name, "Float"))
      return LLVMConstInt(codegen->i1, value_type == codegen->f64 || value_type == codegen->f32, 0);
    if (!strcmp(type_name, "Single"))
      return LLVMConstInt(codegen->i1, value_type == codegen->f32, 0);
    if (!strcmp(type_name, "Bool"))
      return LLVMConstInt(codegen->i1, value_type == codegen->i1, 0);
    if (!strcmp(type_name, "String"))
      return LLVMConstInt(codegen->i1, value_type == codegen->pointer_type, 0);
    ClassInfo *want = class_table_find(&codegen->classes, type_name);
    if (want) {
      if (item->a->kind == N_NAME) {
        Local *local = find_local(codegen, item->a->text);
        if (local && local->class_name[0]) {
          ClassInfo *have = class_table_find(&codegen->classes, local->class_name);
          int match = have && (have == want || class_is_ancestor(want, have));
          return LLVMConstInt(codegen->i1, match, 0);
        }
        Node *global = global_named(codegen, item->a->text);
        if (global && global->class_name[0]) {
          ClassInfo *have = class_table_find(&codegen->classes, global->class_name);
          int match = have && (have == want || class_is_ancestor(want, have));
          return LLVMConstInt(codegen->i1, match, 0);
        }
        if (global && !strcmp(global->type_name, type_name)) return LLVMConstInt(codegen->i1, 1, 0);
      }
      if (item->a->kind == N_NEW && !strcmp(item->a->text, type_name)) return LLVMConstInt(codegen->i1, 1, 0);
      /* Runtime class tags aren't stored on every object yet — null is never an instance. */
      if (value_type == codegen->pointer_type) {
        return LLVMBuildICmp(codegen->builder, LLVMIntNE, value, LLVMConstNull(value_type), "is.nonnull");
      }
    }
    if (typedef_named(codegen, type_name))
      return LLVMConstInt(codegen->i1, typedef_for_type(codegen, value_type) && !strcmp(typedef_for_type(codegen, value_type)->text, type_name), 0);
    die_at(codegen, item, "unsupported `is` type test");
  }
  if (item->kind == N_NAME) {
    if (!strcmp(item->text, "null")) {
      *type = codegen->pointer_type;
      codegen->last_pointer_element_type = NULL;
      return LLVMConstNull(*type);
    }
    Node *enum_variant = NULL;
    Node *enumeration = enum_for_variant(codegen, item->text, &enum_variant);
    if (enumeration && enum_variant) {
      return emit_enum_value(codegen, enumeration, enum_variant, NULL, type);
    }
    size_t length = strlen(item->text);
    if (length > 7 && !strcmp(item->text + length - 7, ".length")) {
      /* Prefer a real record/pointer field named `length` (e.g. ACPI RSDP) over String/Array.length. */
      LLVMValueRef field_value = local_field_value(codegen, item->text, type);
      if (field_value) return field_value;
      field_value = global_field_value(codegen, item->text, type, NULL);
      if (field_value) return field_value;
      char name[256];
      memcpy(name, item->text, length - 7);
      name[length - 7] = '\0';
      LLVMValueRef array_address;
      LLVMTypeRef array_type, element_type;
      if (resolve_growable_array(codegen, name, &array_address, &array_type, &element_type)) {
        *type = codegen->i32;
        return LLVMBuildLoad2(codegen->builder, codegen->i32, growable_array_length_address(codegen, array_address, array_type), "array.length");
      }
      Node *global_declaration = global_named(codegen, name);
      if (global_declaration && global_declaration->is_dynamic_array && global_declaration->a && global_declaration->a->kind == N_ARRAY) {
        unsigned count = 0;
        for (Node *value = global_declaration->a->a; value; value = value->next) count++;
        *type = codegen->i32;
        return LLVMConstInt(codegen->i32, count, 0);
      }
      Local *item_local = find_local(codegen, name);
      if (!item_local) die_at(codegen, item, "length requires a string value");
      *type = codegen->word_type;
      if (item_local->string_length) {
        LLVMTypeRef length_type = LLVMTypeOf(item_local->string_length);
        if (length_type == codegen->word_type) return item_local->string_length;
        if (LLVMGetTypeKind(length_type) == LLVMIntegerTypeKind)
          return convert_integer(codegen, item_local->string_length, length_type, codegen->word_type, 1);
        return item_local->string_length;
      }
      if (item_local->type == codegen->pointer_type) {
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, name);
        LLVMValueRef function = ensure_string_length_function(codegen);
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(function), function, &value, 1, "string.length");
      }
      die_at(codegen, item, "length requires a string value");
    }
    Local *item_local = find_local(codegen, item->text);
    if (item_local && item_local->is_growable_array) {
      *type = codegen->pointer_type;
      return growable_array_data_address(codegen, item_local->address, item_local->type);
    }
    if (!item_local) {
      LLVMValueRef field_value = local_field_value(codegen, item->text, type);
      if (field_value) return field_value;
      field_value = global_field_value(codegen, item->text, type, NULL);
      if (field_value) return field_value;
      Node *global_declaration = global_named(codegen, item->text);
      if (global_declaration && global_declaration->is_imported && global_declaration->is_final && global_declaration->a) {
        LLVMTypeRef value_type;
        LLVMValueRef value = emit_constant(codegen, global_declaration->a, NULL, 0, &value_type);
        LLVMTypeRef declared_type = declaration_type(codegen, global_declaration);
        if (value_type != declared_type) {
          if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(declared_type) != LLVMIntegerTypeKind || !LLVMIsAConstantInt(value)) die_at(codegen, item, "imported final initializer type mismatch");
          value = LLVMConstInt(declared_type, LLVMConstIntGetZExtValue(value), global_declaration->integer_unsigned);
        }
        *type = declared_type;
        return value;
      }
      LLVMValueRef global = LLVMGetNamedGlobal(codegen->module, global_declaration ? global_declaration->text : item->text);
      if (global && global_declaration) {
        if (is_growable_array_declaration(global_declaration)) {
          *type = codegen->pointer_type;
          return growable_array_data_address(codegen, global, LLVMGlobalGetValueType(global));
        }
        if (global_declaration->is_dynamic_array) {
          LLVMTypeRef storage_type = LLVMGlobalGetValueType(global);
          LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
          *type = codegen->pointer_type;
          return LLVMBuildGEP2(codegen->builder, storage_type, global, indices, 2, "array.data");
        }
        /* FixedArray decays to a pointer to the first element, like a C array. */
        if (is_fixed_array_decl(global_declaration)) {
          LLVMTypeRef storage_type = LLVMGlobalGetValueType(global);
          LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
          *type = codegen->pointer_type;
          codegen->last_pointer_element_type = fixed_array_element_type(codegen, global_declaration);
          codegen->last_pointer_element_unsigned = global_declaration->fixed_element_unsigned;
          return LLVMBuildGEP2(codegen->builder, storage_type, global, indices, 2, "fixed.data");
        }
        *type = declaration_type(codegen, global_declaration);
        return LLVMBuildLoad2(codegen->builder, *type, global, item->text);
      }
      if (global) { *type = codegen->pointer_type; return global; }
      item_local = local(codegen, item->text, item);
    }
    LLVMValueRef value = LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, item->text);
    if (LLVMGetTypeKind(item_local->type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(item_local->type) < 32) {
      *type = codegen->i32;
      return item_local->integer_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
    }
    *type = item_local->type;
    return value;
  }
  if (item->kind == N_INDEX) {
    {
      LLVMTypeRef object_element_type;
      LLVMValueRef object_element = object_array_element_address(codegen, item, &object_element_type);
      if (object_element) {
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, object_element_type, object_element, "element.load");
        *type = object_element_type;
        if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
          *type = codegen->i32;
          return LLVMBuildZExt(codegen->builder, value, *type, "zextend");
        }
        return value;
      }
    }
    {
      LLVMTypeRef field_element_type;
      LLVMValueRef field_element = typedef_field_fixed_array_element(codegen, item, &field_element_type);
      if (field_element) {
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, field_element_type, field_element, "element.load");
        *type = field_element_type;
        if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
          *type = codegen->i32;
          return LLVMBuildZExt(codegen->builder, value, *type, "zextend");
        }
        return value;
      }
    }
    if (item->a && item->a->kind == N_NAME) {
      LLVMValueRef growable_address;
      LLVMTypeRef growable_type, growable_element_type;
      if (resolve_growable_array(codegen, item->a->text, &growable_address, &growable_type, &growable_element_type)) {
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
        LLVMValueRef address = growable_array_element_address(codegen, growable_address, growable_type, index);
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, growable_element_type, address, "element.load");
        *type = growable_element_type;
        if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
          *type = codegen->i32;
          return LLVMBuildZExt(codegen->builder, value, *type, "zextend");
        }
        return value;
      }
      Local *dynamic_array = find_local(codegen, item->a->text);
      if (dynamic_array && dynamic_array->dynamic_element_type) {
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
        LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, dynamic_array->type, dynamic_array->address, item->a->text);
        LLVMValueRef address = LLVMBuildGEP2(codegen->builder, dynamic_array->dynamic_element_type, pointer, &index, 1, "element");
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, dynamic_array->dynamic_element_type, address, "element.load");
        *type = dynamic_array->dynamic_element_type;
        if (dynamic_array->pointer_element_type) {
          codegen->last_pointer_element_type = dynamic_array->pointer_element_type;
          codegen->last_pointer_element_unsigned = dynamic_array->dynamic_element_unsigned;
        }
        return value;
      }
      if (dynamic_array && dynamic_array->pointer_element_type) {
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
        LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, dynamic_array->type, dynamic_array->address, item->a->text);
        LLVMValueRef address = LLVMBuildGEP2(codegen->builder, dynamic_array->pointer_element_type, pointer, &index, 1, "element");
        LLVMValueRef value = load_volatile(codegen, dynamic_array->pointer_element_type, address, "element.load");
        *type = dynamic_array->pointer_element_type;
        return value;
      }
      LLVMTypeRef array_type;
      Node *array_field = NULL;
      LLVMValueRef array_pointer = global_field_value(codegen, item->a->text, &array_type, &array_field);
      if (array_pointer && array_field && array_field->is_dynamic_array) {
        LLVMTypeRef element_type = dynamic_element_type(codegen, array_field);
        if (!element_type) die("dynamic array element type is unsupported");
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
        LLVMValueRef address = LLVMBuildGEP2(codegen->builder, element_type, array_pointer, &index, 1, "element");
        *type = element_type;
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, element_type, address, "element.load");
        if (array_field->dynamic_element_is_pointer) {
          Node *typedef_declaration = typedef_named(codegen, array_field->dynamic_element_pointer_type_name);
          codegen->last_pointer_element_type = typedef_declaration ? LLVMGetTypeByName2(codegen->context, typedef_declaration->text) : NULL;
          codegen->last_pointer_element_unsigned = array_field->dynamic_element_unsigned;
        }
        return value;
      }
      Node *global_array = global_named(codegen, item->a->text);
      if (global_array && global_array->is_dynamic_array) {
        LLVMTypeRef element_type = dynamic_element_type(codegen, global_array);
        if (!element_type) die("dynamic array element type is unsupported");
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
        LLVMValueRef global = LLVMGetNamedGlobal(codegen->module, global_array->text);
        LLVMTypeRef storage_type = LLVMGlobalGetValueType(global);
        LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), index };
        LLVMValueRef address = LLVMBuildGEP2(codegen->builder, storage_type, global, indices, 2, "element");
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, element_type, address, "element.load");
        *type = element_type;
        if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
          *type = codegen->i32;
          return global_array->dynamic_element_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
        }
        return value;
      }
      LLVMTypeRef element_type;
      LLVMValueRef global_element = global_fixed_array_element(codegen, item, &element_type);
      if (global_element) {
        LLVMValueRef value = LLVMBuildLoad2(codegen->builder, element_type, global_element, "element.load");
        *type = element_type;
        if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
          *type = codegen->i32;
          return LLVMBuildZExt(codegen->builder, value, *type, "zextend");
        }
        return value;
      }
      Local *string_local = find_local(codegen, item->a->text);
      if (string_local && string_local->type == codegen->pointer_type && !string_local->fixed_element_type && !string_local->dynamic_element_type && !string_local->is_growable_array) {
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b, &index_type), index_type);
        LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, string_local->type, string_local->address, item->a->text);
        LLVMValueRef address = LLVMBuildGEP2(codegen->builder, codegen->i8, pointer, &index, 1, "character");
        *type = codegen->i32;
        return LLVMBuildZExt(codegen->builder, LLVMBuildLoad2(codegen->builder, codegen->i8, address, "byte"), codegen->i32, "charcode");
      }
    }
    Local *array_local;
    LLVMValueRef address = fixed_array_element(codegen, item, &array_local);
    LLVMValueRef value = LLVMBuildLoad2(codegen->builder, array_local->fixed_element_type, address, "element.load");
    *type = array_local->fixed_element_type;
    if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
      *type = codegen->i32;
      return array_local->fixed_element_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
    }
    return value;
  }
  if (item->kind == N_FIELD) {
    if (item->is_safe_field) {
      LLVMTypeRef base_type;
      LLVMValueRef base = emit_expression(codegen, item->a, &base_type);
      if (base_type != codegen->pointer_type) die_at(codegen, item, "?. requires a pointer receiver");
      LLVMBasicBlockRef load_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "safe.load");
      LLVMBasicBlockRef null_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "safe.null");
      LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "safe.end");
      LLVMValueRef is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, base, LLVMConstNull(base_type), "safe.isnull");
      LLVMBuildCondBr(codegen->builder, is_null, null_block, load_block);
      LLVMPositionBuilderAtEnd(codegen->builder, load_block);
      item->is_safe_field = 0;
      LLVMValueRef loaded = emit_expression(codegen, item, type);
      item->is_safe_field = 1;
      LLVMBasicBlockRef load_end = LLVMGetInsertBlock(codegen->builder);
      if (!LLVMGetBasicBlockTerminator(load_end)) LLVMBuildBr(codegen->builder, end_block);
      LLVMPositionBuilderAtEnd(codegen->builder, null_block);
      LLVMValueRef null_value = (*type == codegen->pointer_type || LLVMGetTypeKind(*type) == LLVMPointerTypeKind)
        ? LLVMConstNull(*type)
        : (type_is_float(codegen, *type) ? LLVMConstReal(*type, 0.0) : LLVMConstNull(*type));
      if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind) null_value = LLVMConstInt(*type, 0, 0);
      if (*type == codegen->i1) null_value = LLVMConstInt(codegen->i1, 0, 0);
      LLVMBasicBlockRef null_end = LLVMGetInsertBlock(codegen->builder);
      LLVMBuildBr(codegen->builder, end_block);
      LLVMPositionBuilderAtEnd(codegen->builder, end_block);
      LLVMValueRef phi = LLVMBuildPhi(codegen->builder, *type, "safe");
      LLVMAddIncoming(phi, &loaded, &load_end, 1);
      LLVMAddIncoming(phi, &null_value, &null_end, 1);
      return phi;
    }
    if (item->a && item->a->kind == N_NAME) {
      Node *enumeration = enum_named(codegen, item->a->text);
      Node *variant = enumeration ? enum_variant_in(enumeration, item->text) : NULL;
      if (variant) return emit_enum_value(codegen, enumeration, variant, NULL, type);
      char dotted[512];
      snprintf(dotted, sizeof(dotted), "%s.%s", item->a->text, item->text);
      LLVMValueRef field_value = local_field_value(codegen, dotted, type);
      if (field_value) return field_value;
      field_value = global_field_value(codegen, dotted, type, NULL);
      if (field_value) return field_value;
    }
    LLVMValueRef address = record_field_address(codegen, item, type);
    if (address) return LLVMBuildLoad2(codegen->builder, *type, address, "field.load");
    LLVMTypeRef record_type;
    LLVMValueRef record = emit_expression(codegen, item->a, &record_type);
    if (record_type == codegen->pointer_type) {
      /* Ptr<Typedef> receiver — load pointee layout from last_pointer_element_type or unique typedef field match. */
      LLVMTypeRef pointee = codegen->last_pointer_element_type;
      Node *typedef_declaration = pointee ? typedef_for_type(codegen, pointee) : NULL;
      if (!typedef_declaration && item->a->kind == N_NAME) {
        Local *pointer_local = find_local(codegen, item->a->text);
        if (pointer_local && pointer_local->pointer_element_type) {
          pointee = pointer_local->pointer_element_type;
          typedef_declaration = typedef_for_type(codegen, pointee);
        }
      }
      if (typedef_declaration) {
        unsigned field_index;
        Node *field = typedef_field_named(typedef_declaration, item->text, &field_index);
        if (field) {
          LLVMValueRef field_address = LLVMBuildStructGEP2(codegen->builder, pointee, record, field_index, "ptr.field");
          *type = declaration_type(codegen, field);
          remember_pointer_element_from_decl(codegen, field);
          LLVMValueRef value = LLVMBuildLoad2(codegen->builder, *type, field_address, "field.load");
          if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
            *type = codegen->i32;
            return field->integer_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
          }
          return value;
        }
      }
    }
    if (LLVMGetTypeKind(record_type) == LLVMStructTypeKind) {
      Node *typedef_declaration = typedef_for_type(codegen, record_type);
      unsigned field_index;
      Node *field = typedef_declaration ? typedef_field_named(typedef_declaration, item->text, &field_index) : NULL;
      if (field) {
        LLVMValueRef value = LLVMBuildExtractValue(codegen->builder, record, field_index, "field.extract");
        *type = declaration_type(codegen, field);
        remember_pointer_element_from_decl(codegen, field);
        if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
          *type = codegen->i32;
          return field->integer_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
        }
        return value;
      }
    }
    die("unsupported field access");
  }
  if (item->kind == N_CAST) {
    LLVMTypeRef source_type;
    LLVMValueRef value = emit_expression(codegen, item->a, &source_type);
    LLVMTypeRef target_type = cast_target_type(codegen, item);
    if (!target_type) die_at(codegen, item, "unsupported cast target");
    *type = target_type;
    if (target_type == codegen->pointer_type) {
      if (item->pointer_element_width) {
        codegen->last_pointer_element_type = LLVMIntTypeInContext(codegen->context, item->pointer_element_width);
        codegen->last_pointer_element_unsigned = item->pointer_element_unsigned;
      } else if (item->pointer_type_name[0]) {
        if (!strcmp(item->pointer_type_name, "Void")) {
          codegen->last_pointer_element_type = codegen->i8;
          codegen->last_pointer_element_unsigned = 1;
        } else {
          Node *pointee = typedef_named(codegen, item->pointer_type_name);
          codegen->last_pointer_element_type = pointee ? LLVMGetTypeByName2(codegen->context, pointee->text) : NULL;
          codegen->last_pointer_element_unsigned = 0;
        }
      }
    }
    if (source_type == target_type) return value;
    if (source_type == codegen->pointer_type && LLVMGetTypeKind(target_type) == LLVMIntegerTypeKind) return LLVMBuildPtrToInt(codegen->builder, value, target_type, "ptrtoint");
    if (LLVMGetTypeKind(source_type) == LLVMIntegerTypeKind && target_type == codegen->pointer_type) return LLVMBuildIntToPtr(codegen->builder, value, target_type, "inttoptr");
    if (type_is_float(codegen, source_type) || type_is_float(codegen, target_type))
      return coerce_value(codegen, item, value, source_type, target_type, item->integer_unsigned, "unsupported cast conversion");
    if (LLVMGetTypeKind(source_type) == LLVMIntegerTypeKind && LLVMGetTypeKind(target_type) == LLVMIntegerTypeKind) return convert_integer(codegen, value, source_type, target_type, item->integer_unsigned);
    die_at(codegen, item, "unsupported cast conversion");
  }
  if (item->kind == N_CALL) {
    {
      Node *enumeration = NULL;
      Node *variant = NULL;
      if (enum_pattern_info(codegen, item, &enumeration, &variant) && variant->b) {
        unsigned expected = 0;
        for (Node *parameter = variant->b; parameter; parameter = parameter->next) expected++;
        unsigned got = 0;
        for (Node *argument = item->a; argument; argument = argument->next) got++;
        if (got != expected) die_at(codegen, item, "enum constructor argument count mismatch");
        LLVMValueRef payload = LLVMConstNull(codegen->pointer_type);
        if (item->a) {
          LLVMTypeRef argument_type;
          LLVMValueRef argument = emit_expression(codegen, item->a, &argument_type);
          if (argument_type == codegen->pointer_type) payload = argument;
          else if (LLVMGetTypeKind(argument_type) == LLVMIntegerTypeKind) payload = LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, argument, argument_type, codegen->i64, 1), codegen->pointer_type, "enum.payload");
          else die_at(codegen, item, "enum constructor payload must be a pointer or integer value");
          if (item->a->next) die_at(codegen, item, "enum constructors currently support one payload argument");
        }
        return emit_enum_value(codegen, enumeration, variant, payload, type);
      }
    }
    if (!strcmp(item->text, "sizeof")) {
      if (!item->a || item->a->next) die_at(codegen, item, "sizeof expects exactly one type");
      Node *argument = item->a;
      *type = codegen->i64;
      if (argument->integer_width) {
        return LLVMSizeOf(LLVMIntTypeInContext(codegen->context, argument->integer_width));
      }
      if (argument->pointer_type_name[0] || !strcmp(argument->text, "Ptr") || !strcmp(argument->text, "String") ||
          !strcmp(argument->text, "Dynamic") || !strcmp(argument->text, "Any") || !strcmp(argument->text, "Void") ||
          type_is_word_integer(argument->text)) {
        return LLVMSizeOf(type_is_word_integer(argument->text) ? codegen->word_type : codegen->pointer_type);
      }
      if (argument->kind != N_NAME) die_at(codegen, item, "sizeof expects a typedef or sized type");
      Node *typedef_declaration = typedef_named(codegen, argument->text);
      if (!typedef_declaration) die_at(codegen, item, "sizeof requires a declared typedef or sized type");
      return LLVMSizeOf(declaration_type(codegen, typedef_declaration));
    }
    if (!strcmp(item->text, "asm")) {
      if (!item->a || item->a->kind != N_STR) die("asm expects a string literal");
      if (item->a->next && item->a->next->kind != N_STR) die("asm operands require an LLVM constraint string as the second argument");
      *type = codegen->i32;
      return emit_inline_asm(codegen, item);
    }
    if (!strcmp(item->text, "Std.string") && item->a && !item->a->next) {
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
      if (value_type == codegen->pointer_type) { *type = value_type; return value; }
      if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind) die("Std.string requires a C string or integer");
      *type = codegen->pointer_type;
      return format_integer_string(codegen, value, value_type, 10);
    }
    if ((!strcmp(item->text, "StringTools.lpad") || !strcmp(item->text, "StringTools.rpad")) && item->a && item->a->next && item->a->next->next && !item->a->next->next->next) {
      LLVMTypeRef text_type, width_type, pad_type;
      LLVMValueRef text = emit_expression(codegen, item->a, &text_type);
      LLVMValueRef width = emit_expression(codegen, item->a->next, &width_type);
      LLVMValueRef pad = emit_expression(codegen, item->a->next->next, &pad_type);
      if (text_type != codegen->pointer_type || pad_type != codegen->pointer_type || LLVMGetTypeKind(width_type) != LLVMIntegerTypeKind) die("StringTools.lpad/rpad expects (String, Int, String)");
      *type = codegen->pointer_type;
      return pad_string(codegen, text, as_integer(codegen, width, width_type), pad, !strcmp(item->text, "StringTools.lpad"));
    }
    char receiver_name[256];
    if ((!strcmp(item->text, "toString") && item->b) || call_method_name(item->text, "toString", receiver_name, sizeof(receiver_name))) {
      LLVMTypeRef receiver_type;
      Node receiver_node = { .kind = N_NAME };
      if (item->b) receiver_node = *item->b;
      else strcpy(receiver_node.text, receiver_name);
      /* Keep the call as the diagnostic locus; nested emit would otherwise point at a synthetic node. */
      Node *saved_diagnostic_node = active_diagnostic_node;
      LLVMValueRef receiver = emit_expression(codegen, &receiver_node, &receiver_type);
      active_diagnostic_node = saved_diagnostic_node;
      if (LLVMGetTypeKind(receiver_type) == LLVMIntegerTypeKind) {
        unsigned base = 10;
        if (item->a) {
          if (item->a->next || item->a->kind != N_INT) die_at(codegen, item, "toString base must be an integer literal");
          base = (unsigned)item->a->number;
        }
        *type = codegen->pointer_type;
        return format_integer_string(codegen, receiver, receiver_type, base);
      }
      /* FixedArray of bytes (ACPI signatures/OEM IDs): copy into a NUL-terminated scratch string. */
      if (LLVMGetTypeKind(receiver_type) == LLVMArrayTypeKind) {
        if (item->a) die_at(codegen, item, "FixedArray.toString does not take a base argument");
        if (LLVMGetTypeKind(LLVMGetElementType(receiver_type)) != LLVMIntegerTypeKind)
          die_at(codegen, item, "toString requires an integer or byte FixedArray receiver");
        *type = codegen->pointer_type;
        return fixed_array_as_cstring(codegen, receiver, receiver_type);
      }
      if (receiver_type == codegen->pointer_type) {
        if (item->a) die_at(codegen, item, "String.toString does not take a base argument");
        *type = codegen->pointer_type;
        return receiver;
      }
      die_at(codegen, item, "toString requires an integer, String, or byte FixedArray receiver");
    }
    if (!strcmp(item->text, "String.fromCharCodes") && item->a && !item->a->next) {
      if (item->a->kind != N_NAME) die("String.fromCharCodes expects an array name");
      LLVMValueRef array_address;
      LLVMTypeRef array_type, element_type;
      if (resolve_growable_array(codegen, item->a->text, &array_address, &array_type, &element_type)) {
        LLVMValueRef length = LLVMBuildLoad2(codegen->builder, codegen->i32, growable_array_length_address(codegen, array_address, array_type), "string.length");
        /* Keep one spare byte so traced/C-style string walks stop at the live length. */
        LLVMValueRef can_terminate = LLVMBuildICmp(codegen->builder, LLVMIntULT, length, LLVMConstInt(codegen->i32, GROWABLE_ARRAY_CAPACITY, 0), "string.can_terminate");
        LLVMBasicBlockRef terminate_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "string.terminate");
        LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "string.terminate.done");
        LLVMBuildCondBr(codegen->builder, can_terminate, terminate_block, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, terminate_block);
        LLVMValueRef terminator = LLVMConstInt(element_type, 0, 0);
        LLVMBuildStore(codegen->builder, terminator, growable_array_element_address(codegen, array_address, array_type, length));
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, done_block);
        *type = codegen->pointer_type;
        return growable_array_data_address(codegen, array_address, array_type);
      }
      Node *global = global_named(codegen, item->a->text);
      if (global && global->is_dynamic_array) {
        LLVMValueRef global_value = LLVMGetNamedGlobal(codegen->module, global->text);
        if (!global_value) die("String.fromCharCodes expects an Array");
        LLVMTypeRef storage_type = LLVMGlobalGetValueType(global_value);
        LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, 0, 0) };
        *type = codegen->pointer_type;
        return LLVMBuildGEP2(codegen->builder, storage_type, global_value, indices, 2, "string.data");
      }
      die("String.fromCharCodes expects an Array");
    }
    {
      char receiver[256];
      if (call_method_name(item->text, "push", receiver, sizeof(receiver))) {
        if (!item->a || item->a->next) die("push expects exactly one argument");
        LLVMValueRef array_address;
        LLVMTypeRef array_type, element_type;
        if (!resolve_growable_array(codegen, receiver, &array_address, &array_type, &element_type)) die("push requires a growable Array initialized to []");
        LLVMValueRef length_address = growable_array_length_address(codegen, array_address, array_type);
        LLVMValueRef length = LLVMBuildLoad2(codegen->builder, codegen->i32, length_address, "array.length");
        LLVMValueRef has_room = LLVMBuildICmp(codegen->builder, LLVMIntULT, length, LLVMConstInt(codegen->i32, GROWABLE_ARRAY_CAPACITY - 1, 0), "array.has_room");
        LLVMBasicBlockRef push_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.push");
        LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.push.done");
        LLVMBuildCondBr(codegen->builder, has_room, push_block, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, push_block);
        LLVMTypeRef value_type;
        LLVMValueRef value;
        if (item->a->kind == N_RECORD) {
          Node *typedef_declaration = typedef_for_type(codegen, element_type);
          if (!typedef_declaration) die("record push requires an Array of typedef records");
          value = emit_record_value(codegen, item->a, typedef_declaration, &value_type);
          if (value_type != element_type) die("record push element type mismatch");
        } else {
          value = emit_expression(codegen, item->a, &value_type);
          value = coerce_value(codegen, item->a, value, value_type, element_type, 1, "array element type mismatch");
        }
        LLVMBuildStore(codegen->builder, value, growable_array_element_address(codegen, array_address, array_type, length));
        LLVMBuildStore(codegen->builder, LLVMBuildAdd(codegen->builder, length, LLVMConstInt(codegen->i32, 1, 0), "array.next"), length_address);
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, done_block);
        *type = codegen->i32;
        return LLVMConstInt(codegen->i32, 0, 0);
      }
      if (call_method_name(item->text, "pop", receiver, sizeof(receiver))) {
        if (item->a) die("pop expects no arguments");
        LLVMValueRef array_address;
        LLVMTypeRef array_type, element_type;
        if (!resolve_growable_array(codegen, receiver, &array_address, &array_type, &element_type)) die("pop requires a growable Array initialized to []");
        LLVMValueRef length_address = growable_array_length_address(codegen, array_address, array_type);
        LLVMValueRef length = LLVMBuildLoad2(codegen->builder, codegen->i32, length_address, "array.length");
        LLVMValueRef nonempty = LLVMBuildICmp(codegen->builder, LLVMIntUGT, length, LLVMConstInt(codegen->i32, 0, 0), "array.nonempty");
        LLVMBasicBlockRef pop_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.pop");
        LLVMBasicBlockRef empty_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.pop.empty");
        LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.pop.done");
        LLVMBuildCondBr(codegen->builder, nonempty, pop_block, empty_block);
        LLVMPositionBuilderAtEnd(codegen->builder, pop_block);
        LLVMValueRef next_length = LLVMBuildSub(codegen->builder, length, LLVMConstInt(codegen->i32, 1, 0), "array.prev");
        LLVMValueRef popped = LLVMBuildLoad2(codegen->builder, element_type, growable_array_element_address(codegen, array_address, array_type, next_length), "array.popped");
        LLVMBuildStore(codegen->builder, next_length, length_address);
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, empty_block);
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, done_block);
        LLVMValueRef result = LLVMBuildPhi(codegen->builder, element_type, "array.pop.result");
        LLVMValueRef phi_values[] = { popped, growable_array_zero_element(codegen, element_type) };
        LLVMBasicBlockRef phi_blocks[] = { pop_block, empty_block };
        LLVMAddIncoming(result, phi_values, phi_blocks, 2);
        *type = element_type;
        return result;
      }
      if (call_method_name(item->text, "shift", receiver, sizeof(receiver))) {
        if (item->a) die("shift expects no arguments");
        LLVMValueRef array_address;
        LLVMTypeRef array_type, element_type;
        if (!resolve_growable_array(codegen, receiver, &array_address, &array_type, &element_type)) die("shift requires a growable Array initialized to []");
        LLVMValueRef length_address = growable_array_length_address(codegen, array_address, array_type);
        LLVMValueRef length = LLVMBuildLoad2(codegen->builder, codegen->i32, length_address, "array.length");
        LLVMValueRef nonempty = LLVMBuildICmp(codegen->builder, LLVMIntUGT, length, LLVMConstInt(codegen->i32, 0, 0), "array.nonempty");
        LLVMBasicBlockRef shift_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.shift");
        LLVMBasicBlockRef empty_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.shift.empty");
        LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.shift.done");
        LLVMBuildCondBr(codegen->builder, nonempty, shift_block, empty_block);
        LLVMPositionBuilderAtEnd(codegen->builder, shift_block);
        LLVMValueRef first = LLVMBuildLoad2(codegen->builder, element_type, growable_array_element_address(codegen, array_address, array_type, LLVMConstInt(codegen->i32, 0, 0)), "array.shifted");
        LLVMValueRef next_length = LLVMBuildSub(codegen->builder, length, LLVMConstInt(codegen->i32, 1, 0), "array.prev");
        LLVMValueRef index_address = build_entry_alloca(codegen, codegen->i32, "array.shift.i", 0);
        LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i32, 0, 0), index_address);
        LLVMBasicBlockRef loop_cond = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.shift.cond");
        LLVMBasicBlockRef loop_body = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.shift.body");
        LLVMBasicBlockRef loop_done = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.shift.move.done");
        LLVMBuildBr(codegen->builder, loop_cond);
        LLVMPositionBuilderAtEnd(codegen->builder, loop_cond);
        LLVMValueRef index = LLVMBuildLoad2(codegen->builder, codegen->i32, index_address, "array.shift.i");
        LLVMValueRef keep_moving = LLVMBuildICmp(codegen->builder, LLVMIntULT, index, next_length, "array.shift.more");
        LLVMBuildCondBr(codegen->builder, keep_moving, loop_body, loop_done);
        LLVMPositionBuilderAtEnd(codegen->builder, loop_body);
        LLVMValueRef source_index = LLVMBuildAdd(codegen->builder, index, LLVMConstInt(codegen->i32, 1, 0), "array.shift.src");
        LLVMValueRef moved = LLVMBuildLoad2(codegen->builder, element_type, growable_array_element_address(codegen, array_address, array_type, source_index), "array.shift.moved");
        LLVMBuildStore(codegen->builder, moved, growable_array_element_address(codegen, array_address, array_type, index));
        LLVMBuildStore(codegen->builder, source_index, index_address);
        LLVMBuildBr(codegen->builder, loop_cond);
        LLVMPositionBuilderAtEnd(codegen->builder, loop_done);
        LLVMBuildStore(codegen->builder, next_length, length_address);
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, empty_block);
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, done_block);
        LLVMValueRef result = LLVMBuildPhi(codegen->builder, element_type, "array.shift.result");
        LLVMValueRef phi_values[] = { first, growable_array_zero_element(codegen, element_type) };
        LLVMBasicBlockRef phi_blocks[] = { loop_done, empty_block };
        LLVMAddIncoming(result, phi_values, phi_blocks, 2);
        *type = element_type;
        return result;
      }
      if (call_method_name(item->text, "insert", receiver, sizeof(receiver))) {
        if (!item->a || !item->a->next || item->a->next->next) die("insert expects exactly two arguments");
        LLVMValueRef array_address;
        LLVMTypeRef array_type, element_type;
        if (!resolve_growable_array(codegen, receiver, &array_address, &array_type, &element_type)) die("insert requires a growable Array initialized to []");
        LLVMValueRef length_address = growable_array_length_address(codegen, array_address, array_type);
        LLVMValueRef length = LLVMBuildLoad2(codegen->builder, codegen->i32, length_address, "array.length");
        LLVMTypeRef index_type;
        LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->a, &index_type), index_type);
        LLVMValueRef capped = LLVMBuildSelect(codegen->builder,
          LLVMBuildICmp(codegen->builder, LLVMIntULT, index, length, "array.insert.in_range"),
          index, length, "array.insert.pos");
        LLVMValueRef has_room = LLVMBuildICmp(codegen->builder, LLVMIntULT, length, LLVMConstInt(codegen->i32, GROWABLE_ARRAY_CAPACITY - 1, 0), "array.has_room");
        LLVMBasicBlockRef insert_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.insert");
        LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.insert.done");
        LLVMBuildCondBr(codegen->builder, has_room, insert_block, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, insert_block);
        LLVMValueRef cursor_address = build_entry_alloca(codegen, codegen->i32, "array.insert.i", 0);
        LLVMBuildStore(codegen->builder, length, cursor_address);
        LLVMBasicBlockRef loop_cond = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.insert.cond");
        LLVMBasicBlockRef loop_body = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.insert.body");
        LLVMBasicBlockRef loop_done = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "array.insert.move.done");
        LLVMBuildBr(codegen->builder, loop_cond);
        LLVMPositionBuilderAtEnd(codegen->builder, loop_cond);
        LLVMValueRef cursor = LLVMBuildLoad2(codegen->builder, codegen->i32, cursor_address, "array.insert.i");
        LLVMValueRef keep_moving = LLVMBuildICmp(codegen->builder, LLVMIntUGT, cursor, capped, "array.insert.more");
        LLVMBuildCondBr(codegen->builder, keep_moving, loop_body, loop_done);
        LLVMPositionBuilderAtEnd(codegen->builder, loop_body);
        LLVMValueRef source_index = LLVMBuildSub(codegen->builder, cursor, LLVMConstInt(codegen->i32, 1, 0), "array.insert.src");
        LLVMValueRef moved = LLVMBuildLoad2(codegen->builder, element_type, growable_array_element_address(codegen, array_address, array_type, source_index), "array.insert.moved");
        LLVMBuildStore(codegen->builder, moved, growable_array_element_address(codegen, array_address, array_type, cursor));
        LLVMBuildStore(codegen->builder, source_index, cursor_address);
        LLVMBuildBr(codegen->builder, loop_cond);
        LLVMPositionBuilderAtEnd(codegen->builder, loop_done);
        LLVMTypeRef value_type;
        LLVMValueRef value;
        if (item->a->next->kind == N_RECORD) {
          Node *typedef_declaration = typedef_for_type(codegen, element_type);
          if (!typedef_declaration) die("record insert requires an Array of typedef records");
          value = emit_record_value(codegen, item->a->next, typedef_declaration, &value_type);
          if (value_type != element_type) die("record insert element type mismatch");
        } else {
          value = emit_expression(codegen, item->a->next, &value_type);
          value = coerce_value(codegen, item->a->next, value, value_type, element_type, 1, "array element type mismatch");
        }
        LLVMBuildStore(codegen->builder, value, growable_array_element_address(codegen, array_address, array_type, capped));
        LLVMBuildStore(codegen->builder, LLVMBuildAdd(codegen->builder, length, LLVMConstInt(codegen->i32, 1, 0), "array.next"), length_address);
        LLVMBuildBr(codegen->builder, done_block);
        LLVMPositionBuilderAtEnd(codegen->builder, done_block);
        *type = codegen->i32;
        return LLVMConstInt(codegen->i32, 0, 0);
      }
    }
    if (strstr(item->text, ".charCodeAt") && item->a && !item->a->next) { char name[256]; size_t length = strlen(item->text) - strlen(".charCodeAt"); memcpy(name, item->text, length); name[length] = '\0'; Local *string = local(codegen, name, item); if (string->type != codegen->pointer_type) die("charCodeAt requires a string value"); LLVMTypeRef index_type; LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->a, &index_type), index_type); LLVMValueRef pointer = LLVMBuildGEP2(codegen->builder, codegen->i8, LLVMBuildLoad2(codegen->builder, string->type, string->address, name), &index, 1, "character"); *type = codegen->i32; return LLVMBuildZExt(codegen->builder, LLVMBuildLoad2(codegen->builder, codegen->i8, pointer, "byte"), codegen->i32, "charcode"); }
    if (!strcmp(item->text, "trace") && codegen->trace_handler) { emit_trace_handler(codegen, item->a); *type = codegen->i32; return LLVMConstInt(codegen->i32, 0, 0); }
    {
      LLVMValueRef method_call = emit_method_call(codegen, item, type);
      if (method_call) return method_call;
    }
    if (codegen->has_usings) {
      char receiver[256], method_name[256];
      if (split_receiver_method(item->text, receiver, sizeof(receiver), method_name, sizeof(method_name)) && find_local(codegen, receiver)) {
        Function *extension = NULL;
        for (Node *using_node = codegen->usings; using_node && !extension; using_node = using_node->next) {
          if (using_node->kind != N_USING) continue;
          const char *using_class = unqualified_name(using_node->text);
          char mangled[512];
          snprintf(mangled, sizeof(mangled), "%s_%s", using_class, method_name);
          extension = function_value(codegen, mangled);
          if (!extension) {
            Function *candidate = function_value(codegen, method_name);
            if (candidate && candidate->declaration->is_static &&
                (!candidate->declaration->class_name[0] || !strcmp(unqualified_name(candidate->declaration->class_name), using_class))) {
              extension = candidate;
            }
          }
        }
        if (!extension) {
          Function *candidate = function_value(codegen, method_name);
          if (candidate && candidate->declaration->is_static && candidate->declaration->b) extension = candidate;
        }
        if (extension && extension->declaration->b) {
          LLVMValueRef arguments[256];
          unsigned count = 0;
          Node *dummy_receiver = node_new(N_NAME);
          strcpy(dummy_receiver->text, receiver);
          LLVMTypeRef receiver_type;
          arguments[count++] = emit_expression(codegen, dummy_receiver, &receiver_type);
          free(dummy_receiver);
          Node *parameter = extension->declaration->b;
          LLVMTypeRef first_type = declaration_type(codegen, parameter);
          if (first_type && receiver_type != first_type)
            arguments[0] = coerce_value(codegen, item, arguments[0], receiver_type, first_type, parameter->integer_unsigned, "using extension receiver type mismatch");
          parameter = parameter->next;
          for (Node *argument = item->a; argument; argument = argument->next, parameter = parameter ? parameter->next : NULL) {
            if (!parameter || count == 256) die_at(codegen, item, "using extension argument count mismatch");
            LLVMTypeRef argument_type;
            arguments[count] = emit_expression(codegen, argument, &argument_type);
            LLVMTypeRef parameter_type = declaration_type(codegen, parameter);
            if (argument_type != parameter_type)
              arguments[count] = coerce_value(codegen, argument, arguments[count], argument_type, parameter_type, parameter->integer_unsigned, "using extension argument type mismatch");
            count++;
          }
          if (parameter) die_at(codegen, item, "using extension argument count mismatch");
          *type = extension->return_type;
          LLVMValueRef call = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(extension->value), extension->value, arguments, count, extension->return_type == codegen->void_type ? "" : "using.call");
          return *type == codegen->void_type ? LLVMConstInt(codegen->i32, 0, 0) : call;
        }
      }
    }
    Function *function = NULL;
    if (!strchr(item->text, '.')) function = function_value(codegen, item->text);
    else {
      char receiver[256], method_name[256];
      /* Module/class static calls like limine.enabled / Log.Code — not local receivers. */
      if (split_receiver_method(item->text, receiver, sizeof(receiver), method_name, sizeof(method_name)) && !find_local(codegen, receiver)) {
        function = function_value(codegen, method_name);
        if (!function) {
          char mangled[512];
          snprintf(mangled, sizeof(mangled), "%s_%s", receiver, method_name);
          function = function_value(codegen, mangled);
        }
      }
    }
    if (function) {
      LLVMValueRef arguments[256];
      unsigned count = 0;
      Node *parameter = function->declaration->b;
      for (Node *argument = item->a; argument; argument = argument->next, parameter = parameter ? parameter->next : NULL) {
        if (!parameter || count == 256) die("function call argument count mismatch");
        LLVMTypeRef argument_type;
        LLVMTypeRef parameter_type = is_fixed_array_decl(parameter) ? codegen->pointer_type : declaration_type(codegen, parameter);
        if (is_fixed_array_decl(parameter)) {
          arguments[count] = fixed_array_argument_address(codegen, argument);
          argument_type = codegen->pointer_type;
        } else arguments[count] = emit_expression(codegen, argument, &argument_type);
        if (argument_type != parameter_type) {
          arguments[count] = coerce_value(codegen, argument, arguments[count], argument_type, parameter_type, parameter->integer_unsigned, "function call argument type mismatch");
        }
        count++;
      }
      if (parameter) die("function call argument count mismatch");
      *type = function->return_type;
      LLVMValueRef call = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(function->value), function->value, arguments, count, function->return_type == codegen->void_type ? "" : "call");
      return *type == codegen->void_type ? LLVMConstInt(codegen->i32, 0, 0) : call;
    }
    if (strcmp(item->text, "trace")) die_call(codegen, item);
    if (!item->a || item->a->next) die("trace expects exactly one argument");
    ensure_host_trace_io(codegen);
    LLVMTypeRef value_type;
    LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
    if (value_type == codegen->pointer_type) LLVMBuildCall2(codegen->builder, codegen->puts_type, codegen->puts_fn, &value, 1, "");
    else if (value_type == codegen->i1) {
      LLVMValueRef text = LLVMBuildSelect(
          codegen->builder, value,
          LLVMBuildGlobalStringPtr(codegen->builder, "true", "bool.true"),
          LLVMBuildGlobalStringPtr(codegen->builder, "false", "bool.false"),
          "bool.text");
      LLVMBuildCall2(codegen->builder, codegen->puts_type, codegen->puts_fn, &text, 1, "");
    } else if (type_is_float(codegen, value_type)) {
      note_float_use(codegen);
      LLVMValueRef as_double = value_type == codegen->f64 ? value : LLVMBuildFPExt(codegen->builder, value, codegen->f64, "fpext");
      LLVMValueRef arguments[] = { LLVMBuildGlobalStringPtr(codegen->builder, "%g\n", "fformat"), as_double };
      LLVMBuildCall2(codegen->builder, codegen->printf_type, codegen->printf_fn, arguments, 2, "");
    } else { value = as_integer(codegen, value, value_type); LLVMValueRef arguments[] = { LLVMBuildGlobalStringPtr(codegen->builder, "%d\n", "format"), value }; LLVMBuildCall2(codegen->builder, codegen->printf_type, codegen->printf_fn, arguments, 2, ""); }
    *type = codegen->i32;
    return LLVMConstInt(codegen->i32, 0, 0);
  }
  if (item->kind == N_UNARY) {
    if (!strcmp(item->text, "&")) {
      *type = codegen->pointer_type;
      if (item->a->kind == N_INDEX) {
        LLVMTypeRef element_type;
        LLVMValueRef address = object_array_element_address(codegen, item->a, &element_type);
        if (address) return address;
        address = typedef_field_fixed_array_element(codegen, item->a, &element_type);
        if (address) return address;
        address = global_fixed_array_element(codegen, item->a, &element_type);
        if (address) return address;
        Local *array_local;
        address = fixed_array_element(codegen, item->a, &array_local);
        if (address) return address;
      }
      if (item->a->kind == N_NAME) {
        Local *item_local = find_local(codegen, item->a->text);
        if (item_local) {
          if (item_local->string_length && item_local->type == codegen->pointer_type) {
            return LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, item->a->text);
          }
          return item_local->address;
        }
        LLVMTypeRef field_type;
        LLVMValueRef field_address = local_field_address(codegen, item->a->text, &field_type);
        if (field_address) return field_address;
        field_address = global_field_address(codegen, item->a->text, &field_type);
        if (field_address) return field_address;
        Node *global = global_named(codegen, item->a->text);
        if (global && is_growable_array_declaration(global)) {
          LLVMValueRef global_value = LLVMGetNamedGlobal(codegen->module, global->text);
          if (global_value) return growable_array_data_address(codegen, global_value, LLVMGlobalGetValueType(global_value));
        }
        LLVMValueRef address = global ? LLVMGetNamedGlobal(codegen->module, global->text) : NULL;
        if (address) return address;
      }
      /* &(ptr - hhdm) / &(phys + hhdm): turn an address expression into a Ptr. */
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
      if (value_type == codegen->pointer_type) return value;
      if (LLVMGetTypeKind(value_type) == LLVMIntegerTypeKind) {
        codegen->last_pointer_element_type = codegen->i8;
        codegen->last_pointer_element_unsigned = 1;
        return LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, value, value_type, codegen->i64, 1), codegen->pointer_type, "inttoptr");
      }
      die_at(codegen, item, "address-of requires a local, global, FixedArray element, or address expression");
    }
    if (!strcmp(item->text, "*")) {
      if (item->a->kind == N_INDEX) {
        LLVMTypeRef pointer_type;
        LLVMValueRef pointer = emit_expression(codegen, item->a, &pointer_type);
        if (pointer_type != codegen->pointer_type || !codegen->last_pointer_element_type) die_at(codegen, item, "dereference requires an indexed Ptr<T> value");
        LLVMTypeRef element_type = codegen->last_pointer_element_type;
        codegen->last_pointer_element_type = NULL;
        *type = element_type;
        return load_volatile(codegen, element_type, pointer, "deref");
      }
      if (item->a->kind == N_NAME) {
        Local *item_local = find_local(codegen, item->a->text);
        if (item_local && item_local->pointer_element_type) {
          LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, item->a->text);
          LLVMValueRef value = load_volatile(codegen, item_local->pointer_element_type, pointer, "deref");
          *type = item_local->pointer_element_type;
          if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
            *type = codegen->i32;
            return item_local->pointer_element_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
          }
          return value;
        }
      }
      LLVMTypeRef pointer_type;
      LLVMValueRef pointer = emit_expression(codegen, item->a, &pointer_type);
      if ((pointer_type != codegen->pointer_type || !codegen->last_pointer_element_type) &&
          !raw_address_as_byte_pointer(codegen, &pointer, pointer_type)) {
        die_at(codegen, item, "dereference requires a Ptr<T> value");
      }
      LLVMTypeRef element_type = codegen->last_pointer_element_type;
      int element_unsigned = codegen->last_pointer_element_unsigned;
      codegen->last_pointer_element_type = NULL;
      LLVMValueRef value = load_volatile(codegen, element_type, pointer, "deref");
      *type = element_type;
      if (LLVMGetTypeKind(*type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(*type) < 32) {
        *type = codegen->i32;
        return element_unsigned ? LLVMBuildZExt(codegen->builder, value, *type, "zextend") : LLVMBuildSExt(codegen->builder, value, *type, "sextend");
      }
      return value;
    }
    LLVMTypeRef value_type;
    LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
    if (!strcmp(item->text, "-")) {
      if (type_is_float(codegen, value_type)) {
        note_float_use(codegen);
        *type = value_type;
        return LLVMBuildFNeg(codegen->builder, value, "fneg");
      }
      *type = value_type;
      if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind) {
        *type = codegen->word_type;
        value = as_integer(codegen, value, value_type);
        value_type = *type;
      }
      return LLVMBuildNeg(codegen->builder, value, "neg");
    }
    if (!strcmp(item->text, "~")) {
      if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind) die_at(codegen, item, "bitwise complement requires an integer operand");
      *type = value_type;
      return LLVMBuildNot(codegen->builder, value, "bitnot");
    }
    *type = codegen->i1;
    return LLVMBuildNot(codegen->builder, as_boolean(codegen, value, value_type), "not");
  }
  if (item->kind == N_BINARY) {
    LLVMTypeRef left_type, right_type;
    /* Emit left first and stash pointee — right-hand int loads must not wipe it. */
    LLVMValueRef left = emit_expression(codegen, item->a, &left_type);
    LLVMTypeRef left_element = codegen->last_pointer_element_type;
    LLVMValueRef right = emit_expression(codegen, item->b, &right_type);
    LLVMTypeRef right_element = codegen->last_pointer_element_type;

    /* Typed Ptr + integer / integer + Ptr → element GEP. Never string-concat addresses. */
    if (!strcmp(item->text, "+")) {
      int left_is_ptr = left_type == codegen->pointer_type;
      int right_is_ptr = right_type == codegen->pointer_type;
      int left_is_int = LLVMGetTypeKind(left_type) == LLVMIntegerTypeKind;
      int right_is_int = LLVMGetTypeKind(right_type) == LLVMIntegerTypeKind;
      if (left_is_ptr && right_is_int && !expression_is_cstring(codegen, item->a, left)) {
        LLVMTypeRef element = left_element ? left_element : codegen->i8;
        LLVMValueRef offset = convert_integer(codegen, right, right_type, codegen->i64, 1);
        codegen->last_pointer_element_type = element;
        *type = codegen->pointer_type;
        return LLVMBuildGEP2(codegen->builder, element, left, &offset, 1, "ptr.add");
      }
      if (right_is_ptr && left_is_int && !expression_is_cstring(codegen, item->b, right)) {
        LLVMTypeRef element = right_element ? right_element : codegen->i8;
        LLVMValueRef offset = convert_integer(codegen, left, left_type, codegen->i64, 1);
        codegen->last_pointer_element_type = element;
        *type = codegen->pointer_type;
        return LLVMBuildGEP2(codegen->builder, element, right, &offset, 1, "ptr.add");
      }
    }

    if (!strcmp(item->text, "+") && (left_type == codegen->pointer_type || right_type == codegen->pointer_type ||
        LLVMGetTypeKind(left_type) == LLVMArrayTypeKind || LLVMGetTypeKind(right_type) == LLVMArrayTypeKind)) {
      if (LLVMGetTypeKind(left_type) == LLVMArrayTypeKind) {
        if (LLVMGetTypeKind(LLVMGetElementType(left_type)) != LLVMIntegerTypeKind)
          die_at(codegen, item, "string concatenation requires a byte FixedArray");
        left = fixed_array_as_cstring(codegen, left, left_type);
        left_type = codegen->pointer_type;
      }
      if (LLVMGetTypeKind(right_type) == LLVMArrayTypeKind) {
        if (LLVMGetTypeKind(LLVMGetElementType(right_type)) != LLVMIntegerTypeKind)
          die_at(codegen, item, "string concatenation requires a byte FixedArray");
        right = fixed_array_as_cstring(codegen, right, right_type);
        right_type = codegen->pointer_type;
      }
      if (left_type == codegen->pointer_type) {
        if (!expression_is_cstring(codegen, item->a, left)) left = pointer_as_address_string(codegen, left);
      } else if (LLVMGetTypeKind(left_type) == LLVMIntegerTypeKind) {
        left = format_integer_string(codegen, left, left_type, 10);
      } else if (type_is_float(codegen, left_type)) {
        left = format_float_string(codegen, left, left_type);
      } else die_at(codegen, item->a ? item->a : item, "string concatenation requires strings, integers, floats, or Ptr values");
      if (right_type == codegen->pointer_type) {
        if (!expression_is_cstring(codegen, item->b, right)) right = pointer_as_address_string(codegen, right);
      } else if (LLVMGetTypeKind(right_type) == LLVMIntegerTypeKind) {
        right = format_integer_string(codegen, right, right_type, 10);
      } else if (type_is_float(codegen, right_type)) {
        right = format_float_string(codegen, right, right_type);
      } else die_at(codegen, item->b ? item->b : item, "string concatenation requires strings, integers, floats, or Ptr values");
      *type = codegen->pointer_type;
      return concat_strings(codegen, left, right);
    }

    /* Ptr - integer → address as integer (HHDM virt→phys). */
    if (!strcmp(item->text, "-") && left_type == codegen->pointer_type && LLVMGetTypeKind(right_type) == LLVMIntegerTypeKind) {
      LLVMValueRef address = LLVMBuildPtrToInt(codegen->builder, left, codegen->i64, "ptrtoint");
      LLVMValueRef offset = convert_integer(codegen, right, right_type, codegen->i64, 1);
      *type = codegen->i64;
      return LLVMBuildSub(codegen->builder, address, offset, "phys");
    }
    if (!strcmp(item->text, "+") || !strcmp(item->text, "-") || !strcmp(item->text, "*") || !strcmp(item->text, "/") || !strcmp(item->text, "%") || !strcmp(item->text, "&") || !strcmp(item->text, "|") || !strcmp(item->text, "^") || !strcmp(item->text, "<<") || !strcmp(item->text, ">>") || !strcmp(item->text, ">>>")) {
      if (type_is_float(codegen, left_type) || type_is_float(codegen, right_type)) {
        note_float_use(codegen);
        if (!strcmp(item->text, "%") || !strcmp(item->text, "&") || !strcmp(item->text, "|") || !strcmp(item->text, "^") || !strcmp(item->text, "<<") || !strcmp(item->text, ">>") || !strcmp(item->text, ">>>"))
          die_at(codegen, item, "bitwise/shift ops are integer-only");
        *type = (left_type == codegen->f32 && right_type == codegen->f32) ? codegen->f32 : codegen->f64;
        left = cast_to_float(codegen, left, left_type, *type);
        right = cast_to_float(codegen, right, right_type, *type);
        if (!strcmp(item->text, "+")) return LLVMBuildFAdd(codegen->builder, left, right, "fadd");
        if (!strcmp(item->text, "-")) return LLVMBuildFSub(codegen->builder, left, right, "fsub");
        if (!strcmp(item->text, "*")) return LLVMBuildFMul(codegen->builder, left, right, "fmul");
        return LLVMBuildFDiv(codegen->builder, left, right, "fdiv");
      }
      if (LLVMGetTypeKind(left_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(right_type) != LLVMIntegerTypeKind) die_at(codegen, item, "arithmetic requires integer operands");
      /* Widen to the larger operand so 64-bit values (addresses) are not truncated. */
      unsigned width = LLVMGetIntTypeWidth(left_type);
      if (LLVMGetIntTypeWidth(right_type) > width) width = LLVMGetIntTypeWidth(right_type);
      if (width < 32) width = 32;
      *type = LLVMIntTypeInContext(codegen->context, width);
      left = convert_integer(codegen, left, left_type, *type, 1);
      right = convert_integer(codegen, right, right_type, *type, 1);
      if (!strcmp(item->text, "+")) return LLVMBuildAdd(codegen->builder, left, right, "add");
      if (!strcmp(item->text, "-")) return LLVMBuildSub(codegen->builder, left, right, "sub");
      if (!strcmp(item->text, "*")) return LLVMBuildMul(codegen->builder, left, right, "mul");
      if (width == 128 && (!strcmp(item->text, "/") || !strcmp(item->text, "%"))) {
        int is_unsigned = expression_integer_unsigned(codegen, item);
        return soft_i128_div_or_rem(codegen, left, right, *type, !strcmp(item->text, "%"), is_unsigned);
      }
      if (!strcmp(item->text, "/")) return LLVMBuildSDiv(codegen->builder, left, right, "div");
      if (!strcmp(item->text, "%")) return LLVMBuildSRem(codegen->builder, left, right, "rem");
      if (!strcmp(item->text, "&")) return LLVMBuildAnd(codegen->builder, left, right, "and");
      if (!strcmp(item->text, "|")) return LLVMBuildOr(codegen->builder, left, right, "or");
      if (!strcmp(item->text, "^")) return LLVMBuildXor(codegen->builder, left, right, "xor");
      if (!strcmp(item->text, "<<")) return LLVMBuildShl(codegen->builder, left, right, "shl");
      if (!strcmp(item->text, ">>")) return LLVMBuildAShr(codegen->builder, left, right, "ashr");
      return LLVMBuildLShr(codegen->builder, left, right, "lshr");
    }
    if (!strcmp(item->text, "&&") || !strcmp(item->text, "||")) { *type = codegen->i1; return !strcmp(item->text, "&&") ? LLVMBuildAnd(codegen->builder, as_boolean(codegen, left, left_type), as_boolean(codegen, right, right_type), "and") : LLVMBuildOr(codegen->builder, as_boolean(codegen, left, left_type), as_boolean(codegen, right, right_type), "or"); }
    if (!strcmp(item->text, "??")) {
      if (left_type != codegen->pointer_type || right_type != codegen->pointer_type) {
        die_at(codegen, item, "?? currently requires Ptr operands");
      }
      *type = codegen->pointer_type;
      LLVMValueRef is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, left, LLVMConstNull(left_type), "isnull");
      return LLVMBuildSelect(codegen->builder, is_null, right, left, "nullcoal");
    }
    if (!strcmp(item->text, "...")) {
      die_at(codegen, item, "`...` range expressions are only valid as for-in iterators");
    }
    {
      LLVMValueRef array = NULL, string = NULL;
      LLVMTypeRef array_type = NULL;
      if (LLVMGetTypeKind(left_type) == LLVMArrayTypeKind && right_type == codegen->pointer_type) {
        array = left;
        array_type = left_type;
        string = right;
      } else if (left_type == codegen->pointer_type && LLVMGetTypeKind(right_type) == LLVMArrayTypeKind) {
        array = right;
        array_type = right_type;
        string = left;
      }
      if (array && (!strcmp(item->text, "==") || !strcmp(item->text, "!="))) {
        LLVMTypeRef element_type = LLVMGetElementType(array_type);
        if (LLVMGetTypeKind(element_type) != LLVMIntegerTypeKind) {
          die_at(codegen, item, "FixedArray string comparison requires a byte FixedArray");
        }
        unsigned length = (unsigned)LLVMGetArrayLength2(array_type);
        LLVMValueRef matched = LLVMConstInt(codegen->i1, 1, 0);
        for (unsigned index = 0; index < length; index++) {
          LLVMValueRef element = LLVMBuildExtractValue(codegen->builder, array, index, "sig.byte");
          LLVMValueRef byte = convert_integer(codegen, element, element_type, codegen->i8, 1);
          LLVMValueRef offset = LLVMConstInt(codegen->i64, index, 0);
          LLVMValueRef character_address = LLVMBuildGEP2(codegen->builder, codegen->i8, string, &offset, 1, "sig.char");
          LLVMValueRef character = LLVMBuildLoad2(codegen->builder, codegen->i8, character_address, "sig.load");
          matched = LLVMBuildAnd(codegen->builder, matched, LLVMBuildICmp(codegen->builder, LLVMIntEQ, byte, character, "sig.eq"), "sig.match");
        }
        *type = codegen->i1;
        return !strcmp(item->text, "==") ? matched : LLVMBuildNot(codegen->builder, matched, "sig.ne");
      }
    }
    if (left_type == codegen->pointer_type || right_type == codegen->pointer_type) {
      if (strcmp(item->text, "==") && strcmp(item->text, "!=")) die_at(codegen, item, "pointers support only == and != comparisons");
      if (left_type != codegen->pointer_type || right_type != codegen->pointer_type) {
        die_at(codegen, item, "cannot compare a pointer with a non-pointer; declare nullable ABI fields as Ptr<T>");
      }
      *type = codegen->i1;
      return LLVMBuildICmp(codegen->builder, !strcmp(item->text, "==") ? LLVMIntEQ : LLVMIntNE, left, right, "compare");
    }
    if (type_is_float(codegen, left_type) || type_is_float(codegen, right_type)) {
      note_float_use(codegen);
      LLVMTypeRef float_type = (left_type == codegen->f32 && right_type == codegen->f32) ? codegen->f32 : codegen->f64;
      left = cast_to_float(codegen, left, left_type, float_type);
      right = cast_to_float(codegen, right, right_type, float_type);
      *type = codegen->i1;
      LLVMRealPredicate predicate = !strcmp(item->text, "==") ? LLVMRealOEQ : !strcmp(item->text, "!=") ? LLVMRealONE : !strcmp(item->text, "<") ? LLVMRealOLT : !strcmp(item->text, "<=") ? LLVMRealOLE : !strcmp(item->text, ">") ? LLVMRealOGT : LLVMRealOGE;
      return LLVMBuildFCmp(codegen->builder, predicate, left, right, "fcmp");
    }
    if (LLVMGetTypeKind(left_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(right_type) != LLVMIntegerTypeKind) {
      die_at(codegen, item, "comparison requires integer operands or Ptr<T> operands");
    }
    /* Keep full operand width — never smash UIntSize<64>/addresses down to i32 on amd64. */
    unsigned width = LLVMGetIntTypeWidth(left_type);
    if (LLVMGetIntTypeWidth(right_type) > width) width = LLVMGetIntTypeWidth(right_type);
    LLVMTypeRef compare_type = LLVMIntTypeInContext(codegen->context, width);
    int unsigned_compare = expression_integer_unsigned(codegen, item->a) && expression_integer_unsigned(codegen, item->b);
    left = convert_integer(codegen, left, left_type, compare_type, unsigned_compare);
    right = convert_integer(codegen, right, right_type, compare_type, unsigned_compare);
    *type = codegen->i1;
    LLVMIntPredicate predicate;
    if (!strcmp(item->text, "==")) predicate = LLVMIntEQ;
    else if (!strcmp(item->text, "!=")) predicate = LLVMIntNE;
    else if (!strcmp(item->text, "<")) predicate = unsigned_compare ? LLVMIntULT : LLVMIntSLT;
    else if (!strcmp(item->text, "<=")) predicate = unsigned_compare ? LLVMIntULE : LLVMIntSLE;
    else if (!strcmp(item->text, ">")) predicate = unsigned_compare ? LLVMIntUGT : LLVMIntSGT;
    else predicate = unsigned_compare ? LLVMIntUGE : LLVMIntSGE;
    return LLVMBuildICmp(codegen->builder, predicate, left, right, "compare");
  }
  die_at(codegen, item, "invalid expression");
  return NULL;
}

static void emit_trace_handler(Codegen *codegen, Node *argument) {
  if (!codegen->trace_handler->text[0]) die("trace override requires a parameter");
  LLVMTypeRef type; LLVMValueRef value = emit_expression(codegen, argument, &type);
  add_local(codegen, codegen->trace_handler->text, type, value, expression_string_length(codegen, argument), 1);
  emit_statement(codegen, codegen->trace_handler->a);
}

static Node *function_named(Codegen *codegen, const char *name) {
  const char *simple_name = unqualified_name(name);
  for (Node *function = codegen->functions; function; function = function->next) if (!strcmp(function->text, simple_name)) return function;
  die("constant initializer calls an unknown function");
  return NULL;
}

static Node *find_function_named(Codegen *codegen, const char *name) {
  const char *simple_name = unqualified_name(name);
  for (Node *function = codegen->functions; function; function = function->next) if (!strcmp(function->text, simple_name)) return function;
  return NULL;
}

/* Fold Array.create/concat and constant helpers (any arity) into an N_ARRAY literal. */
static Node *expand_constant_array_initializer(Codegen *codegen, Node *item) {
  if (!item) return NULL;
  Node *expanded = expand_constant_array(item);
  if (expanded) return expanded;
  if (item->kind != N_CALL) return NULL;
  Node *function = find_function_named(codegen, item->text);
  if (!function) return NULL;
  Node *body = function->a;
  Node *statement = body ? body->a : NULL;
  if (!statement || statement->kind != N_RETURN || !statement->a || statement->a->kind != N_ARRAY) return NULL;

  ConstantBinding bindings[16];
  unsigned binding_count = 0;
  Node *parameter = function->b;
  Node *argument = item->a;
  while (parameter || argument) {
    if (!parameter || !argument || binding_count == 16) return NULL;
    LLVMTypeRef argument_type;
    LLVMValueRef argument_value = emit_constant_bindings(codegen, argument, NULL, 0, &argument_type);
    if (argument_type != codegen->i64 || !LLVMIsAConstantInt(argument_value)) return NULL;
    bindings[binding_count].name = parameter->text;
    bindings[binding_count].value = LLVMConstIntGetZExtValue(argument_value);
    binding_count++;
    parameter = parameter->next;
    argument = argument->next;
  }

  Node *array = calloc(1, sizeof(*array));
  if (!array) die("out of memory");
  array->kind = N_ARRAY;
  Node **tail = &array->a;
  for (Node *element = statement->a->a; element; element = element->next) {
    LLVMTypeRef element_type;
    LLVMValueRef element_value = emit_constant_bindings(codegen, element, bindings, binding_count, &element_type);
    if (!LLVMIsAConstantInt(element_value) || LLVMGetTypeKind(element_type) != LLVMIntegerTypeKind) {
      free(array);
      return NULL;
    }
    *tail = integer_node(LLVMConstIntGetZExtValue(element_value));
    tail = &(*tail)->next;
  }
  return array;
}

static LLVMValueRef emit_constant_bindings(Codegen *codegen, Node *item, ConstantBinding *bindings, unsigned binding_count, LLVMTypeRef *type) {
  if (item->kind == N_INT) { *type = codegen->i64; return LLVMConstInt(codegen->i64, item->number, 0); }
  if (item->kind == N_BOOL) { *type = codegen->i1; return LLVMConstInt(codegen->i1, item->value, 0); }
  if (item->kind == N_NULL) { *type = codegen->pointer_type; return LLVMConstNull(*type); }
  if (item->kind == N_NAME) {
    for (unsigned index = 0; index < binding_count; index++) {
      if (!strcmp(item->text, bindings[index].name)) {
        *type = codegen->i64;
        return LLVMConstInt(codegen->i64, bindings[index].value, 0);
      }
    }
    Node *global = global_named(codegen, item->text);
    if (global && global->a) return emit_constant_bindings(codegen, global->a, NULL, 0, type);
  }
  if (item->kind == N_ARRAY) {
    LLVMValueRef values[256];
    unsigned count = 0;
    for (Node *value = item->a; value; value = value->next) {
      LLVMTypeRef value_type;
      if (count == 256 || emit_constant_bindings(codegen, value, bindings, binding_count, &value_type) == NULL || value_type != codegen->i64) die("array initializer must contain integer constants");
      values[count++] = emit_constant_bindings(codegen, value, bindings, binding_count, &value_type);
    }
    *type = LLVMArrayType2(codegen->i64, count);
    return LLVMConstArray2(codegen->i64, values, count);
  }
  if (item->kind == N_CALL) {
    Node *function = function_named(codegen, item->text);
    Node *body = function->a;
    Node *statement = body ? body->a : NULL;
    if (!statement || statement->kind != N_RETURN) die("constant helper must contain one return statement");
    ConstantBinding call_bindings[16];
    unsigned call_binding_count = 0;
    Node *parameter = function->b;
    Node *argument = item->a;
    while (parameter || argument) {
      if (!parameter || !argument || call_binding_count == 16) die("constant helper argument count mismatch");
      LLVMTypeRef argument_type;
      LLVMValueRef value = emit_constant_bindings(codegen, argument, bindings, binding_count, &argument_type);
      if (argument_type != codegen->i64 || !LLVMIsAConstantInt(value)) die("constant helper argument must be an integer");
      call_bindings[call_binding_count].name = parameter->text;
      call_bindings[call_binding_count].value = LLVMConstIntGetZExtValue(value);
      call_binding_count++;
      parameter = parameter->next;
      argument = argument->next;
    }
    return emit_constant_bindings(codegen, statement->a, call_bindings, call_binding_count, type);
  }
  die_at(codegen, item, "global initializer must be a constant integer, array, or constant helper call");
  return NULL;
}

static LLVMValueRef emit_constant(Codegen *codegen, Node *item, const char *parameter, uint64_t argument, LLVMTypeRef *type) {
  if (parameter) {
    ConstantBinding binding = { parameter, argument };
    return emit_constant_bindings(codegen, item, &binding, 1, type);
  }
  return emit_constant_bindings(codegen, item, NULL, 0, type);
}

static int assigns_global(Node *item, const char *name) {
  if (!item) return 0;
  if (item->kind == N_ASSIGN) {
    size_t name_length = strlen(name);
    if (!strncmp(item->text, name, name_length) && item->text[name_length] == '.') return 1;
    if (item->b && item->b->kind == N_INDEX && item->b->a && item->b->a->kind == N_NAME && !strcmp(item->b->a->text, name)) return 1;
    if (item->b && item->b->kind == N_FIELD && item->b->a && item->b->a->kind == N_INDEX && item->b->a->a && item->b->a->a->kind == N_NAME && !strcmp(item->b->a->a->text, name)) return 1;
  }
  return assigns_global(item->a, name) || assigns_global(item->b, name) || assigns_global(item->c, name) || assigns_global(item->d, name) || assigns_global(item->next, name);
}

static void emit_global(Codegen *codegen, Node *item) {
  LLVMTypeRef type;
  LLVMValueRef initializer;
  if (item->is_dynamic_array && item->a) {
    Node *array_literal = expand_constant_array(item->a);
    if (array_literal) item->a = array_literal;
  }
  if (is_fixed_array_decl(item) && !item->is_flexible_array) {
    /* C-style [N x T] storage: constant aggregate, not the small emit_constant i64 path. */
    LLVMTypeRef element_type = fixed_array_element_type(codegen, item);
    type = fixed_array_type(codegen, item);
    if (!element_type || !type) die_at(codegen, item, "FixedArray element type is unsupported");
    unsigned capacity = item->fixed_array_length;
    LLVMValueRef *values = calloc(capacity ? capacity : 1, sizeof(*values));
    if (!values) die("out of memory");
    unsigned count = 0;
    if (item->a) {
      Node *array_literal = expand_constant_array_initializer(codegen, item->a);
      if (array_literal) item->a = array_literal;
      if (item->a->kind != N_ARRAY) die_at(codegen, item->a, "FixedArray requires an array literal or constant helper initializer");
      for (Node *value_node = item->a->a; value_node; value_node = value_node->next) {
        if (count == capacity) die_at(codegen, value_node, "FixedArray initializer is too long");
        LLVMTypeRef value_type;
        LLVMValueRef value = emit_constant(codegen, value_node, NULL, 0, &value_type);
        if (!LLVMIsAConstantInt(value) || LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind)
          die_at(codegen, value_node, "FixedArray initializer must contain integer constants");
        values[count++] = LLVMConstInt(element_type, LLVMConstIntGetZExtValue(value), item->fixed_element_unsigned);
      }
    }
    while (count < capacity) values[count++] = LLVMConstNull(element_type);
    initializer = LLVMConstArray2(element_type, values, capacity);
    free(values);
  } else if (item->is_dynamic_array && item->a && item->a->kind == N_ARRAY) {
    LLVMTypeRef element_type = dynamic_element_type(codegen, item);
    if (!element_type) die("dynamic array element type is unsupported");
    if (is_empty_array_literal(item->a)) {
      type = growable_array_type(codegen, element_type);
      initializer = LLVMConstNull(type);
    } else {
      LLVMValueRef values[CONSTANT_ARRAY_CAPACITY];
      unsigned count = 0;
      for (Node *value_node = item->a->a; value_node; value_node = value_node->next) {
        LLVMTypeRef value_type;
        LLVMValueRef value = emit_constant(codegen, value_node, NULL, 0, &value_type);
        if (count == CONSTANT_ARRAY_CAPACITY || !LLVMIsAConstantInt(value) || LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind) die("dynamic array initializer must contain integer constants");
        values[count++] = LLVMConstInt(element_type, LLVMConstIntGetZExtValue(value), item->dynamic_element_unsigned);
      }
      type = LLVMArrayType2(element_type, count);
      initializer = LLVMConstArray2(element_type, values, count);
    }
  } else if (item->a && item->a->kind == N_RECORD) {
    Node *typedef_declaration = typedef_named(codegen, item->type_name);
    if (!typedef_declaration) die_at(codegen, item, "record initializer requires a typedef type");
    initializer = emit_record_constant(codegen, item->a, typedef_declaration, NULL, 0, &type);
  } else if (item->a) initializer = emit_constant(codegen, item->a, NULL, 0, &type);
  else {
    type = declaration_type(codegen, item);
    if (!type) die("unsupported global type");
    initializer = LLVMConstNull(type);
  }
  LLVMTypeRef declared_type = declaration_type(codegen, item);
  if (declared_type && declared_type != type && LLVMGetTypeKind(declared_type) == LLVMIntegerTypeKind && LLVMGetTypeKind(type) == LLVMIntegerTypeKind) {
    if (!LLVMIsAConstantInt(initializer)) die_at(codegen, item->a, "typed global initializer must be an integer constant");
    initializer = LLVMConstInt(declared_type, LLVMConstIntGetZExtValue(initializer), item->integer_unsigned);
    type = declared_type;
  }
  LLVMValueRef global = LLVMAddGlobal(codegen->module, type, item->text);
  LLVMSetInitializer(global, initializer);
  if (item->alignment) LLVMSetAlignment(global, item->alignment);
  LLVMSetGlobalConstant(global, item->a && (!item->is_mutable || item->is_final) && !is_growable_array_declaration(item) && !assigns_global(codegen->functions, item->text));
  apply_global_section(codegen, item, global);
  if (!global_has_section(item, (ObjectFormat)codegen->object_format) && item->is_static) LLVMSetLinkage(global, LLVMPrivateLinkage);
}

static void declare_imported_globals(Codegen *codegen) {
  for (Node *item = codegen->globals; item; item = item->next) {
    if (!(item->is_imported || item->is_extern)) continue;
    /* Initializers on imported bindings are for constant folding in this TU; storage lives elsewhere. */
    if (item->a && (!item->is_mutable || item->is_dynamic_array)) continue;
    if (LLVMGetNamedGlobal(codegen->module, item->text)) continue;
    LLVMTypeRef type = declaration_type(codegen, item);
    if (!type) die_at(codegen, item, "unsupported imported global type");
    LLVMAddGlobal(codegen->module, type, item->text);
  }
}

static void emit_class_initializers(Codegen *codegen) {
  for (Node *global = codegen->globals; global; global = global->next) {
    if (global->is_imported) continue;
    Node *typedef_declaration = typedef_named(codegen, global->type_name);
    if (!typedef_declaration) continue;
    int has_initializer = 0;
    size_t global_name_length = strlen(global->text);
    for (Node *initializer = codegen->class_initializers; initializer; initializer = initializer->next) {
      if (!strncmp(initializer->text, global->text, global_name_length) && initializer->text[global_name_length] == '.') {
        has_initializer = 1;
        break;
      }
    }
    if (!has_initializer) continue;
    LLVMTypeRef struct_type = declaration_type(codegen, global);
    LLVMValueRef values[256];
    unsigned count = 0;
    for (Node *field = typedef_declaration->a; field; field = field->next) {
      if (count == 256) die("typedef has too many fields");
      LLVMTypeRef field_type = declaration_type(codegen, field);
      values[count] = LLVMConstNull(field_type);
      for (Node *initializer = codegen->class_initializers; initializer; initializer = initializer->next) {
        if (strncmp(initializer->text, global->text, global_name_length) || initializer->text[global_name_length] != '.' || !same_field_name(initializer->text + global_name_length + 1, field->text)) continue;
        LLVMTypeRef value_type;
        LLVMValueRef value = emit_constant(codegen, initializer->a, NULL, 0, &value_type);
        if (value_type != field_type) die("constant field initializer type mismatch");
        values[count] = value;
      }
      count++;
    }
    LLVMSetInitializer(LLVMGetNamedGlobal(codegen->module, global->text), LLVMConstNamedStruct(struct_type, values, count));
  }
}

static Local *emit_static_local(Codegen *codegen, Node *item) {
  LLVMTypeRef type;
  LLVMValueRef initializer = emit_constant(codegen, item->a, NULL, 0, &type);
  LLVMTypeRef declared_type = declared_integer_type(codegen, item);
  if (declared_type) {
    if (!LLVMIsAConstantInt(initializer)) die("sized static variable requires an integer constant");
    initializer = LLVMConstInt(declared_type, LLVMConstIntGetZExtValue(initializer), 0);
    type = declared_type;
  }
  LLVMValueRef global = LLVMAddGlobal(codegen->module, type, item->text);
  LLVMSetInitializer(global, initializer);
  if (item->alignment) LLVMSetAlignment(global, item->alignment);
  LLVMSetLinkage(global, LLVMPrivateLinkage);
  Local *item_local = calloc(1, sizeof(*item_local));
  if (!item_local) die("out of memory");
  strcpy(item_local->name, item->text);
  item_local->type = type;
  item_local->integer_unsigned = item->integer_unsigned;
  item_local->address = global;
  item_local->next = codegen->locals;
  codegen->locals = item_local;
  return item_local;
}

static void emit_local_record(Codegen *codegen, Node *item, Node *typedef_declaration) {
  LLVMTypeRef struct_type = declaration_type(codegen, item);
  Local *record_local = add_local_aligned(codegen, item->text, struct_type, LLVMConstNull(struct_type), NULL, 1, item->alignment);
  for (Node *value_field = item->a->a; value_field; value_field = value_field->next) {
    unsigned index;
    Node *field = typedef_field_named(typedef_declaration, value_field->text, &index);
    if (!field) die("record initializer contains an unknown field");
    LLVMTypeRef value_type;
    LLVMValueRef value = emit_expression(codegen, value_field->a, &value_type);
    LLVMTypeRef field_type = declaration_type(codegen, field);
    if (value_type != field_type) {
      if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(field_type) != LLVMIntegerTypeKind) die("record field initializer type mismatch");
      value = convert_integer(codegen, value, value_type, field_type, field->integer_unsigned);
    }
    LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, index, 0) };
    LLVMValueRef address = LLVMBuildGEP2(codegen->builder, struct_type, record_local->address, indices, 2, "field");
    LLVMBuildStore(codegen->builder, value, address);
  }
}

static LLVMValueRef emit_switch_expression(Codegen *codegen, Node *item, LLVMTypeRef *type);
static void emit_switch(Codegen *codegen, Node *item, int as_expression, LLVMTypeRef *type, LLVMValueRef *value_out);

static void emit_switch(Codegen *codegen, Node *item, int as_expression, LLVMTypeRef *type, LLVMValueRef *value_out) {
  LLVMTypeRef selector_type;
  LLVMValueRef selector = emit_expression(codegen, item->a, &selector_type);
  LLVMValueRef switch_selector = selector;
  if (LLVMGetTypeKind(selector_type) == LLVMStructTypeKind) {
    switch_selector = LLVMBuildExtractValue(codegen->builder, selector, 0, "enum.tag");
    selector_type = codegen->i32;
  }
  if (LLVMGetTypeKind(selector_type) != LLVMIntegerTypeKind) die("switch selector must be an integer or enum");
  unsigned case_count = 0, default_index = 0, has_default = 0, index = 0;
  for (Node *switch_case = item->b; switch_case; switch_case = switch_case->next, case_count++) {
    if (!switch_case->a) {
      default_index = case_count;
      has_default = 1;
    }
  }
  if (!case_count) die("switch requires at least one case");
  LLVMBasicBlockRef case_blocks[case_count];
  for (Node *switch_case = item->b; switch_case; switch_case = switch_case->next, index++) {
    case_blocks[index] = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, switch_case->a ? "switch.case" : "switch.default");
  }
  LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "switch.end");
  LLVMValueRef switch_value = LLVMBuildSwitch(codegen->builder, switch_selector, has_default ? case_blocks[default_index] : end_block, case_count);
  index = 0;
  for (Node *switch_case = item->b; switch_case; switch_case = switch_case->next, index++) {
    if (!switch_case->a) continue;
    Node *pattern_enum = NULL;
    Node *pattern_variant = NULL;
    LLVMValueRef case_value;
    if (enum_pattern_info(codegen, switch_case->a, &pattern_enum, &pattern_variant)) {
      case_value = LLVMConstInt(codegen->i32, pattern_variant->number, 0);
    } else {
      LLVMTypeRef case_type;
      case_value = emit_expression(codegen, switch_case->a, &case_type);
      if (LLVMGetTypeKind(case_type) == LLVMStructTypeKind) case_value = LLVMBuildExtractValue(codegen->builder, case_value, 0, "enum.case.tag");
      else if (case_type != selector_type || !LLVMIsAConstantInt(case_value)) die("switch cases must be constant integers or enum constructors matching the selector");
      if (!LLVMIsAConstantInt(case_value)) die("switch cases must be constant integers or enum constructors matching the selector");
    }
    LLVMAddCase(switch_value, case_value, case_blocks[index]);
  }
  LLVMValueRef result_slot = NULL;
  LLVMTypeRef result_type = NULL;
  LLVMBasicBlockRef saved_break_target = codegen->break_target;
  codegen->break_target = end_block;
  index = 0;
  for (Node *switch_case = item->b; switch_case; switch_case = switch_case->next, index++) {
    LLVMPositionBuilderAtEnd(codegen->builder, case_blocks[index]);
    Node *body = switch_case->b && switch_case->b->kind == N_BLOCK ? switch_case->b->a : switch_case->b;
    if (as_expression) {
      Node *stmt = body;
      if (!stmt) die_at(codegen, item, "switch expression case requires a value");
      while (stmt->next) {
        emit_statement(codegen, stmt);
        stmt = stmt->next;
      }
      if (stmt->kind != N_EXPR || !stmt->a) die_at(codegen, item, "switch expression case must end with a value");
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_expression(codegen, stmt->a, &value_type);
      if (!result_slot) {
        result_type = value_type;
        result_slot = build_entry_alloca(codegen, result_type, "switch.result", 0);
        LLVMPositionBuilderAtEnd(codegen->builder, case_blocks[index]);
      } else if (value_type != result_type) {
        die_at(codegen, item, "switch expression cases must produce the same type");
      }
      LLVMBuildStore(codegen->builder, value, result_slot);
    } else {
      for (Node *stmt = body; stmt && !LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)); stmt = stmt->next) {
        emit_statement(codegen, stmt);
      }
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, end_block);
  }
  codegen->break_target = saved_break_target;
  LLVMPositionBuilderAtEnd(codegen->builder, end_block);
  if (as_expression) {
    if (!result_slot) die_at(codegen, item, "switch expression requires a value");
    *type = result_type;
    *value_out = LLVMBuildLoad2(codegen->builder, result_type, result_slot, "switch.value");
  }
}

static LLVMValueRef emit_switch_expression(Codegen *codegen, Node *item, LLVMTypeRef *type) {
  LLVMValueRef value = NULL;
  emit_switch(codegen, item, 1, type, &value);
  return value;
}

static LLVMValueRef apply_integer_compound_assignment(Codegen *codegen, Node *item, LLVMValueRef address, LLVMTypeRef element_type, LLVMValueRef value, int is_volatile) {
  if (!item->assignment_operator[0] || !strcmp(item->assignment_operator, "=")) return value;
  LLVMValueRef current = is_volatile
      ? load_volatile(codegen, element_type, address, "element.load")
      : LLVMBuildLoad2(codegen->builder, element_type, address, "element.load");
  if (!strcmp(item->assignment_operator, "+=")) return LLVMBuildAdd(codegen->builder, current, value, "add");
  if (!strcmp(item->assignment_operator, "-=")) return LLVMBuildSub(codegen->builder, current, value, "sub");
  if (!strcmp(item->assignment_operator, "*=")) return LLVMBuildMul(codegen->builder, current, value, "mul");
  if (LLVMGetTypeKind(element_type) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(element_type) == 128 &&
      (!strcmp(item->assignment_operator, "/=") || !strcmp(item->assignment_operator, "%="))) {
    return soft_i128_div_or_rem(codegen, current, value, element_type, !strcmp(item->assignment_operator, "%="), 0);
  }
  if (!strcmp(item->assignment_operator, "/=")) return LLVMBuildSDiv(codegen->builder, current, value, "div");
  if (!strcmp(item->assignment_operator, "%=")) return LLVMBuildSRem(codegen->builder, current, value, "rem");
  if (!strcmp(item->assignment_operator, "&=")) return LLVMBuildAnd(codegen->builder, current, value, "and");
  if (!strcmp(item->assignment_operator, "|=")) return LLVMBuildOr(codegen->builder, current, value, "or");
  if (!strcmp(item->assignment_operator, "^=")) return LLVMBuildXor(codegen->builder, current, value, "xor");
  if (!strcmp(item->assignment_operator, "<<=")) return LLVMBuildShl(codegen->builder, current, value, "shl");
  if (!strcmp(item->assignment_operator, ">>=")) return LLVMBuildAShr(codegen->builder, current, value, "ashr");
  if (!strcmp(item->assignment_operator, ">>>=")) return LLVMBuildLShr(codegen->builder, current, value, "lshr");
  die_at(codegen, item, "unsupported compound index assignment");
  return value;
}

static void emit_statement(Codegen *codegen, Node *item) {
  active_diagnostic_node = item;
  debug_set_location(codegen, item);
  if (item->kind == N_BLOCK) { for (Node *child = item->a; child && !LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)); child = child->next) emit_statement(codegen, child); return; }
  if (item->kind == N_TRY || item->kind == N_THROW) {
    LLVMTypeRef ignored;
    emit_expression(codegen, item, &ignored);
    return;
  }
  if (item->kind == N_VAR) {
    if (item->is_flexible_array) die_at(codegen, item, "flexible FixedArray is only supported as a trailing typedef field");
    if (item->fixed_array_length) {
      if (item->is_static) die("static FixedArray variables are not supported");
      if (item->a) {
        Node *array_literal = expand_constant_array_initializer(codegen, item->a);
        if (array_literal) item->a = array_literal;
      }
      if (item->a && item->a->kind != N_ARRAY) die("FixedArray requires an array literal or constant helper initializer");
      LLVMTypeRef type = fixed_array_type(codegen, item);
      LLVMTypeRef element_type = fixed_array_element_type(codegen, item);
      if (!type || !element_type) die("FixedArray element type is unsupported");
      Local *array_local = add_local_aligned(codegen, item->text, type, LLVMConstNull(type), NULL, 1, item->alignment);
      array_local->fixed_element_type = element_type;
      array_local->fixed_array_length = item->fixed_array_length;
      array_local->fixed_element_unsigned = item->fixed_element_unsigned;
      unsigned index = 0;
      for (Node *value_node = item->a ? item->a->a : NULL; value_node; value_node = value_node->next) {
        if (index == item->fixed_array_length) die("FixedArray initializer is too long");
        LLVMTypeRef value_type;
        LLVMValueRef value = emit_expression(codegen, value_node, &value_type);
        if (LLVMGetTypeKind(element_type) == LLVMIntegerTypeKind) value = convert_integer(codegen, value, value_type, element_type, item->fixed_element_unsigned);
        else if (value_type != element_type) die_at(codegen, value_node, "FixedArray element initializer type mismatch");
        LLVMValueRef indices[] = { LLVMConstInt(codegen->i32, 0, 0), LLVMConstInt(codegen->i32, index++, 0) };
        LLVMBuildStore(codegen->builder, value, LLVMBuildGEP2(codegen->builder, type, array_local->address, indices, 2, "element"));
      }
      return;
    }
    if (item->is_static) { emit_static_local(codegen, item); return; }
    if (item->is_dynamic_array && is_empty_array_literal(item->a)) {
      LLVMTypeRef element_type = dynamic_element_type(codegen, item);
      if (!element_type) die("dynamic array element type is unsupported");
      LLVMTypeRef type = growable_array_type(codegen, element_type);
      Local *array_local = add_local_aligned(codegen, item->text, type, LLVMConstNull(type), NULL, 1, item->alignment);
      array_local->is_growable_array = 1;
      array_local->growable_capacity = GROWABLE_ARRAY_CAPACITY;
      array_local->dynamic_element_type = element_type;
      array_local->dynamic_element_unsigned = item->dynamic_element_unsigned;
      return;
    }
    Node *typedef_declaration = typedef_named(codegen, item->type_name);
    if (typedef_declaration && item->a && item->a->kind == N_RECORD) {
      emit_local_record(codegen, item, typedef_declaration);
      return;
    }
    if (!item->a) {
      LLVMTypeRef type = declaration_type(codegen, item);
      if (!type) die("unsupported local variable type");
      Local *item_local = add_local_aligned(codegen, item->text, type, LLVMConstNull(type), NULL, item->integer_unsigned, item->alignment);
      LLVMTypeRef element_type = pointer_element_type(codegen, item);
      if (element_type) {
        item_local->pointer_element_type = element_type;
        item_local->pointer_element_unsigned = item->pointer_element_unsigned;
      }
      return;
    }
    LLVMTypeRef type;
    LLVMValueRef value = emit_expression(codegen, item->a, &type);
    LLVMTypeRef declared_type = declared_integer_type(codegen, item);
    if (declared_type && type == codegen->pointer_type) {
      value = LLVMBuildPtrToInt(codegen->builder, value, declared_type, "ptrtoint");
      type = declared_type;
      codegen->last_pointer_element_type = NULL;
      add_local_aligned(codegen, item->text, type, value, NULL, item->integer_unsigned, item->alignment);
      return;
    }
    if (type == codegen->pointer_type && codegen->last_pointer_element_type) {
      Local *pointer_local = add_local_aligned(codegen, item->text, type, value, NULL, 1, item->alignment);
      pointer_local->pointer_element_type = codegen->last_pointer_element_type;
      pointer_local->pointer_element_unsigned = codegen->last_pointer_element_unsigned;
      stamp_class_name_from_declaration(codegen, pointer_local, item, item->a);
      codegen->last_pointer_element_type = NULL;
      return;
    }
    LLVMTypeRef element_type = pointer_element_type(codegen, item);
    if (element_type) {
      if (type != codegen->pointer_type) {
        value = LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, value, type, codegen->i64, 1), codegen->pointer_type, "address");
      }
      type = codegen->pointer_type;
      Local *pointer_local = add_local_aligned(codegen, item->text, type, value, NULL, 1, item->alignment);
      pointer_local->pointer_element_type = element_type;
      pointer_local->pointer_element_unsigned = item->pointer_element_unsigned;
      stamp_class_name_from_declaration(codegen, pointer_local, item, item->a);
      return;
    }
    Node *origin = item->a ? item->a : item;
    if (declared_type) {
      value = coerce_value(codegen, origin, value, type, declared_type, item->integer_unsigned, "local initializer type mismatch");
      type = declared_type;
    } else if (item->type_name[0]) {
      /* Empty type_name means inferred — do not treat as Dynamic/pointer. */
      LLVMTypeRef wanted = declaration_type(codegen, item);
      if (wanted && wanted != type && (type_is_float(codegen, wanted) || type_is_float(codegen, type) ||
          (LLVMGetTypeKind(wanted) == LLVMIntegerTypeKind && LLVMGetTypeKind(type) == LLVMIntegerTypeKind))) {
        value = coerce_value(codegen, origin, value, type, wanted, item->integer_unsigned, "local initializer type mismatch");
        type = wanted;
      }
    }
    Local *created = add_local_aligned(codegen, item->text, type, value, expression_string_length(codegen, item->a), item->integer_unsigned, item->alignment);
    if (item->a && item->a->kind == N_NEW) strcpy(created->class_name, item->a->text);
    else if (item->type_name[0] && class_table_find(&codegen->classes, item->type_name)) strcpy(created->class_name, item->type_name);
    return;
  }
  if (item->kind == N_ASSIGN) {
    if (!strcmp(item->text, "haxe.Log.trace") && item->a->kind == N_FUNCTION) { codegen->trace_handler = item->a; return; }
    if (is_empty_array_literal(item->a)) {
      LLVMValueRef array_address;
      LLVMTypeRef array_type, element_type;
      if (resolve_growable_array(codegen, item->text, &array_address, &array_type, &element_type)) {
        LLVMBuildStore(codegen->builder, LLVMConstInt(codegen->i32, 0, 0), growable_array_length_address(codegen, array_address, array_type));
        return;
      }
    }
    Local *assigned_local = find_local(codegen, item->text);
    if (assigned_local && !item->b && item->a && item->a->kind == N_RECORD) {
      Node *typedef_declaration = typedef_for_type(codegen, assigned_local->type);
      if (!typedef_declaration) die_at(codegen, item, "record assignment requires a typedef local or parameter type");
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_record_value(codegen, item->a, typedef_declaration, &value_type);
      if (value_type != assigned_local->type) die_at(codegen, item, "record assignment type mismatch");
      LLVMBuildStore(codegen->builder, value, assigned_local->address);
      return;
    }
    if (assigned_local && !item->b && item->a && (assigned_local->string_length || is_string_expression(item->a) || expression_string_length(codegen, item->a))) {
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
      if (value_type != assigned_local->type && !(assigned_local->type == codegen->pointer_type && value_type == codegen->pointer_type)) {
        if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(assigned_local->type) != LLVMIntegerTypeKind) die("assignment type mismatch");
        value = convert_integer(codegen, value, value_type, assigned_local->type, assigned_local->integer_unsigned);
      }
      LLVMBuildStore(codegen->builder, value, assigned_local->address);
      assigned_local->string_length = expression_string_length(codegen, item->a);
      if (!assigned_local->string_length && item->a->kind == N_STR) assigned_local->string_length = LLVMConstInt(codegen->word_type, strlen(item->a->text), 0);
      return;
    }
    if ((item->b && item->b->kind == N_FIELD) || strchr(item->text, '.')) {
      LLVMTypeRef field_type;
      LLVMValueRef address = item->b ? record_field_address(codegen, item->b, &field_type) : global_field_address(codegen, item->text, &field_type);
      if (!address && !item->b) address = local_field_address(codegen, item->text, &field_type);
      if (!address) die("unsupported member assignment");
      LLVMTypeRef value_type;
      LLVMValueRef value;
      if (item->a->kind == N_RECORD) {
        Node *typedef_declaration = typedef_for_type(codegen, field_type);
        if (!typedef_declaration) die("record assignment requires a typedef field type");
        value = emit_record_value(codegen, item->a, typedef_declaration, &value_type);
        if (value_type != field_type) die("record field assignment type mismatch");
        LLVMBuildStore(codegen->builder, value, address);
        return;
      } else value = emit_expression(codegen, item->a, &value_type);
      if (value_type == codegen->pointer_type && field_type == codegen->pointer_type) {
        if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die("unsupported compound pointer member assignment");
      } else if (value_type != field_type) {
        if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(field_type) != LLVMIntegerTypeKind) die_at(codegen, item, "member assignment type mismatch");
        value = convert_integer(codegen, value, value_type, field_type, 1);
      }
      if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) {
        LLVMValueRef current = LLVMBuildLoad2(codegen->builder, field_type, address, "field.load");
        if (!strcmp(item->assignment_operator, "+=")) value = LLVMBuildAdd(codegen->builder, current, value, "add");
        else if (!strcmp(item->assignment_operator, "-=")) value = LLVMBuildSub(codegen->builder, current, value, "sub");
        else if (!strcmp(item->assignment_operator, "*=")) value = LLVMBuildMul(codegen->builder, current, value, "mul");
        else if (!strcmp(item->assignment_operator, "/=")) value = LLVMBuildSDiv(codegen->builder, current, value, "div");
        else if (!strcmp(item->assignment_operator, "%=")) value = LLVMBuildSRem(codegen->builder, current, value, "rem");
        else if (!strcmp(item->assignment_operator, "&=")) value = LLVMBuildAnd(codegen->builder, current, value, "and");
        else if (!strcmp(item->assignment_operator, "|=")) value = LLVMBuildOr(codegen->builder, current, value, "or");
        else if (!strcmp(item->assignment_operator, "^=")) value = LLVMBuildXor(codegen->builder, current, value, "xor");
        else if (!strcmp(item->assignment_operator, "<<=")) value = LLVMBuildShl(codegen->builder, current, value, "shl");
        else if (!strcmp(item->assignment_operator, ">>=")) value = LLVMBuildAShr(codegen->builder, current, value, "ashr");
        else if (!strcmp(item->assignment_operator, ">>>=")) value = LLVMBuildLShr(codegen->builder, current, value, "lshr");
        else die("unsupported compound member assignment");
      }
      LLVMBuildStore(codegen->builder, value, address);
      return;
    }
    if (item->b && item->b->kind == N_INDEX) {
      LLVMTypeRef object_element_type;
      LLVMValueRef object_address = object_array_element_address(codegen, item->b, &object_element_type);
      if (object_address) {
        LLVMTypeRef value_type;
        LLVMValueRef value;
        if (item->a->kind == N_RECORD) {
          Node *typedef_declaration = typedef_for_type(codegen, object_element_type);
          if (!typedef_declaration) die("record assignment requires a typedef FixedArray element");
          value = emit_record_value(codegen, item->a, typedef_declaration, &value_type);
          if (value_type != object_element_type) die("FixedArray element assignment type mismatch");
          if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die_at(codegen, item, "unsupported compound record index assignment");
        } else {
          value = emit_expression(codegen, item->a, &value_type);
          value = convert_integer(codegen, value, value_type, object_element_type, 1);
          value = apply_integer_compound_assignment(codegen, item, object_address, object_element_type, value, 0);
        }
        LLVMBuildStore(codegen->builder, value, object_address);
        return;
      }
      LLVMTypeRef field_element_type;
      LLVMValueRef field_address = typedef_field_fixed_array_element(codegen, item->b, &field_element_type);
      if (field_address) {
        LLVMTypeRef value_type;
        LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
        value = convert_integer(codegen, value, value_type, field_element_type, 1);
        value = apply_integer_compound_assignment(codegen, item, field_address, field_element_type, value, 0);
        LLVMBuildStore(codegen->builder, value, field_address);
        return;
      }
      LLVMTypeRef global_element_type;
      LLVMValueRef global_address = global_fixed_array_element(codegen, item->b, &global_element_type);
      if (global_address) {
        LLVMTypeRef value_type;
        LLVMValueRef value;
        if (item->a->kind == N_RECORD) {
          Node *typedef_declaration = typedef_for_type(codegen, global_element_type);
          if (!typedef_declaration) die("record assignment requires a typedef FixedArray element");
          value = emit_record_constant(codegen, item->a, typedef_declaration, NULL, 0, &value_type);
          if (value_type != global_element_type) die("FixedArray element assignment type mismatch");
          if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die_at(codegen, item, "unsupported compound record index assignment");
        } else {
          value = emit_expression(codegen, item->a, &value_type);
          if (value_type != global_element_type) {
            if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(global_element_type) != LLVMIntegerTypeKind) die("FixedArray element assignment type mismatch");
            value = convert_integer(codegen, value, value_type, global_element_type, 1);
          }
          value = apply_integer_compound_assignment(codegen, item, global_address, global_element_type, value, 0);
        }
        LLVMBuildStore(codegen->builder, value, global_address);
        return;
      }
      if (item->b->a && item->b->a->kind == N_NAME) {
        Local *pointer_local = find_local(codegen, item->b->a->text);
        if (pointer_local && pointer_local->pointer_element_type) {
          LLVMTypeRef index_type;
          LLVMValueRef index = as_integer(codegen, emit_expression(codegen, item->b->b, &index_type), index_type);
          LLVMValueRef pointer = LLVMBuildLoad2(codegen->builder, pointer_local->type, pointer_local->address, item->b->a->text);
          LLVMValueRef address = LLVMBuildGEP2(codegen->builder, pointer_local->pointer_element_type, pointer, &index, 1, "element");
          LLVMTypeRef value_type;
          LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
          value = convert_integer(codegen, value, value_type, pointer_local->pointer_element_type, pointer_local->pointer_element_unsigned);
          value = apply_integer_compound_assignment(codegen, item, address, pointer_local->pointer_element_type, value, 1);
          store_volatile(codegen, value, address);
          return;
        }
      }
      Local *array_local;
      LLVMValueRef address = fixed_array_element(codegen, item->b, &array_local);
      LLVMTypeRef value_type;
      LLVMValueRef value;
      if (item->a->kind == N_RECORD) {
        Node *typedef_declaration = typedef_for_type(codegen, array_local->fixed_element_type);
        if (!typedef_declaration) die("record assignment requires a typedef FixedArray element");
        value = emit_record_value(codegen, item->a, typedef_declaration, &value_type);
        if (value_type != array_local->fixed_element_type) die("FixedArray element assignment type mismatch");
        if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die_at(codegen, item, "unsupported compound record index assignment");
      } else {
        value = emit_expression(codegen, item->a, &value_type);
        value = convert_integer(codegen, value, value_type, array_local->fixed_element_type, array_local->fixed_element_unsigned);
        value = apply_integer_compound_assignment(codegen, item, address, array_local->fixed_element_type, value, 0);
      }
      LLVMBuildStore(codegen->builder, value, address);
      return;
    }
    if (item->b && item->b->kind == N_UNARY && !strcmp(item->b->text, "*")) {
      Node *address_node = item->b->a;
      LLVMValueRef pointer = NULL;
      LLVMTypeRef element_type = NULL;
      int element_unsigned = 1;
      if (address_node->kind == N_NAME) {
        Local *pointer_local = find_local(codegen, address_node->text);
        if (pointer_local && pointer_local->pointer_element_type) {
          pointer = LLVMBuildLoad2(codegen->builder, pointer_local->type, pointer_local->address, address_node->text);
          element_type = pointer_local->pointer_element_type;
          element_unsigned = pointer_local->pointer_element_unsigned;
        }
      }
      if (!pointer) {
        LLVMTypeRef pointer_type;
        pointer = emit_expression(codegen, address_node, &pointer_type);
        if ((pointer_type != codegen->pointer_type || !codegen->last_pointer_element_type) &&
            !raw_address_as_byte_pointer(codegen, &pointer, pointer_type))
          die_at(codegen, item, "dereference assignment requires a Ptr<T> value");
        element_type = codegen->last_pointer_element_type;
        element_unsigned = codegen->last_pointer_element_unsigned;
        codegen->last_pointer_element_type = NULL;
      }
      LLVMTypeRef value_type;
      LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
      value = convert_integer(codegen, value, value_type, element_type, element_unsigned);
      value = apply_integer_compound_assignment(codegen, item, pointer, element_type, value, 1);
      store_volatile(codegen, value, pointer);
      return;
    }
    Node *global_declaration = global_named(codegen, item->text);
    LLVMValueRef global_address = global_declaration ? LLVMGetNamedGlobal(codegen->module, global_declaration->text) : NULL;
    if (global_address) {
      if (global_declaration->is_final) die("cannot assign to a final variable");
      LLVMTypeRef global_type = declaration_type(codegen, global_declaration);
      LLVMTypeRef value_type;
      LLVMValueRef value;
      if (item->a && item->a->kind == N_RECORD) {
        Node *typedef_declaration = typedef_for_type(codegen, global_type);
        if (!typedef_declaration) typedef_declaration = typedef_named(codegen, global_declaration->type_name);
        if (!typedef_declaration) die_at(codegen, item, "record assignment requires a typedef global type");
        value = emit_record_value(codegen, item->a, typedef_declaration, &value_type);
        if (value_type != global_type) die_at(codegen, item, "record assignment type mismatch");
        if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die_at(codegen, item, "unsupported compound record assignment");
        LLVMBuildStore(codegen->builder, value, global_address);
        return;
      }
      value = emit_expression(codegen, item->a, &value_type);
      if (value_type == codegen->pointer_type && global_type == codegen->pointer_type) {
        if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die("unsupported compound pointer global assignment");
        if (type_is_any(global_declaration->type_name)) {
          if (item->a->kind == N_NEW) designate_any_class(global_declaration, item->a->text);
          else if (item->a->kind == N_NAME) {
            Local *source_local = find_local(codegen, item->a->text);
            if (source_local && source_local->class_name[0]) designate_any_class(global_declaration, source_local->class_name);
            else {
              Node *source = global_named(codegen, item->a->text);
              if (source && source->class_name[0]) designate_any_class(global_declaration, source->class_name);
              else if (source && source->type_name[0] && !type_is_any(source->type_name)) designate_any_class(global_declaration, source->type_name);
            }
          }
        }
      } else if (value_type == global_type) {
        if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) die_at(codegen, item, "unsupported compound global assignment");
      } else {
        if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(global_type) != LLVMIntegerTypeKind) {
          die_at(codegen, item, "cannot assign a pointer value to a non-pointer global");
        }
        value = convert_integer(codegen, value, value_type, global_type, global_declaration->integer_unsigned);
        if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) {
          LLVMValueRef current = LLVMBuildLoad2(codegen->builder, global_type, global_address, item->text);
          if (!strcmp(item->assignment_operator, "+=")) value = LLVMBuildAdd(codegen->builder, current, value, "add");
          else if (!strcmp(item->assignment_operator, "-=")) value = LLVMBuildSub(codegen->builder, current, value, "sub");
          else if (!strcmp(item->assignment_operator, "*=")) value = LLVMBuildMul(codegen->builder, current, value, "mul");
          else if (!strcmp(item->assignment_operator, "/=")) value = LLVMBuildSDiv(codegen->builder, current, value, "div");
          else if (!strcmp(item->assignment_operator, "%=")) value = LLVMBuildSRem(codegen->builder, current, value, "rem");
          else if (!strcmp(item->assignment_operator, "&=")) value = LLVMBuildAnd(codegen->builder, current, value, "and");
          else if (!strcmp(item->assignment_operator, "|=")) value = LLVMBuildOr(codegen->builder, current, value, "or");
          else if (!strcmp(item->assignment_operator, "^=")) value = LLVMBuildXor(codegen->builder, current, value, "xor");
          else if (!strcmp(item->assignment_operator, "<<=")) value = LLVMBuildShl(codegen->builder, current, value, "shl");
          else if (!strcmp(item->assignment_operator, ">>=")) value = LLVMBuildAShr(codegen->builder, current, value, "ashr");
          else if (!strcmp(item->assignment_operator, ">>>=")) value = LLVMBuildLShr(codegen->builder, current, value, "lshr");
        }
      }
      LLVMBuildStore(codegen->builder, value, global_address);
      return;
    }
    Local *item_local = local(codegen, item->text, item); LLVMTypeRef type; LLVMValueRef value = emit_expression(codegen, item->a, &type); if (item_local->pointer_element_type && type != codegen->pointer_type) value = LLVMBuildIntToPtr(codegen->builder, convert_integer(codegen, value, type, codegen->i64, 1), codegen->pointer_type, "address"); else if (type_is_float(codegen, type) || type_is_float(codegen, item_local->type)) { note_float_use(codegen); value = coerce_value(codegen, item, value, type, item_local->type, item_local->integer_unsigned, "assignment type mismatch"); if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) { LLVMValueRef current = LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, item->text); if (!strcmp(item->assignment_operator, "+=")) value = LLVMBuildFAdd(codegen->builder, current, value, "fadd"); else if (!strcmp(item->assignment_operator, "-=")) value = LLVMBuildFSub(codegen->builder, current, value, "fsub"); else if (!strcmp(item->assignment_operator, "*=")) value = LLVMBuildFMul(codegen->builder, current, value, "fmul"); else if (!strcmp(item->assignment_operator, "/=")) value = LLVMBuildFDiv(codegen->builder, current, value, "fdiv"); else die("unsupported float compound assignment"); } } else if (LLVMGetTypeKind(type) != LLVMIntegerTypeKind || LLVMGetTypeKind(item_local->type) != LLVMIntegerTypeKind) { if (type != item_local->type) die("assignment type mismatch"); } else { value = convert_integer(codegen, value, type, item_local->type, item_local->integer_unsigned); if (item->assignment_operator[0] && strcmp(item->assignment_operator, "=")) { LLVMValueRef current = LLVMBuildLoad2(codegen->builder, item_local->type, item_local->address, item->text); if (!strcmp(item->assignment_operator, "+=")) value = LLVMBuildAdd(codegen->builder, current, value, "add"); else if (!strcmp(item->assignment_operator, "-=")) value = LLVMBuildSub(codegen->builder, current, value, "sub"); else if (!strcmp(item->assignment_operator, "*=")) value = LLVMBuildMul(codegen->builder, current, value, "mul"); else if (!strcmp(item->assignment_operator, "/=")) value = LLVMBuildSDiv(codegen->builder, current, value, "div"); else if (!strcmp(item->assignment_operator, "%=")) value = LLVMBuildSRem(codegen->builder, current, value, "rem"); else if (!strcmp(item->assignment_operator, "&=")) value = LLVMBuildAnd(codegen->builder, current, value, "and"); else if (!strcmp(item->assignment_operator, "|=")) value = LLVMBuildOr(codegen->builder, current, value, "or"); else if (!strcmp(item->assignment_operator, "^=")) value = LLVMBuildXor(codegen->builder, current, value, "xor"); else if (!strcmp(item->assignment_operator, "<<=")) value = LLVMBuildShl(codegen->builder, current, value, "shl"); else if (!strcmp(item->assignment_operator, ">>=")) value = LLVMBuildAShr(codegen->builder, current, value, "ashr"); else if (!strcmp(item->assignment_operator, ">>>=")) value = LLVMBuildLShr(codegen->builder, current, value, "lshr"); else die("unsupported compound assignment"); } } LLVMBuildStore(codegen->builder, value, item_local->address); return;
  }
  if (item->kind == N_EXPR) { LLVMTypeRef ignored; emit_expression(codegen, item->a, &ignored); return; }
  if (item->kind == N_RETURN) {
    if (codegen->current_return_type == codegen->void_type) { LLVMBuildRetVoid(codegen->builder); return; }
    if (!item->a) die("non-void function must return a value");
    if (item->a->kind == N_ARRAY && LLVMGetTypeKind(codegen->current_return_type) == LLVMArrayTypeKind) {
      LLVMTypeRef subtypes[1];
      LLVMGetSubtypes(codegen->current_return_type, subtypes);
      LLVMTypeRef element_type = subtypes[0];
      unsigned capacity = (unsigned)LLVMGetArrayLength2(codegen->current_return_type);
      LLVMValueRef value = LLVMGetUndef(codegen->current_return_type);
      unsigned index = 0;
      for (Node *element = item->a->a; element; element = element->next) {
        if (index == capacity) die_at(codegen, element, "FixedArray return initializer is too long");
        LLVMTypeRef value_type;
        LLVMValueRef element_value = emit_expression(codegen, element, &value_type);
        if (LLVMGetTypeKind(element_type) == LLVMIntegerTypeKind)
          element_value = convert_integer(codegen, element_value, value_type, element_type, 1);
        else if (value_type != element_type) die_at(codegen, element, "FixedArray return element type mismatch");
        value = LLVMBuildInsertValue(codegen->builder, value, element_value, index++, "array");
      }
      LLVMBuildRet(codegen->builder, value);
      return;
    }
    if (item->a->kind == N_RECORD) {
      Node *typedef_declaration = typedef_for_type(codegen, codegen->current_return_type);
      if (!typedef_declaration) die("record return requires a declared record type");
      LLVMValueRef value = LLVMGetUndef(codegen->current_return_type);
      for (Node *value_field = item->a->a; value_field; value_field = value_field->next) {
        unsigned field_index;
        Node *field = typedef_field_named(typedef_declaration, value_field->text, &field_index);
        if (!field) die("record return contains an unknown field");
        LLVMTypeRef field_type = declaration_type(codegen, field);
        LLVMTypeRef value_type;
        LLVMValueRef field_value = emit_expression(codegen, value_field->a, &value_type);
        if (value_type != field_type) {
          if (LLVMGetTypeKind(value_type) != LLVMIntegerTypeKind || LLVMGetTypeKind(field_type) != LLVMIntegerTypeKind) die("record return field type mismatch");
          field_value = convert_integer(codegen, field_value, value_type, field_type, field->integer_unsigned);
        }
        value = LLVMBuildInsertValue(codegen->builder, value, field_value, field_index, "record");
      }
      LLVMBuildRet(codegen->builder, value);
      return;
    }
    LLVMTypeRef value_type;
    LLVMValueRef value = emit_expression(codegen, item->a, &value_type);
    if (value_type != codegen->current_return_type) {
      value = coerce_value(codegen, item->a, value, value_type, codegen->current_return_type, 0, "return type mismatch");
    }
    LLVMBuildRet(codegen->builder, value);
    return;
  }
  if (item->kind == N_BREAK) {
    if (!codegen->break_target) die("break is only supported inside a loop");
    LLVMBuildBr(codegen->builder, codegen->break_target);
    return;
  }
  if (item->kind == N_CONTINUE) {
    if (!codegen->continue_target) die("continue is only supported inside a loop");
    LLVMBuildBr(codegen->builder, codegen->continue_target);
    return;
  }
  if (item->kind == N_SWITCH) {
    emit_switch(codegen, item, 0, NULL, NULL);
    return;
  }
  if (item->kind == N_IF) { LLVMTypeRef type; LLVMValueRef condition = emit_expression(codegen, item->a, &type); LLVMBasicBlockRef then_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "then"), else_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "else"), end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "endif"); LLVMBuildCondBr(codegen->builder, as_boolean(codegen, condition, type), then_block, else_block); LLVMPositionBuilderAtEnd(codegen->builder, then_block); emit_statement(codegen, item->b); if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, end_block); LLVMPositionBuilderAtEnd(codegen->builder, else_block); emit_statement(codegen, item->c); if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, end_block); LLVMPositionBuilderAtEnd(codegen->builder, end_block); return; }
  if (item->kind == N_WHILE) { LLVMBasicBlockRef condition_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "while.condition"), body_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "while.body"), end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "while.end"); LLVMBuildBr(codegen->builder, condition_block); LLVMPositionBuilderAtEnd(codegen->builder, condition_block); LLVMTypeRef type; LLVMValueRef condition = emit_expression(codegen, item->a, &type); LLVMBuildCondBr(codegen->builder, as_boolean(codegen, condition, type), body_block, end_block); LLVMPositionBuilderAtEnd(codegen->builder, body_block); LLVMBasicBlockRef saved_break_target = codegen->break_target, saved_continue_target = codegen->continue_target; codegen->break_target = end_block; codegen->continue_target = condition_block; emit_statement(codegen, item->b); codegen->break_target = saved_break_target; codegen->continue_target = saved_continue_target; if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, condition_block); LLVMPositionBuilderAtEnd(codegen->builder, end_block); return; }
  if (item->kind == N_DO_WHILE) { LLVMBasicBlockRef body_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "do.body"), condition_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "do.condition"), end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "do.end"); LLVMBuildBr(codegen->builder, body_block); LLVMPositionBuilderAtEnd(codegen->builder, body_block); LLVMBasicBlockRef saved_break_target = codegen->break_target, saved_continue_target = codegen->continue_target; codegen->break_target = end_block; codegen->continue_target = condition_block; emit_statement(codegen, item->b); codegen->break_target = saved_break_target; codegen->continue_target = saved_continue_target; if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, condition_block); LLVMPositionBuilderAtEnd(codegen->builder, condition_block); LLVMTypeRef type; LLVMValueRef condition = emit_expression(codegen, item->a, &type); LLVMBuildCondBr(codegen->builder, as_boolean(codegen, condition, type), body_block, end_block); LLVMPositionBuilderAtEnd(codegen->builder, end_block); return; }
  if (item->kind == N_FOR) {
    /* Induction variable follows target word (i64 on amd64/arm64), not a hard-coded i32. */
    LLVMTypeRef start_type, end_type;
    LLVMTypeRef index_type = codegen->word_type;
    LLVMValueRef start = as_integer(codegen, emit_expression(codegen, item->a, &start_type), start_type);
    LLVMValueRef end = as_integer(codegen, emit_expression(codegen, item->b, &end_type), end_type);
    Local *index = add_local(codegen, item->text, index_type, start, NULL, 1);
    LLVMBasicBlockRef condition_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "for.condition");
    LLVMBasicBlockRef body_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "for.body");
    LLVMBasicBlockRef increment_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "for.increment");
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(codegen->context, codegen->main_fn, "for.end");
    LLVMBuildBr(codegen->builder, condition_block);
    LLVMPositionBuilderAtEnd(codegen->builder, condition_block);
    LLVMValueRef current = LLVMBuildLoad2(codegen->builder, index_type, index->address, item->text);
    LLVMBuildCondBr(codegen->builder, LLVMBuildICmp(codegen->builder, LLVMIntULT, current, end, "for.test"), body_block, end_block);
    LLVMPositionBuilderAtEnd(codegen->builder, body_block);
    LLVMBasicBlockRef saved_break_target = codegen->break_target, saved_continue_target = codegen->continue_target;
    codegen->break_target = end_block;
    codegen->continue_target = increment_block;
    emit_statement(codegen, item->c);
    codegen->break_target = saved_break_target;
    codegen->continue_target = saved_continue_target;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) LLVMBuildBr(codegen->builder, increment_block);
    LLVMPositionBuilderAtEnd(codegen->builder, increment_block);
    LLVMValueRef next = LLVMBuildAdd(codegen->builder, LLVMBuildLoad2(codegen->builder, index_type, index->address, item->text), LLVMConstInt(index_type, 1, 0), "for.next");
    LLVMBuildStore(codegen->builder, next, index->address);
    LLVMBuildBr(codegen->builder, condition_block);
    LLVMPositionBuilderAtEnd(codegen->builder, end_block);
    return;
  }
}

static int has_trace_override(Node *item) { if (!item) return 0; if (item->kind == N_ASSIGN && !strcmp(item->text, "haxe.Log.trace") && item->a && item->a->kind == N_FUNCTION) return 1; return has_trace_override(item->a) || has_trace_override(item->b) || has_trace_override(item->c) || has_trace_override(item->next); }

static void apply_target_abi(Codegen *codegen) {
  /* x86-64 SysV has a 128-byte red zone; Windows/UEFI Microsoft ABI does not.
     Only stamp noredzone where SysV would otherwise assume one. */
  if (codegen->target_abi == TARGET_ABI_SYSV && codegen->is_x86_64) {
    unsigned kind = LLVMGetEnumAttributeKindForName("noredzone", strlen("noredzone"));
    if (!kind) die("LLVM build is missing the noredzone attribute");
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(codegen->context, kind, 0);
    for (LLVMValueRef function = LLVMGetFirstFunction(codegen->module); function; function = LLVMGetNextFunction(function)) {
      LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, attribute);
    }
  }
  if (codegen->target_abi == TARGET_ABI_WIN64) {
    /* Freestanding PE/UEFI has no CRT __chkstk — don't emit stack probes. */
    LLVMAttributeRef no_probe = LLVMCreateStringAttribute(codegen->context, "no-stack-arg-probe", (unsigned)strlen("no-stack-arg-probe"), "", 0);
    for (LLVMValueRef function = LLVMGetFirstFunction(codegen->module); function; function = LLVMGetNextFunction(function)) {
      LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, no_probe);
    }
    for (LLVMValueRef function = LLVMGetFirstFunction(codegen->module); function; function = LLVMGetNextFunction(function)) {
      if (LLVMGetFunctionCallConv(function) == LLVMWin64CallConv) continue;
      int skip = 0;
      for (Function *declared = codegen->function_values; function && declared; declared = declared->next) {
        if (declared->value == function && declared->declaration->is_naked) { skip = 1; break; }
      }
      if (skip) continue;
      LLVMSetFunctionCallConv(function, LLVMWin64CallConv);
      for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block; block = LLVMGetNextBasicBlock(block)) {
        for (LLVMValueRef inst = LLVMGetFirstInstruction(block); inst; inst = LLVMGetNextInstruction(inst)) {
          if (!LLVMIsACallInst(inst)) continue;
          LLVMValueRef callee = LLVMGetCalledValue(inst);
          if (callee && LLVMIsAInlineAsm(callee)) continue;
          LLVMSetInstructionCallConv(inst, LLVMWin64CallConv);
        }
      }
    }
  }
}

static void declare_functions(Codegen *codegen) {
  for (Node *declaration = codegen->functions; declaration; declaration = declaration->next) {
    if (declaration->is_entry_point || declaration->is_dynamic_array || declaration->is_macro) continue;
    LLVMTypeRef parameters[256];
    unsigned count = 0;
    for (Node *parameter = declaration->b; parameter; parameter = parameter->next) {
      if (parameter->is_flexible_array) die("flexible FixedArray is only supported as a trailing typedef field");
      if (count == 256 || !(parameters[count] = is_fixed_array_decl(parameter) ? codegen->pointer_type : declaration_type(codegen, parameter))) die("unsupported function parameter type");
      count++;
    }
    Function *function = calloc(1, sizeof(*function));
    if (!function) die("out of memory");
    function->declaration = declaration;
    function->return_type = function_return_type(codegen, declaration);
    function->value = LLVMGetNamedFunction(codegen->module, declaration->text);
    if (!function->value) function->value = LLVMAddFunction(codegen->module, declaration->text, LLVMFunctionType(function->return_type, parameters, count, 0));
    if (declaration->is_naked) {
      unsigned naked_kind = LLVMGetEnumAttributeKindForName("naked", strlen("naked"));
      if (naked_kind) LLVMAddAttributeAtIndex(function->value, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(codegen->context, naked_kind, 0));
    }
    function->next = codegen->function_values;
    codegen->function_values = function;
  }
}

static void emit_functions(Codegen *codegen) {
  for (Function *function = codegen->function_values; function; function = function->next) {
    if (function->declaration->is_imported || function->declaration->is_extern || function->declaration->is_macro) continue;
    Local *saved_locals = codegen->locals;
    LLVMValueRef saved_function = codegen->main_fn;
    LLVMTypeRef saved_return_type = codegen->current_return_type;
    LLVMMetadataRef saved_scope = codegen->di_scope;
    codegen->locals = NULL;
    codegen->main_fn = function->value;
    codegen->current_return_type = function->return_type;
    strcpy(codegen->current_class_name, function->declaration->class_name);
    debug_attach_subprogram(codegen, function->value, function->declaration);
    LLVMPositionBuilderAtEnd(codegen->builder, LLVMAppendBasicBlockInContext(codegen->context, function->value, "entry"));
    debug_set_location(codegen, function->declaration);
    unsigned index = 0;
    for (Node *parameter = function->declaration->b; parameter; parameter = parameter->next, index++) {
      Local *local_parameter;
      if (is_fixed_array_decl(parameter)) {
        local_parameter = calloc(1, sizeof(*local_parameter));
        if (!local_parameter) die("out of memory");
        strcpy(local_parameter->name, parameter->text);
        local_parameter->type = fixed_array_type(codegen, parameter);
        local_parameter->address = LLVMGetParam(function->value, index);
        local_parameter->integer_unsigned = parameter->integer_unsigned;
        local_parameter->next = codegen->locals;
        codegen->locals = local_parameter;
      } else local_parameter = add_local(codegen, parameter->text, declaration_type(codegen, parameter), LLVMGetParam(function->value, index), NULL, parameter->integer_unsigned);
      if (parameter->type_name[0] && class_table_find(&codegen->classes, parameter->type_name)) strcpy(local_parameter->class_name, parameter->type_name);
      else if (!strcmp(parameter->text, "this") && function->declaration->class_name[0]) strcpy(local_parameter->class_name, function->declaration->class_name);
      if (parameter->is_dynamic_array) {
        local_parameter->dynamic_element_type = parameter->dynamic_element_is_pointer ? codegen->pointer_type : (parameter->dynamic_element_width ? LLVMIntTypeInContext(codegen->context, parameter->dynamic_element_width) : codegen->i64);
        local_parameter->dynamic_element_unsigned = parameter->dynamic_element_unsigned;
        if (parameter->dynamic_element_is_pointer) {
          Node *element = typedef_named(codegen, parameter->dynamic_element_pointer_type_name);
          local_parameter->pointer_element_type = element ? LLVMGetTypeByName2(codegen->context, element->text) : NULL;
        }
      }
      if (is_fixed_array_decl(parameter)) {
        local_parameter->fixed_element_type = fixed_array_element_type(codegen, parameter);
        local_parameter->fixed_array_length = parameter->fixed_array_length;
        local_parameter->fixed_element_unsigned = parameter->fixed_element_unsigned;
      }
      if (parameter->pointer_element_width || parameter->pointer_type_name[0]) {
        local_parameter->pointer_element_type = pointer_element_type(codegen, parameter);
        local_parameter->pointer_element_unsigned = parameter->pointer_element_unsigned;
      }
    }
    emit_statement(codegen, function->declaration->a);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
      if (function->declaration->is_naked) LLVMBuildUnreachable(codegen->builder);
      else if (function->return_type == codegen->void_type) LLVMBuildRetVoid(codegen->builder);
      else LLVMBuildRet(codegen->builder, LLVMConstNull(function->return_type));
    }
    codegen->locals = saved_locals;
    codegen->main_fn = saved_function;
    codegen->current_return_type = saved_return_type;
    codegen->di_scope = saved_scope;
  }
}

int generate_output(Node *tree, const char *path, OutputKind output_kind, const char *cpu_features, const char *target_triple, int emit_debug) {
  Codegen codegen = {0};
  codegen.diagnostic = haxellvm_diagnostic_create(tree->source, tree->source_path);
  active_diagnostic = codegen.diagnostic;
  active_diagnostic_node = tree;
  codegen.tree = tree;
  codegen.emit_debug = emit_debug;
  codegen.context = LLVMContextCreate();
  codegen.module = LLVMModuleCreateWithNameInContext("haxellvm", codegen.context);
  codegen.builder = LLVMCreateBuilderInContext(codegen.context);
  codegen.i1 = LLVMInt1TypeInContext(codegen.context); codegen.i8 = LLVMInt8TypeInContext(codegen.context); codegen.i16 = LLVMInt16TypeInContext(codegen.context); codegen.i32 = LLVMInt32TypeInContext(codegen.context); codegen.i64 = LLVMInt64TypeInContext(codegen.context); codegen.void_type = LLVMVoidTypeInContext(codegen.context); codegen.pointer_type = LLVMPointerType(codegen.i8, 0);
  codegen.f32 = LLVMFloatTypeInContext(codegen.context);
  codegen.f64 = LLVMDoubleTypeInContext(codegen.context);
  codegen.functions = tree->c;
  codegen.typedefs = tree->next;
  codegen.globals = tree->b;
  codegen.class_initializers = tree->d;
  codegen.usings = tree->metadata_nodes;
  for (Node *using_node = tree->metadata_nodes; using_node; using_node = using_node->next) {
    if (using_node->kind == N_USING) codegen.has_usings = 1;
  }
  int custom_triple = target_triple && target_triple[0];
  char *triple = custom_triple ? strdup(target_triple) : LLVMGetDefaultTargetTriple();
  if (!triple) { fprintf(stderr, "haxellvm: cannot determine target triple\n"); return 1; }
  codegen.object_format = object_format_from_triple(triple);
  codegen.target_abi = target_abi_from_triple(triple);
  codegen.is_x86_64 = triple_is_x86_64(triple);
  codegen.word_bits = word_bits_from_triple(triple);
  codegen.word_type = LLVMIntTypeInContext(codegen.context, codegen.word_bits);
  LLVMSetTarget(codegen.module, triple);
  apply_sysv_host_data_layout(&codegen);
  debug_init(&codegen, path);
  class_table_build(&codegen.classes, tree);
  refine_any_globals(&codegen);
  emit_typedefs(&codegen);
  emit_class_types(&codegen);
  declare_imported_globals(&codegen);
  for (Node *global = tree->b; global; global = global->next) if (!global->is_imported && !global->is_extern) emit_global(&codegen, global);
  emit_class_initializers(&codegen);
  declare_functions(&codegen);
  emit_vtables(&codegen);
  emit_functions(&codegen);
  if (tree->a) {
    if (!has_trace_override(tree->a)) ensure_host_trace_io(&codegen);
    LLVMTypeRef main_type = LLVMFunctionType(codegen.i32, NULL, 0, 0);
    codegen.main_fn = LLVMAddFunction(codegen.module, tree->text, main_type);
    codegen.current_return_type = codegen.i32;
    debug_attach_subprogram(&codegen, codegen.main_fn, tree);
    LLVMPositionBuilderAtEnd(codegen.builder, LLVMAppendBasicBlockInContext(codegen.context, codegen.main_fn, "entry"));
    debug_set_location(&codegen, tree);
    emit_statement(&codegen, tree->a);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen.builder))) LLVMBuildRet(codegen.builder, LLVMConstInt(codegen.i32, 0, 0));
  }
  apply_target_abi(&codegen);
  if (codegen.uses_float && codegen.object_format == OBJECT_FORMAT_PE && !LLVMGetNamedGlobal(codegen.module, "_fltused")) {
    /* MSVC/lld-link require this when any TU references floating-point. */
    LLVMValueRef fltused = LLVMAddGlobal(codegen.module, codegen.i32, "_fltused");
    LLVMSetInitializer(fltused, LLVMConstInt(codegen.i32, 0, 0));
  }
  debug_finalize(&codegen);
  char *message = NULL;
  if (LLVMVerifyModule(codegen.module, LLVMReturnStatusAction, &message)) { fprintf(stderr, "haxellvm: invalid LLVM module: %s\n", message); LLVMDisposeMessage(message); if (custom_triple) free(triple); else LLVMDisposeMessage(triple); return 1; }
  if (output_kind == OUTPUT_LLVM_IR) {
    if (LLVMPrintModuleToFile(codegen.module, path, &message)) { fprintf(stderr, "haxellvm: cannot write %s: %s\n", path, message); LLVMDisposeMessage(message); if (custom_triple) free(triple); else LLVMDisposeMessage(triple); return 1; }
  } else {
    if (custom_triple) {
      LLVMInitializeAllTargetInfos();
      LLVMInitializeAllTargets();
      LLVMInitializeAllTargetMCs();
      LLVMInitializeAllAsmPrinters();
      LLVMInitializeAllAsmParsers();
    } else {
      LLVMInitializeNativeTarget();
      LLVMInitializeNativeAsmPrinter();
      LLVMInitializeNativeAsmParser();
    }
    LLVMTargetRef target;
    if (!strncmp(triple, "x86_64", 6) || !strncmp(triple, "i386", 4) || !strncmp(triple, "i486", 4) || !strncmp(triple, "i586", 4) || !strncmp(triple, "i686", 4)) {
      const char *options[] = { "haxellvm", "-x86-asm-syntax=intel" };
      LLVMParseCommandLineOptions(2, options, "haxellvm LLVM backend options");
    }
    if (LLVMGetTargetFromTriple(triple, &target, &message)) {
      fprintf(stderr, "haxellvm: target error: %s\n", message);
      LLVMDisposeMessage(message);
      if (custom_triple) free(triple); else LLVMDisposeMessage(triple);
      return 1;
    }
    char feature_buffer[512];
    const char *features = cpu_features ? cpu_features : "";
    if (codegen.uses_float && codegen.is_x86_64) {
      if (strstr(features, "soft-float") || strstr(features, "-sse2") || strstr(features, "-sse")) {
        fprintf(stderr, "haxellvm: float ops need SSE (+sse2); ditch +soft-float/-sse — this ain't soft-float RISC-V cosplay\n");
        if (custom_triple) free(triple); else LLVMDisposeMessage(triple);
        return 1;
      }
      if (!features[0]) {
        snprintf(feature_buffer, sizeof(feature_buffer), "+sse2,+sse");
        features = feature_buffer;
      } else if (!strstr(features, "+sse")) {
        snprintf(feature_buffer, sizeof(feature_buffer), "%s,+sse2,+sse", features);
        features = feature_buffer;
      }
    }
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(target, triple, "generic", features, LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    if (!machine) {
      fprintf(stderr, "haxellvm: error: cannot create target machine\n");
      if (custom_triple) free(triple); else LLVMDisposeMessage(triple);
      return 1;
    }
    LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(machine);
    char *data_layout = LLVMCopyStringRepOfTargetData(target_data);
    LLVMSetDataLayout(codegen.module, data_layout);
    LLVMDisposeMessage(data_layout);
    LLVMDisposeTargetData(target_data);
    LLVMCodeGenFileType file_type = output_kind == OUTPUT_OBJECT ? LLVMObjectFile : LLVMAssemblyFile;
    if (LLVMTargetMachineEmitToFile(machine, codegen.module, (char *)path, file_type, &message)) {
      fprintf(stderr, "haxellvm: cannot emit %s: %s\n", output_kind == OUTPUT_OBJECT ? "object" : "assembly", message ? message : "target machine unavailable");
      if (message) LLVMDisposeMessage(message);
      LLVMDisposeTargetMachine(machine);
      if (custom_triple) free(triple); else LLVMDisposeMessage(triple);
      return 1;
    }
    LLVMDisposeTargetMachine(machine);
  }
  if (custom_triple) free(triple); else LLVMDisposeMessage(triple);
  LLVMDisposeBuilder(codegen.builder); LLVMDisposeModule(codegen.module); LLVMContextDispose(codegen.context);
  return 0;
}