#include <haxellvm/lexer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HaxellvmDiagnostic *active_diagnostic;
static size_t active_start_offset;
static size_t active_end_offset;

static void die(const char *message) {
  if (active_diagnostic) haxellvm_diagnostic_error(active_diagnostic, active_start_offset, active_end_offset, message);
  else fprintf(stderr, "haxellvm: %s\n", message);
  exit(1);
}

/* Haxe master keyword set from src/syntax/lexer.ml (Hashtbl keywords). */
static const char *const HAXE_KEYWORDS[] = {
  "function", "class", "static", "var", "if", "else", "while", "do", "for",
  "break", "return", "continue", "extends", "implements", "import",
  "switch", "case", "default", "public", "private", "try", "untyped",
  "catch", "new", "this", "throw", "extern", "enum", "in", "interface",
  "cast", "override", "dynamic", "typedef", "package",
  "inline", "using", "null", "true", "false", "abstract", "macro", "final",
  "operator", "overload",
};

int lexer_is_keyword(const char *text) {
  for (size_t index = 0; index < sizeof(HAXE_KEYWORDS) / sizeof(*HAXE_KEYWORDS); index++) {
    if (!strcmp(text, HAXE_KEYWORDS[index])) return 1;
  }
  return 0;
}

static int is_word(int character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') || character == '_' ||
         (character >= '0' && character <= '9');
}

static int hex_value(int character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

static void append_byte(char *destination, int *length, unsigned value) {
  if (*length >= 255) die("string literal too long");
  destination[(*length)++] = (char)value;
}

static void append_unicode(char *destination, int *length, unsigned value) {
  if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) die("invalid Unicode escape");
  if (value <= 0x7f) append_byte(destination, length, value);
  else if (value <= 0x7ff) {
    append_byte(destination, length, 0xc0 | (value >> 6));
    append_byte(destination, length, 0x80 | (value & 0x3f));
  } else if (value <= 0xffff) {
    append_byte(destination, length, 0xe0 | (value >> 12));
    append_byte(destination, length, 0x80 | ((value >> 6) & 0x3f));
    append_byte(destination, length, 0x80 | (value & 0x3f));
  } else {
    append_byte(destination, length, 0xf0 | (value >> 18));
    append_byte(destination, length, 0x80 | ((value >> 12) & 0x3f));
    append_byte(destination, length, 0x80 | ((value >> 6) & 0x3f));
    append_byte(destination, length, 0x80 | (value & 0x3f));
  }
}

static void finish_token(Lexer *lexer, const char **input) {
  lexer->token.end_offset = (size_t)(*input - lexer->source);
  active_end_offset = lexer->token.end_offset;
}

static int try_append_numeric_suffix(Lexer *lexer, const char **input, int *length, int allow_bare_float_suffix) {
  /* Haxe: optional i32/u64/f64-style suffixes after literals. Bare `f`/`F` marks Single. */
  if (!(**input == 'i' || **input == 'u' || **input == 'f' || **input == 'I' || **input == 'U' || **input == 'F')) return 0;
  int start = *length;
  char mark = **input;
  lexer->token.text[(*length)++] = *(*input)++;
  while (**input >= '0' && **input <= '9') {
    if (*length == 255) die("numeric literal too long");
    lexer->token.text[(*length)++] = *(*input)++;
  }
  if (*length == start + 1) {
    if (allow_bare_float_suffix && (mark == 'f' || mark == 'F')) {
      lexer->token.text[*length] = '\0';
      (void)lexer;
      return 1;
    }
    /* Lone letter is not a suffix — rewind. */
    (*input)--;
    (*length)--;
    lexer->token.text[*length] = '\0';
    return 0;
  }
  lexer->token.text[*length] = '\0';
  (void)lexer;
  return 1;
}

