#!/usr/bin/env bash
# Runs the emulator against the MinGW-built test programs and checks output.
set -u
cd "$(dirname "$0")/.."

fail=0
pass() { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

expected_out=$'hello from PE code\nconsole type: 1\nguest OS: 10.0.15063.0\nalloc: ok\n'

# 1. Hello test: output, exit code, and that imports and relocations work.
out=$(./xbemu tests/hello/hello.exe; echo "rc=$?")
actual=${out%rc=*}
code=${out##*rc=}
code=${code%$'\n'}
if [[ "$actual" == "$expected_out" ]]; then pass "hello output"; else
    bad "hello output"; printf 'got:\n%s\n' "$actual"; fi
if [[ $code -eq 7 ]]; then pass "hello exit code 7"; else bad "hello exit code (got $code)"; fi

# 2. Verbose mode reports the load address; it must differ from the PE ImageBase
#    (0x140000000 for MinGW), so relocation actually ran.
load_line=$(./xbemu -v tests/hello/hello.exe 2>&1 >/dev/null | grep 'loaded')
if [[ "$load_line" != *"0x140000000"* && -n "$load_line" ]]; then
    pass "image relocated away from preferred base"
else
    bad "relocation check: $load_line"
fi

# 3. Non-PE input is rejected cleanly.
if ./xbemu tests/run_tests.sh >/dev/null 2>&1; then
    bad "non-PE file was accepted"
else
    pass "non-PE file rejected"
fi

# 4. Missing file is rejected.
if ./xbemu /nonexistent.exe >/dev/null 2>&1; then bad "missing file accepted"; else pass "missing file rejected"; fi

# 5. C++ exceptions: a throw crosses frames, runs a destructor, and is caught.
#    Also checks that a non-matching catch is skipped.
eh_out=$(./xbemu tests/eh/throw.exe 2>&1)
eh_code=$?
if [[ $eh_code -eq 0 && "$eh_out" == *"caught: boom cleanup ran"* && "$eh_out" == *"int passed through to catch(...)"* ]]; then
    pass "C++ exception unwinding"
else
    bad "C++ exception unwinding (rc=$eh_code)"; printf '%s\n' "$eh_out"
fi

# 6. An ordinary C program with the default MinGW C runtime: startup, printf, malloc,
#    the argument vector, and the exit code.
crt_out=$(./xbemu tests/crt/hello_crt.exe first second 2>&1; echo "rc=$?")
expected_crt=$'hello 42 from crt\nheap 4\nargc 3 argv1 first\npi 3.14 e 2.5\nrc=3'
if [[ "$crt_out" == "$expected_crt" ]]; then pass "default C runtime program"; else
    bad "default C runtime program"; printf '%s\n' "$crt_out"; fi

# 7. XAudio2: a 440 Hz tone reaches the audio output at the right frequency.
if ./tests/audio/run_audio_test.sh; then :; else fail=1; fi

# 8. XAudio2 routing: a submix with channel volumes, and a direct source.
if ./tests/audio/run_routing_test.sh; then :; else fail=1; fi

# 8b. Two XAudio2 engines in one process play at the same time.
two_out=$(./xbemu tests/audio/two_engines.exe 2>/dev/null); two_rc=$?
if [[ $two_rc -eq 0 && "$two_out" == *"both engines played"* ]]; then pass "two XAudio2 engines"; else bad "two XAudio2 engines (rc=$two_rc)"; fi

# 8c. Vulkan float arguments reach the driver intact (checked through the shim's trace).
if vk_out=$(XBEMU_TRACE_VK=1 ./xbemu tests/vulkan/float.exe 2>&1) && [[ "$vk_out" == *"float arg 1 = 2.5"* && "$vk_out" == *"float arg 1 = 0.25"* && "$vk_out" == *"float arg 2 = 0.75"* && "$vk_out" == *"float calls recorded"* ]]; then
    pass "Vulkan float arguments"
else
    bad "Vulkan float arguments"; printf '%s\n' "$vk_out"
fi

# 8d. An exception thrown in a window procedure reaches the guest catch around DispatchMessageW.
if [[ -n "${DISPLAY:-}" ]]; then
    wp_out=$(./xbemu tests/eh/wndproc.exe 2>&1); wp_rc=$?
    if [[ $wp_rc -eq 0 && "$wp_out" == *"caught: from window procedure cleanup ran"* ]]; then
        pass "exception from a window procedure"
    else
        bad "exception from a window procedure (rc=$wp_rc)"; printf '%s\n' "$wp_out"
    fi
else
    echo "SKIP: exception from a window procedure (no DISPLAY)"
fi

# 9. Threads: SRW locks and condition variables under contention.
th_out=$(./xbemu tests/threads/sync.exe 2>&1); th_rc=$?
if [[ $th_rc -eq 0 && "$th_out" == *"counter 80000 expected 80000"* && "$th_out" == *"cs-handshake ok"* && "$th_out" == *"srw-handshake ok"* ]]; then
    pass "SRW locks and condition variables"
else
    bad "SRW locks and condition variables (rc=$th_rc)"; printf '%s\n' "$th_out"
fi

# 10. Registry: values written by one run are read back by the next, through XBEMU_REGISTRY.
reg_file=$(mktemp -p "${TMPDIR:-$PWD}" xbemu-reg.XXXXXX)
rm -f "$reg_file"
reg1=$(XBEMU_REGISTRY="$reg_file" ./xbemu tests/registry/reg.exe 2>&1)
reg2=$(XBEMU_REGISTRY="$reg_file" ./xbemu tests/registry/reg.exe 2>&1); reg_rc=$?
rm -f "$reg_file"
if [[ "$reg1" == "stored values" && "$reg2" == "found stored values" && $reg_rc -eq 0 ]]; then
    pass "registry persists between runs"
else
    bad "registry persistence"; printf '%s / %s\n' "$reg1" "$reg2"
fi

# 11. Keyboard and mouse messages, and raw mouse input (need an X display; skipped without one).
if [[ -n "${DISPLAY:-}" ]]; then
    if ./tests/input/run_input_test.sh; then :; else fail=1; fi
    if ./tests/input/run_raw_test.sh; then :; else fail=1; fi
else
    echo "SKIP: input messages (no DISPLAY)"
fi

exit $fail
