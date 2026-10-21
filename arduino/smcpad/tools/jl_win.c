/*
 * jl.exe: tools/jl.sh for Windows. Runs a tool of the JieLi Windows toolchain (the pi32 folder
 * that jieli_fetch.cmd extracts from JieLi's installer) with the arguments the platform gives.
 *
 *   jl.exe <toolchain dir> <host tools dir> <tool> [args...]
 *   tool: clang, clang++, ld, ar, nm, objcopy, objdump
 *   jl.exe <toolchain dir> <host tools dir> fetch     runs <toolchain dir>\jieli_fetch.cmd (the
 *                                                     platform's pre-build hook: the path is built
 *                                                     here, out of reach of quoting rules)
 *
 * Built with mingw-w64 (arduino/release/build_windows_tools.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const struct { const char *tool, *exe, *extra; } map[] = {
    {"clang", "clang.exe", 0},
    {"clang++", "clang.exe", "--driver-mode=g++"},
    {"ld", "pi32v2-ld.exe", 0},
    {"ar", "llvm-ar.exe", 0},
    {"nm", "pi32v2-nm.exe", 0},
    {"objcopy", "llvm-objcopy.exe", 0},
    {"objdump", "llvm-objdump.exe", 0},
};

/* one argument, quoted as CommandLineToArgvW / the MSVC runtime parse it back */
static void append_arg(char **cmd, size_t *len, size_t *cap, const char *arg)
{
    size_t need = *len + 2 * strlen(arg) + 4;
    int quote = !*arg || strpbrk(arg, " \t\"") != NULL;
    char *p;

    if (need > *cap) {
        *cap = need * 2;
        *cmd = realloc(*cmd, *cap);
    }
    p = *cmd + *len;
    if (*len) *p++ = ' ';
    if (!quote) {
        strcpy(p, arg);
        p += strlen(arg);
    } else {
        *p++ = '"';
        for (const char *a = arg;; a++) {
            unsigned slashes = 0;

            while (*a == '\\') { slashes++; a++; }
            if (!*a) {                              /* before the closing quote: double them */
                for (unsigned i = 0; i < 2 * slashes; i++) *p++ = '\\';
                break;
            }
            if (*a == '"') {                        /* escape the quote and the slashes before it */
                for (unsigned i = 0; i < 2 * slashes + 1; i++) *p++ = '\\';
            } else {
                for (unsigned i = 0; i < slashes; i++) *p++ = '\\';
            }
            *p++ = *a;
        }
        *p++ = '"';
    }
    *p = 0;
    *len = (size_t)(p - *cmd);
}

int main(int argc, char **argv)
{
    char exe[MAX_PATH * 2];
    const char *name = 0, *extra = 0;
    char *cmd = 0;
    size_t len = 0, cap = 0;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (argc < 4) {
        fprintf(stderr, "usage: jl.exe <toolchain dir> <host tools dir> <tool> [args...]\n");
        return 2;
    }
    if (!strcmp(argv[3], "fetch")) {
        char script[MAX_PATH * 2], line[MAX_PATH * 3];

        snprintf(script, sizeof(script), "%s\\jieli_fetch.cmd", argv[1]);
        for (char *c = script; *c; c++) if (*c == '/') *c = '\\';
        /* cmd /s /c "<line>": /s strips exactly the outer quotes, whatever is inside */
        snprintf(line, sizeof(line), "cmd.exe /d /s /c \"\"%s\"\"", script);
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "jl.exe: cannot run %s (error %lu)\n", script, GetLastError());
            return 1;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)code;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(argv[3], map[i].tool)) {
            name = map[i].exe;
            extra = map[i].extra;
        }
    }
    if (!name) {
        fprintf(stderr, "jl.exe: unknown tool %s\n", argv[3]);
        return 2;
    }
    snprintf(exe, sizeof(exe), "%s\\pi32\\bin\\%s", argv[1], name);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "jl.exe: %s is missing: the JieLi compiler is fetched at the first build "
                        "(jieli_fetch.cmd); build again with an internet connection\n", exe);
        return 1;
    }
    append_arg(&cmd, &len, &cap, exe);
    if (extra) append_arg(&cmd, &len, &cap, extra);
    for (int i = 4; i < argc; i++) append_arg(&cmd, &len, &cap, argv[i]);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "jl.exe: cannot run %s (error %lu)\n", exe, GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
