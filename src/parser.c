#include <haxellvm/parser.h>

#include <haxellvm/diagnostic.h>
#include <haxellvm/lexer.h>
#include <haxellvm/macro.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Node *node_new(NodeKind kind) {
  Node *result = calloc(1, sizeof(*result));
  if (!result) {
    fprintf(stderr, "haxellvm: out of memory\n");
    exit(1);
  }
  result->kind = kind;
  return result;
}

static void die_parse(Lexer *lexer, size_t start_offset, size_t end_offset, const char *message) {
  haxellvm_diagnostic_error(lexer->diagnostic, start_offset, end_offset, message);
  exit(1);
}

/* Haxe allows i32/u64/f64-style suffixes on numeric literals (lexer.ml split_*_suffix). */
static void strip_numeric_suffix(char *literal) {
  size_t length = strlen(literal);
  if (length < 2) return;
  size_t index = length;
  while (index > 0 && literal[index - 1] >= '0' && literal[index - 1] <= '9') index--;
  if (index == 0) return;
  char mark = literal[index - 1];
  if (mark != 'i' && mark != 'u' && mark != 'f' && mark != 'I' && mark != 'U' && mark != 'F') return;
  /* Hex digits include a-f/A-F; only i/u suffixes are valid on 0x… literals. */
  int is_hex = literal[0] == '0' && (literal[1] == 'x' || literal[1] == 'X');
  if (is_hex && (mark == 'f' || mark == 'F')) return;
  /* Bare f/F (Single) or i32/u64/f64-style — strip the mark and optional digits. */
  if (index == length) {
    /* bare letter suffix */
    literal[index - 1] = '\0';
    return;
  }
  literal[index - 1] = '\0';
}

static void parse_function_name(Lexer *lexer, char *destination) {
  /* Haxe constructors are `function new` — `new` is a keyword used as a name. */
  if (lexer->token.kind == TK_KWD && !strcmp(lexer->token.text, "new")) {
    strcpy(destination, "new");
    lexer_next(lexer);
    return;
  }
  lexer_name(lexer, destination);
}

static Node *expression(Lexer *lexer, int minimum_precedence);
static Node *expression_until_angle_close(Lexer *lexer);
static int precedence(const char *operator);
static Node *block(Lexer *lexer);
static Node *statement(Lexer *lexer);
static Node *statement_ex(Lexer *lexer, int allow_omit_semi_before_rbrace);
static Node *statement_or_block(Lexer *lexer);
static Node *parse_switch(Lexer *lexer);
static Node *parse_haxe_from_root(const char *source, const char *source_path, const char *import_root);
static Node *parse_import(Node *import_site, const char *import_root);
static int using_module_exists(const char *module_name, const char *source_path, const char *import_root);
static Node **anonymous_type_tail;
static unsigned anonymous_type_index;

static void append_nodes(Node **head, Node *nodes) {
  while (*head) head = &(*head)->next;
  *head = nodes;
}

typedef struct {
  char section[256];
  char section_elf[256];
  char section_pe[256];
  char section_macho[256];
  char macho_segment[256];
} SectionMetas;

static void parse_build_macro_path(Lexer *lexer, char *destination);
static void unsupported_metadata(Lexer *lexer, Token token);

static void parse_section_string(Lexer *lexer, const char *meta_name, char *destination) {
  lexer_expect(lexer, "(");
  if (lexer->token.kind != TK_STR) {
    fprintf(stderr, "haxellvm: %s expects a string literal\n", meta_name);
    exit(1);
  }
  strcpy(destination, lexer->token.text);
  lexer_next(lexer);
  lexer_expect(lexer, ")");
}

/* @:aligned(N) — N must be a power-of-two integer literal. Returns 1 if handled. */
static int apply_aligned_meta(Lexer *lexer, const char *metadata, unsigned *alignment_out) {
  if (strcmp(metadata, "aligned")) return 0;
  lexer_expect(lexer, "(");
  if (lexer->token.kind != TK_NUM) {
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, "@:aligned expects an integer literal");
    exit(1);
  }
  unsigned alignment = (unsigned)strtoul(lexer->token.text, NULL, 0);
  if (alignment < 1 || (alignment & (alignment - 1)) != 0) {
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, "@:aligned requires a power-of-two alignment");
    exit(1);
  }
  lexer_next(lexer);
  lexer_expect(lexer, ")");
  if (alignment_out) {
    if (*alignment_out && *alignment_out != alignment) {
      haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, "conflicting @:aligned values");
      exit(1);
    }
    *alignment_out = alignment;
  }
  return 1;
}

/* Returns 1 if metadata named a section placement attribute. */
static int apply_section_meta(Lexer *lexer, const char *metadata, SectionMetas *sections) {
  if (!strcmp(metadata, "section")) {
    parse_section_string(lexer, "section", sections->section);
    return 1;
  }
  if (!strcmp(metadata, "elfSection")) {
    parse_section_string(lexer, "elfSection", sections->section_elf);
    return 1;
  }
  if (!strcmp(metadata, "peSection") || !strcmp(metadata, "coffSection")) {
    parse_section_string(lexer, metadata, sections->section_pe);
    return 1;
  }
  if (!strcmp(metadata, "machoSection")) {
    parse_section_string(lexer, "machoSection", sections->section_macho);
    return 1;
  }
  if (!strcmp(metadata, "machoSegment")) {
    parse_section_string(lexer, "machoSegment", sections->macho_segment);
    return 1;
  }
  return 0;
}

static void copy_section_metas(Node *item, const SectionMetas *sections) {
  strcpy(item->section, sections->section);
  strcpy(item->section_elf, sections->section_elf);
  strcpy(item->section_pe, sections->section_pe);
  strcpy(item->section_macho, sections->section_macho);
  strcpy(item->macho_segment, sections->macho_segment);
}

static void mark_imported_nodes(Node *nodes, Node *import_site) {
  for (Node *item = nodes; item; item = item->next) {
    /* Keep the original import site for transitively imported declarations. */
    if (item->is_imported) continue;
    item->is_imported = 1;
    if (!import_site) continue;
    item->reference_source = import_site->source;
    item->reference_path = import_site->source_path;
    item->reference_offset = import_site->source_offset;
    item->reference_end_offset = import_site->source_end_offset;
  }
}

static void qualified_name(Lexer *lexer, char *destination) {
  lexer_name(lexer, destination);
  while (lexer_accept(lexer, ".")) {
    size_t length = strlen(destination);
    if (length + 2 >= 256) {
      fprintf(stderr, "haxellvm: qualified name too long\n");
      exit(1);
    }
    destination[length] = '.';
    lexer_name(lexer, destination + length + 1);
  }
}

static void parse_build_macro_path(Lexer *lexer, char *destination) {
  lexer_expect(lexer, "(");
  qualified_name(lexer, destination);
  if (lexer_accept(lexer, "(")) {
    int depth = 1;
    while (depth) {
      if (lexer_accept(lexer, "(")) depth++;
      else if (lexer_accept(lexer, ")")) depth--;
      else lexer_next(lexer);
    }
  }
  lexer_expect(lexer, ")");
}

static void import_name(Lexer *lexer, char *destination) {
  size_t length = 0;
  unsigned dots = 0;
  /* The lexer folds "..." into the range token, so count raw dots rather than pairs. */
  while (lexer->token.kind == TK_SYM && lexer->token.text[0] == '.') {
    dots += (unsigned)strlen(lexer->token.text);
    lexer_next(lexer);
  }
  if (dots % 2) {
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, "import parent prefix must use pairs of dots ('..' per directory)");
    exit(1);
  }
  for (unsigned parent = 0; parent < dots / 2; parent++) {
    if (length + 3 >= 256) {
      fprintf(stderr, "haxellvm: import path too long\n");
      exit(1);
    }
    destination[length++] = '.';
    destination[length++] = '.';
    destination[length++] = '/';
  }
  qualified_name(lexer, destination + length);
}