void lexer_next(Lexer *lexer) {
  lexer->previous_end_offset = lexer->token.end_offset;
  const char **input = &lexer->input;
  while (**input == ' ' || **input == '\t' || **input == '\n' || **input == '\r') {
    (*input)++;
  }
  if ((*input)[0] == '/' && (*input)[1] == '/') {
    while (**input && **input != '\n') (*input)++;
    lexer_next(lexer);
    return;
  }
  if ((*input)[0] == '/' && (*input)[1] == '*') {
    *input += 2;
    while (**input && !((*input)[0] == '*' && (*input)[1] == '/')) (*input)++;
    if (!**input) die("unterminated block comment");
    *input += 2;
    lexer_next(lexer);
    return;
  }
  lexer->token.start_offset = (size_t)(*input - lexer->source);
  active_start_offset = lexer->token.start_offset;
  lexer->token.text[0] = '\0';
  if (!**input) {
    lexer->token.kind = TK_EOF;
    lexer->token.end_offset = lexer->token.start_offset;
    active_end_offset = lexer->token.end_offset;
    return;
  }

  /* Identifiers / keywords (and $idents as ordinary IDs with leading $). */
  if ((**input >= 'a' && **input <= 'z') || (**input >= 'A' && **input <= 'Z') || **input == '_' || **input == '$') {
    int length = 0;
    if (**input == '$') lexer->token.text[length++] = *(*input)++;
    if (!((**input >= 'a' && **input <= 'z') || (**input >= 'A' && **input <= 'Z') || **input == '_')) {
      if (length == 1) die("expected identifier after '$'");
    }
    while (is_word(**input)) {
      if (length == 255) die("identifier too long");
      lexer->token.text[length++] = *(*input)++;
    }
    lexer->token.text[length] = '\0';
    lexer->token.kind = (lexer->token.text[0] != '$' && lexer_is_keyword(lexer->token.text)) ? TK_KWD : TK_ID;
    finish_token(lexer, input);
    return;
  }

  /* Numbers: int, hex, float — suffixes optional (Haxe split_*_suffix). */
  if (**input >= '0' && **input <= '9') {
    int length = 0;
    if ((*input)[0] == '0' && ((*input)[1] == 'x' || (*input)[1] == 'X')) {
      lexer->token.kind = TK_NUM;
      lexer->token.text[length++] = *(*input)++;
      lexer->token.text[length++] = *(*input)++;
      while ((**input >= '0' && **input <= '9') || (**input >= 'a' && **input <= 'f') || (**input >= 'A' && **input <= 'F') || **input == '_') {
        if (**input == '_') { (*input)++; continue; }
        if (length == 255) die("integer literal too long");
        lexer->token.text[length++] = *(*input)++;
      }
      try_append_numeric_suffix(lexer, input, &length, 0);
      lexer->token.text[length] = '\0';
      finish_token(lexer, input);
      return;
    }

    while ((**input >= '0' && **input <= '9') || **input == '_') {
      if (**input == '_') { (*input)++; continue; }
      if (length == 255) die("numeric literal too long");
      lexer->token.text[length++] = *(*input)++;
    }

    /* Float: digits.digits[eE][+-]digits  OR  digits[eE][+-]digits */
    int is_float = 0;
    if (**input == '.' && (*input)[1] >= '0' && (*input)[1] <= '9') {
      is_float = 1;
      lexer->token.text[length++] = *(*input)++;
      while ((**input >= '0' && **input <= '9') || **input == '_') {
        if (**input == '_') { (*input)++; continue; }
        if (length == 255) die("numeric literal too long");
        lexer->token.text[length++] = *(*input)++;
      }
    }
    if (**input == 'e' || **input == 'E') {
      is_float = 1;
      lexer->token.text[length++] = *(*input)++;
      if (**input == '+' || **input == '-') lexer->token.text[length++] = *(*input)++;
      if (!(**input >= '0' && **input <= '9')) die("expected exponent digits");
      while ((**input >= '0' && **input <= '9') || **input == '_') {
        if (**input == '_') { (*input)++; continue; }
        if (length == 255) die("numeric literal too long");
        lexer->token.text[length++] = *(*input)++;
      }
    }

    try_append_numeric_suffix(lexer, input, &length, is_float);
    lexer->token.text[length] = '\0';
    lexer->token.kind = is_float ? TK_FLOAT : TK_NUM;
    finish_token(lexer, input);
    return;
  }

  /* Leading .digits float */
  if (**input == '.' && (*input)[1] >= '0' && (*input)[1] <= '9') {
    int length = 0;
    lexer->token.kind = TK_FLOAT;
    lexer->token.text[length++] = *(*input)++;
    while ((**input >= '0' && **input <= '9') || **input == '_') {
      if (**input == '_') { (*input)++; continue; }
      if (length == 255) die("numeric literal too long");
      lexer->token.text[length++] = *(*input)++;
    }
    if (**input == 'e' || **input == 'E') {
      lexer->token.text[length++] = *(*input)++;
      if (**input == '+' || **input == '-') lexer->token.text[length++] = *(*input)++;
      if (!(**input >= '0' && **input <= '9')) die("expected exponent digits");
      while ((**input >= '0' && **input <= '9') || **input == '_') {
        if (**input == '_') { (*input)++; continue; }
        if (length == 255) die("numeric literal too long");
        lexer->token.text[length++] = *(*input)++;
      }
    }
    try_append_numeric_suffix(lexer, input, &length, 1);
    lexer->token.text[length] = '\0';
    finish_token(lexer, input);
    return;
  }

  if (**input == '"' || **input == '\'') {
    int length = 0;
    int delimiter = **input;
    lexer->token.kind = TK_STR;
    (*input)++;
    while (**input && **input != delimiter) {
      if (**input == '\\' && (*input)[1]) {
        (*input)++;
        switch (**input) {
          case 'a': append_byte(lexer->token.text, &length, '\a'); break;
          case 'b': append_byte(lexer->token.text, &length, '\b'); break;
          case 'f': append_byte(lexer->token.text, &length, '\f'); break;
          case 'n': append_byte(lexer->token.text, &length, '\n'); break;
          case 'r': append_byte(lexer->token.text, &length, '\r'); break;
          case 't': append_byte(lexer->token.text, &length, '\t'); break;
          case 'v': append_byte(lexer->token.text, &length, '\v'); break;
          case '\\': append_byte(lexer->token.text, &length, '\\'); break;
          case '"': append_byte(lexer->token.text, &length, '"'); break;
          case '\'': append_byte(lexer->token.text, &length, '\''); break;
          case 'x': {
            int high = hex_value((*input)[1]);
            int low = hex_value((*input)[2]);
            if (high < 0 || low < 0) die("hex escape requires two digits");
            append_byte(lexer->token.text, &length, (unsigned)((high << 4) | low));
            *input += 2;
            break;
          }
          case 'u': {
            unsigned value = 0;
            int digits = 0;
            if ((*input)[1] == '{') {
              *input += 2;
              while (**input != '}') {
                int digit = hex_value(**input);
                if (digit < 0 || ++digits > 6) die("invalid Unicode escape");
                value = (value << 4) | (unsigned)digit;
                (*input)++;
              }
              if (!digits) die("invalid Unicode escape");
            } else {
              for (int index = 1; index <= 4; index++) {
                int digit = hex_value((*input)[index]);
                if (digit < 0) die("Unicode escape requires four digits");
                value = (value << 4) | (unsigned)digit;
              }
              *input += 4;
            }
            append_unicode(lexer->token.text, &length, value);
            break;
          }
          default:
            if (**input >= '0' && **input <= '7') {
              unsigned value = 0;
              for (int index = 0; index < 3 && **input >= '0' && **input <= '7'; index++, (*input)++) value = (value << 3) | (unsigned)(**input - '0');
              (*input)--;
              if (value > 255) die("octal escape exceeds byte range");
              append_byte(lexer->token.text, &length, value);
            } else {
              append_byte(lexer->token.text, &length, '\\');
              append_byte(lexer->token.text, &length, **input);
            }
            break;
        }
        (*input)++;
      } else append_byte(lexer->token.text, &length, (unsigned)*(*input)++);
    }
    if (**input != delimiter) die("unterminated string");
    (*input)++;
    lexer->token.text[length] = '\0';
    finish_token(lexer, input);
    return;
  }

  lexer->token.kind = TK_SYM;
  lexer->token.text[0] = *(*input)++;
  lexer->token.text[1] = '\0';

  /* Multi-character operators aligned with Haxe binop/unop/token set. */
  if (lexer->token.text[0] == '?' && **input == '?') {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = '\0';
  } else if (lexer->token.text[0] == '?' && **input == '.') {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = '\0';
  } else if (lexer->token.text[0] == '=' && **input == '>') {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = '\0';
  } else if (lexer->token.text[0] == '-' && **input == '>') {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = '\0';
  } else if (lexer->token.text[0] == '>' && (*input)[0] == '>' && (*input)[1] == '>') {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = *(*input)++;
    if (**input == '=') {
      lexer->token.text[3] = *(*input)++;
      lexer->token.text[4] = '\0';
    } else lexer->token.text[3] = '\0';
  } else if ((lexer->token.text[0] == '<' || lexer->token.text[0] == '>') && **input == lexer->token.text[0]) {
    lexer->token.text[1] = *(*input)++;
    if (**input == '=') {
      lexer->token.text[2] = *(*input)++;
      lexer->token.text[3] = '\0';
    } else lexer->token.text[2] = '\0';
  } else if ((lexer->token.text[0] == '+' || lexer->token.text[0] == '-') && **input == lexer->token.text[0]) {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = '\0';
  } else if ((strchr("=!<>+-*/%&|^", lexer->token.text[0]) && **input == '=') ||
             (lexer->token.text[0] == '&' && **input == '&') ||
             (lexer->token.text[0] == '|' && **input == '|')) {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = '\0';
  }
  if (lexer->token.text[0] == '.' && (*input)[0] == '.' && (*input)[1] == '.') {
    lexer->token.text[1] = *(*input)++;
    lexer->token.text[2] = *(*input)++;
    lexer->token.text[3] = '\0';
  }
  finish_token(lexer, input);
}

