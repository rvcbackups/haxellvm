#include <haxellvm/codegen.h>
#include <haxellvm/parser.h>
#include <haxellvm/preprocess.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
  fprintf(stderr, "usage: haxellvm [-D NAME[=VALUE]] [-g] [-Xcpu FEATURES] [-target TRIPLE] [--emit=llvm|--emit=asm|--emit=obj] INPUT.hx -o OUTPUT\n");
}

int main(int argc, char **argv) {
  OutputKind output_kind = OUTPUT_OBJECT;
  const char *input_path = NULL;
  const char *output_path = NULL;
  const char *cpu_features = "";
  const char *target_triple = NULL;
  int emit_debug = 0;
  CompilerDefines defines = {0};
  for (int argument = 1; argument < argc; argument++) {
    if (!strcmp(argv[argument], "--emit=asm")) output_kind = OUTPUT_ASSEMBLY;
    else if (!strcmp(argv[argument], "--emit=obj")) output_kind = OUTPUT_OBJECT;
    else if (!strcmp(argv[argument], "--emit=llvm")) output_kind = OUTPUT_LLVM_IR;
    else if (!strcmp(argv[argument], "-g")) emit_debug = 1;
    else if (!strcmp(argv[argument], "-Xcpu")) {
      if (++argument == argc || cpu_features[0]) { usage(); return 1; }
      cpu_features = argv[argument];
    }
    else if (!strcmp(argv[argument], "-target")) {
      if (++argument == argc || target_triple) { usage(); return 1; }
      target_triple = argv[argument];
    }
    else if (!strcmp(argv[argument], "-D") || !strcmp(argv[argument], "--define")) {
      if (++argument == argc || !compiler_define_add(&defines, argv[argument])) { usage(); return 1; }
    }
    else if (!strcmp(argv[argument], "-o")) {
      if (++argument == argc || output_path) { usage(); return 1; }
      output_path = argv[argument];
    } else if (!input_path) input_path = argv[argument];
    else { usage(); return 1; }
  }
  if (!input_path || !output_path) { usage(); return 1; }
  FILE *file = fopen(input_path, "rb");
  if (!file) { perror(input_path); return 1; }
  if (fseek(file, 0, SEEK_END) || ftell(file) < 0) { fclose(file); fprintf(stderr, "haxellvm: cannot read input\n"); return 1; }
  long size = ftell(file);
  rewind(file);
  char *source = calloc((size_t)size + 1, 1);
  if (!source || fread(source, 1, (size_t)size, file) != (size_t)size) { fclose(file); free(source); fprintf(stderr, "haxellvm: cannot read input\n"); return 1; }
  fclose(file);
  if (!preprocess_source(source, &defines, input_path)) { free(source); return 1; }
  Node *tree = parse_haxe(source, input_path);
  int status = generate_output(tree, output_path, output_kind, cpu_features, target_triple, emit_debug);
  free(source);
  return status;
}