static void optional_type(Lexer *lexer, Node *declaration) {
  if (!lexer_accept(lexer, ":")) return;
  if (lexer_accept(lexer, "{")) {
    Node *anonymous_type = node_new(N_TYPEDEF);
    snprintf(anonymous_type->text, sizeof(anonymous_type->text), "__haxellvm_anonymous_type_%u", anonymous_type_index++);
    Node **fields = &anonymous_type->a;
    while (!lexer_accept(lexer, "}")) {
      Node *field = node_new(N_FIELD);
      lexer_accept(lexer, "var");
      lexer_name(lexer, field->text);
      optional_type(lexer, field);
      *fields = field;
      fields = &field->next;
      if (!lexer_accept(lexer, ";") && !lexer_accept(lexer, ",") && strcmp(lexer->token.text, "}")) {
        fprintf(stderr, "haxellvm: anonymous type fields must be separated by ',' or ';'\n");
        exit(1);
      }
    }
    if (!anonymous_type_tail) {
      fprintf(stderr, "haxellvm: anonymous types are only valid inside a module\n");
      exit(1);
    }
    *anonymous_type_tail = anonymous_type;
    anonymous_type_tail = &anonymous_type->next;
    if (declaration) {
      strcpy(declaration->type_name, anonymous_type->text);
      declaration->type = node_new(N_TYPE);
      declaration->type->type_kind = TYPE_ANONYMOUS;
      strcpy(declaration->type->text, anonymous_type->text);
      declaration->type->a = anonymous_type->a;
    }
    return;
  }
  if (lexer_accept(lexer, "(")) {
    int depth = 1;
    while (depth) {
      if (lexer->token.kind == TK_EOF) {
        fprintf(stderr, "haxellvm: unterminated function type\n");
        exit(1);
      }
      if (lexer_accept(lexer, "(")) depth++;
      else if (lexer_accept(lexer, ")")) depth--;
      else lexer_next(lexer);
    }
    lexer_expect(lexer, "->");
    char return_type[256];
    qualified_name(lexer, return_type);
    if (lexer_accept(lexer, "<")) {
      int generic_depth = 1;
      while (generic_depth) {
        if (lexer->token.kind == TK_EOF) {
          fprintf(stderr, "haxellvm: unterminated function return type\n");
          exit(1);
        }
        if (lexer_accept(lexer, "<")) generic_depth++;
        else if (lexer_accept(lexer, ">")) generic_depth--;
        else lexer_next(lexer);
      }
    }
    if (declaration) {
      declaration->is_function_pointer = 1;
      strcpy(declaration->type_name, "Function");
    }
    return;
  }
  char type_name[256];
  qualified_name(lexer, type_name);
  if (declaration) strcpy(declaration->type_name, type_name);
  if (lexer_accept(lexer, "<")) {
    if (declaration && !strcmp(type_name, "Ptr")) {
      char element_type[256];
      qualified_name(lexer, element_type);
      if (!strcmp(element_type, "IntSize") || !strcmp(element_type, "UIntSize") || !strcmp(element_type, "IntType") || !strcmp(element_type, "UIntType")) {
        lexer_expect(lexer, "<");
        if (lexer->token.kind != TK_NUM) {
          fprintf(stderr, "haxellvm: Ptr element width must be an integer\n");
          exit(1);
        }
        declaration->pointer_element_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
        declaration->pointer_element_unsigned = !strcmp(element_type, "UIntSize") || !strcmp(element_type, "UIntType");
        if (!declaration->pointer_element_width || declaration->pointer_element_width > 65535) {
          fprintf(stderr, "haxellvm: Ptr element width must be between 1 and 65535\n");
          exit(1);
        }
        lexer_next(lexer);
        lexer_expect(lexer, ">");
      } else if (!strcmp(element_type, "Int") || !strcmp(element_type, "UInt")) {
        strcpy(declaration->pointer_type_name, element_type);
        declaration->pointer_element_unsigned = !strcmp(element_type, "UInt");
      } else if (!strcmp(element_type, "Array")) {
        lexer_expect(lexer, "<");
        int depth = 1;
        while (depth) {
          if (lexer->token.kind == TK_EOF) {
            fprintf(stderr, "haxellvm: unterminated Ptr<Array<T>> type\n");
            exit(1);
          }
          if (lexer_accept(lexer, "<")) depth++;
          else if (lexer_accept(lexer, ">")) depth--;
          else lexer_next(lexer);
        }
        strcpy(declaration->pointer_type_name, "Array");
      } else {
        strcpy(declaration->pointer_type_name, element_type);
      }
      lexer_expect(lexer, ">");
      return;
    }
    if (declaration && (!strcmp(type_name, "FixedArray") || !strcmp(type_name, "Array"))) {
      char element_type[256];
      qualified_name(lexer, element_type);
      if (!strcmp(type_name, "Array") && !strcmp(element_type, "Ptr")) {
        lexer_expect(lexer, "<");
        char pointee_type[256];
        qualified_name(lexer, pointee_type);
        if (!strcmp(pointee_type, "IntSize") || !strcmp(pointee_type, "UIntSize") || !strcmp(pointee_type, "IntType") || !strcmp(pointee_type, "UIntType")) {
          lexer_expect(lexer, "<");
          if (lexer->token.kind != TK_NUM) { fprintf(stderr, "haxellvm: Array pointer element width must be an integer\n"); exit(1); }
          declaration->dynamic_element_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
          declaration->dynamic_element_unsigned = !strcmp(pointee_type, "UIntSize") || !strcmp(pointee_type, "UIntType");
          lexer_next(lexer);
          lexer_expect(lexer, ">");
        } else strcpy(declaration->dynamic_element_pointer_type_name, pointee_type);
        declaration->is_dynamic_array = 1;
        declaration->dynamic_element_is_pointer = 1;
        lexer_expect(lexer, ">");
        lexer_expect(lexer, ">");
        return;
      }
      if (!strcmp(type_name, "Array") && (!strcmp(element_type, "IntSize") || !strcmp(element_type, "UIntSize") || !strcmp(element_type, "IntType") || !strcmp(element_type, "UIntType"))) {
        lexer_expect(lexer, "<");
        if (lexer->token.kind != TK_NUM) {
          fprintf(stderr, "haxellvm: Array element width must be an integer\n");
          exit(1);
        }
        declaration->dynamic_element_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
        declaration->dynamic_element_unsigned = !strcmp(element_type, "UIntSize") || !strcmp(element_type, "UIntType");
        if (!declaration->dynamic_element_width || declaration->dynamic_element_width > 65535) {
          fprintf(stderr, "haxellvm: Array element width must be between 1 and 65535\n");
          exit(1);
        }
        declaration->is_dynamic_array = 1;
        lexer_next(lexer);
        lexer_expect(lexer, ">");
        lexer_expect(lexer, ">");
        return;
      }
      if (!strcmp(type_name, "Array") && lexer_accept(lexer, ">")) {
        declaration->is_dynamic_array = 1;
        strcpy(declaration->dynamic_element_type_name, element_type);
        declaration->dynamic_element_unsigned = !strcmp(element_type, "UInt");
        return;
      }
      if (!strcmp(element_type, "Int") || !strcmp(element_type, "UInt")) {
        strcpy(declaration->fixed_element_type_name, element_type);
        declaration->fixed_element_unsigned = !strcmp(element_type, "UInt");
      } else if (!strcmp(element_type, "IntSize") || !strcmp(element_type, "UIntSize") || !strcmp(element_type, "IntType") || !strcmp(element_type, "UIntType")) {
        lexer_expect(lexer, "<");
        if (lexer->token.kind != TK_NUM) {
          die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "FixedArray element width must be an integer");
        }
        declaration->fixed_element_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
        declaration->fixed_element_unsigned = !strcmp(element_type, "UIntSize") || !strcmp(element_type, "UIntType");
        lexer_next(lexer);
        lexer_expect(lexer, ">");
      } else if (!strcmp(element_type, "Ptr")) {
        /* Pointer elements are opaque; the pointee type only matters to the reader. */
        lexer_expect(lexer, "<");
        int depth = 1;
        while (depth) {
          if (lexer->token.kind == TK_EOF) {
            die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "unterminated FixedArray pointer element");
          }
          if (lexer_accept(lexer, "<")) depth++;
          else if (lexer_accept(lexer, ">")) depth--;
          else lexer_next(lexer);
        }
        declaration->fixed_element_is_pointer = 1;
      } else if (!strcmp(element_type, "String") || !strcmp(element_type, "Dynamic")) {
        declaration->fixed_element_is_pointer = 1;
      } else strcpy(declaration->fixed_element_type_name, element_type);
      lexer_expect(lexer, ",");
      /* Length may be a constant integer or a runtime expression. The latter is a
         trailing flexible array member (XSDT-style): LLVM lowers it as [0 x T]. */
      Token length_token = lexer->token;
      Node *length_expr = expression_until_angle_close(lexer);
      if (!length_expr) {
        die_parse(lexer, length_token.start_offset, length_token.end_offset, "FixedArray length must be an integer");
      }
      if (length_expr->kind == N_INT) {
        declaration->fixed_array_length = (unsigned)length_expr->number;
        if ((!declaration->fixed_element_width && !declaration->fixed_element_type_name[0] && !declaration->fixed_element_is_pointer) ||
            !declaration->fixed_array_length || declaration->fixed_element_width > 65535) {
          die_parse(lexer, length_expr->source_offset, length_expr->source_end_offset, "FixedArray dimensions must be positive");
        }
      } else {
        if ((!declaration->fixed_element_width && !declaration->fixed_element_type_name[0] && !declaration->fixed_element_is_pointer) ||
            declaration->fixed_element_width > 65535) {
          die_parse(lexer, length_expr->source_offset, length_expr->source_end_offset, "FixedArray dimensions must be positive");
        }
        declaration->is_flexible_array = 1;
        declaration->fixed_array_length_expr = length_expr;
      }
      lexer_expect(lexer, ">");
      return;
    }
    if (declaration && (!strcmp(type_name, "IntSize") || !strcmp(type_name, "UIntSize") || !strcmp(type_name, "IntType") || !strcmp(type_name, "UIntType"))) {
      if (lexer->token.kind != TK_NUM) {
        fprintf(stderr, "haxellvm: %s requires an integer width\n", type_name);
        exit(1);
      }
      unsigned long width = strtoul(lexer->token.text, NULL, 0);
      if (!width || width > 65535) {
        fprintf(stderr, "haxellvm: integer width must be between 1 and 65535\n");
        exit(1);
      }
      declaration->integer_width = (unsigned)width;
      declaration->integer_unsigned = !strcmp(type_name, "UIntSize") || !strcmp(type_name, "UIntType");
      lexer_next(lexer);
      lexer_expect(lexer, ">");
      return;
    }
    int depth = 1;
    while (depth) {
      if (lexer_accept(lexer, "<")) depth++;
      else if (lexer_accept(lexer, ">")) depth--;
      else lexer_next(lexer);
    }
  } else if (declaration && (!strcmp(type_name, "Int") || !strcmp(type_name, "UInt"))) {
    declaration->integer_unsigned = !strcmp(type_name, "UInt");
  }
}

static void cast_type(Lexer *lexer, Node *cast) {
  char type_name[256];
  qualified_name(lexer, type_name);
  strcpy(cast->type_name, type_name);
  if (!strcmp(type_name, "Int") || !strcmp(type_name, "UInt")) {
    cast->integer_unsigned = !strcmp(type_name, "UInt");
    if (lexer_accept(lexer, "<")) {
      fprintf(stderr, "haxellvm: Int/UInt do not take a width; use IntSize/UIntSize\n");
      exit(1);
    }
    return;
  }
  if (!lexer_accept(lexer, "<")) return;
  if (!strcmp(type_name, "IntSize") || !strcmp(type_name, "UIntSize") || !strcmp(type_name, "IntType") || !strcmp(type_name, "UIntType")) {
    if (lexer->token.kind != TK_NUM) {
      fprintf(stderr, "haxellvm: cast integer width must be an integer\n");
      exit(1);
    }
    cast->integer_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
    cast->integer_unsigned = !strcmp(type_name, "UIntSize") || !strcmp(type_name, "UIntType");
    if (!cast->integer_width || cast->integer_width > 65535) {
      fprintf(stderr, "haxellvm: cast integer width must be between 1 and 65535\n");
      exit(1);
    }
    lexer_next(lexer);
    lexer_expect(lexer, ">");
    return;
  }
  if (!strcmp(type_name, "Ptr")) {
    char element_type[256];
    qualified_name(lexer, element_type);
    strcpy(cast->pointer_type_name, element_type);
    if (!strcmp(element_type, "IntSize") || !strcmp(element_type, "UIntSize") || !strcmp(element_type, "IntType") || !strcmp(element_type, "UIntType")) {
      lexer_expect(lexer, "<");
      if (lexer->token.kind != TK_NUM) {
        fprintf(stderr, "haxellvm: pointer cast element width must be an integer\n");
        exit(1);
      }
      cast->pointer_element_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
      cast->pointer_element_unsigned = !strcmp(element_type, "UIntSize") || !strcmp(element_type, "UIntType");
      lexer_next(lexer);
      lexer_expect(lexer, ">");
    } else if (!strcmp(element_type, "Int") || !strcmp(element_type, "UInt")) {
      cast->pointer_element_unsigned = !strcmp(element_type, "UInt");
    }
    lexer_expect(lexer, ">");
    return;
  }
  fprintf(stderr, "haxellvm: unsupported cast target '%s'\n", type_name);
  exit(1);
}

