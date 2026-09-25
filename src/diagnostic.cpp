#include <haxellvm/diagnostic.h>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <map>
#include <memory>
#include <string>

struct HaxellvmDiagnostic {
  llvm::SourceMgr source_manager;
  unsigned primary_buffer_id = 0;
  std::string primary_path;
  std::map<std::string, unsigned> buffers;
};

static unsigned ensure_buffer(HaxellvmDiagnostic *diagnostic, const char *source, const char *path) {
  std::string key = path ? path : "";
  auto existing = diagnostic->buffers.find(key);
  if (existing != diagnostic->buffers.end()) return existing->second;
  unsigned buffer_id = diagnostic->source_manager.AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBufferCopy(source ? source : "", path ? path : ""), llvm::SMLoc());
  diagnostic->buffers.emplace(key, buffer_id);
  return buffer_id;
}

static void print_message(HaxellvmDiagnostic *diagnostic, unsigned buffer_id, size_t start_offset,
                          size_t end_offset, llvm::SourceMgr::DiagKind kind, const char *message) {
  const auto *buffer = diagnostic->source_manager.getMemoryBuffer(buffer_id);
  const char *start = buffer->getBufferStart();
  const char *end = buffer->getBufferEnd();
  size_t length = static_cast<size_t>(end - start);
  start_offset = start_offset > length ? length : start_offset;
  end_offset = end_offset < start_offset ? start_offset : (end_offset > length ? length : end_offset);
  /* SMRange is half-open [Start, End). Callers pass exclusive end offsets. */
  llvm::SMLoc location = llvm::SMLoc::getFromPointer(start + start_offset);
  llvm::SMRange range(location, llvm::SMLoc::getFromPointer(start + end_offset));
  diagnostic->source_manager.PrintMessage(location, kind, message, {range});
}

extern "C" HaxellvmDiagnostic *haxellvm_diagnostic_create(const char *source, const char *path) {
  auto *diagnostic = new HaxellvmDiagnostic;
  diagnostic->primary_path = path ? path : "";
  diagnostic->primary_buffer_id = ensure_buffer(diagnostic, source, path);
  return diagnostic;
}

extern "C" void haxellvm_diagnostic_destroy(HaxellvmDiagnostic *diagnostic) {
  delete diagnostic;
}

extern "C" const char *haxellvm_diagnostic_primary_path(HaxellvmDiagnostic *diagnostic) {
  return diagnostic->primary_path.c_str();
}

extern "C" void haxellvm_diagnostic_error(HaxellvmDiagnostic *diagnostic, size_t start_offset, size_t end_offset, const char *message) {
  print_message(diagnostic, diagnostic->primary_buffer_id, start_offset, end_offset, llvm::SourceMgr::DK_Error, message);
}

extern "C" void haxellvm_diagnostic_error_at(HaxellvmDiagnostic *diagnostic, const char *source, const char *path,
                                             size_t start_offset, size_t end_offset, const char *message) {
  unsigned buffer_id = ensure_buffer(diagnostic, source, path);
  print_message(diagnostic, buffer_id, start_offset, end_offset, llvm::SourceMgr::DK_Error, message);
}

extern "C" void haxellvm_diagnostic_note_at(HaxellvmDiagnostic *diagnostic, const char *source, const char *path,
                                            size_t start_offset, size_t end_offset, const char *message) {
  unsigned buffer_id = ensure_buffer(diagnostic, source, path);
  print_message(diagnostic, buffer_id, start_offset, end_offset, llvm::SourceMgr::DK_Note, message);
}

extern "C" void haxellvm_diagnostic_error_one_line(HaxellvmDiagnostic *diagnostic, size_t offset, const char *message) {
  const auto *buffer = diagnostic->source_manager.getMemoryBuffer(diagnostic->primary_buffer_id);
  const char *start = buffer->getBufferStart();
  const char *end = buffer->getBufferEnd();
  size_t length = static_cast<size_t>(end - start);
  llvm::SMLoc location = llvm::SMLoc::getFromPointer(start + (offset > length ? length : offset));
  auto [line, column] = diagnostic->source_manager.getLineAndColumn(location, diagnostic->primary_buffer_id);
  llvm::errs() << buffer->getBufferIdentifier() << ':' << line << ':' << column << ": error: " << message << '\n';
}
