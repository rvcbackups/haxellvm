#include <haxellvm/preprocess.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int compiler_define_add(CompilerDefines *defines, const char *definition) {
  if (!definition[0] || defines->count == sizeof(defines->items) / sizeof(*defines->items)) return 0;
  defines->items[defines->count++] = definition;
  return 1;
}

static int define_matches(const char *definition, const char *name, size_t name_length) {
  return !strncmp(definition, name, name_length) && (definition[name_length] == '\0' || definition[name_length] == '=');
}

static int is_defined(const CompilerDefines *defines, const char *name, size_t name_length) {
  for (unsigned index = 0; index < defines->count; index++) if (define_matches(defines->items[index], name, name_length)) return 1;
  return 0;
}

static void blank_line(char *line, char *end) {
  for (; line < end; line++) if (*line != '\n' && *line != '\r') *line = ' ';
}

int preprocess_source(char *source, const CompilerDefines *defines, const char *source_path) {
  int active[64] = {1};
  int branch_taken[64] = {0};
  unsigned depth = 0;
  for (char *line = source; *line;) {
    char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    char *cursor = line;
    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
    int parent_active = active[depth];
    if (cursor < end && *cursor == '#') {
      char *directive = ++cursor;
      while (cursor < end && isalpha((unsigned char)*cursor)) cursor++;
      size_t directive_length = (size_t)(cursor - directive);
      while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
      char *name = cursor;
      while (cursor < end && (isalnum((unsigned char)*cursor) || *cursor == '_')) cursor++;
      size_t name_length = (size_t)(cursor - name);
      if (directive_length == 2 && !strncmp(directive, "if", 2)) {
        if (depth + 1 == sizeof(active) / sizeof(*active) || !name_length) { fprintf(stderr, "%s: invalid #if\n", source_path); return 0; }
        int condition = is_defined(defines, name, name_length);
        active[++depth] = parent_active && condition;
        branch_taken[depth] = condition;
      } else if (directive_length == 6 && !strncmp(directive, "elseif", 6)) {
        if (!depth) { fprintf(stderr, "%s: #elseif without #if\n", source_path); return 0; }
        if (!name_length) { fprintf(stderr, "%s: invalid #elseif\n", source_path); return 0; }
        if (branch_taken[depth]) active[depth] = 0;
        else {
          int condition = is_defined(defines, name, name_length);
          active[depth] = active[depth - 1] && condition;
          branch_taken[depth] = condition;
        }
      } else if (directive_length == 4 && !strncmp(directive, "else", 4)) {
        if (!depth) { fprintf(stderr, "%s: #else without #if\n", source_path); return 0; }
        active[depth] = active[depth - 1] && !branch_taken[depth];
        branch_taken[depth] = 1;
      } else if (directive_length == 3 && !strncmp(directive, "end", 3)) {
        if (!depth) { fprintf(stderr, "%s: #end without #if\n", source_path); return 0; }
        depth--;
      } else { fprintf(stderr, "%s: unsupported compiler directive\n", source_path); return 0; }
      blank_line(line, end);
    } else if (!active[depth]) blank_line(line, end);
    line = *end ? end + 1 : end;
  }
  if (depth) { fprintf(stderr, "%s: unterminated #if\n", source_path); return 0; }
  return 1;
}