static Node *apply_postfix(Lexer *lexer, Node *result) {
  for (;;) {
    if (lexer_accept(lexer, "(")) {
      if (result->kind == N_NAME) {
        result->kind = N_CALL;
        result->a = NULL;
        if (!strcmp(result->text, "sizeof")) {
          /* sizeof(Type) — type args like UIntSize<64> must not be parsed as comparisons. */
          if (lexer_accept(lexer, ")")) {
            die_parse(lexer, result->source_offset, lexer->previous_end_offset, "sizeof expects exactly one type");
          }
          Node *type_arg = node_new(N_NAME);
          type_arg->source_offset = lexer->token.start_offset;
          qualified_name(lexer, type_arg->text);
          if (lexer_accept(lexer, "<")) {
            if (!strcmp(type_arg->text, "IntSize") || !strcmp(type_arg->text, "UIntSize") ||
                !strcmp(type_arg->text, "IntType") || !strcmp(type_arg->text, "UIntType")) {
              if (lexer->token.kind != TK_NUM) {
                die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "sizeof integer width must be an integer");
              }
              type_arg->integer_width = (unsigned)strtoul(lexer->token.text, NULL, 0);
              type_arg->integer_unsigned = !strcmp(type_arg->text, "UIntSize") || !strcmp(type_arg->text, "UIntType");
              if (!type_arg->integer_width || type_arg->integer_width > 65535) {
                die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "integer width must be between 1 and 65535");
              }
              lexer_next(lexer);
              lexer_expect(lexer, ">");
            } else if (!strcmp(type_arg->text, "Ptr")) {
              char pointee[256];
              qualified_name(lexer, pointee);
              strcpy(type_arg->pointer_type_name, pointee);
              if (!strcmp(pointee, "IntSize") || !strcmp(pointee, "UIntSize") || !strcmp(pointee, "IntType") || !strcmp(pointee, "UIntType")) {
                lexer_expect(lexer, "<");
                if (lexer->token.kind != TK_NUM) {
                  die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "sizeof pointer element width must be an integer");
                }
                lexer_next(lexer);
                lexer_expect(lexer, ">");
              } else if (lexer_accept(lexer, "<")) {
                int depth = 1;
                while (depth) {
                  if (lexer->token.kind == TK_EOF) {
                    die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "unterminated sizeof pointer type");
                  }
                  if (lexer_accept(lexer, "<")) depth++;
                  else if (lexer_accept(lexer, ">")) depth--;
                  else lexer_next(lexer);
                }
              }
              lexer_expect(lexer, ">");
            } else {
              die_parse(lexer, type_arg->source_offset, lexer->token.end_offset, "sizeof expects a typedef or sized type");
            }
          }
          type_arg->source_end_offset = lexer->previous_end_offset;
          result->a = type_arg;
          lexer_expect(lexer, ")");
          result->source_end_offset = lexer->previous_end_offset;
          continue;
        }
        if (!lexer_accept(lexer, ")")) {
          Node **tail = &result->a;
          do {
            *tail = expression(lexer, 0);
            tail = &(*tail)->next;
          } while (lexer_accept(lexer, ","));
          lexer_expect(lexer, ")");
        }
        result->source_end_offset = lexer->previous_end_offset;
        continue;
      }
      if (result->kind == N_FIELD) {
        Node *call = node_new(N_CALL);
        strcpy(call->text, result->text);
        call->source_offset = result->source_offset;
        call->source_end_offset = result->source_end_offset;
        call->b = result->a;
        if (!lexer_accept(lexer, ")")) {
          Node **tail = &call->a;
          do {
            *tail = expression(lexer, 0);
            tail = &(*tail)->next;
          } while (lexer_accept(lexer, ","));
          lexer_expect(lexer, ")");
        }
        call->source_end_offset = lexer->previous_end_offset;
        result = call;
        continue;
      }
      fprintf(stderr, "haxellvm: cannot call this expression\n");
      exit(1);
    }
    if (lexer_accept(lexer, "[")) {
      Node *index = node_new(N_INDEX);
      index->source_offset = result->source_offset;
      index->a = result;
      index->b = expression(lexer, 0);
      lexer_expect(lexer, "]");
      index->source_end_offset = lexer->previous_end_offset;
      result = index;
      continue;
    }
    int safe_field = 0;
    if (lexer_accept(lexer, "?.")) safe_field = 1;
    else if (!lexer_accept(lexer, ".")) {
      break;
    }
    if (!safe_field && result->kind == N_STR) {
      if (!lexer_accept(lexer, "code")) {
        fprintf(stderr, "haxellvm: string literals only support the '.code' field\n");
        exit(1);
      }
      if (!result->text[0]) {
        fprintf(stderr, "haxellvm: '.code' requires a non-empty string literal\n");
        exit(1);
      }
      result->kind = N_INT;
      result->number = (unsigned char)result->text[0];
      result->value = (int)result->number;
      continue;
    }
    Node *field = node_new(N_FIELD);
    field->source_offset = result->source_offset;
    field->a = result;
    field->is_safe_field = safe_field;
    lexer_field_name(lexer, field->text);
    field->source_end_offset = lexer->previous_end_offset;
    result = field;
    continue;
  }
  return result;
}

