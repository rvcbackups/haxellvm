import subprocess
import tempfile
import unittest
import platform
from pathlib import Path


class CompilerTests(unittest.TestCase):
    compiler = Path(__file__).parents[1] / "haxellvm"

    def test_packed_typedef_layout_and_default_abi(self):
        source = """
        @:packed typedef Packed = { var limit:UIntSize<16>; var base:Ptr<UIntSize<8>>; };
        typedef Ordinary = { var limit:UIntSize<16>; var base:Ptr<UIntSize<8>>; };
        class Main {
            @:entryPoint static function main():Void {
                trace(sizeof(Packed));
                trace(sizeof(Ordinary));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "packed.hx"
            ir_path = Path(directory) / "packed.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "-target", "x86_64-unknown-linux-gnu", "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            self.assertIn("%Packed = type <{ i16, ptr }>", ir)
            self.assertIn("%Ordinary = type { i16, ptr }", ir)
            if platform.machine() == "x86_64":
                result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout, "10\n16\n")

    def test_unsupported_metadata_uses_source_manager(self):
        for declaration in ("var value:Int;", "typedef Value = { var value:Int; };", "class Main {\n@:bogus\nstatic function run():Void {}\n}"):
            source = declaration if declaration.startswith("class") else "\n@:bogus\n" + declaration
            with self.subTest(declaration=declaration), tempfile.TemporaryDirectory() as directory:
                source_path = Path(directory) / "metadata.hx"
                source_path.write_text(source)
                result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad.o"], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"{source_path}:2:3: error: unsupported metadata 'bogus'", result.stderr)
                self.assertIn("@:bogus\n  ^~~~~", result.stderr)

    def test_unknown_variable_underline_excludes_trailing_space(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                if (MemoryMapUsable && true) {}
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "unknown.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown variable 'MemoryMapUsable'", result.stderr)
        self.assertIn("MemoryMapUsable && true", result.stderr)
        # Exact span of MemoryMapUsable (15 chars): caret + 14 tildes, no trailing space.
        self.assertIn("^~~~~~~~~~~~~~~", result.stderr)
        self.assertNotIn("^~~~~~~~~~~~~~~~", result.stderr)

    def test_emits_runnable_llvm_for_control_flow(self):
        source = """
        class Main {
                    @:entryPoint
          static function main():Void {
            var value:Int = 2;
            while (value > 0) {
              trace(value);
              value = value - 1;
            }
            if (value == 0) { trace("done"); } else { trace("bad"); }
          }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "main.hx"
            output_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", output_path], check=True)
            result = subprocess.run(["lli", output_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\n1\ndone\n")

    def test_accepts_emit_option_after_input_and_output(self):
        source = "class Main { @:entryPoint static function main():Void { trace(7); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "main.hx"
            output_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", output_path, "--emit=llvm"], check=True)
            ir = output_path.read_text()
        self.assertIn("define i32 @main()", ir)

    def test_compiler_defines_select_haxe_conditional_branches(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                #if debug
                trace("debug");
                #else
                trace("release");
                #end
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "defines.hx"
            ir_path = Path(directory) / "defines.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "-D", "debug", "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "debug\n")

    def test_accepts_cpu_features_after_input_and_output(self):
        source = "class Main { @:entryPoint static function main():Void { asm(\"nop\"); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "main.hx"
            assembly_path = Path(directory) / "main.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", assembly_path, "-Xcpu", "-x87,-mmx,-sse,-sse2,+soft-float"], check=True)
            assembly = assembly_path.read_text()
        self.assertIn("nop", assembly)

    def test_rejects_inline_asm_constraint_operand_mismatches_before_codegen(self):
        source = "class Main { @:entryPoint static function main():Void { asm(\"mov eax, ebx\", \"{eax},{ebx}\"); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-asm-arity.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad-asm-arity.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("inline asm constraint list expects 2 operands but call supplies 0", result.stderr)
        self.assertIn(source, result.stderr)
        self.assertIn("^", result.stderr)

    def test_accepts_block_comments_between_tokens(self):
        source = """
        /* module */ class Main {
            /* entry */ @:entryPoint static function main():Void {
                var value:Int = 40 /* answer */ + 2;
                /* output */ trace(value);
            }
        } /* end */
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "comments.hx"
            ir_path = Path(directory) / "comments.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_decodes_standard_string_escapes(self):
        source = """
        class Escapes {
            @:entryPoint static function main():Void {
                var text = "\\n\\t\\\\";
                for (index in 0...text.length) { trace(text.charCodeAt(index)); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "escapes.hx"
            ir_path = Path(directory) / "escapes.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "10\n9\n92\n")
        self.assertIn('c"\\0A\\09\\\\\\00"', ir)

    def test_decodes_octal_hexadecimal_and_unicode_escapes(self):
        source = """
        class Escapes {
            @:entryPoint static function main():Void {
                var text = "\\033\\x1b\\u001b\\u{1b}";
                for (index in 0...text.length) { trace(text.charCodeAt(index)); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "broad-escapes.hx"
            ir_path = Path(directory) / "broad-escapes.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "27\n27\n27\n27\n")
        self.assertIn('c"\\1B\\1B\\1B\\1B\\00"', ir)

    def test_rejects_unterminated_block_comment(self):
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "comment.hx"
            source_path.write_text("/* unfinished")
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "comment.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unterminated block comment", result.stderr)

    def test_reports_empty_compilation_unit_with_source_manager_context(self):
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "empty.hx"
            source_path.write_text("\n")
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "empty.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:2:1: error: empty compilation unit", result.stderr)
        self.assertIn("^", result.stderr)

    def test_reports_parser_errors_with_llvm_source_manager_locations(self):
        source = "class Main { @:entryPoint static function main():Void { trace(1]; } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:1:64: error: expected ')', got ']'", result.stderr)
        self.assertIn(source, result.stderr)
        self.assertIn("                                                               ^", result.stderr)

    def test_reports_non_constant_global_initializers_at_the_initializer(self):
        source = "var value:Int = nope();"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-global.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad-global.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(
            "global initializer must be a constant" in result.stderr
            or "constant initializer calls an unknown function" in result.stderr,
            result.stderr,
        )
    def test_reports_unknown_new_types_at_the_construction(self):
        source = "class Main { @:entryPoint static function main():Void { new Missing(); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-new.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad-new.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:1:57: error: new requires a known class type", result.stderr)
        self.assertIn("                                                        ^~~~~~~~~~~~~", result.stderr)

    def test_reports_member_assignment_errors_at_the_assignment(self):
        source = "typedef Entry = { var value:UIntSize<16>; }; class Main { var entry:Entry; @:entryPoint static function main():Void { this.entry = [1]; } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-member-assignment.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad-member-assignment.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("error: unsupported member assignment", result.stderr)
        self.assertIn("this.entry = [1]", result.stderr)
        self.assertIn("^~~~~~~~~~", result.stderr)

    def test_reports_codegen_errors_with_llvm_source_manager_locations(self):
        source = "class Main { @:entryPoint static function main():Void { var pointer:Ptr<UIntSize<64>> = 0; asm(\"mov $0, $0\", \"{eax}\", pointer); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-asm.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad-asm.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:1:", result.stderr)
        self.assertIn("error: pointer asm operand requires a 64-bit fixed register such as {rax}", result.stderr)
        self.assertIn(source, result.stderr)
        self.assertIn("^", result.stderr)

    def test_reports_unknown_variables_with_llvm_source_manager_locations(self):
        source = "class Main { @:entryPoint static function main():Void { trace(missing); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "unknown-variable.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "unknown-variable.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:1:63: error: unknown variable 'missing'", result.stderr)
        self.assertIn(source, result.stderr)
        self.assertIn("                                                              ^~~~~~~", result.stderr)

    def test_rejects_unsupported_function_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "main.hx"
            source = "class Main { @:entryPoint static function main() { nope(); } }"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "main.ll"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("call to undeclared function 'nope'", result.stderr)
        self.assertIn("declare it with 'function' or import", result.stderr)
        self.assertIn(f"{source_path}:1:52: error:", result.stderr)
        self.assertIn(source, result.stderr)
        self.assertIn("^~~~~~", result.stderr)

    def test_emits_intel_syntax_assembly(self):
        if platform.machine() not in {"x86_64", "i386", "i486", "i586", "i686"}:
            self.skipTest("Intel syntax applies only to x86 targets")
        source = "class Main { @:entryPoint static function main() { var value:Int = 42; asm(\"nop\"); trace(value); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "main.hx"
            output_path = Path(directory) / "main.s"
            executable_path = Path(directory) / "main"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", output_path], check=True)
            assembly = output_path.read_text()
            subprocess.run(["clang", output_path, "-o", executable_path], check=True)
            result = subprocess.run([executable_path], capture_output=True, text=True, check=True)
        self.assertIn(".intel_syntax noprefix", assembly)
        self.assertRegex(assembly, r"mov\s+(dword|qword) ptr \[")
        self.assertIn("nop", assembly)
        self.assertEqual(result.stdout, "42\n")

    def test_x86_assembly_uses_no_vector_registers(self):
        if platform.machine() not in {"x86_64", "i386", "i486", "i586", "i686"}:
            self.skipTest("x86 feature policy applies only to x86 targets")
        source = """
        class Kernel {
            @:entryPoint static function boot():Void {
                var words:FixedArray<UIntSize<64>, 16> = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kernel.hx"
            assembly_path = Path(directory) / "kernel.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "-Xcpu", "-x87,-mmx,-sse,-sse2,+soft-float", "--emit=asm", source_path, "-o", assembly_path], check=True)
            assembly = assembly_path.read_text().lower()
        for instruction in ("xmm", "movups", "movaps", "movdqu", "movdqa", "xorps"):
            self.assertNotIn(instruction, assembly)

    def test_accepts_sized_integer_dynamic_array_type(self):
        source = """
        static var scan_codes:Array<UIntSize<8>> = [
            'a'.code, '\\''.code, '\\\\'.code, '-'.code, '='.code, '['.code, ']'.code,
        ];
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "scan_codes.hx"
            output_path = Path(directory) / "scan_codes.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", output_path], check=True)

    def test_lowers_anonymous_record_function_returns(self):
        source = """
        class Keyboard {
            static function read(): { scancode: UIntSize<8>, ascii: UIntSize<8> } {
                return { scancode: 30, ascii: 'a'.code };
            }
            @:entryPoint static function main():Void {
                var event = read();
                trace(event.ascii);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "anonymous-record.hx"
            output_path = Path(directory) / "anonymous-record.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", output_path], check=True)
            result = subprocess.run(["lli", output_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "97\n")

    def test_extracts_field_from_record_function_return(self):
        source = """
        class Keyboard {
            static function read(): { scancode: UIntSize<8>, ascii: UIntSize<8> } {
                return { scancode: 30, ascii: 'a'.code };
            }
            @:entryPoint static function main():Void {
                trace(read().ascii);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "anonymous-record-field.hx"
            output_path = Path(directory) / "anonymous-record-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", output_path], check=True)
            result = subprocess.run(["lli", output_path], capture_output=True, text=True, check=True)
            ir = output_path.read_text()
        self.assertEqual(result.stdout, "97\n")
        self.assertIn("extractvalue", ir)

    def test_indexes_sized_integer_global_dynamic_array(self):
        source = """
        static var scan_codes:Array<UIntSize<8>> = [0, 'a'.code];
        class Keyboard {
            @:entryPoint static function main():Void {
                trace(scan_codes.length);
                trace(scan_codes[1]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "scan_codes.hx"
            output_path = Path(directory) / "scan_codes.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", output_path], check=True)
            result = subprocess.run(["lli", output_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\n97\n")

    def test_emits_linkable_native_object(self):
        source = "class Main { @:entryPoint static function main() { trace(42); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "main.hx"
            object_path = Path(directory) / "main.o"
            executable_path = Path(directory) / "main"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            self.assertGreater(object_path.stat().st_size, 0)
            subprocess.run(["clang", object_path, "-o", executable_path], check=True)
            result = subprocess.run([executable_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_lowers_limine_style_entry_point_and_sections(self):
        source = """
        class Main {
            static function limine_base_revision(revision:Int):Array<Int> {
                return [0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, revision];
            }
            @:elfSection(".limine_requests") static var limine_base_revision:Array<Int> = limine_base_revision(6);
            @:elfSection(".limine_requests") static var limine_requests_start_header:Array<Int> = [0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, 0xc7b1dd30df4c8b88, 0x18e2b0d053c1a02f];
            @:entryPoint static function kmain():Void { asm("hlt"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kernel.hx"
            object_path = Path(directory) / "kernel.o"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            sections = subprocess.run(["llvm-readelf", "-S", object_path], capture_output=True, text=True, check=True).stdout
            symbols = subprocess.run(["llvm-nm", object_path], capture_output=True, text=True, check=True).stdout
        self.assertIn(".limine_requests", sections)
        self.assertRegex(symbols, r"(?m) T kmain$")
        self.assertRegex(symbols, r"(?m) R limine_base_revision$")
        self.assertRegex(symbols, r"(?m) R limine_requests_start_header$")

    def test_folds_fixed_array_constant_helper_initializer(self):
        module = """
        function limineBaseRevision(n:UIntSize<64>):FixedArray<UIntSize<64>, 3> {
            return [0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, n];
        }
        """
        source = """
        import limine;
        class Main {
            @:elfSection(".limine_requests")
            @:mutable
            var baseRevision:FixedArray<UIntSize<64>, 3> = limine.limineBaseRevision(6);
            @:entryPoint static function kmain():Void { asm("hlt"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "limine.hx"
            source_path = Path(directory) / "kernel.hx"
            object_path = Path(directory) / "kernel.o"
            ir_path = Path(directory) / "kernel.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            sections = subprocess.run(["llvm-readelf", "-S", object_path], capture_output=True, text=True, check=True).stdout
        self.assertIn(".limine_requests", sections)
        self.assertIn("@baseRevision = global [3 x i64]", ir)
        self.assertIn("i64 6", ir)
        self.assertIn('section ".limine_requests"', ir)

    def test_folds_two_arg_fixed_array_constant_helper(self):
        source = """
        function limineRequestMagic(first:UIntSize<64>, second:UIntSize<64>):FixedArray<UIntSize<64>, 4> {
            return [0xc7b1dd30df4c8b88, 0x0a82e883a194f07b, first, second];
        }
        var framebufferRequestId:FixedArray<UIntSize<64>, 4> = limineRequestMagic(0x9d5827dcd881dd75, 0xa3148604f6fab11b);
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "limine.hx"
            ir_path = Path(directory) / "limine.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@framebufferRequestId = global [4 x i64]", ir)
        self.assertIn("i64 -7108888182325650059", ir)
        self.assertIn("i64 -6695579390111469285", ir)

    def test_lowers_mutable_section_global(self):
        source = """
        class Kernel {
            @:elfSection(".requests") @:mutable static var revision:Array<Int> = [6];
            @:entryPoint static function boot():Void { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "mutable.hx"
            object_path = Path(directory) / "mutable.o"
            ir_path = Path(directory) / "mutable.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            sections = subprocess.run(["llvm-readelf", "-S", object_path], capture_output=True, text=True, check=True).stdout
            ir = ir_path.read_text()
        self.assertIn("@revision = global [1 x i64] [i64 6], section \".requests\"", ir)
        self.assertRegex(sections, r"\.requests\s+PROGBITS.*\bWA\b")

    def test_lowers_mutable_boolean_global_initializer(self):
        source = """
        var isInPanic:Bool = false;
        class Main {
            @:entryPoint static function main():Void {
                trace(isInPanic);
                isInPanic = true;
                trace(isInPanic);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bool-global.hx"
            ir_path = Path(directory) / "bool-global.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "false\ntrue\n")
        self.assertIn("@isInPanic = global i1 false", ir)

    def test_lowers_module_final_variables_without_var_keyword(self):
        source = """
        final MemoryMapUsable:UIntSize<32> = 0;
        final MemoryMapReserved:UIntSize<32> = 1;
        class Main {
            @:entryPoint static function main():Void {
                trace(MemoryMapUsable);
                trace(MemoryMapReserved);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "module-final.hx"
            ir_path = Path(directory) / "module-final.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "0\n1\n")
        self.assertIn("@MemoryMapUsable = constant i32 0", ir)

    def test_section_meta_is_format_agnostic(self):
        source = """
        class Main {
            @:section(".portable") static var payload:Array<Int> = [1];
            @:entryPoint static function main():Void { }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "portable.hx"
            ir_path = Path(directory) / "portable.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn('section ".portable"', ir)

    def test_pe_section_applies_only_for_pe_target(self):
        source = """
        class Main {
            @:peSection(".rdata_pe") static var payload:Array<Int> = [1];
            @:entryPoint static function main():Void { }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pe.hx"
            elf_ir = Path(directory) / "elf.ll"
            pe_ir = Path(directory) / "pe.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", "-target", "x86_64-unknown-linux-gnu", source_path, "-o", elf_ir], check=True)
            subprocess.run([self.compiler, "--emit=llvm", "-target", "x86_64-pc-windows-msvc", source_path, "-o", pe_ir], check=True)
            elf_text = elf_ir.read_text()
            pe_text = pe_ir.read_text()
        self.assertNotIn("section", elf_text.split("@payload", 1)[1].split("\n", 1)[0])
        self.assertIn('section ".rdata_pe"', pe_text)

    def test_macho_segment_and_section_compose(self):
        source = """
        class Main {
            @:machoSegment("__DATA") @:machoSection("__limine") static var payload:Array<Int> = [1];
            @:entryPoint static function main():Void { }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "macho.hx"
            ir_path = Path(directory) / "macho.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", "-target", "x86_64-apple-darwin", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn('section "__DATA,__limine"', ir)

    def test_emits_coff_object_for_windows_target(self):
        source = """
        class Main {
            @:peSection(".rdata") static var payload:Array<Int> = [42];
            @:entryPoint static function main():Void { }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "win.hx"
            object_path = Path(directory) / "win.obj"
            source_path.write_text(source)
            completed = subprocess.run(
                [self.compiler, "-target", "x86_64-pc-windows-msvc", source_path, "-o", object_path],
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                self.skipTest(completed.stderr.strip() or "windows target unavailable")
            artifact = subprocess.run(["file", object_path], capture_output=True, text=True, check=True).stdout
        self.assertRegex(artifact, r"(?i)coff|PE32\+|Intel 80386|x86-64")

    def test_emits_macho_object_for_darwin_target(self):
        source = """
        class Main {
            @:machoSection("__DATA,__payload") static var payload:Array<Int> = [42];
            @:entryPoint static function main():Void { }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "darwin.hx"
            object_path = Path(directory) / "darwin.o"
            source_path.write_text(source)
            completed = subprocess.run(
                [self.compiler, "-target", "x86_64-apple-darwin", source_path, "-o", object_path],
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                self.skipTest(completed.stderr.strip() or "darwin target unavailable")
            artifact = subprocess.run(["file", object_path], capture_output=True, text=True, check=True).stdout
        self.assertRegex(artifact, r"(?i)Mach-O")

    def test_passes_global_array_data_not_its_first_word_as_a_pointer(self):
        source = """
        class Kernel {
            static function supported(revision:Array<Int>):Bool { return revision[2] == 0; }
            @:mutable static var revision:Array<Int> = [1, 2, 0];
            @:entryPoint static function boot():Void {
                if (supported(revision)) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "revision.hx"
            ir_path = Path(directory) / "revision.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("call i1 @supported(ptr @revision)", ir)
        self.assertNotIn("load ptr, ptr @revision", ir)

    def test_lowers_typedef_record_literals(self):
        source = """
        class Records {
            typedef Point = {
                var x:UIntSize<32>;
                var y:UIntSize<32>;
            };
            static var origin:Point = { x: 10, y: 20 };
            @:entryPoint static function boot():Void {
                var point:Point = { x: 1, y: 2 };
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "records.hx"
            ir_path = Path(directory) / "records.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Point = type { i32, i32 }", ir)
        self.assertIn("@origin = private constant %Point { i32 10, i32 20 }", ir)
        self.assertIn("%point = alloca %Point", ir)
        self.assertIn("store i32 1", ir)
        self.assertIn("store i32 2", ir)

    def test_lowers_unsized_arrays_of_typedefs_as_pointers(self):
        source = """
        class Kernel {
            typedef VideoMode = { var width:UIntSize<64>; var height:UIntSize<64>; };
            typedef Framebuffer = { var modes:Array<VideoMode>; };
            static var framebuffer:Framebuffer;
            @:entryPoint static function boot():Void { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "limine.hx"
            ir_path = Path(directory) / "limine.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Framebuffer = type { ptr }", ir)

    def test_accepts_pointer_to_unsized_array_in_typedef(self):
        source = """
        typedef VideoMode = { var width:UIntSize<64>; };
        typedef Response = { var modes:Ptr<Array<VideoMode>>; };
        class Kernel {
            @:elfSection(".requests") static var response:Response;
            @:entryPoint static function boot():Void { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "response.hx"
            ir_path = Path(directory) / "response.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Response = type { ptr }", ir)

    def test_emits_standalone_typedef_module(self):
        source = "typedef Screen = { var width:UIntSize<64>; var pixels:FixedArray<UIntSize<8>, 16>; };"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "screen.hx"
            object_path = Path(directory) / "screen.o"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            sections = subprocess.run(["llvm-readelf", "-S", object_path], capture_output=True, text=True, check=True).stdout
        self.assertIn(".symtab", sections)

    def test_requires_typedef_semicolon(self):
        source = "typedef Screen = { var width:UIntSize<64>; }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "screen.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "screen.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected ';'", result.stderr)

    def test_lowers_function_local_static_variable(self):
        source = "class Main { @:entryPoint static function kernel():Void { static var uart:Int = 0x3F8; asm(\"out $0, $1\", \"{dx},{al},~{dirflag},~{fpsr},~{flags}\", uart, 65); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kernel.hx"
            ir_path = Path(directory) / "kernel.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@uart = private global i64 1016", ir)

    def test_lowers_generic_inline_asm_operands_and_constraints(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var left:Int = 1;
                var right:Int = 2;
                asm("cmp $0, $1", "r,r,~{flags}", left, right);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "asm.hx"
            ir_path = Path(directory) / "asm.ll"
            assembly_path = Path(directory) / "asm.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", assembly_path], check=True)
            ir = ir_path.read_text()
            assembly = assembly_path.read_text()
        self.assertIn('asm sideeffect inteldialect "cmp $0, $1", "r,r,~{flags}"', ir)
        self.assertIn(".intel_syntax noprefix", assembly)
        self.assertRegex(assembly, r"\bcmp\s+[a-z]+, [a-z]+")

    def test_narrows_fixed_register_inline_asm_operands(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                asm("out $0, $1", "{dx},{al},~{dirflag},~{fpsr},~{flags}", 0x64, 0xAE);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "out.hx"
            assembly_path = Path(directory) / "out.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", assembly_path], check=True)
            assembly = assembly_path.read_text()
        self.assertRegex(assembly, r"\bout\s+dx, al")

    def test_lowers_generic_inline_asm_output_operand(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var value:UIntSize<8>;
                asm("mov $0, $1", "=r,r", value, 42);
                trace(value);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "asm-output.hx"
            ir_path = Path(directory) / "asm-output.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn('call i8 asm sideeffect inteldialect "mov $0, $1", "=r,r"(i32 42)', ir)
        self.assertIn("store i8", ir)

    def test_lowers_generic_inline_asm_memory_operand(self):
        source = """
        typedef Descriptor = { var limit:UIntSize<16>; var base:Ptr<Dynamic>; };
        class Main {
            static var descriptor:Descriptor;
            @:entryPoint static function main():Void {
                asm("lgdt [$0]", "m,~{memory}", descriptor);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "lgdt.hx"
            assembly_path = Path(directory) / "lgdt.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", assembly_path], check=True)
            assembly = assembly_path.read_text()
        self.assertIn("lgdt", assembly)

    def test_inline_asm_memory_operand_accepts_object_field(self):
        source = """
        typedef Descriptor = { var limit:UIntSize<16>; var base:Ptr<Dynamic>; };
        class Table {
            var pointer:Descriptor;
            public function new() {}
            public function load():Void {
                asm("lidt [$0]", "m,~{memory}", this.pointer);
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var table = new Table();
                table.load();
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "lidt-field.hx"
            ir_path = Path(directory) / "lidt-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn('call void asm sideeffect inteldialect "lidt $0"', ir)
        self.assertIn("*m,~{memory}", ir)

    def test_aligned_local_fixed_array_sets_alloca_align(self):
        source = """
        function boot():Void {
            @:aligned(16)
            var region: FixedArray<UIntSize<8>, 512>;
            asm("fxsave [$0]", "m,~{memory}", region);
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "aligned.hx"
            ir_path = Path(directory) / "aligned.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertRegex(ir, r"alloca \[512 x i8\], align 16")

    def test_intsize_128_uses_sysv_align_16_on_host(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var ticks:IntSize<128> = 1;
                ticks = ticks + 2;
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "i128-align.hx"
            ir_path = Path(directory) / "i128-align.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("i128:128", ir)
        self.assertRegex(ir, r"alloca i128, align 16")
        self.assertRegex(ir, r"store i128 .*, ptr %ticks, align 16")
        self.assertRegex(ir, r"load i128, ptr %ticks, align 16")

    def test_intsize_128_div_uses_freestanding_helper_not_libgcc(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var n:IntSize<128> = 150000000;
                var q:IntSize<128> = n / 10000000;
                var r:IntSize<128> = n % 10000000;
                trace(cast(q, IntSize<64>));
                trace(cast(r, IntSize<64>));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "i128-div.hx"
            ir_path = Path(directory) / "i128-div.ll"
            object_path = Path(directory) / "i128-div.o"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            ir = ir_path.read_text()
            symbols = subprocess.run(["nm", object_path], capture_output=True, text=True, check=True).stdout
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertIn("__haxellvm_i128_udivmod", ir)
        self.assertNotIn("__divti3", ir)
        self.assertNotIn("__modti3", ir)
        self.assertNotIn("sdiv i128", ir)
        self.assertNotRegex(symbols, r"U __divti3")
        self.assertNotRegex(symbols, r"U __modti3")
        self.assertEqual(result.stdout, "15\n0\n")

    def test_multi_output_cpuid_asm_writes_all_register_locals(self):
        source = """
        function boot():Void {
            var eax: UIntSize<32>;
            var ebx: UIntSize<32>;
            var ecx: UIntSize<32>;
            var edx: UIntSize<32>;
            asm("cpuid", "={eax},={ebx},={ecx},={edx},{eax}", eax, ebx, ecx, edx, 1);
            if ((edx & (1 << 25)) == 0) { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "cpuid.hx"
            ir_path = Path(directory) / "cpuid.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertRegex(ir, r"call \{ i32, i32, i32, i32 \} asm")
        self.assertIn("={eax},={ebx},={ecx},={edx},{eax}", ir)
        self.assertIn("extractvalue { i32, i32, i32, i32 }", ir)

    def test_marks_written_global_fixed_array_mutable(self):
        source = """
        typedef Entry = { var value:UIntSize<16>; };
        var entries:FixedArray<Entry, 1>;
        class Main {
            @:entryPoint static function main():Void {
                entries[0] = { value: 42 };
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "entries.hx"
            ir_path = Path(directory) / "entries.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@entries = global [1 x %Entry] zeroinitializer", ir)
        self.assertNotIn("@entries = constant", ir)

    def test_keeps_64_bit_operands_wide_through_shifts_and_masks(self):
        source = """
        class Gates {
            @:entryPoint static function main():Void {
                var address:UIntSize<64> = 0xFFFFFFFF80010C44;
                var high:UIntSize<32> = (address >> 32) & 0xFFFFFFFF;
                var middle:UIntSize<16> = (address >> 16) & 0xFFFF;
                var low:UIntSize<16> = address & 0xFFFF;
                trace(high == 0xFFFFFFFF);
                trace(middle);
                trace(low);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "gates.hx"
            ir_path = Path(directory) / "gates.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "true\n32769\n3140\n")
        self.assertIn("ashr i64", ir)

    def test_lowers_signed_and_unsigned_sized_integers(self):
        source = """
        class Widths {
            @:entryPoint @:retainFunctionName static function main():Void {
                var byte:UIntSize<8> = 255;
                var signed:IntSize<16> = -2;
                byte = byte + 1;
                trace(byte);
                trace(signed);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "widths.hx"
            ir_path = Path(directory) / "widths.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "0\n-2\n")
        self.assertIn("alloca i8", ir)
        self.assertIn("alloca i16", ir)
        self.assertIn("zext i8", ir)
        self.assertIn("sext i16", ir)

    def test_zero_initializes_typed_local_without_initializer(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var commandByte:UIntSize<8>;
                trace(commandByte);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "local.hx"
            ir_path = Path(directory) / "local.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "0\n")
        self.assertIn("alloca i8", ir)

    def test_lowers_integer_bitwise_shift_modulo_and_compound_operators(self):
        source = """
        class Operators {
            @:entryPoint static function main():Void {
                var value:Int = 13 % 5;
                value |= 8;
                value ^= 1;
                value &= 14;
                value <<= 1;
                value >>= 2;
                trace(value);
                var unsigned:UIntSize<8> = 128;
                unsigned >>>= 7;
                trace(unsigned);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "operators.hx"
            ir_path = Path(directory) / "operators.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "5\n1\n")
        for instruction in ("or i64", "xor i64", "and i64", "shl i64", "ashr i64", "lshr i8"):
            self.assertIn(instruction, ir)

    def test_lowers_unary_bitwise_complement(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var byte:UIntSize<8> = 0xF0;
                trace(~byte & 0xFF);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bitnot.hx"
            ir_path = Path(directory) / "bitnot.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "15\n")
        self.assertIn("xor i32", ir)

    def test_lowers_break_and_continue_for_while_and_for_loops(self):
        source = """
        class Loops {
            @:entryPoint static function main():Void {
                var value:Int = 0;
                while (value < 5) {
                    value++;
                    if (value == 2) { continue; }
                    trace(value);
                    if (value == 3) { break; }
                }
                for (index in 0...5) {
                    if (index == 1) { continue; }
                    trace(index);
                    if (index == 3) { break; }
                }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "loops.hx"
            ir_path = Path(directory) / "loops.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1\n3\n0\n2\n3\n")

    def test_while_condition_block_expression(self):
        source = """
        typedef Point = { var x:Int; var y:Int; };
        function bump(value:Int):Int { return value + 1; }
        class Main {
            @:entryPoint static function main():Void {
                var n:Int = 0;
                while ({
                    n = bump(n);
                    n < 3
                }) {
                }
                trace(n);
                var point:Point = { x: 1, y: 2 };
                trace(point.x);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "block-while.hx"
            ir_path = Path(directory) / "block-while.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "3\n1\n")

    def test_lowers_integer_switch_cases_and_default(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                for (value in 1...4) {
                    switch (value) {
                        case 1:
                            trace("one");
                        case 2:
                            trace("two");
                            break;
                        default:
                            trace("other");
                    }
                }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "switch.hx"
            ir_path = Path(directory) / "switch.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "one\ntwo\nother\n")

    def test_lowers_c_string_integer_conversion_and_concatenation(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var value:UIntSize<16> = 0x2af;
                trace("value=" + value.toString(16) + ", decimal=" + Std.string(value));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "string-conversion.hx"
            ir_path = Path(directory) / "string-conversion.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "-target", "x86_64-unknown-linux-gnu", "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "value=2af, decimal=687\n")
        self.assertIn("rep movsb", ir)
        self.assertNotIn("llvm.memcpy", ir)

    def test_x86_string_padding_uses_rep_stosb(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                trace(StringTools.lpad("x", 3, " "));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "string-lpad.hx"
            ir_path = Path(directory) / "string-lpad.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "-target", "x86_64-unknown-linux-gnu", "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "  x\n")
        self.assertIn("rep stosb", ir)
        self.assertIn("rep movsb", ir)
        self.assertNotIn("llvm.memset", ir)

    def test_non_x86_string_concat_keeps_llvm_memcpy(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                trace("a" + "b");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "string-aarch64.hx"
            ir_path = Path(directory) / "string-aarch64.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "-target", "aarch64-unknown-linux-gnu", "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("llvm.memcpy", ir)
        self.assertNotIn("rep movsb", ir)
    def test_fixed_array_tostring_and_record_length_field(self):
        source = """
        typedef Hdr = { var length:UIntSize<32>; };
        class Main {
            @:entryPoint static function main():Void {
                var signature:FixedArray<UIntSize<8>, 4> = [82, 83, 68, 0];
                trace(signature.toString());
                var h:Hdr = { length: 7 };
                var p:Ptr<Hdr> = &h;
                trace(p.length.toString());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "fixed-tostring.hx"
            ir_path = Path(directory) / "fixed-tostring.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "RSD\n7\n")

    def test_tostring_error_points_at_call_not_file_start(self):
        source = """
        // header comment should not be the caret target
        typedef Hdr = { var length:UIntSize<32>; };
        class Main {
            @:entryPoint static function main():Void {
                var h:Hdr = { length: 1 };
                trace(h.toString());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-tostring.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("toString requires an integer, String, or byte FixedArray receiver", result.stderr)
        self.assertIn("h.toString()", result.stderr)
        self.assertNotIn("header comment should not be the caret target\n^", result.stderr)

    def test_trace_inside_method_without_entry_point_does_not_segfault(self):
        source = """
        class Clock {
            public function new() {
                var hours:UIntSize<8> = 12;
                trace("time=" + hours.toString(10));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "clock.hx"
            object_path = Path(directory) / "clock.o"
            ir_path = Path(directory) / "clock.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare i32 @puts(ptr)", ir)
        self.assertIn("call i32 @puts", ir)

    def test_lowers_unit_enums_in_parameters_and_switches(self):
        source = """
        enum Severity {
            Trace;
            Warning;
            Fatal;
        }
        class Log {
            public static function Code(severity:Severity):Int {
                switch (severity) {
                    case Trace: return 1;
                    case Warning: return 2;
                    case Fatal: return 3;
                }
                return 0;
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                trace(Log.Code(Warning));
                trace(Log.Code(Fatal));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "enum.hx"
            ir_path = Path(directory) / "enum.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\n3\n")

    def test_lowers_parameterized_enum_constructors_and_switch(self):
        source = """
        enum Kind {
            Empty;
            Named(label:String);
        }
        class Main {
            @:entryPoint static function main():Void {
                var empty:Kind = Kind.Empty;
                var named:Kind = Kind.Named("ok");
                switch (empty) {
                    case Kind.Empty: trace(1);
                    case Kind.Named(_): trace(0);
                }
                switch (named) {
                    case Empty: trace(0);
                    case Named(_): trace(2);
                }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "enum-payload.hx"
            ir_path = Path(directory) / "enum-payload.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "1\n2\n")
        self.assertIn("%Kind = type { i32, ptr }", ir)

    def test_lpads_and_rpads_c_strings(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                trace(StringTools.lpad("2af", 5, "0"));
                trace(StringTools.rpad("ok", 5, "."));
                trace(StringTools.lpad("wide", 2, "0"));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "string-pad.hx"
            ir_path = Path(directory) / "string-pad.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "002af\nok...\nwide\n")

    def test_reuses_string_scratch_for_chained_concatenation(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                trace("a" + "b" + "c" + "d" + "e");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "string-scratch.hx"
            ir_path = Path(directory) / "string-scratch.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "abcde\n")
        self.assertEqual(ir.count("alloca [1024 x i8]"), 1)

    def test_switch_without_default_skips_unmatched_values(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                switch (3) {
                    case 1:
                        trace("bad");
                    case 2:
                        trace("also bad");
                }
                trace("done");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "switch-no-default.hx"
            ir_path = Path(directory) / "switch-no-default.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "done\n")

    def test_lowers_do_while_with_break_and_continue(self):
        source = """
        class Loops {
            @:entryPoint static function main():Void {
                var once:Int = 0;
                do {
                    once++;
                } while (false);
                trace(once);
                var value:Int = 0;
                do {
                    value++;
                    if (value == 2) { continue; }
                    trace(value);
                    if (value == 3) { break; }
                } while (value < 5);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "do-while.hx"
            ir_path = Path(directory) / "do-while.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1\n1\n3\n")

    def test_assigns_global_record_and_indexed_fixed_array_fields(self):
        source = """
        typedef Entry = { var low:UIntSize<16>; var flags:UIntSize<8>; };
        typedef Pointer = { var limit:UIntSize<16>; var base:Ptr<Entry>; };
        var entries:FixedArray<Entry, 1>;
        var pointer:Pointer;
        class Main {
            @:entryPoint static function main():Void {
                entries[0] = { low: 0x12345, flags: 1 };
                entries[0].flags |= 0x80;
                pointer.limit = entries[0].low;
                pointer.base = &entries[0];
                trace(pointer.limit);
                trace(entries[0].flags);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "records.hx"
            ir_path = Path(directory) / "records.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "9029\n129\n")

    def test_concatenates_typed_pointer_as_hex_address_not_cstring(self):
        source = """
        class Holder {
            public var addr:Ptr<UIntSize<64>>;
            public function new(value:UIntSize<64>) {
                this.addr = cast(value, Ptr<UIntSize<64>>);
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var holder:Holder = new Holder(0x1000);
                trace("addr: " + holder.addr);
                var local:Ptr<UIntSize<8>> = cast(0x20, Ptr<UIntSize<8>>);
                trace("local: " + local);
                trace("text: " + "ok");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "ptr-concat.hx"
            ir_path = Path(directory) / "ptr-concat.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "addr: 1000\nlocal: 20\ntext: ok\n")

    def test_typed_pointer_plus_integer_is_arithmetic_not_concat(self):
        source = """
        class Bitmap {
            public var base:Ptr<UIntSize<8>>;
            public var size:UIntSize<64>;
            public function new(base:Ptr<UIntSize<8>>, size:UIntSize<64>) {
                this.base = base;
                this.size = size;
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var bmp:Bitmap = new Bitmap(cast(0x1000, Ptr<UIntSize<8>>), 0x20);
                var end:Ptr<UIntSize<8>> = bmp.base + bmp.size;
                var also:Ptr<UIntSize<8>> = bmp.base + 16;
                var flipped:Ptr<UIntSize<8>> = 8 + bmp.base;
                var local:Ptr<UIntSize<8>> = cast(0x2000, Ptr<UIntSize<8>>);
                var stepped:Ptr<UIntSize<8>> = local + 0x10;
                trace("" + end);
                trace("" + also);
                trace("" + flipped);
                trace("" + stepped);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "ptr-add.hx"
            ir_path = Path(directory) / "ptr-add.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            self.assertIn("ptr.add", ir)
            self.assertIn("getelementptr", ir)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1020\n1010\n1008\n2010\n")

    def test_uint64_and_hhdm_compares_stay_i64_not_truncated_to_i32(self):
        source = """
        var hhdmOffset:UIntSize<64> = 0xffff800000000000;
        class Checker {
            var bitmap:Ptr<UIntSize<8>> = null;
            private function ptrValid(ptr:Ptr<Void>):Bool {
                return ptr != null && hhdmOffset != 0 && cast(ptr, UIntSize<64>) >= hhdmOffset;
            }
            public function new() {
                this.bitmap = cast(hhdmOffset + 0x1000, Ptr<UIntSize<8>>);
                if (!this.ptrValid(this.bitmap)) trace("fail");
                else trace("ok");
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var checker:Checker = new Checker();
                var high:UIntSize<64> = 0xffff800000001000;
                var low:UIntSize<64> = 0xffff800000000000;
                if (high >= low && low != 0) trace(1); else trace(0);
                var signed:Int = -2;
                if (signed < 0) trace(1); else trace(0);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "hhdm-cmp.hx"
            ir_path = Path(directory) / "hhdm-cmp.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            self.assertNotIn("trunc i64", ir)
            self.assertIn("icmp ne i64", ir)
            self.assertIn("icmp uge i64", ir)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "ok\n1\n1\n")

    def test_address_of_pointer_minus_offset_and_integer_plus_offset(self):
        source = """
        var hhdm:UIntSize<64> = 1000;
        class Main {
            @:entryPoint static function main():Void {
                var virt:Ptr<UIntSize<8>> = cast(5000, Ptr<UIntSize<8>>);
                var phys:UIntSize<64> = &(virt - hhdm);
                var back:Ptr<UIntSize<8>> = &(phys + hhdm);
                trace(phys);
                if (back != null) trace(1);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "addr.hx"
            ir_path = Path(directory) / "addr.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "4000\n1\n")

    def test_assigns_record_literal_to_typedef_global(self):
        source = """
        typedef Reset = { var reg:UIntSize<64>; var value:UIntSize<8>; };
        var details:Reset;
        class Main {
            @:entryPoint static function main():Void {
                details = { reg: 0x1000, value: 0x06 };
                trace(details.reg);
                trace(details.value);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "reset-global.hx"
            ir_path = Path(directory) / "reset-global.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "4096\n6\n")

    def test_evaluates_typedef_sizeof(self):
        source = """
        typedef GDTEntry = {
            var limit_low:UIntSize<16>;
            var base_low:UIntSize<16>;
            var base_middle:UIntSize<8>;
            var access:UIntSize<8>;
            var limit_high_flags:UIntSize<8>;
            var base_high:UIntSize<8>;
        };
        class Main {
            @:entryPoint static function main():Void {
                trace(sizeof(GDTEntry));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "sizeof.hx"
            ir_path = Path(directory) / "sizeof.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "8\n")

    def test_flexible_fixed_array_trailing_field_and_sizeof_width(self):
        source = """
        @:packed
        typedef Header = {
            var length:UIntSize<32>;
            var pad:UIntSize<32>;
        };
        @:packed
        typedef Table = {
            var header:Header;
            var entries:FixedArray<UIntSize<64>, header.length - sizeof(Header) / sizeof(UIntSize<64>)>;
        };
        class Main {
            @:entryPoint static function main():Void {
                trace(sizeof(Header));
                trace(sizeof(UIntSize<64>));
                var table:Ptr<Table> = cast(0, Ptr<Table>);
                if (table != null) trace(table.entries[0]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "xsdt-flex.hx"
            ir_path = Path(directory) / "xsdt-flex.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Table = type <{ %Header, [0 x i64] }>", ir)
        self.assertIn("getelementptr (%Header, ptr null, i32 1)", ir)
        self.assertIn("getelementptr (i64, ptr null, i32 1)", ir)
        self.assertIn("getelementptr [0 x i64], ptr %field, i32 0, i32 0", ir)

    def test_indexes_flexible_array_through_typed_pointer_class_field(self):
        source = """
        @:packed
        typedef Header = { var length:UIntSize<32>; var pad:UIntSize<32>; };
        @:packed
        typedef Table = {
            var header:Header;
            var entries:FixedArray<UIntSize<64>, header.length - sizeof(Header) / sizeof(UIntSize<64>)>;
        };
        class Holder {
            var table:Ptr<Table>;
            public function new(table:Ptr<Table>) {
                this.table = table;
                trace(this.table.entries[0]);
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var storage:FixedArray<UIntSize<64>, 2> = [0, 99];
                new Holder(cast(&storage[0], Ptr<Table>));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "this-ptr-flex-index.hx"
            ir_path = Path(directory) / "this-ptr-flex-index.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "99\n")

    def test_fixed_array_zero_length_uses_sourcemgr(self):
        source = "typedef Bad = { var bytes:FixedArray<UIntSize<8>, 0>; };\nclass Main { @:entryPoint static function main():Void {} }\n"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-fixed.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FixedArray dimensions must be positive", result.stderr)
        self.assertIn(str(source_path), result.stderr)
        self.assertIn("^", result.stderr)

    def test_compares_byte_fixed_array_with_string_literal(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var signature:FixedArray<UIntSize<8>, 4> = [70, 65, 67, 80];
                if (signature == "FACP") trace("yes");
                if (signature != "RSDP") trace("ok");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "sig.hx"
            ir_path = Path(directory) / "sig.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "yes\nok\n")

    def test_compares_typedef_byte_field_with_string_literal(self):
        source = """
        typedef Header = { var signature:FixedArray<UIntSize<8>, 4>; };
        class Main {
            @:entryPoint static function main():Void {
                var signature:FixedArray<UIntSize<8>, 4> = [70, 65, 67, 80];
                var header:Header;
                header.signature = signature;
                if (header.signature == "FACP") trace("yes");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "sig-field.hx"
            ir_path = Path(directory) / "sig-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "yes\n")

    def test_pointer_compare_error_underlines_the_comparison(self):
        source = 'class Main { @:entryPoint static function main():Void { var p:Ptr<UIntSize<8>> = null; if (p == 1) {} } }\n'
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "ptr-cmp.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot compare a pointer with a non-pointer", result.stderr)
        self.assertIn("p == 1", result.stderr)
        self.assertNotIn(":1:1: error: cannot compare a pointer", result.stderr)

    def test_indexes_local_string_fixed_array_with_wide_index(self):
        source = """
        typedef Frame = { var vector:UIntSize<64>; };
        class Main {
            @:entryPoint static function main():Void {
                var frame:Frame = { vector: 13 };
                var names:FixedArray<String, 14> = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "gp"];
                trace("Exception: " + names[frame.vector]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "vector-names.hx"
            ir_path = Path(directory) / "vector-names.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "Exception: gp\n")

    def test_lowers_fixed_array_literal_indexing_and_assignment(self):
        source = """
        class Arrays {
            @:entryPoint @:retainFunctionName static function main():Void {
                var bytes:FixedArray<UIntSize<8>, 4> = [1, 2, 3, 4];
                bytes[1] = 258;
                trace(bytes[1]);
                trace(bytes[3]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "arrays.hx"
            ir_path = Path(directory) / "arrays.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "2\n4\n")
        self.assertIn("alloca [4 x i8]", ir)
        self.assertIn("getelementptr [4 x i8]", ir)

    def test_passes_fixed_array_parameters_by_reference(self):
        source = """
        class Main {
            static function write(entries:FixedArray<UIntSize<8>, 1>):Void {
                entries[0] = 42;
            }
            @:entryPoint static function main():Void {
                var entries:FixedArray<UIntSize<8>, 1> = [0];
                write(entries);
                trace(entries[0]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "fixed-array-reference.hx"
            ir_path = Path(directory) / "fixed-array-reference.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_lowers_uninitialized_fixed_array_of_typedefs(self):
        source = """
        class Tables {
            typedef Entry = { var value:UIntSize<64>; };
            @:entryPoint static function main():Void {
                var entries:FixedArray<Entry, 5>;
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "tables.hx"
            ir_path = Path(directory) / "tables.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("alloca [5 x %Entry]", ir)

    def test_lowers_uninitialized_named_fixed_array_global_in_standalone_module(self):
        source = """
        typedef Entry = { var value:UIntSize<64>; };
        var entries:FixedArray<Entry, 5>;
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "tables.hx"
            ir_path = Path(directory) / "tables.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@entries = global [5 x %Entry] zeroinitializer", ir)

    def test_lowers_uninitialized_standalone_global_as_writable_storage(self):
        source = "typedef Context = { var value:UIntSize<64>; }; var context:Ptr<Context>;"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "context.hx"
            ir_path = Path(directory) / "context.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@context = global ptr null", ir)

    def test_assigns_and_reads_indexed_global_fixed_array_record_fields(self):
        source = """
        typedef Entry = { var value:UIntSize<16>; };
        @:mutable var entries:FixedArray<Entry, 2>;
        class Tables {
            @:entryPoint static function main():Void {
                entries[1].value = 258;
                trace(entries[1].value);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "tables.hx"
            ir_path = Path(directory) / "tables.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "258\n")

    def test_resolves_instance_fixed_array_references(self):
        source = """
        typedef Entry = { var value:UIntSize<16>; };
        class Table {
            var entries:FixedArray<Entry, 1>;
            var first:Ptr<Entry>;
            public function new() {
                this.entries[0] = { value: 0x1234 };
                var first:Ptr<Entry> = &this.entries[0];
                this.first = &this.entries[0];
            }
            public function Value():UIntSize<16> { return this.entries[0].value; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var table = new Table();
                trace(table.Value());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "this-global-fixed-array.hx"
            ir_path = Path(directory) / "this-global-fixed-array.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "4660\n")

    def test_assigns_record_literal_to_instance_field(self):
        source = """
        typedef Pointer = { var limit:UIntSize<16>; };
        class Table {
            var pointer:Pointer;
            public function new() { this.pointer = { limit: 0x1234 }; }
            public function Limit():UIntSize<16> { return this.pointer.limit; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var table = new Table();
                trace(table.Limit());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "instance-record-field.hx"
            ir_path = Path(directory) / "instance-record-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "4660\n")

    def test_lowers_typed_pointer_address_of_and_dereference(self):
        source = """
        class Pointers {
            @:entryPoint static function main():Void {
                var value:IntSize<16> = -2;
                var pointer:Ptr<IntSize<16>> = &value;
                *pointer = -7;
                trace(*pointer);
                var byte:UIntSize<8> = 9;
                var bytePointer:Ptr<UIntSize<8>> = &byte;
                trace(*bytePointer);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointers.hx"
            ir_path = Path(directory) / "pointers.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "-7\n9\n")
        self.assertIn("alloca ptr", ir)
        self.assertIn("store ptr %value", ir)
        self.assertIn("store i16 -7", ir)
        self.assertIn("sext i16", ir)

    def test_dereferences_null_and_integer_addresses_as_bytes(self):
        fault = """
        class Main {
            @:entryPoint static function main():Void {
                var test:Ptr<Void> = *null;
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "nullptr.hx"
            ir_path = Path(directory) / "nullptr.ll"
            source_path.write_text(fault)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("load volatile i8, ptr null", ir)

        source = """
        class Main {
            @:entryPoint static function main():Void {
                var cell:UIntSize<8> = 42;
                var addr:UIntSize<64> = cast(&cell, UIntSize<64>);
                trace(*addr);
                trace(*(addr + 0));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "addr-deref.hx"
            ir_path = Path(directory) / "addr-deref.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "42\n42\n")
        self.assertIn("inttoptr", ir)
        self.assertIn("load volatile i8", ir)

    def test_compound_assigns_through_cast_pointer_expression(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var cell:UIntSize<64> = 2;
                *cast(cast(&cell, UIntSize<64>), Ptr<UIntSize<64>>) |= 1;
                trace(cell);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "cast-deref-assign.hx"
            ir_path = Path(directory) / "cast-deref-assign.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "3\n")
        self.assertIn("or i64", ir)

    def test_dereferences_typed_pointer_class_field(self):
        source = """
        @:packed typedef Entry = { var value:UIntSize<16>; };
        class Holder {
            var entry:Ptr<Entry>;
            public function new(entry:Ptr<Entry>) {
                this.entry = entry;
                var copy:Entry = *this.entry;
                trace(copy.value);
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var entry:Entry = { value: 42 };
                new Holder(&entry);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "class-field-deref.hx"
            ir_path = Path(directory) / "class-field-deref.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_reads_fields_and_byte_arrays_through_typed_pointer_class_field(self):
        source = """
        @:packed typedef Entry = {
            var signature:FixedArray<UIntSize<8>, 8>;
            var value:UIntSize<16>;
        };
        class Holder {
            var entry:Ptr<Entry>;
            public function new(entry:Ptr<Entry>) {
                this.entry = entry;
                trace("sig=" + this.entry.signature);
                trace(this.entry.value);
            }
        }
        class Main {
            @:entryPoint static function main():Void {
                var entry:Entry = { signature: [82, 83, 68, 32, 80, 84, 82, 32], value: 7 };
                new Holder(&entry);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "ptr-field-walk.hx"
            ir_path = Path(directory) / "ptr-field-walk.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "sig=RSD PTR \n7\n")

    def test_assigns_record_literal_to_typedef_parameter_and_instance_field(self):
        source = """
        typedef Tss = { var rsp0:UIntSize<64>; var ioMapBase:UIntSize<16>; };
        class Holder {
            var tss:Tss;
            public function new(tss:Tss) {
                tss = { rsp0: 0, ioMapBase: sizeof(Tss) };
                this.tss = tss;
            }
            public function IoMapBase():UIntSize<16> { return this.tss.ioMapBase; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var initial:Tss = { rsp0: 1, ioMapBase: 2 };
                var holder = new Holder(initial);
                trace(holder.IoMapBase());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "parameter-record.hx"
            ir_path = Path(directory) / "parameter-record.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "16\n")

    def test_assigns_through_typed_pointer_record_field(self):
        source = """
        typedef Entry = { var value:UIntSize<16>; };
        class Main {
            @:mutable static var entry:Entry = { value: 0 };
            @:entryPoint static function main():Void {
                var pointer:Ptr<Entry> = &entry;
                pointer.value = 42;
                trace(pointer.value);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointer-field.hx"
            ir_path = Path(directory) / "pointer-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_assigns_through_indexed_typed_pointer_record_field(self):
        source = """
        typedef Entry = { var value:UIntSize<16>; };
        class Main {
            @:entryPoint static function main():Void {
                var entries:FixedArray<Entry, 2>;
                var pointer:Ptr<Entry> = &entries[0];
                pointer[1].value = 42;
                trace(pointer[1].value);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointer-index-field.hx"
            ir_path = Path(directory) / "pointer-index-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_indexes_typed_pointer_function_parameters(self):
        source = """
        static function copy(dest:Ptr<UIntType<8>>, source:Ptr<UIntType<8>>, count:UIntType<64>):Ptr<UIntType<8>> {
            var index:UIntType<64> = 0;
            while (index < count) {
                dest[index] = source[index];
                index += 1;
            }
            return dest;
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "copy.hx"
            ir_path = Path(directory) / "copy.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("define ptr @copy(ptr", ir)
        self.assertIn("getelementptr i8, ptr", ir)
        self.assertIn("store i8", ir)

    def test_calls_methods_on_global_typed_class_pointers(self):
        source = """
        class Counter {
            var value:UIntSize<32> = 0;
            public function new() {}
            public function bump():UIntSize<32> {
                this.value++;
                return this.value;
            }
        }
        var counter:Ptr<Counter>;
        class Main {
            @:entryPoint static function main():Void {
                counter = new Counter();
                trace(counter.bump());
                trace(counter.bump());
                var local:Ptr<Counter> = new Counter();
                trace(local.bump());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "ptr-method.hx"
            ir_path = Path(directory) / "ptr-method.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1\n2\n1\n")

    def test_indexes_typed_pointer_instance_fields(self):
        source = """
        class Buffer {
            var bytes:Ptr<UIntSize<8>>;
            public function new(bytes:Ptr<UIntSize<8>>) { this.bytes = bytes; }
            public function Set(index:UIntSize<64>, value:UIntSize<8>):Void { this.bytes[index] = value; }
            public function ClearBit(index:UIntSize<64>, bit:UIntSize<8>):Void { this.bytes[index] &= ~(1 << bit); }
        }
        class Main {
            @:entryPoint static function main():Void {
                var bytes:FixedArray<UIntSize<8>, 1> = [15];
                var buffer = new Buffer(&bytes[0]);
                buffer.ClearBit(0, 0);
                trace(bytes[0]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointer-field-index.hx"
            ir_path = Path(directory) / "pointer-field-index.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "14\n")

    def test_increments_and_decrements_indexed_pointer_fields(self):
        source = """
        class Refs {
            var pageRefs:Ptr<UIntSize<16>>;
            public function new(pageRefs:Ptr<UIntSize<16>>) { this.pageRefs = pageRefs; }
            public function bump(page:UIntSize<64>):Void { this.pageRefs[page]++; }
            public function drop(page:UIntSize<64>):Void { this.pageRefs[page]--; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var refs:FixedArray<UIntSize<16>, 2> = [3, 2];
                var table = new Refs(&refs[0]);
                table.bump(0);
                table.drop(1);
                refs[0] += 2;
                refs[1]--;
                trace(refs[0]);
                trace(refs[1]);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "index-incdec.hx"
            ir_path = Path(directory) / "index-incdec.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "6\n0\n")
        self.assertIn("load i16", ir)
        self.assertIn("add i16", ir)
        self.assertIn("sub i16", ir)

    def test_index_increment_error_underlines_the_target(self):
        source = 'class Main { @:entryPoint static function main():Void { (1 + 2)++; } }\n'
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-inc.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("increment and decrement require an assignable target", result.stderr)
        self.assertIn("1 + 2", result.stderr)
        self.assertNotIn("haxellvm: increment and decrement require a variable name", result.stderr)

    def test_lowers_named_typedef_pointer_and_typed_null(self):
        source = """
        class Kernel {
            typedef Response = { var count:UIntSize<64>; };
            typedef Request = { var response:Ptr<Response>; };
            @:mutable static var request:Request = { response: null };
            @:entryPoint static function main():Void {
                if (request.response == null) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointer-record.hx"
            ir_path = Path(directory) / "pointer-record.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Request = type { ptr }", ir)
        self.assertIn("@request = private global %Request zeroinitializer", ir)
        self.assertIn("icmp eq ptr", ir)

    def test_traverses_through_named_pointer_record_fields(self):
        source = """
        class Kernel {
            typedef Response = { var count:UIntSize<64>; };
            typedef Request = { var response:Ptr<Response>; };
            @:mutable static var request:Request = { response: null };
            @:entryPoint static function boot():Void {
                if (request.response.count == 0) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointer-path.hx"
            ir_path = Path(directory) / "pointer-path.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertRegex(ir, r"load ptr, ptr (?:@request|%field)")
        self.assertRegex(ir, r"getelementptr inbounds(?: nuw)? %Response, ptr")
        self.assertIn("load i64, ptr", ir)

    def test_traverses_global_ptr_to_typedef_fields(self):
        source = """
        typedef Framebuffer = {
            var width: UIntSize<64>;
            var height: UIntSize<64>;
        };
        var framebuffer: Ptr<Framebuffer>;
        function boot():Void {
            if (framebuffer.width == 0) { asm("nop"); }
            if (framebuffer.height == 0) { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "global-ptr-field.hx"
            ir_path = Path(directory) / "global-ptr-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@framebuffer = global ptr null", ir)
        self.assertRegex(ir, r"load ptr, ptr @framebuffer")
        self.assertRegex(ir, r"getelementptr inbounds(?: nuw)? %Framebuffer, ptr")
        self.assertIn("load i64, ptr", ir)

    def test_passes_global_ptr_fixed_array_field_as_argument(self):
        source = """
        typedef Params = {
            var colors: FixedArray<UIntSize<32>, 8>;
        };
        var params: Ptr<Params>;
        @:extern function takeColors(colors: FixedArray<UIntSize<32>, 8>):Void;
        function boot():Void {
            takeColors(params.colors);
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "global-ptr-fixedarray-arg.hx"
            ir_path = Path(directory) / "global-ptr-fixedarray-arg.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertRegex(ir, r"load ptr, ptr @params")
        self.assertRegex(ir, r"getelementptr inbounds(?: nuw)? %Params, ptr")
        self.assertRegex(ir, r"call void @takeColors\(ptr")

    def test_address_of_global_ptr_record_field(self):
        source = """
        typedef Params = {
            var defaultBg: UIntSize<32>;
            var defaultFg: UIntSize<32>;
        };
        var params: Ptr<Params>;
        @:extern function takeColor(color: Ptr<UIntSize<32>>):Void;
        function boot():Void {
            takeColor(&params.defaultBg);
            takeColor(&params.defaultFg);
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "global-ptr-addr-field.hx"
            ir_path = Path(directory) / "global-ptr-addr-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertRegex(ir, r"load ptr, ptr @params")
        self.assertRegex(ir, r"getelementptr inbounds(?: nuw)? %Params, ptr")
        self.assertNotIn("inttoptr", ir)
        self.assertRegex(ir, r"call void @takeColor\(ptr")

    def test_traverses_local_record_fields_after_dynamic_array_index(self):
        source = """
        class Kernel {
            typedef Framebuffer = { var width:UIntSize<64>; };
            typedef Response = { var framebuffers:Array<Framebuffer>; };
            typedef Request = { var response:Ptr<Response>; };
            @:mutable static var request:Request = { response: null };
            @:entryPoint static function boot():Void {
                var framebuffer = request.response.framebuffers[0];
                if (framebuffer.width == 0) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "local-path.hx"
            ir_path = Path(directory) / "local-path.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Framebuffer = type { i64 }", ir)
        self.assertRegex(ir, r"getelementptr inbounds(?: nuw)? %Framebuffer, ptr")
        self.assertIn("load i64, ptr", ir)

    def test_indexes_array_of_typedef_pointers(self):
        source = """
        class Kernel {
            typedef Framebuffer = { var width:UIntSize<64>; };
            typedef Response = { var framebuffers:Array<Ptr<Framebuffer>>; };
            typedef Request = { var response:Ptr<Response>; };
            @:mutable static var request:Request = { response: null };
            @:entryPoint static function boot():Void {
                var framebuffer = request.response.framebuffers[0];
                if (framebuffer.width == 0) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "pointer-array.hx"
            ir_path = Path(directory) / "pointer-array.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Response = type { ptr }", ir)
        self.assertIn("getelementptr ptr, ptr", ir)
        self.assertIn("load ptr, ptr %element", ir)
        self.assertRegex(ir, r"getelementptr inbounds(?: nuw)? %Framebuffer, ptr %framebuffer")

    def test_lowers_raw_address_to_typed_pointer(self):
        source = """
        class Hardware {
            @:entryPoint static function boot():Void {
                var vga:Ptr<UIntSize<16>> = 0xB8000;
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "hardware.hx"
            ir_path = Path(directory) / "hardware.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("inttoptr (i64 753664 to ptr)", ir)

    def test_lowers_haxe_cast_between_pointer_and_sized_integer(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var value:UIntSize<64> = 42;
                var pointer:Ptr<UIntSize<64>> = cast(value, Ptr<UIntSize<64>>);
                trace(cast(pointer, UIntSize<64>));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "cast.hx"
            ir_path = Path(directory) / "cast.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("inttoptr i64", ir)
        self.assertIn("ptrtoint ptr", ir)

    def test_entry_point_retains_its_method_name(self):
        source = "class Kernel { @:entryPoint static function boot():Void { asm(\"nop\"); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kernel.hx"
            ir_path = Path(directory) / "kernel.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("define i32 @boot()", ir)

    def test_emits_function_export_module_without_entry_point(self):
        source = "static function helper(value:UIntSize<64>):Array<Int> { return [value]; }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "exports.hx"
            object_path = Path(directory) / "exports.o"
            source_path.write_text(source)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            artifact = subprocess.run(["file", object_path], capture_output=True, text=True, check=True).stdout
        self.assertIn("relocatable", artifact)

    def test_initializes_instance_fixed_array_of_strings(self):
        source = """
        class Names {
            var VectorNames:FixedArray<String, 3> = ["divide", "debug", "nmi"];
            public function new() {}
            public function Name(index:Int):String { return this.VectorNames[index]; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var names = new Names();
                trace(names.Name(2));
                trace(names.Name(0));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "names.hx"
            ir_path = Path(directory) / "names.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "nmi\ndivide\n")

    def test_reports_unsupported_class_field_type_at_the_field(self):
        source = "class Bad {\n    var weird:FixedArray<Nope, 2>;\n    public function new() {}\n}\nclass Main { @:entryPoint static function main():Void { new Bad(); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "bad-field.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "bad-field.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:2:9: error: unsupported class field type", result.stderr)

    def test_binds_extern_pointer_table_and_retained_dispatcher_for_asm_stubs(self):
        source = """
        typedef Frame = { var r15:UIntSize<64>; var vector:UIntSize<64>; };
        @:extern static var IsrStubTable:FixedArray<Ptr<Void>, 256>;
        @:retainFunctionName static function InterruptDispatch(frame:Ptr<Frame>):Void { asm("nop"); }
        class Main {
            @:entryPoint static function main():Void {
                var stub:Ptr<Void> = IsrStubTable[14];
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "isr.hx"
            ir_path = Path(directory) / "isr.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@IsrStubTable = external global [256 x ptr]", ir)
        self.assertIn("define void @InterruptDispatch(ptr", ir)
        self.assertRegex(ir, r"load ptr, ptr getelementptr \(\[256 x ptr\], ptr @IsrStubTable, i32 0, i32 14\)")

    def test_rejects_retain_function_name_on_instance_methods(self):
        source = "class Idt {\n    @:retainFunctionName public function Dispatch():Void { asm(\"nop\"); }\n}"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "retain.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "retain.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:2:42: error: @:retainFunctionName requires a static function", result.stderr)
        self.assertIn("Dispatch", result.stderr)

    def test_fromclass_changes_extern_instance_method_symbol_owner(self):
        source = """
        class Owner {
            @:extern @:fromclass("Implementation")
            public function SetDescriptor(value:Int):Void;
            public function new() { this.SetDescriptor(7); }
        }
        class Main {
            @:entryPoint static function main():Void { new Owner(); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "fromclass.hx"
            ir_path = Path(directory) / "fromclass.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare void @Implementation_SetDescriptor(ptr, i64)", ir)
        self.assertRegex(ir, r"call void @Implementation_SetDescriptor\(ptr %[^,]+, i64 7\)")
        self.assertNotIn("@Owner_SetDescriptor", ir)

    def test_lowers_extern_function_metadata_to_llvm_declaration(self):
        source = """
        class Kernel {
            @:extern static function hardware_ready():Bool;
            @:entryPoint static function boot():Void {
                if (hardware_ready()) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "extern.hx"
            ir_path = Path(directory) / "extern.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare i1 @hardware_ready()", ir)
        self.assertIn("call i1 @hardware_ready()", ir)
        self.assertNotIn("define i1 @hardware_ready()", ir)

    def test_lowers_extern_function_pointer_parameters(self):
        source = """
        typedef Context = { var state:UIntSize<64>; };
        @:extern
        static function initialize(callback:(UIntSize<64>) -> Ptr<Dynamic>, context:Ptr<Context>):Ptr<Context> {}
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "extern.hx"
            ir_path = Path(directory) / "extern.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare ptr @initialize(ptr, ptr)", ir)

    def test_tracks_extern_pointer_returns_without_string_length(self):
        source = """
        typedef Context = { var state:UIntSize<64>; };
        @:extern static function initialize():Ptr<Context>;
        class Kernel {
            @:entryPoint static function boot():Void {
                var context = initialize();
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "extern-pointer.hx"
            ir_path = Path(directory) / "extern-pointer.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("call ptr @initialize()", ir)
        self.assertIn("%context = alloca ptr", ir)

    def test_computes_string_length_for_dynamic_function_parameters(self):
        source = """
        class Kernel {
            static function write(value):Void {
                var text = Std.string(value);
                for (index in 0...text.length) { asm("nop"); }
            }
            @:entryPoint static function main():Void { write("hello"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "dynamic-string.hx"
            ir_path = Path(directory) / "dynamic-string.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("define internal i32 @haxellvm_string_length(ptr", ir)
        self.assertIn("call i32 @haxellvm_string_length", ir)

    def test_resolves_imported_typedefs_and_constant_helpers(self):
        module = """
        static function revision(value:Int):Array<Int> { return [value]; }
        typedef Request = { var id:UIntSize<64>; };
        """
        source = """
        import limine;
        class Kernel {
            @:elfSection(".requests") static var request:limine.Request = { id: 6 };
            @:elfSection(".requests") static var revision_data:Array<Int> = limine.revision(7);
            @:entryPoint static function boot():Void { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "limine.hx"
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("%Request = type { i64 }", ir)
        self.assertIn("@request = constant %Request { i64 6 }, section \".requests\"", ir)
        self.assertIn("@revision_data = constant [1 x i64] [i64 7], section \".requests\"", ir)

    def test_resolves_nested_imports_from_entry_source_directory(self):
        source = """
        import strings.trace;
        class Main {
            @:entryPoint static function main():Void { helper(); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "src"
            (root / "strings").mkdir(parents=True)
            (root / "bindings").mkdir()
            (root / "bindings" / "helper.hx").write_text("static function helper():Void { asm(\"nop\"); }")
            (root / "strings" / "sibling.hx").write_text("static function sibling():Void { asm(\"nop\"); }")
            (root / "strings" / "trace.hx").write_text("import bindings.helper; import sibling;")
            source_path = root / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare void @helper()", ir)

    def test_resolves_parent_relative_imports(self):
        source = "import strings.trace; class Main { @:entryPoint static function main():Void { helper(); } }"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "src"
            (root / "strings").mkdir(parents=True)
            (root / "bindings").mkdir()
            (root / "bindings" / "helper.hx").write_text("static function helper():Void { asm(\"nop\"); }")
            (root / "strings" / "trace.hx").write_text("import ..bindings.helper;")
            source_path = root / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare void @helper()", ir)

    def test_resolves_multi_level_parent_relative_imports(self):
        source = "import arch.amd64.idt; class Main { @:entryPoint static function main():Void { helper(); } }"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "src"
            (root / "arch" / "amd64").mkdir(parents=True)
            (root / "strings").mkdir()
            (root / "strings" / "trace.hx").write_text("static function helper():Void { asm(\"nop\"); }")
            (root / "arch" / "amd64" / "idt.hx").write_text("import ....strings.trace;")
            source_path = root / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare void @helper()", ir)

    def test_rejects_odd_parent_import_dots_with_source_location(self):
        source = "import ...strings.trace;"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "odd.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "odd.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:1:11: error: import parent prefix must use pairs of dots", result.stderr)

    def test_unresolved_import_uses_sourcemgr(self):
        source = "import missing.module;\n"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "missing-import.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "missing-import.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot resolve import 'missing.module'", result.stderr)
        self.assertIn(str(source_path), result.stderr)
        self.assertIn("^", result.stderr)
        self.assertNotIn("haxellvm: cannot resolve import", result.stderr)

    def test_declares_imported_executable_functions_without_redefining_them(self):
        module = "static function enabled(value:UIntSize<64>):Bool { return value == 6; }"
        source = """
        import limine;
        class Kernel {
            @:entryPoint static function main():Void {
                if (limine.enabled(6)) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "limine.hx"
            source_path = Path(directory) / "main.hx"
            module_object_path = Path(directory) / "limine.o"
            object_path = Path(directory) / "main.o"
            executable_path = Path(directory) / "kernel"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, module_path, "-o", module_object_path], check=True)
            subprocess.run([self.compiler, source_path, "-o", object_path], check=True)
            symbols = subprocess.run(["llvm-nm", object_path], capture_output=True, text=True, check=True).stdout
            subprocess.run(["clang", module_object_path, object_path, "-o", executable_path], check=True)
        self.assertRegex(symbols, r"(?m) U enabled$")

    def test_deduplicates_functions_from_recursive_imports(self):
        shared_module = "static function enabled():Bool { return true; }"
        first_module = "import shared;"
        second_module = "import shared;"
        source = """
        import first;
        import second;
        class Main {
            @:entryPoint static function main():Void {
                if (enabled()) { asm("nop"); }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "shared.hx").write_text(shared_module)
            (Path(directory) / "first.hx").write_text(first_module)
            (Path(directory) / "second.hx").write_text(second_module)
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("declare i1 @enabled()", ir)
        self.assertNotIn("@enabled.1", ir)

    def test_deduplicates_classes_from_diamond_imports(self):
        gdt_module = "class Gdt { public function new() {} public function Limit():Int { return 39; } }"
        tss_module = "import gdt; class Tss { public function new() {} }"
        source = """
        import gdt;
        import tss;
        class Main {
            @:entryPoint static function main():Void {
                var gdt = new Gdt();
                var tss = new Tss();
                trace(gdt.Limit());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "gdt.hx").write_text(gdt_module)
            (Path(directory) / "tss.hx").write_text(tss_module)
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertEqual(ir.count("%class.Gdt = type"), 1)
        self.assertEqual(ir.count("declare ptr @Gdt_new(ptr)"), 1)
        self.assertIn("declare ptr @Tss_new(ptr)", ir)

    def test_reports_duplicate_classes_with_llvm_source_manager_locations(self):
        source = "class Twice { public function new() {} }\nclass Twice { public function new() {} }\nclass Main { @:entryPoint static function main():Void { asm(\"nop\"); } }"
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "duplicate.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, source_path, "-o", Path(directory) / "duplicate.o"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{source_path}:2:7: error: duplicate class 'Twice'", result.stderr)
        self.assertIn("      ^~~~~", result.stderr)

    def test_resolves_imported_constant_globals(self):
        module = "static var header:Array<Int> = [1, 2, 3];"
        source = """
        import constants;
        class Kernel {
            @:elfSection(".requests") static var copy:Array<Int> = constants.header;
            @:entryPoint static function boot():Void { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "constants.hx"
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@copy = constant [3 x i64] [i64 1, i64 2, i64 3], section \".requests\"", ir)
        self.assertNotIn("@header =", ir)

    def test_folds_imported_final_scalar_globals(self):
        module = "final MemoryMapUsable:UIntSize<32> = 0;"
        source = """
        import constants;
        class Main {
            @:entryPoint static function main():Void { trace(MemoryMapUsable); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "constants.hx").write_text(module)
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "0\n")
        self.assertNotIn("@MemoryMapUsable =", ir)

    def test_declares_imported_global_fixed_array(self):
        module = "typedef Entry = { var value:UIntSize<16>; }; var entries:FixedArray<Entry, 1>;"
        source = """
        import tables;
        class Main {
            @:entryPoint static function main():Void {
                var entry = entries[0];
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "tables.hx"
            source_path = Path(directory) / "main.hx"
            module_ir_path = Path(directory) / "tables.ll"
            ir_path = Path(directory) / "main.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", module_path, "-o", module_ir_path], check=True)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            module_ir = module_ir_path.read_text()
            ir = ir_path.read_text()
        self.assertIn("@entries = global [1 x %Entry] zeroinitializer", module_ir)
        self.assertIn("@entries = external global [1 x %Entry]", ir)
        self.assertIn("load %Entry, ptr @entries", ir)

    def test_indexes_fixed_array_function_parameters(self):
        source = """
        typedef Entry = { var value:UIntSize<8>; };
        static function set(values:FixedArray<Entry, 2>):Void {
            values[1] = { value: 7 };
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "fixed-array-parameter.hx"
            ir_path = Path(directory) / "fixed-array-parameter.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("getelementptr [2 x %Entry]", ir)

    def test_assigns_imported_pointer_global(self):
        module = "var context:Ptr<Dynamic>;"
        source = """
        import context;
        class Main {
            @:entryPoint static function main():Void {
                context = null;
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "context.hx"
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@context = external global ptr", ir)
        self.assertIn("store ptr null, ptr @context", ir)

    def test_loads_imported_pointer_global_value(self):
        module = "var context:Ptr<Dynamic>;"
        source = """
        import context;
        static function accepts(value:Ptr<Dynamic>):Void { asm("nop"); }
        class Main {
            @:entryPoint static function main():Void { accepts(context); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "context.hx"
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@context = external global ptr", ir)
        self.assertIn("load ptr, ptr @context", ir)

    def test_declares_imported_any_global(self):
        module = "var handle: Any;"
        source = """
        import handles;
        class Main {
            @:entryPoint static function main():Void {
                handle = null;
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "handles.hx"
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            module_path.write_text(module)
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("@handle = external global ptr", ir)
        self.assertIn("store ptr null, ptr @handle", ir)

    def test_any_global_method_resolves_to_unique_class(self):
        module = "var machine: Any;"
        source = """
        import handles;
        class Device {
            public function poke(value:Int):Void { asm("nop"); }
            public function new() {}
        }
        class Main {
            @:entryPoint static function main():Void {
                machine = new Device();
                machine.poke(1);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "handles.hx").write_text(module)
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("call void @Device_poke(ptr", ir)

    def test_any_global_method_resolves_across_import_without_assignment(self):
        handles = "var machine: Any;"
        device = """
        class Device {
            public function poke(value:Int):Void { asm("nop"); }
            public function new() {}
        }
        """
        source = """
        import handles;
        import device;
        class Main {
            @:entryPoint static function main():Void {
                machine.poke(1);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "handles.hx").write_text(handles)
            (Path(directory) / "device.hx").write_text(device)
            source_path = Path(directory) / "main.hx"
            ir_path = Path(directory) / "main.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("call void @Device_poke(ptr", ir)
        self.assertIn("load ptr, ptr @machine", ir)

    def test_imported_global_type_error_mentions_import_site(self):
        module = "var bad: NotARealType;"
        source = """
        import broken;
        class Main {
            @:entryPoint static function main():Void { asm("nop"); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "broken.hx"
            source_path = Path(directory) / "main.hx"
            module_path.write_text(module)
            source_path.write_text(source)
            result = subprocess.run(
                [self.compiler, source_path, "-o", Path(directory) / "bad.o"],
                capture_output=True,
                text=True,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"{module_path}:1:5: error: unsupported imported global type", result.stderr)
        self.assertIn("var bad: NotARealType;", result.stderr)
        self.assertIn("    ^~~~~~~~~~~~~~~~~~", result.stderr)
        self.assertIn(f"{source_path}:2:9: note: imported here", result.stderr)
        self.assertIn("import broken;", result.stderr)

    def test_lowers_haxe_style_freestanding_uart_output(self):
        if platform.machine() not in {"x86_64", "i386", "i486", "i586", "i686"}:
            self.skipTest("UART out instruction applies only to x86 targets")
        source = """
        class Main {
            @:entryPoint
            static function kmain():Void {
                var uart:Int = 0x3F8;
                haxe.Log.trace = function(value) {
                    var text = Std.string(value);
                    for (index in 0...text.length) {
                        asm("out $0, $1", "{dx},{al},~{dirflag},~{fpsr},~{flags}", uart, text.charCodeAt(index));
                    }
                };
                trace("kernel go brrrrr");
                asm("hlt");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kernel.hx"
            ir_path = Path(directory) / "kernel.ll"
            assembly_path = Path(directory) / "kernel.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", assembly_path], check=True)
            ir = ir_path.read_text()
            assembly = assembly_path.read_text()
        self.assertNotIn("@printf", ir)
        self.assertNotIn("@puts", ir)
        self.assertIn('"out $0, $1", "{dx},{al}', ir)
        self.assertRegex(assembly, r"out\s+dx, al")
        self.assertIn("hlt", assembly)

    def test_grows_empty_array_with_push_and_string_from_char_codes(self):
        source = """
        class Main {
            var shellInput:Array<UIntSize<8>> = [];
            @:entryPoint
            static function main():Void {
                shellInput.push(65);
                shellInput.push(66);
                var input = String.fromCharCodes(shellInput);
                trace(input.length);
                shellInput = [];
                shellInput.push(67);
                var again = String.fromCharCodes(shellInput);
                trace(again.length);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "shell.hx"
            ir_path = Path(directory) / "shell.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\n1\n")

    def test_pushes_runtime_record_literals_into_typed_arrays(self):
        source = """
        typedef PciDevice = {
            var bus: UIntSize<8>;
            var device: UIntSize<8>;
            var vendorID: UIntSize<16>;
        };
        class Main {
            @:entryPoint static function main():Void {
                var devices:Array<PciDevice> = [];
                var bus:UIntSize<8> = 2;
                devices.push({ bus: bus, device: 7, vendorID: 0x1234 });
                trace(devices[0].bus);
                trace(devices[0].device);
                trace(devices[0].vendorID);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "record-push.hx"
            ir_path = Path(directory) / "record-push.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\n7\n4660\n")

    def test_array_pop_shortens_growable_array(self):
        source = """
        class Main {
            var shellInput:Array<UIntSize<8>> = [];
            @:entryPoint
            static function main():Void {
                shellInput.push(65);
                shellInput.push(66);
                shellInput.push(67);
                shellInput.pop();
                var input = String.fromCharCodes(shellInput);
                trace(input.length);
                trace(input);
                trace(shellInput.pop());
                trace(shellInput.length);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "shell.hx"
            ir_path = Path(directory) / "shell.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\nAB\n66\n1\n")


    def test_string_from_char_codes_ignores_stale_buffer_tail(self):
        source = """
        class Main {
            var shellInput:Array<UIntSize<8>> = [];
            @:entryPoint
            static function main():Void {
                shellInput.push(97);
                shellInput.push(98);
                shellInput.push(99);
                shellInput.push(100);
                var longInput = String.fromCharCodes(shellInput);
                trace(longInput.length);
                shellInput = [];
                shellInput.push(120);
                shellInput.push(121);
                var shortInput = String.fromCharCodes(shellInput);
                trace(shortInput.length);
                trace(shortInput);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "shell.hx"
            ir_path = Path(directory) / "shell.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "4\n2\nxy\n")

    def test_new_instance_field_read_write(self):
        source = """
        class Box {
            var n: Int;
            public function new(v: Int) { this.n = v; }
            public function Get(): Int { return this.n; }
            public function Set(v: Int): Void { this.n = v; }
        }
        class Main {
            @:entryPoint
            static function main(): Void {
                var box = new Box(3);
                trace(box.Get());
                box.Set(9);
                trace(box.Get());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "box.hx"
            ir_path = Path(directory) / "box.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "3\n9\n")
        self.assertIn("__haxellvm_arena", ir)

    def test_calls_instance_methods_on_class_typed_globals(self):
        source = """
        class Clock {
            var hours:Int;
            public function new(hours:Int) { this.hours = hours; }
            public function Stamp():String { return "h=" + this.hours.toString(10); }
        }
        var global_clock:Clock;
        class Main {
            @:entryPoint static function main():Void {
                global_clock = new Clock(7);
                trace(global_clock.Stamp());
                trace(global_clock.hours);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "global-clock.hx"
            ir_path = Path(directory) / "global-clock.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "h=7\n7\n")

    def test_calls_instance_methods_through_instance_fields(self):
        source = """
        class Counter {
            var value:Int;
            public function new(value:Int) { this.value = value; }
            public function Add(amount:Int):Int { this.value += amount; return this.value; }
        }
        class Owner {
            var counter:Counter;
            public function new(counter:Counter) { this.counter = counter; }
            public function Allocate(size:Int):Int { return this.counter.Add(size); }
        }
        class Main {
            @:entryPoint static function main():Void {
                var owner = new Owner(new Counter(40));
                trace(owner.Allocate(2));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "nested-method.hx"
            ir_path = Path(directory) / "nested-method.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "42\n")

    def test_reads_scalar_fields_through_instance_fields(self):
        source = """
        class PageManager {
            final pageSize:UIntSize<64> = 4096;
            public function new() {}
        }
        class Allocator {
            var pmm:PageManager;
            public function new(pmm:PageManager) { this.pmm = pmm; }
            public function Pages(size:UIntSize<64>):UIntSize<64> { return (size + this.pmm.pageSize - 1) / this.pmm.pageSize; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var allocator = new Allocator(new PageManager());
                trace(allocator.Pages(4097));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "nested-field.hx"
            ir_path = Path(directory) / "nested-field.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "2\n")

    def test_initializes_scalar_instance_fields_before_constructor_body(self):
        source = """
        class Ports {
            final address: UIntSize<16> = 0xCF8;
            var data: UIntSize<16> = 0xCFC;
            public function new() {}
            public function Address(): UIntSize<16> { return this.address; }
            public function Data(): UIntSize<16> { return this.data; }
        }
        class Main {
            @:entryPoint static function main(): Void {
                var ports = new Ports();
                trace(ports.Address());
                trace(ports.Data());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "instance-initializers.hx"
            ir_path = Path(directory) / "instance-initializers.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "3320\n3324\n")

    def test_extends_override_virtual_call_through_parent_type(self):
        source = """
        abstract class Base {
            public function new() {}
            public abstract function Value(): Int;
        }
        class Child extends Base {
            var n: Int;
            public function new() { super(); this.n = 42; }
            public override function Value(): Int { return this.n; }
        }
        class Main {
            @:entryPoint
            static function main(): Void {
                var item: Base = new Child();
                trace(item.Value());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "virtual.hx"
            ir_path = Path(directory) / "virtual.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "42\n")
        self.assertIn("vtable.", ir)

    def test_instance_array_field_index(self):
        source = """
        class Table {
            var codes: Array<Int> = [10, 20, 30];
            public function new() {}
            public function At(i: Int): Int { return this.codes[i]; }
        }
        class Main {
            @:entryPoint
            static function main(): Void {
                var table = new Table();
                trace(table.At(1));
                trace(table.At(2));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "table.hx"
            ir_path = Path(directory) / "table.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "20\n30\n")

    def test_pushes_records_into_uninitialized_instance_arrays(self):
        source = """
        typedef PciDevice = { var bus:UIntSize<8>; var vendorID:UIntSize<16>; };
        class Devices {
            var entries:Array<PciDevice>;
            public function new() {
                this.entries = [];
                this.entries.push({ bus: 2, vendorID: 0x1234 });
            }
            public function VendorID():UIntSize<16> { return this.entries[0].vendorID; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var devices = new Devices();
                trace(devices.VendorID());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "instance-record-push.hx"
            ir_path = Path(directory) / "instance-record-push.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "4660\n")

    def test_static_final_array_via_this(self):
        source = """
        class Table {
            private static final codes: Array<Int> = [10, 20, 30];
            public function new() {}
            public function At(i: Int): Int { return this.codes[i]; }
        }
        class Main {
            @:entryPoint
            static function main(): Void {
                var table = new Table();
                trace(table.At(0));
                trace(table.At(2));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "final_table.hx"
            ir_path = Path(directory) / "final_table.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "10\n30\n")
        self.assertIn("constant", ir)

    def test_rejects_extend_final_class(self):
        source = """
        final class Sealed {
            public function new() {}
        }
        class Child extends Sealed {
            public function new() { super(); }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "final_class.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("final class", result.stderr)

    def test_rejects_override_final_method(self):
        source = """
        class Base {
            public function new() {}
            public final function Value(): Int { return 1; }
        }
        class Child extends Base {
            public function new() { super(); }
            public override function Value(): Int { return 2; }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "final_method.hx"
            source_path.write_text(source)
            result = subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("final method", result.stderr)

    def test_rejects_unimplemented_abstract_method(self):
        source = """
        abstract class Base {
            public abstract function Value(): Int;
        }
        class Child extends Base {
            public function new() {}
        }
        class Main {
            @:entryPoint
            static function main(): Void {}
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "abstract-missing.hx"
            ir_path = Path(directory) / "abstract-missing.ll"
            source_path.write_text(source)
            completed = subprocess.run(
                [self.compiler, "--emit=llvm", source_path, "-o", ir_path],
                capture_output=True,
                text=True,
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("does not implement abstract method", completed.stderr)

    def test_bump_arena_supports_multiple_news(self):
        source = """
        class Box {
            var n: Int;
            public function new(v: Int) { this.n = v; }
            public function Get(): Int { return this.n; }
        }
        class Main {
            @:entryPoint
            static function main(): Void {
                var a = new Box(1);
                var b = new Box(2);
                var c = new Box(3);
                trace(a.Get());
                trace(b.Get());
                trace(c.Get());
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "arena.hx"
            ir_path = Path(directory) / "arena.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "1\n2\n3\n")
        self.assertIn("__haxellvm_arena_cursor", ir)

    def test_void_fields_in_haxe_style_typedefs(self):
        source = """
        typedef Table = {
            header: Void,
            vendor: Void,
            revision: UIntSize<32>
        };
        class Main {
            @:entryPoint static function main():Void {
                var table: Table = { header: null, vendor: null, revision: 7 };
                trace(table.revision);
                trace(sizeof(Void));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "void-typedef.hx"
            ir_path = Path(directory) / "void-typedef.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "7\n8\n")
        self.assertIn("%Table = type { ptr, ptr, i32 }", ir)

    def test_int_uint_are_native_word_sized(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var signed: Int = -1;
                var unsigned: UInt = 0xFFFFFFFFFFFFFFFF;
                trace(sizeof(Int));
                trace(sizeof(UInt));
                trace(signed);
                trace(unsigned);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "word-int.hx"
            ir_path = Path(directory) / "word-int.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
            ir = ir_path.read_text()
        self.assertEqual(result.stdout, "8\n8\n-1\n-1\n")
        self.assertIn("alloca i64", ir)
        self.assertNotIn("alloca i32", ir)

        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "word-int32.hx"
            ir_path = Path(directory) / "word-int32.ll"
            source_path.write_text(source)
            subprocess.run(
                [self.compiler, "--emit=llvm", "-target", "i386-unknown-linux-gnu", source_path, "-o", ir_path],
                check=True,
            )
            ir = ir_path.read_text()
        self.assertIn("alloca i32", ir)
        self.assertNotIn("alloca i64", ir)

    def test_windows_uefi_targets_use_win64_not_sysv(self):
        source = """
        class Main {
            @:entryPoint static function main():Void { asm("nop"); }
            @:retainFunctionName static function helper():Void { asm("nop"); }
        }
        """
        for triple in ("x86_64-pc-windows-msvc", "x86_64-unknown-uefi"):
            with tempfile.TemporaryDirectory() as directory:
                source_path = Path(directory) / "abi.hx"
                ir_path = Path(directory) / "abi.ll"
                source_path.write_text(source)
                subprocess.run([self.compiler, "--emit=llvm", "-target", triple, source_path, "-o", ir_path], check=True)
                ir = ir_path.read_text()
            self.assertNotIn("noredzone", ir)
            self.assertIn("win64cc", ir)

        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "sysv.hx"
            ir_path = Path(directory) / "sysv.ll"
            source_path.write_text(source)
            subprocess.run(
                [self.compiler, "--emit=llvm", "-target", "x86_64-unknown-linux-gnu", source_path, "-o", ir_path],
                check=True,
            )
            ir = ir_path.read_text()
        self.assertIn("noredzone", ir)
        self.assertNotIn("win64cc", ir)

    def test_haxe_syntax_package_using_ternary_range_and_keywords(self):
        source = """
        package kernel.boot;
        using Math;
        interface Device {
            function ready():Bool;
        }
        class Main implements Device {
            @:entryPoint static function main():Void {
                var flag:Bool = true;
                var value:Int = flag ? 7 : 3;
                for (i in 0...3) value += i;
                var pointer:Ptr<UIntSize<8>> = null;
                var fallback:Ptr<UIntSize<8>> = 0x1000;
                pointer = pointer ?? fallback;
                trace(value);
                if (null == null) trace(1);
            }
            function ready():Bool { return true; }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "syntax.hx"
            ir_path = Path(directory) / "syntax.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "10\n1\n")

    def test_haxe_abstract_rejected_systems_types_kept(self):
        source = """
        abstract Word(Int) {}
        class Main {
            @:entryPoint static function main():Void {}
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "abstract.hx"
            source_path.write_text(source)
            result = subprocess.run(
                [self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"],
                capture_output=True,
                text=True,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Haxe abstracts are not supported", result.stderr)

    def test_elseif_preprocessor(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                #if missing
                trace(1);
                #elseif also_missing
                trace(2);
                #else
                trace(3);
                #end
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "elseif.hx"
            ir_path = Path(directory) / "elseif.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "3\n")

    def test_keyword_cannot_be_identifier(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var class:Int = 1;
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "kw.hx"
            source_path.write_text(source)
            result = subprocess.run(
                [self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"],
                capture_output=True,
                text=True,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected keyword", result.stderr)

    def test_lowers_float_ops_via_sse(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var a:Float = 1.5;
                var b:Float = 2.5;
                var c:Float = a + b * 2.0;
                if (c > 6.0) trace(1); else trace(0);
                var s:Single = 3.0f;
                if (s < 4.0f) trace(1); else trace(0);
                trace(cast(c, Int));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "float.hx"
            ir_path = Path(directory) / "float.ll"
            asm_path = Path(directory) / "float.s"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            self.assertIn("fadd", ir)
            self.assertIn("fmul", ir)
            self.assertIn("fcmp", ir)
            subprocess.run([self.compiler, "--emit=asm", source_path, "-o", asm_path], check=True)
            asm = asm_path.read_text()
            self.assertTrue(
                "cvttsd2si" in asm
                or "addsd" in asm
                or "mulsd" in asm
                or "addss" in asm
                or "mulss" in asm
                or "xmm" in asm.lower()
                or "cvtsi2sd" in asm
            )
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1\n1\n6\n")

    def test_untyped_float_local_infers_float(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var hz = 1e15;
                trace(cast(hz / 1e14, Int));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "untyped_float.hx"
            ir_path = Path(directory) / "untyped_float.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "10\n")

    def test_string_concat_formats_float_via_sse_trunc(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var hz:Float = 42.9;
                trace("hz=" + hz);
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "float_concat.hx"
            ir_path = Path(directory) / "float_concat.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            self.assertIn("fptosi", ir)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "hz=42\n")

    def test_ptr_deref_in_empty_while_is_volatile(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var base:UIntSize<64> = 0x1000;
                var h0:UIntSize<64> = *cast(base + 0xF0, Ptr<UIntSize<64>>);
                while (*cast(base + 0xF0, Ptr<UIntSize<64>>) - h0 < 100000) {}
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "mmio_spin.hx"
            ir_path = Path(directory) / "mmio_spin.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("load volatile i64", ir)
        self.assertGreaterEqual(ir.count("load volatile i64"), 2)

    def test_local_initializer_mismatch_points_at_initializer(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var x:Float = null;
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "mismatch.hx"
            source_path.write_text(source)
            result = subprocess.run(
                [self.compiler, "--emit=llvm", source_path, "-o", Path(directory) / "out.ll"],
                capture_output=True,
                text=True,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("local initializer type mismatch", result.stderr)
        self.assertIn("null", result.stderr)
        self.assertNotIn("import", result.stderr)

    def test_float_ops_reject_soft_float_cpu(self):
        source = """
        class Main {
            @:entryPoint static function main():Void {
                var a:Float = 1.0 + 2.0;
                trace(cast(a, Int));
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "soft.hx"
            source_path.write_text(source)
            result = subprocess.run(
                [self.compiler, "--emit=asm", "-Xcpu", "-x87,-mmx,-sse,-sse2,+soft-float", source_path, "-o", Path(directory) / "soft.s"],
                capture_output=True,
                text=True,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SSE", result.stderr)

    def test_lowers_safe_navigation_try_is_and_using(self):
        source = """
        using Extensions;
        typedef Box = { var value:Int; };
        class Extensions {
            static function doubled(value:Int):Int { return value * 2; }
        }
        class Main {
            @:entryPoint static function main():Void {
                var missing:Ptr<Box> = null;
                if (missing?.value == 0) trace(1); else trace(0);
                var n:Int = 21;
                trace(n.doubled());
                if (n is Int) trace(1); else trace(0);
                try {
                    throw cast(7, Ptr<UIntSize<8>>);
                    trace(0);
                } catch (err:Ptr<UIntSize<8>>) {
                    if (err != null) trace(1); else trace(0);
                }
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "lower.hx"
            ir_path = Path(directory) / "lower.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
            self.assertIn("safe.", ir)
            self.assertIn("using.call", ir)
            self.assertIn("catch", ir)
            result = subprocess.run(["lli", ir_path], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "1\n42\n1\n1\n")

    def test_build_macro_generates_naked_isr_stubs(self):
        source = """
        class Isr {
            public static macro function build():Array<Field> {
                var fields = [];
                for (i in 0...256) {
                    fields.push({
                        name: "isr" + i,
                        access: [AStatic, APublic],
                        kind: FFun({
                            args: [],
                            ret: null,
                            expr: null
                        })
                    });
                }
                return fields;
            }
        }
        @:build(Isr.build())
        class Stubs {
        }
        class Main {
            @:entryPoint static function main():Void {
                asm("nop");
            }
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "isr-macro.hx"
            ir_path = Path(directory) / "isr-macro.ll"
            source_path.write_text(source)
            subprocess.run([self.compiler, "--emit=llvm", source_path, "-o", ir_path], check=True)
            ir = ir_path.read_text()
        self.assertIn("define void @isr0()", ir)
        self.assertIn("define void @isr14()", ir)
        self.assertIn("define void @isr255()", ir)
        self.assertIn("naked", ir)
        self.assertIn("push 14", ir)
        self.assertIn("jmp idtCommonHandler", ir)


if __name__ == "__main__":
    unittest.main()