void lexer_init(Lexer *lexer, const char *source, const char *source_path) {
  lexer->source = source;
  lexer->input = source;
  lexer->token = (Token){0};
  lexer->previous_end_offset = 0;
  lexer->diagnostic = haxellvm_diagnostic_create(source, source_path);
  active_diagnostic = lexer->diagnostic;
  lexer_next(lexer);
}

void lexer_dispose(Lexer *lexer) {
  haxellvm_diagnostic_destroy(lexer->diagnostic);
  lexer->diagnostic = NULL;
  active_diagnostic = NULL;
}

int lexer_accept(Lexer *lexer, const char *value) {
  if (lexer->token.kind == TK_STR) return 0;
  /* Generic closing '>': peel one '>' off '>>' / '>>>' (Haxe splits OpGt). */
  if (!strcmp(value, ">") && lexer->token.text[0] == '>' && lexer->token.text[1] == '>') {
    memmove(lexer->token.text, lexer->token.text + 1, strlen(lexer->token.text));
    return 1;
  }
  if (strcmp(lexer->token.text, value)) return 0;
  lexer_next(lexer);
  return 1;
}

void lexer_expect(Lexer *lexer, const char *value) {
  if (!lexer_accept(lexer, value)) {
    char message[576];
    snprintf(message, sizeof(message), "expected '%s', got '%s'", value, lexer->token.text);
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, message);
    exit(1);
  }
}

void lexer_name(Lexer *lexer, char *destination) {
  /* Haxe allows `this` / `super` as primary names (then `.field` via qualified_name). */
  if (lexer->token.kind == TK_KWD && (!strcmp(lexer->token.text, "this") || !strcmp(lexer->token.text, "super"))) {
    strcpy(destination, lexer->token.text);
    lexer_next(lexer);
    return;
  }
  if (lexer->token.kind != TK_ID) {
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset,
                              lexer->token.kind == TK_KWD ? "unexpected keyword (expected identifier)" : "expected identifier");
    exit(1);
  }
  strcpy(destination, lexer->token.text);
  lexer_next(lexer);
}

void lexer_field_name(Lexer *lexer, char *destination) {
  if (lexer->token.kind != TK_ID && lexer->token.kind != TK_KWD) {
    haxellvm_diagnostic_error(lexer->diagnostic, lexer->token.start_offset, lexer->token.end_offset, "expected field name");
    exit(1);
  }
  strcpy(destination, lexer->token.text);
  lexer_next(lexer);
}

int lexer_assignment_ahead(const Lexer *lexer) {
  const char *cursor = lexer->input;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') cursor++;
  return cursor[0] == '=' && cursor[1] != '=' && cursor[1] != '>';
}