static Node *unary(Lexer *lexer) {
  Node *result;
  if (lexer->token.kind == TK_SYM && (!strcmp(lexer->token.text, "-") || !strcmp(lexer->token.text, "!") || !strcmp(lexer->token.text, "~") || !strcmp(lexer->token.text, "&") || !strcmp(lexer->token.text, "*"))) {
    result = node_new(N_UNARY);
    result->source_offset = lexer->token.start_offset;
    strcpy(result->text, lexer->token.text);
    lexer_next(lexer);
    result->a = unary(lexer);
    result->source_end_offset = result->a->source_end_offset;
    return result;
  }
  if (lexer_accept(lexer, "(")) {
    result = expression(lexer, 0);
    lexer_expect(lexer, ")");
    return apply_postfix(lexer, result);
  }
  if (lexer->token.kind == TK_NUM) {
    result = node_new(N_INT);
    result->source_offset = lexer->token.start_offset;
    char literal[256];
    strcpy(literal, lexer->token.text);
    strip_numeric_suffix(literal);
    result->number = strtoull(literal, NULL, 0);
    result->value = (int)result->number;
    lexer_next(lexer);
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (lexer->token.kind == TK_FLOAT) {
    result = node_new(N_FLOAT);
    result->source_offset = lexer->token.start_offset;
    strcpy(result->text, lexer->token.text);
    {
      /* Capture f/f32 vs f64 before stripping; default Float is f64 (SSE2). */
      size_t length = strlen(result->text);
      size_t index = length;
      while (index > 0 && result->text[index - 1] >= '0' && result->text[index - 1] <= '9') index--;
      if (index > 0 && (result->text[index - 1] == 'f' || result->text[index - 1] == 'F')) {
        unsigned digits = (unsigned)(length - index);
        if (digits == 0 || digits == 32) result->integer_width = 32;
        else if (digits == 64) result->integer_width = 64;
        else result->integer_width = 64;
      } else result->integer_width = 64;
    }
    strip_numeric_suffix(result->text);
    lexer_next(lexer);
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (lexer_accept(lexer, "[")) {
    result = node_new(N_ARRAY);
    Node **tail = &result->a;
    if (!lexer_accept(lexer, "]")) {
      for (;;) {
        *tail = expression(lexer, 0);
        tail = &(*tail)->next;
        if (!lexer_accept(lexer, ",")) break;
        if (lexer_accept(lexer, "]")) return apply_postfix(lexer, result);
      }
      lexer_expect(lexer, "]");
    }
    return apply_postfix(lexer, result);
  }
  if (!strcmp(lexer->token.text, "{")) {
    size_t source_offset = lexer->token.start_offset;
    /* Haxe block expressions: { stmt; ...; value }. Records look like { field: value }. */
    Lexer lookahead = *lexer;
    lexer_next(&lookahead);
    int is_record = 0;
    if (lexer_accept(&lookahead, "}")) is_record = 1;
    else if (lookahead.token.kind == TK_ID) {
      lexer_next(&lookahead);
      if (!strcmp(lookahead.token.text, ":")) is_record = 1;
    }
    if (!is_record) {
      Node *block_expr = node_new(N_BLOCK);
      block_expr->is_block_expression = 1;
      block_expr->source_offset = source_offset;
      Node **tail = &block_expr->a;
      lexer_next(lexer); /* consume '{' */
      while (!lexer_accept(lexer, "}")) {
        *tail = statement_ex(lexer, 1);
        tail = &(*tail)->next;
      }
      block_expr->source_end_offset = lexer->previous_end_offset;
      return apply_postfix(lexer, block_expr);
    }
    lexer_next(lexer);
    result = node_new(N_RECORD);
    result->source_offset = source_offset;
    Node **fields = &result->a;
    if (!lexer_accept(lexer, "}")) {
      do {
        Node *field = node_new(N_FIELD);
        lexer_name(lexer, field->text);
        lexer_expect(lexer, ":");
        field->a = expression(lexer, 0);
        *fields = field;
        fields = &field->next;
      } while (lexer_accept(lexer, ","));
      lexer_expect(lexer, "}");
    }
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (lexer->token.kind == TK_STR) {
    result = node_new(N_STR);
    result->source_offset = lexer->token.start_offset;
    strcpy(result->text, lexer->token.text);
    lexer_next(lexer);
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (lexer_accept(lexer, "function")) {
    result = node_new(N_FUNCTION);
    if (lexer->token.kind == TK_ID) lexer_name(lexer, result->text);
    lexer_expect(lexer, "(");
    if (!lexer_accept(lexer, ")")) {
      if (!result->text[0]) lexer_name(lexer, result->text);
      optional_type(lexer, NULL);
      if (lexer_accept(lexer, ",")) {
        fprintf(stderr, "haxellvm: function values currently support one parameter\n");
        exit(1);
      }
      lexer_expect(lexer, ")");
    }
    optional_type(lexer, NULL);
    result->a = block(lexer);
    return result;
  }
  if (lexer_accept(lexer, "untyped") || lexer_accept(lexer, "inline")) {
    return unary(lexer);
  }
  if (lexer_accept(lexer, "macro")) {
    /* Reification: `macro :Void` or `macro asm("...")`. Types become string names. */
    if (lexer_accept(lexer, ":")) {
      Node *type_name = node_new(N_STR);
      qualified_name(lexer, type_name->text);
      if (lexer_accept(lexer, "<")) {
        int depth = 1;
        while (depth) {
          if (lexer_accept(lexer, "<")) depth++;
          else if (lexer_accept(lexer, ">")) depth--;
          else lexer_next(lexer);
        }
      }
      return type_name;
    }
    return unary(lexer);
  }
  if (lexer_accept(lexer, "throw")) {
    result = node_new(N_THROW);
    result->source_offset = lexer->previous_end_offset;
    result->a = expression(lexer, 0);
    result->source_end_offset = result->a->source_end_offset;
    return result;
  }
  if (lexer_accept(lexer, "try")) {
    result = node_new(N_TRY);
    result->source_offset = lexer->previous_end_offset;
    result->a = statement_or_block(lexer);
    Node **catches = &result->b;
    while (lexer_accept(lexer, "catch")) {
      Node *catch_clause = node_new(N_CASE);
      lexer_expect(lexer, "(");
      lexer_name(lexer, catch_clause->text);
      optional_type(lexer, catch_clause);
      lexer_expect(lexer, ")");
      catch_clause->a = statement_or_block(lexer);
      *catches = catch_clause;
      catches = &catch_clause->next;
    }
    if (!result->b) die_parse(lexer, result->source_offset, lexer->token.end_offset, "try requires at least one catch");
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (!strcmp(lexer->token.text, "cast")) {
    result = node_new(N_CAST);
    result->source_offset = lexer->token.start_offset;
    lexer_next(lexer);
    lexer_expect(lexer, "(");
    result->a = expression(lexer, 0);
    lexer_expect(lexer, ",");
    cast_type(lexer, result);
    lexer_expect(lexer, ")");
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (!strcmp(lexer->token.text, "new")) {
    result = node_new(N_NEW);
    result->source_offset = lexer->token.start_offset;
    lexer_next(lexer);
    if (lexer->token.kind != TK_ID) {
      fprintf(stderr, "haxellvm: expected type name after new\n");
      exit(1);
    }
    lexer_name(lexer, result->text);
    lexer_expect(lexer, "(");
    if (!lexer_accept(lexer, ")")) {
      Node **tail = &result->a;
      do {
        *tail = expression(lexer, 0);
        tail = &(*tail)->next;
      } while (lexer_accept(lexer, ","));
      lexer_expect(lexer, ")");
    }
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (!strcmp(lexer->token.text, "switch")) {
    return apply_postfix(lexer, parse_switch(lexer));
  }
  if (lexer->token.kind != TK_ID && lexer->token.kind != TK_KWD) {
    fprintf(stderr, "haxellvm: expected expression, got '%s'\n", lexer->token.text);
    exit(1);
  }
  if (!strcmp(lexer->token.text, "null")) {
    result = node_new(N_NULL);
    result->source_offset = lexer->token.start_offset;
    strcpy(result->text, "null");
    lexer_next(lexer);
    result->source_end_offset = lexer->previous_end_offset;
    return apply_postfix(lexer, result);
  }
  if (!strcmp(lexer->token.text, "true") || !strcmp(lexer->token.text, "false")) {
    result = node_new(N_BOOL);
    result->source_offset = lexer->token.start_offset;
    result->value = lexer->token.text[0] == 't';
    strcpy(result->text, lexer->token.text);
    lexer_next(lexer);
    result->source_end_offset = lexer->previous_end_offset;
    return result;
  }
  if (lexer->token.kind == TK_KWD && strcmp(lexer->token.text, "this") && strcmp(lexer->token.text, "super")) {
    fprintf(stderr, "haxellvm: unexpected keyword '%s' in expression\n", lexer->token.text);
    exit(1);
  }
  result = node_new(N_NAME);
  result->source_offset = lexer->token.start_offset;
  qualified_name(lexer, result->text);
  result->source_end_offset = lexer->previous_end_offset;
  return apply_postfix(lexer, result);
}

static int precedence(const char *operator) {
  if (!strcmp(operator, "??")) return 1;
  if (!strcmp(operator, "||")) return 2;
  if (!strcmp(operator, "&&")) return 3;
  if (!strcmp(operator, "|")) return 4;
  if (!strcmp(operator, "^")) return 5;
  if (!strcmp(operator, "&")) return 6;
  if (!strcmp(operator, "==") || !strcmp(operator, "!=")) return 7;
  if (!strcmp(operator, "<") || !strcmp(operator, "<=") || !strcmp(operator, ">") || !strcmp(operator, ">=")) return 8;
  if (!strcmp(operator, "<<") || !strcmp(operator, ">>") || !strcmp(operator, ">>>")) return 9;
  if (!strcmp(operator, "+") || !strcmp(operator, "-")) return 10;
  if (!strcmp(operator, "*") || !strcmp(operator, "/") || !strcmp(operator, "%")) return 11;
  if (!strcmp(operator, "...")) return 12;
  return -1;
}

static Node *expression(Lexer *lexer, int minimum_precedence) {
  Node *left = unary(lexer);
  for (;;) {
    /* Soft keyword `is` (Haxe Ident "is", not reserved). */
    if (minimum_precedence <= 8 && lexer->token.kind == TK_ID && !strcmp(lexer->token.text, "is")) {
      Node *result = node_new(N_IS);
      result->source_offset = left->source_offset;
      lexer_next(lexer);
      result->a = left;
      result->b = node_new(N_TYPE);
      result->b->source_offset = lexer->token.start_offset;
      qualified_name(lexer, result->b->text);
      result->b->source_end_offset = lexer->previous_end_offset;
      result->source_end_offset = result->b->source_end_offset;
      left = result;
      continue;
    }
    if (precedence(lexer->token.text) < minimum_precedence) break;
    Node *result = node_new(N_BINARY);
    result->source_offset = left->source_offset;
    int priority = precedence(lexer->token.text);
    strcpy(result->text, lexer->token.text);
    lexer_next(lexer);
    result->a = left;
    result->b = expression(lexer, priority + 1);
    result->source_end_offset = result->b->source_end_offset;
    left = result;
  }
  /* Ternary binds looser than binaries (Haxe expr_next Question/DblDot). */
  if (minimum_precedence <= 0 && lexer_accept(lexer, "?")) {
    Node *result = node_new(N_TERNARY);
    result->source_offset = left->source_offset;
    result->a = left;
    result->b = expression(lexer, 0);
    lexer_expect(lexer, ":");
    result->c = expression(lexer, 0);
    result->source_end_offset = result->c->source_end_offset;
    return result;
  }
  return left;
}

/* Type-argument expressions must not consume the closing '>' as a comparison. */
static Node *expression_until_angle_close_min(Lexer *lexer, int minimum_precedence) {
  Node *left = unary(lexer);
  while (precedence(lexer->token.text) >= minimum_precedence) {
    if (!strcmp(lexer->token.text, ">") || !strcmp(lexer->token.text, ">>")) break;
    Node *result = node_new(N_BINARY);
    result->source_offset = left->source_offset;
    int priority = precedence(lexer->token.text);
    strcpy(result->text, lexer->token.text);
    lexer_next(lexer);
    result->a = left;
    result->b = expression_until_angle_close_min(lexer, priority + 1);
    result->source_end_offset = result->b->source_end_offset;
    left = result;
  }
  return left;
}

static Node *expression_until_angle_close(Lexer *lexer) {
  return expression_until_angle_close_min(lexer, 0);
}

static Node *statement(Lexer *lexer) {
  return statement_ex(lexer, 0);
}

static Node *parse_switch(Lexer *lexer) {
  Node *result = node_new(N_SWITCH);
  result->source_offset = lexer->token.start_offset;
  lexer_expect(lexer, "switch");
  lexer_expect(lexer, "(");
  result->a = expression(lexer, 0);
  lexer_expect(lexer, ")");
  lexer_expect(lexer, "{");
  Node **cases = &result->b;
  while (!lexer_accept(lexer, "}")) {
    Node *switch_case = node_new(N_CASE);
    if (lexer_accept(lexer, "case")) switch_case->a = expression(lexer, 0);
    else if (!lexer_accept(lexer, "default")) {
      fprintf(stderr, "haxellvm: expected case or default in switch, got '%s'\n", lexer->token.text);
      exit(1);
    }
    lexer_expect(lexer, ":");
    switch_case->b = node_new(N_BLOCK);
    Node **body = &switch_case->b->a;
    while (strcmp(lexer->token.text, "case") && strcmp(lexer->token.text, "default") && strcmp(lexer->token.text, "}")) {
      *body = statement(lexer);
      body = &(*body)->next;
    }
    *cases = switch_case;
    cases = &switch_case->next;
  }
  result->source_end_offset = lexer->previous_end_offset;
  return result;
}

static Node *statement_ex(Lexer *lexer, int allow_omit_semi_before_rbrace) {
  Node *result = node_new(N_EXPR);
  if (!strcmp(lexer->token.text, "++") || !strcmp(lexer->token.text, "--")) {
    result->kind = N_ASSIGN;
    strcpy(result->assignment_operator, !strcmp(lexer->token.text, "++") ? "+=" : "-=");
    lexer_next(lexer);
    lexer_name(lexer, result->text);
    result->a = node_new(N_INT);
    result->a->value = 1;
    result->a->number = 1;
    lexer_expect(lexer, ";");
    return result;
  }
  unsigned statement_alignment = 0;
  while (lexer_accept(lexer, "@")) {
    lexer_expect(lexer, ":");
    Token metadata_token = lexer->token;
    char metadata[256];
    lexer_field_name(lexer, metadata);
    if (!apply_aligned_meta(lexer, metadata, &statement_alignment)) unsupported_metadata(lexer, metadata_token);
  }
  size_t decl_start = lexer->token.start_offset;
  int is_static = lexer_accept(lexer, "static");
  int is_final = lexer_accept(lexer, "final");
  if (lexer_accept(lexer, "var") || (is_final && lexer->token.kind == TK_ID && strcmp(lexer->token.text, "function"))) {
    result->kind = N_VAR;
    result->is_static = is_static;
    result->is_final = is_final;
    result->is_mutable = !is_final;
    result->alignment = statement_alignment;
    result->source_offset = decl_start;
    lexer_name(lexer, result->text);
    optional_type(lexer, result);
    if (lexer_accept(lexer, "=")) result->a = expression(lexer, 0);
    else if (!result->fixed_array_length && !result->is_flexible_array && !result->type_name[0]) {
      fprintf(stderr, "haxellvm: local variable without initializer requires a type\n");
      exit(1);
    }
    if (result->is_flexible_array) {
      Node *length = result->fixed_array_length_expr;
      die_parse(lexer,
                length ? length->source_offset : lexer->token.start_offset,
                length ? length->source_end_offset : lexer->token.end_offset,
                "flexible FixedArray is only supported as a trailing typedef field");
    }
    if (is_final && !result->a) {
      fprintf(stderr, "haxellvm: final local variable requires an initializer\n");
      exit(1);
    }
    lexer_expect(lexer, ";");
    result->source_end_offset = lexer->previous_end_offset;
    return result;
  }
  if (statement_alignment) {
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, "@:aligned only applies to variable declarations");
    exit(1);
  }
  if (is_final) {
    fprintf(stderr, "haxellvm: final is only supported on variable declarations here\n");
    exit(1);
  }
  if (is_static) {
    fprintf(stderr, "haxellvm: static is only supported on local var declarations\n");
    exit(1);
  }
  if (lexer_accept(lexer, "if")) {
    result->kind = N_IF;
    lexer_expect(lexer, "(");
    result->a = expression(lexer, 0);
    lexer_expect(lexer, ")");
    result->b = statement_or_block(lexer);
    result->c = lexer_accept(lexer, "else") ? statement_or_block(lexer) : node_new(N_BLOCK);
    return result;
  }
  if (lexer_accept(lexer, "while")) {
    result->kind = N_WHILE;
    lexer_expect(lexer, "(");
    result->a = expression(lexer, 0);
    lexer_expect(lexer, ")");
    result->b = statement_or_block(lexer);
    return result;
  }
  if (lexer_accept(lexer, "do")) {
    result->kind = N_DO_WHILE;
    result->b = statement_or_block(lexer);
    lexer_expect(lexer, "while");
    lexer_expect(lexer, "(");
    result->a = expression(lexer, 0);
    lexer_expect(lexer, ")");
    lexer_expect(lexer, ";");
    return result;
  }
  if (lexer_accept(lexer, "for")) {
    result->kind = N_FOR;
    lexer_expect(lexer, "(");
    lexer_name(lexer, result->text);
    lexer_expect(lexer, "in");
    /* Haxe: for (x in expr). Range loops use the `...` binop (OpInterval). */
    Node *iterator = expression(lexer, 0);
    if (iterator->kind == N_BINARY && !strcmp(iterator->text, "...")) {
      result->a = iterator->a;
      result->b = iterator->b;
    } else {
      die_parse(lexer, iterator->source_offset, iterator->source_end_offset,
                "for-in currently requires a range expression (a...b); use while for other iterators");
    }
    lexer_expect(lexer, ")");
    result->c = statement_or_block(lexer);
    return result;
  }
  if (!strcmp(lexer->token.text, "switch")) {
    free(result);
    return parse_switch(lexer);
  }
  if (lexer_accept(lexer, "return")) {
    result->kind = N_RETURN;
    if (!lexer_accept(lexer, ";")) {
      result->a = expression(lexer, 0);
      lexer_expect(lexer, ";");
    }
    return result;
  }
  if (lexer_accept(lexer, "break")) {
    result->kind = N_BREAK;
    lexer_expect(lexer, ";");
    return result;
  }
  if (lexer_accept(lexer, "continue")) {
    result->kind = N_CONTINUE;
    lexer_expect(lexer, ";");
    return result;
  }
  if (lexer_accept(lexer, "throw")) {
    result->kind = N_THROW;
    result->source_offset = lexer->previous_end_offset;
    result->a = expression(lexer, 0);
    result->source_end_offset = result->a->source_end_offset;
    lexer_expect(lexer, ";");
    return result;
  }
  if (lexer_accept(lexer, "try")) {
    result->kind = N_TRY;
    result->source_offset = lexer->previous_end_offset;
    result->a = statement_or_block(lexer);
    Node **catches = &result->b;
    while (lexer_accept(lexer, "catch")) {
      Node *catch_clause = node_new(N_CASE);
      lexer_expect(lexer, "(");
      lexer_name(lexer, catch_clause->text);
      optional_type(lexer, catch_clause);
      lexer_expect(lexer, ")");
      catch_clause->a = statement_or_block(lexer);
      *catches = catch_clause;
      catches = &catch_clause->next;
    }
    if (!result->b) die_parse(lexer, result->source_offset, lexer->token.end_offset, "try requires at least one catch");
    result->source_end_offset = lexer->previous_end_offset;
    return result;
  }
  result->a = expression(lexer, 0);
  if (!strcmp(lexer->token.text, "++") || !strcmp(lexer->token.text, "--")) {
    if (result->a->kind != N_NAME && result->a->kind != N_INDEX && result->a->kind != N_FIELD && (result->a->kind != N_UNARY || strcmp(result->a->text, "*"))) {
      die_parse(lexer, result->a->source_offset, lexer->token.end_offset, "increment and decrement require an assignable target");
    }
    result->kind = N_ASSIGN;
    result->source_offset = result->a->source_offset;
    strcpy(result->assignment_operator, !strcmp(lexer->token.text, "++") ? "+=" : "-=");
    if (result->a->kind == N_NAME) strcpy(result->text, result->a->text);
    else result->b = result->a;
    result->a = node_new(N_INT);
    result->a->value = 1;
    result->a->number = 1;
    lexer_next(lexer);
    result->source_end_offset = lexer->previous_end_offset;
    lexer_expect(lexer, ";");
    return result;
  }
  if (!strcmp(lexer->token.text, "=") || !strcmp(lexer->token.text, "+=") || !strcmp(lexer->token.text, "-=") || !strcmp(lexer->token.text, "*=") || !strcmp(lexer->token.text, "/=") || !strcmp(lexer->token.text, "%=") || !strcmp(lexer->token.text, "&=") || !strcmp(lexer->token.text, "|=") || !strcmp(lexer->token.text, "^=") || !strcmp(lexer->token.text, "<<=") || !strcmp(lexer->token.text, ">>=") || !strcmp(lexer->token.text, ">>>=")) {
    if (result->a->kind != N_NAME && result->a->kind != N_INDEX && result->a->kind != N_FIELD && (result->a->kind != N_UNARY || strcmp(result->a->text, "*"))) {
      die_parse(lexer, result->a->source_offset, lexer->token.end_offset, "assignment target is not assignable");
    }
    result->kind = N_ASSIGN;
    result->source_offset = result->a->source_offset;
    result->source_end_offset = result->a->source_end_offset;
    strcpy(result->assignment_operator, lexer->token.text);
    lexer_next(lexer);
    if (result->a->kind == N_NAME) strcpy(result->text, result->a->text);
    else result->b = result->a;
    result->a = expression(lexer, 0);
    lexer_expect(lexer, ";");
    return result;
  }
  if (allow_omit_semi_before_rbrace && !strcmp(lexer->token.text, "}")) return result;
  lexer_expect(lexer, ";");
  return result;
}

static Node *block(Lexer *lexer) {
  Node *result = node_new(N_BLOCK);
  Node **tail = &result->a;
  lexer_expect(lexer, "{");
  while (!lexer_accept(lexer, "}")) {
    *tail = statement(lexer);
    tail = &(*tail)->next;
  }
  return result;
}

static Node *statement_or_block(Lexer *lexer) {
  return !strcmp(lexer->token.text, "{") ? block(lexer) : statement(lexer);
}


static int class_body_keyword(const char *text) {
  return !strcmp(text, "static") || !strcmp(text, "public") || !strcmp(text, "private") ||
         !strcmp(text, "var") || !strcmp(text, "final") || !strcmp(text, "function") || !strcmp(text, "typedef") ||
         !strcmp(text, "abstract") || !strcmp(text, "override") || !strcmp(text, "macro");
}

static int class_is_oop(Node *class_node) {
  if (class_node->build_macro[0]) return 1;
  if (class_node->is_abstract_class || class_node->is_interface || class_node->parent_name[0] || class_node->c) return 1;
  for (Node *method = class_node->b; method; method = method->next) {
    if (method->is_abstract || method->is_override || method->is_constructor || !strcmp(method->text, "new")) return 1;
  }
  return 0;
}

static void prepend_this_parameter(Node *method, const char *class_name) {
  Node *this_parameter = node_new(N_FIELD);
  strcpy(this_parameter->text, "this");
  strcpy(this_parameter->type_name, class_name);
  this_parameter->next = method->b;
  method->b = this_parameter;
}

static void finish_oop_class(Node ***functions, Node *class_node) {
  for (Node *method = class_node->b; method; method = method->next) {
    strcpy(method->class_name, class_node->text);
    if (method->is_abstract) continue;
    Node *emitted = node_new(N_FUNCTION);
    memcpy(emitted, method, sizeof(*emitted));
    emitted->next = NULL;
    const char *symbol_class = method->external_class_name[0] ? method->external_class_name : class_node->text;
    if (!strcmp(method->text, "new")) snprintf(emitted->text, sizeof(emitted->text), "%s_new", symbol_class);
    else if (method->retain_function_name) strcpy(emitted->text, method->text);
    else snprintf(emitted->text, sizeof(emitted->text), "%s_%s", symbol_class, method->text);
    if (!method->retain_function_name) prepend_this_parameter(emitted, class_node->text);
    **functions = emitted;
    *functions = &emitted->next;
  }
}

static void flatten_non_oop_class(Node ***functions, Node ***globals, Node *class_node) {
  for (Node *field = class_node->a; field; field = field->next) {
    Node *global = node_new(N_GLOBAL);
    memcpy(global, field, sizeof(*global));
    global->kind = N_GLOBAL;
    global->next = NULL;
    global->is_static = 1;
    global->is_mutable = 1;
    **globals = global;
    *globals = &global->next;
  }
  class_node->a = NULL;
  if (class_node->b) {
    Node *methods = class_node->b;
    class_node->b = NULL;
    **functions = methods;
    while (**functions) *functions = &(**functions)->next;
  }
}


static void unsupported_metadata(Lexer *lexer, Token token) {
  char message[320];
  snprintf(message, sizeof(message), "unsupported metadata '%s'", token.text);
  haxellvm_diagnostic_error(lexer->diagnostic, token.start_offset, token.end_offset, message);
  exit(1);
}

static Node *record_typedef(Lexer *lexer) {
  Lexer lookahead = *lexer;
  while (lexer_accept(&lookahead, "@")) {
    if (!lexer_accept(&lookahead, ":") || (lookahead.token.kind != TK_ID && lookahead.token.kind != TK_KWD)) return NULL;
    lexer_next(&lookahead);
  }
  if (strcmp(lookahead.token.text, "typedef")) return NULL;
  Node *declaration = node_new(N_TYPEDEF);
  while (lexer_accept(lexer, "@")) {
    lexer_expect(lexer, ":");
    Token metadata = lexer->token;
    if (strcmp(metadata.text, "packed")) unsupported_metadata(lexer, metadata);
    declaration->is_packed = 1;
    lexer_next(lexer);
  }
  lexer_expect(lexer, "typedef");
  lexer_name(lexer, declaration->text);
  lexer_expect(lexer, "=");
  lexer_expect(lexer, "{");
  Node **fields = &declaration->a;
  while (!lexer_accept(lexer, "}")) {
    Node *field = node_new(N_FIELD);
    field->source_offset = lexer->token.start_offset;
    /* Haxe allows both `var name: T;` and anonymous-style `name: T,`. */
    lexer_accept(lexer, "var");
    if (lexer->token.kind != TK_ID) {
      die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "expected typedef field name");
    }
    lexer_name(lexer, field->text);
    field->source_end_offset = lexer->previous_end_offset;
    optional_type(lexer, field);
    if (!field->type_name[0] && !field->integer_width && !field->pointer_element_width && !field->pointer_type_name[0] &&
        !field->fixed_array_length && !field->is_dynamic_array && !field->is_function_pointer) {
      die_parse(lexer, field->source_offset, field->source_end_offset, "typedef field requires a type");
    }
    *fields = field;
    fields = &field->next;
    if (!lexer_accept(lexer, ";") && !lexer_accept(lexer, ",") && strcmp(lexer->token.text, "}")) {
      die_parse(lexer, lexer->token.start_offset, lexer->token.end_offset, "typedef fields must be separated by ',' or ';'");
    }
  }
  for (Node *field = declaration->a; field; field = field->next) {
    if (field->is_flexible_array && field->next) {
      die_parse(lexer, field->source_offset ? field->source_offset : lexer->token.start_offset,
                field->source_end_offset ? field->source_end_offset : lexer->token.end_offset,
                "flexible FixedArray must be the last field of a typedef");
    }
  }
  lexer_expect(lexer, ";");
  return declaration;
}

static Node *parse_haxe_from_root(const char *source, const char *source_path, const char *import_root) {
  Lexer lexer;
  lexer_init(&lexer, source, source_path);
  Node *imports = NULL;
  Node **import_tail = &imports;

  /* Optional package (Haxe parse_file); path resolution stays file-based. */
  if (lexer_accept(&lexer, "package")) {
    if (strcmp(lexer.token.text, ";")) {
      char package_name[256];
      qualified_name(&lexer, package_name);
      (void)package_name;
    }
    lexer_expect(&lexer, ";");
  }

  while (!strcmp(lexer.token.text, "import") || !strcmp(lexer.token.text, "using")) {
    int is_using = !strcmp(lexer.token.text, "using");
    size_t import_start = lexer.token.start_offset;
    lexer_next(&lexer);
    Node *imported_module = node_new(is_using ? N_USING : N_NAME);
    imported_module->source = source;
    imported_module->source_path = source_path;
    imported_module->source_offset = import_start;
    import_name(&lexer, imported_module->text);
    lexer_expect(&lexer, ";");
    imported_module->source_end_offset = lexer.previous_end_offset;
    /* using Foo also pulls Foo.hx like import, then marks it for extension lookup. */
    *import_tail = imported_module;
    import_tail = &imported_module->next;
  }
  Node *program = node_new(N_BLOCK);
  program->source = source;
  program->source_path = source_path;
  Node **globals = &program->b;
  Node **functions = &program->c;
  Node **initializers = &program->d;
  Node **saved_anonymous_type_tail = anonymous_type_tail;
  anonymous_type_tail = &program->next;
  char active_class_name[256] = "";

  while (lexer.token.kind != TK_EOF) {
    Node *typedef_declaration = record_typedef(&lexer);
    if (typedef_declaration) {
      typedef_declaration->source = source;
      typedef_declaration->source_path = source_path;
      *anonymous_type_tail = typedef_declaration;
      anonymous_type_tail = &typedef_declaration->next;
      continue;
    }

    if (lexer_accept(&lexer, "enum")) {
      Node *enumeration = node_new(N_ENUM);
      enumeration->source = source;
      enumeration->source_path = source_path;
      enumeration->source_offset = lexer.token.start_offset;
      lexer_name(&lexer, enumeration->text);
      lexer_expect(&lexer, "{");
      Node **variants = &enumeration->a;
      unsigned tag = 0;
      while (!lexer_accept(&lexer, "}")) {
        Node *variant = node_new(N_FIELD);
        variant->source_offset = lexer.token.start_offset;
        lexer_name(&lexer, variant->text);
        if (lexer_accept(&lexer, "(")) {
          Node **parameters = &variant->b;
          if (!lexer_accept(&lexer, ")")) {
            do {
              Node *parameter = node_new(N_FIELD);
              parameter->source_offset = lexer.token.start_offset;
              lexer_name(&lexer, parameter->text);
              optional_type(&lexer, parameter);
              if (!parameter->type_name[0] && !parameter->integer_width && !parameter->pointer_type_name[0] && !parameter->pointer_element_width) {
                haxellvm_diagnostic_error(lexer.diagnostic, parameter->source_offset, lexer.token.end_offset, "enum constructor parameter requires a type");
                exit(1);
              }
              parameter->source_end_offset = lexer.previous_end_offset;
              *parameters = parameter;
              parameters = &parameter->next;
            } while (lexer_accept(&lexer, ","));
            lexer_expect(&lexer, ")");
          }
        }
        lexer_expect(&lexer, ";");
        variant->source_end_offset = lexer.previous_end_offset;
        variant->number = tag++;
        *variants = variant;
        variants = &variant->next;
      }
      enumeration->source_end_offset = lexer.previous_end_offset;
      *anonymous_type_tail = enumeration;
      anonymous_type_tail = &enumeration->next;
      continue;
    }

    char pending_build_macro[256] = "";
    {
      Lexer build_lookahead = lexer;
      if (lexer_accept(&build_lookahead, "@") && lexer_accept(&build_lookahead, ":") && !strcmp(build_lookahead.token.text, "build")) {
        lexer_expect(&lexer, "@");
        lexer_expect(&lexer, ":");
        lexer_field_name(&lexer, pending_build_macro); /* consume "build" */
        parse_build_macro_path(&lexer, pending_build_macro);
      }
    }

    int is_abstract_class = 0;
    int is_final_class = 0;
    int is_interface = 0;
    int starting_class = 0;
    Lexer class_lookahead = lexer;
    int final_starts_class = lexer_accept(&class_lookahead, "final") &&
                             (lexer_accept(&class_lookahead, "class") ||
                              (lexer_accept(&class_lookahead, "abstract") && lexer_accept(&class_lookahead, "class")));
    if (final_starts_class) {
      lexer_expect(&lexer, "final");
      is_final_class = 1;
      if (lexer_accept(&lexer, "abstract")) {
        lexer_expect(&lexer, "class");
        is_abstract_class = 1;
      } else {
        lexer_expect(&lexer, "class");
      }
      starting_class = 1;
    } else if (!strcmp(lexer.token.text, "abstract")) {
      Lexer abstract_lookahead = lexer;
      lexer_next(&abstract_lookahead);
      if (!strcmp(abstract_lookahead.token.text, "class")) {
        lexer_expect(&lexer, "abstract");
        lexer_expect(&lexer, "class");
        is_abstract_class = 1;
        starting_class = 1;
      } else {
        die_parse(&lexer, lexer.token.start_offset, lexer.token.end_offset,
                  "Haxe abstracts are not supported; use IntSize/UIntSize/Ptr/FixedArray for systems types");
      }
    } else if (lexer_accept(&lexer, "interface")) {
      is_interface = 1;
      is_abstract_class = 1;
      starting_class = 1;
    } else if (lexer_accept(&lexer, "class")) {
      starting_class = 1;
    }

    if (starting_class) {
      Node *class_node = node_new(is_interface ? N_INTERFACE : N_CLASS);
      class_node->is_abstract_class = is_abstract_class;
      class_node->is_interface = is_interface;
      class_node->is_final = is_final_class;
      class_node->source_offset = lexer.token.start_offset;
      class_node->source_end_offset = lexer.token.end_offset;
      class_node->source = source;
      class_node->source_path = source_path;
      strcpy(class_node->build_macro, pending_build_macro);
      pending_build_macro[0] = '\0';
      lexer_name(&lexer, class_node->text);
      strcpy(active_class_name, class_node->text);
      if (lexer_accept(&lexer, "extends")) lexer_name(&lexer, class_node->parent_name);
      Node **implements_tail = &class_node->c;
      while (lexer_accept(&lexer, "implements")) {
        Node *iface = node_new(N_NAME);
        lexer_name(&lexer, iface->text);
        *implements_tail = iface;
        implements_tail = &iface->next;
      }
      while (lexer_accept(&lexer, "@")) {
        lexer_expect(&lexer, ":");
        Token metadata_token = lexer.token;
        char metadata[256];
        lexer_field_name(&lexer, metadata);
        if (!strcmp(metadata, "build")) parse_build_macro_path(&lexer, class_node->build_macro);
        else unsupported_metadata(&lexer, metadata_token);
      }
      lexer_expect(&lexer, "{");
      Node **class_fields = &class_node->a;
      Node **class_methods = &class_node->b;

      while (!lexer_accept(&lexer, "}")) {
        Node *nested_typedef = record_typedef(&lexer);
        if (nested_typedef) {
          Node *typedef_declaration = nested_typedef;
          *anonymous_type_tail = typedef_declaration;
          anonymous_type_tail = &typedef_declaration->next;
          continue;
        }
        if (lexer.token.kind == TK_ID && !class_body_keyword(lexer.token.text) && strcmp(lexer.token.text, "@")) {
          Node *initializer = statement(&lexer);
          if (initializer->kind != N_ASSIGN || !strchr(initializer->text, '.')) {
            fprintf(stderr, "haxellvm: class body only supports declarations and constant field assignments\n");
            exit(1);
          }
          *initializers = initializer;
          initializers = &initializer->next;
          continue;
        }
        SectionMetas sections = {0};
        int entry_point = 0;
        int is_mutable = 0;
        int is_extern = 0;
        int retain_function_name = 0;
        int is_naked = 0;
        unsigned declaration_alignment = 0;
        char external_class_name[256] = "";
        while (lexer_accept(&lexer, "@")) {
          lexer_expect(&lexer, ":");
          Token metadata_token = lexer.token;
          char metadata[256];
          lexer_field_name(&lexer, metadata);
          if (!strcmp(metadata, "entryPoint")) entry_point = 1;
          else if (!strcmp(metadata, "mutable")) is_mutable = 1;
          else if (!strcmp(metadata, "extern")) is_extern = 1;
          else if (!strcmp(metadata, "naked")) is_naked = 1;
          else if (!strcmp(metadata, "build")) {
            parse_build_macro_path(&lexer, class_node->build_macro);
            continue;
          }
          else if (!strcmp(metadata, "retainFunctionName")) retain_function_name = 1;
          else if (!strcmp(metadata, "fromclass")) {
            lexer_expect(&lexer, "(");
            if (lexer.token.kind != TK_STR) {
              haxellvm_diagnostic_error(lexer.diagnostic, lexer.token.start_offset, lexer.token.end_offset, "@:fromclass expects a class name string");
              exit(1);
            }
            strcpy(external_class_name, lexer.token.text);
            lexer_next(&lexer);
            lexer_expect(&lexer, ")");
          }
          else if (apply_aligned_meta(&lexer, metadata, &declaration_alignment)) {
            /* stored on the following field/global */
          }
          else if (!apply_section_meta(&lexer, metadata, &sections)) {
            unsupported_metadata(&lexer, metadata_token);
          }
        }
        lexer_accept(&lexer, "public");
        lexer_accept(&lexer, "private");
        int declaration_is_static = lexer_accept(&lexer, "static");
        int is_override = 0;
        int is_final = 0;
        for (;;) {
          if (lexer_accept(&lexer, "override")) is_override = 1;
          else if (lexer_accept(&lexer, "final")) is_final = 1;
          else break;
        }
        int is_abstract_method = 0;
        int is_macro = 0;
        if (lexer_accept(&lexer, "abstract")) {
          lexer_expect(&lexer, "function");
          is_abstract_method = 1;
        } else if (lexer_accept(&lexer, "macro")) {
          lexer_expect(&lexer, "function");
          is_macro = 1;
        } else if (lexer_accept(&lexer, "var") || (is_final && lexer.token.kind == TK_ID && strcmp(lexer.token.text, "function"))) {
          if (is_override) {
            fprintf(stderr, "haxellvm: fields cannot be marked override\n");
            exit(1);
          }
          Node *field = node_new(declaration_is_static ? N_GLOBAL : N_FIELD);
          field->source = source;
          field->source_path = source_path;
          field->source_offset = lexer.token.start_offset;
          lexer_name(&lexer, field->text);
          copy_section_metas(field, &sections);
          strcpy(field->class_name, active_class_name);
          field->is_static = 1;
          field->is_final = is_final;
          field->is_mutable = (!is_final) && (is_mutable || !declaration_is_static);
          field->alignment = declaration_alignment;
          optional_type(&lexer, field);
          if (field->is_flexible_array) {
            Node *length = field->fixed_array_length_expr;
            die_parse(&lexer,
                      length ? length->source_offset : lexer.token.start_offset,
                      length ? length->source_end_offset : lexer.token.end_offset,
                      "flexible FixedArray is only supported as a trailing typedef field");
          }
          if (lexer_accept(&lexer, "=")) field->a = expression(&lexer, 0);
          else if (!field->type_name[0]) { fprintf(stderr, "haxellvm: global without initializer requires a type\n"); exit(1); }
          if (is_final && !field->a) {
            fprintf(stderr, "haxellvm: final field requires an initializer\n");
            exit(1);
          }
          lexer_expect(&lexer, ";");
          field->source_end_offset = lexer.previous_end_offset;
          field->is_extern = is_extern;
          /* @:extern instance fields are assembly/C symbols (this.foo → @foo), not object layout. */
          if (declaration_is_static || is_extern) {
            field->kind = N_GLOBAL;
            field->is_static = 1;
            *globals = field;
            globals = &field->next;
          } else {
            field->kind = N_FIELD;
            *class_fields = field;
            class_fields = &field->next;
          }
          continue;
        } else {
          lexer_expect(&lexer, "function");
        }

        char function[256];
        Token function_token = lexer.token;
        parse_function_name(&lexer, function);
        if (retain_function_name && !declaration_is_static) {
          /* Instance methods take `this` in rdi, so a raw C symbol would mis-bind its arguments. */
          haxellvm_diagnostic_error(lexer.diagnostic, function_token.start_offset, function_token.end_offset,
                                    "@:retainFunctionName requires a static function; instance methods carry a hidden 'this' parameter");
          exit(1);
        }
        lexer_expect(&lexer, "(");
        Node *parameters = NULL;
        Node **parameter_tail = &parameters;
        if (!lexer_accept(&lexer, ")")) {
          do {
            Node *parameter = node_new(N_FIELD);
            lexer_name(&lexer, parameter->text);
            optional_type(&lexer, parameter);
            *parameter_tail = parameter;
            parameter_tail = &parameter->next;
          } while (lexer_accept(&lexer, ","));
          lexer_expect(&lexer, ")");
        }
        Node return_type = {0};
        optional_type(&lexer, &return_type);
        Node *body = NULL;
        if (is_interface) is_abstract_method = 1;
        if (is_abstract_method) {
          lexer_expect(&lexer, ";");
        } else if (is_extern) {
          if (!lexer_accept(&lexer, ";")) {
            body = block(&lexer);
            if (body->a) { fprintf(stderr, "haxellvm: extern function body must be empty\n"); exit(1); }
          }
        } else body = block(&lexer);

        Node *declaration = node_new(N_FUNCTION);
        strcpy(declaration->text, function);
        declaration->b = parameters;
        strcpy(declaration->type_name, return_type.type_name);
        declaration->type = return_type.type;
        declaration->integer_width = return_type.integer_width;
        declaration->integer_unsigned = return_type.integer_unsigned;
        declaration->is_dynamic_array = return_type.is_dynamic_array;
        declaration->dynamic_element_width = return_type.dynamic_element_width;
        declaration->dynamic_element_unsigned = return_type.dynamic_element_unsigned;
        declaration->fixed_array_length = return_type.fixed_array_length;
        declaration->is_flexible_array = return_type.is_flexible_array;
        declaration->fixed_element_width = return_type.fixed_element_width;
        declaration->fixed_element_unsigned = return_type.fixed_element_unsigned;
        declaration->fixed_element_is_pointer = return_type.fixed_element_is_pointer;
        strcpy(declaration->fixed_element_type_name, return_type.fixed_element_type_name);
        declaration->fixed_array_length_expr = return_type.fixed_array_length_expr;
        declaration->pointer_element_width = return_type.pointer_element_width;
        declaration->pointer_element_unsigned = return_type.pointer_element_unsigned;
        strcpy(declaration->pointer_type_name, return_type.pointer_type_name);
        declaration->is_function_pointer = return_type.is_function_pointer;
        declaration->is_extern = is_extern;
        strcpy(declaration->external_class_name, external_class_name);
        declaration->is_static = declaration_is_static || is_macro;
        declaration->retain_function_name = retain_function_name;
        declaration->is_macro = is_macro;
        declaration->is_naked = is_naked;
        declaration->is_entry_point = entry_point;
        declaration->is_abstract = is_abstract_method;
        declaration->is_override = is_override;
        declaration->is_final = is_final;
        declaration->is_constructor = !strcmp(function, "new");
        declaration->a = body;
        strcpy(declaration->class_name, active_class_name);

        if (is_abstract_method && !class_node->is_abstract_class) {
          fprintf(stderr, "haxellvm: abstract function only allowed on abstract class\n");
          exit(1);
        }

        if (declaration_is_static) {
          *functions = declaration;
          functions = &declaration->next;
        } else {
          *class_methods = declaration;
          class_methods = &declaration->next;
        }
        if (entry_point) {
          if (program->a) { fprintf(stderr, "haxellvm: multiple entry points\n"); exit(1); }
          strcpy(program->metadata, active_class_name);
          strcpy(program->text, function);
          program->a = body;
        }
      }

      if (class_is_oop(class_node)) {
        finish_oop_class(&functions, class_node);
        *anonymous_type_tail = class_node;
        anonymous_type_tail = &class_node->next;
      } else {
        flatten_non_oop_class(&functions, &globals, class_node);
        free(class_node);
      }
      continue;
    }

    /* Module-level declarations outside a class. */
    SectionMetas sections = {0};
    int entry_point = 0;
    int is_extern = 0;
    int retain_function_name = 0;
    unsigned declaration_alignment = 0;
    while (lexer_accept(&lexer, "@")) {
      lexer_expect(&lexer, ":");
      Token metadata_token = lexer.token;
      char metadata[256];
      lexer_field_name(&lexer, metadata);
      if (!strcmp(metadata, "entryPoint")) entry_point = 1;
      else if (!strcmp(metadata, "extern")) is_extern = 1;
      else if (!strcmp(metadata, "mutable")) {
        /* Accepted for Haxe-style markup; module vars are mutable unless final. */
      }
      else if (!strcmp(metadata, "retainFunctionName")) retain_function_name = 1;
      else if (apply_aligned_meta(&lexer, metadata, &declaration_alignment)) {
        /* stored on the following global */
      }
      else if (!apply_section_meta(&lexer, metadata, &sections)) {
        unsupported_metadata(&lexer, metadata_token);
      }
    }
    lexer_accept(&lexer, "public");
    lexer_accept(&lexer, "private");
    int declaration_is_static = lexer_accept(&lexer, "static");
    int is_final = lexer_accept(&lexer, "final");
    if (lexer_accept(&lexer, "var") || (is_final && lexer.token.kind == TK_ID && strcmp(lexer.token.text, "function"))) {
      Node *global = node_new(N_GLOBAL);
      global->source = source;
      global->source_path = source_path;
      global->source_offset = lexer.token.start_offset;
      lexer_name(&lexer, global->text);
      copy_section_metas(global, &sections);
      global->is_static = declaration_is_static;
      global->is_final = is_final;
      global->is_mutable = !is_final;
      global->is_extern = is_extern;
      global->alignment = declaration_alignment;
      optional_type(&lexer, global);
      if (lexer_accept(&lexer, "=")) global->a = expression(&lexer, 0);
      else if (!global->type_name[0]) { fprintf(stderr, "haxellvm: global without initializer requires a type\n"); exit(1); }
      if (is_final && !global->a) {
        fprintf(stderr, "haxellvm: final global requires an initializer\n");
        exit(1);
      }
      lexer_expect(&lexer, ";");
      global->source_end_offset = lexer.previous_end_offset;
      *globals = global;
      globals = &global->next;
      continue;
    }
    if (is_final) {
      fprintf(stderr, "haxellvm: final module functions are not supported yet\n");
      exit(1);
    }
    int is_macro = 0;
    if (lexer_accept(&lexer, "macro")) {
      lexer_expect(&lexer, "function");
      is_macro = 1;
    } else lexer_expect(&lexer, "function");
    char function[256];
    parse_function_name(&lexer, function);
    lexer_expect(&lexer, "(");
    Node *parameters = NULL;
    Node **parameter_tail = &parameters;
    if (!lexer_accept(&lexer, ")")) {
      do {
        Node *parameter = node_new(N_FIELD);
        lexer_name(&lexer, parameter->text);
        optional_type(&lexer, parameter);
        *parameter_tail = parameter;
        parameter_tail = &parameter->next;
      } while (lexer_accept(&lexer, ","));
      lexer_expect(&lexer, ")");
    }
    Node return_type = {0};
    optional_type(&lexer, &return_type);
    Node *body = NULL;
    if (is_extern) {
      if (!lexer_accept(&lexer, ";")) {
        body = block(&lexer);
        if (body->a) { fprintf(stderr, "haxellvm: extern function body must be empty\n"); exit(1); }
      }
    } else body = block(&lexer);
    Node *declaration = node_new(N_FUNCTION);
    declaration->source = source;
    declaration->source_path = source_path;
    strcpy(declaration->text, function);
    declaration->b = parameters;
    strcpy(declaration->type_name, return_type.type_name);
    declaration->type = return_type.type;
    declaration->integer_width = return_type.integer_width;
    declaration->integer_unsigned = return_type.integer_unsigned;
    declaration->is_dynamic_array = return_type.is_dynamic_array;
    declaration->dynamic_element_width = return_type.dynamic_element_width;
    declaration->dynamic_element_unsigned = return_type.dynamic_element_unsigned;
    declaration->fixed_array_length = return_type.fixed_array_length;
    declaration->is_flexible_array = return_type.is_flexible_array;
    declaration->fixed_element_width = return_type.fixed_element_width;
    declaration->fixed_element_unsigned = return_type.fixed_element_unsigned;
    declaration->fixed_element_is_pointer = return_type.fixed_element_is_pointer;
    strcpy(declaration->fixed_element_type_name, return_type.fixed_element_type_name);
    declaration->fixed_array_length_expr = return_type.fixed_array_length_expr;
    declaration->pointer_element_width = return_type.pointer_element_width;
    declaration->pointer_element_unsigned = return_type.pointer_element_unsigned;
    strcpy(declaration->pointer_type_name, return_type.pointer_type_name);
    declaration->is_function_pointer = return_type.is_function_pointer;
    declaration->is_extern = is_extern;
    declaration->is_static = declaration_is_static || is_macro;
    declaration->retain_function_name = retain_function_name;
    declaration->is_macro = is_macro;
    declaration->is_entry_point = entry_point;
    declaration->a = body;
    *functions = declaration;
    functions = &declaration->next;
    if (entry_point) {
      if (program->a) { fprintf(stderr, "haxellvm: multiple entry points\n"); exit(1); }
      strcpy(program->metadata, active_class_name);
      strcpy(program->text, function);
      program->a = body;
    }
  }

  for (Node *imported_module = imports; imported_module; imported_module = imported_module->next) {
    if (imported_module->kind == N_USING) {
      Node *using_node = node_new(N_USING);
      strcpy(using_node->text, imported_module->text);
      using_node->next = program->metadata_nodes;
      program->metadata_nodes = using_node;
      /* Same-file `using Foo` is fine; only pull Foo.hx when the module exists. */
      if (!using_module_exists(imported_module->text, source_path, import_root)) continue;
    }
    Node *module = parse_import(imported_module, import_root);
    mark_imported_nodes(module->c, imported_module);
    mark_imported_nodes(module->next, imported_module);
    append_nodes(&program->c, module->c);
    append_nodes(&program->next, module->next);
    mark_imported_nodes(module->b, imported_module);
    append_nodes(&program->b, module->b);
  }
  if (!program->a && !program->b && !program->c && !program->next && !imports) {
    haxellvm_diagnostic_error(lexer.diagnostic, lexer.token.start_offset, lexer.token.end_offset, "empty compilation unit");
    exit(1);
  }
  anonymous_type_tail = saved_anonymous_type_tail;
  haxellvm_expand_macros(program);
  return program;
}

Node *parse_haxe(const char *source, const char *source_path) {
  char import_root[1024] = "";
  const char *separator = strrchr(source_path, '/');
  if (separator) {
    size_t length = (size_t)(separator - source_path + 1);
    if (length >= sizeof(import_root)) { fprintf(stderr, "haxellvm: import root path too long\n"); exit(1); }
    memcpy(import_root, source_path, length);
    import_root[length] = '\0';
  }
  return parse_haxe_from_root(source, source_path, import_root);
}

static char imported_paths[256][1024];
static unsigned imported_path_count;

static int using_module_exists(const char *module_name, const char *source_path, const char *import_root) {
  char module_path[512];
  char imported_path[1024];
  const char *module_path_name = module_name;
  unsigned parents = 0;
  while (!strncmp(module_path_name, "../", 3)) {
    parents++;
    module_path_name += 3;
  }
  size_t length = strlen(module_path_name);
  if (length + 4 >= sizeof(module_path)) return 0;
  for (size_t index = 0; index < length; index++) module_path[index] = module_path_name[index] == '.' ? '/' : module_path_name[index];
  strcpy(module_path + length, ".hx");
  if (parents) {
    const char *directory_end = strrchr(source_path, '/');
    size_t directory_length = directory_end ? (size_t)(directory_end - source_path + 1) : 0;
    if (directory_length >= sizeof(imported_path)) return 0;
    memcpy(imported_path, source_path, directory_length);
    imported_path[directory_length] = '\0';
    while (parents--) {
      size_t current_length = strlen(imported_path);
      if (!current_length) return 0;
      if (imported_path[current_length - 1] == '/') imported_path[--current_length] = '\0';
      char *parent_separator = strrchr(imported_path, '/');
      if (!parent_separator) return 0;
      parent_separator[1] = '\0';
    }
  } else {
    if (strlen(import_root) >= sizeof(imported_path)) return 0;
    strcpy(imported_path, import_root);
  }
  if (strlen(imported_path) + strlen(module_path) >= sizeof(imported_path)) return 0;
  strcat(imported_path, module_path);
  FILE *file = fopen(imported_path, "rb");
  if (!file) {
    const char *separator = strrchr(source_path, '/');
    size_t directory_length = separator ? (size_t)(separator - source_path + 1) : 0;
    if (directory_length + strlen(module_path) >= sizeof(imported_path)) return 0;
    memcpy(imported_path, source_path, directory_length);
    strcpy(imported_path + directory_length, module_path);
    file = fopen(imported_path, "rb");
  }
  if (!file) return 0;
  fclose(file);
  return 1;
}

static Node *parse_import(Node *import_site, const char *import_root) {
  char module_path[512];
  char imported_path[1024];
  const char *module_name = import_site->text;
  const char *source_path = import_site->source_path;
  const char *module_path_name = module_name;
  unsigned parents = 0;
  while (!strncmp(module_path_name, "../", 3)) {
    parents++;
    module_path_name += 3;
  }
  size_t length = strlen(module_path_name);
  if (length + 4 >= sizeof(module_path)) { fprintf(stderr, "haxellvm: import path too long\n"); exit(1); }
  for (size_t index = 0; index < length; index++) module_path[index] = module_path_name[index] == '.' ? '/' : module_path_name[index];
  strcpy(module_path + length, ".hx");
  if (parents) {
    const char *directory_end = strrchr(source_path, '/');
    size_t directory_length = directory_end ? (size_t)(directory_end - source_path + 1) : 0;
    if (directory_length >= sizeof(imported_path)) { fprintf(stderr, "haxellvm: import path too long\n"); exit(1); }
    memcpy(imported_path, source_path, directory_length);
    imported_path[directory_length] = '\0';
    while (parents--) {
      size_t current_length = strlen(imported_path);
      if (!current_length) { fprintf(stderr, "haxellvm: parent import escapes source root\n"); exit(1); }
      if (imported_path[current_length - 1] == '/') imported_path[--current_length] = '\0';
      char *parent_separator = strrchr(imported_path, '/');
      if (!parent_separator) { fprintf(stderr, "haxellvm: parent import escapes source root\n"); exit(1); }
      parent_separator[1] = '\0';
    }
  } else strcpy(imported_path, import_root);
  if (strlen(imported_path) + strlen(module_path) >= sizeof(imported_path)) { fprintf(stderr, "haxellvm: import path too long\n"); exit(1); }
  strcat(imported_path, module_path);
  FILE *file = fopen(imported_path, "rb");
  if (!file) {
    const char *separator = strrchr(source_path, '/');
    size_t directory_length = separator ? (size_t)(separator - source_path + 1) : 0;
    if (directory_length + strlen(module_path) >= sizeof(imported_path)) { fprintf(stderr, "haxellvm: import path too long\n"); exit(1); }
    memcpy(imported_path, source_path, directory_length);
    strcpy(imported_path + directory_length, module_path);
    file = fopen(imported_path, "rb");
  }
  if (!file) {
    char message[512];
    snprintf(message, sizeof(message), "cannot resolve import '%s'", module_name);
    HaxellvmDiagnostic *diagnostic = haxellvm_diagnostic_create(import_site->source, import_site->source_path);
    haxellvm_diagnostic_error_at(diagnostic, import_site->source, import_site->source_path,
                                 import_site->source_offset, import_site->source_end_offset, message);
    exit(1);
  }
  /* Diamond imports (main -> gdt, main -> tss -> gdt) must not re-register declarations. */
  for (unsigned index = 0; index < imported_path_count; index++) {
    if (!strcmp(imported_paths[index], imported_path)) { fclose(file); return node_new(N_BLOCK); }
  }
  if (imported_path_count == sizeof(imported_paths) / sizeof(*imported_paths)) { fclose(file); fprintf(stderr, "haxellvm: too many imports\n"); exit(1); }
  /* Durable path storage: stack `imported_path` is invalid after return. */
  unsigned path_index = imported_path_count;
  strcpy(imported_paths[imported_path_count++], imported_path);
  if (fseek(file, 0, SEEK_END) || ftell(file) < 0) { fclose(file); fprintf(stderr, "haxellvm: cannot read import '%s'\n", module_name); exit(1); }
  long size = ftell(file);
  rewind(file);
  char *imported_source = calloc((size_t)size + 1, 1);
  if (!imported_source || fread(imported_source, 1, (size_t)size, file) != (size_t)size) { fclose(file); free(imported_source); fprintf(stderr, "haxellvm: cannot read import '%s'\n", module_name); exit(1); }
  fclose(file);
  /* Keep imported_source alive: AST nodes retain pointers into it for diagnostics. */
  return parse_haxe_from_root(imported_source, imported_paths[path_index], import_root);
}