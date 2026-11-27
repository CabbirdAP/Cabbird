#include <Windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaximumRequestBytes = 64U * 1024U;
constexpr std::size_t kResponseChunkBytes = 64U * 1024U;

std::optional<std::string> Utf8(std::wstring_view value) {
    if (value.empty()) return std::string{};
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::nullopt;
    std::string output(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        output.data(), length, nullptr, nullptr) != length) {
        return std::nullopt;
    }
    return output;
}

// Keep the local help aligned with Analyzer::Execute. The runtime's help command
// describes the commands provided by the version loaded in the selected process.
constexpr char kUsageText[] =
    "cabbird-cli - one-shot client for the Cabbird runtime diagnostics pipe\n"
    "\n"
    "usage:\n"
    "  cabbird-cli --pid <pid> <command> [arguments...]\n"
    "  cabbird-cli --help\n"
    "\n"
    "output contract:\n"
    "  For --pid commands, stdout is exactly one UTF-8 JSON reply line, for example\n"
    "    {\"ok\":true,\"pid\":1234}\n"
    "    {\"ok\":false,\"error\":\"unknown command; use help\"}\n"
    "  Parse stdout as JSON and check \"ok\". Exit code 0 means a reply was received,\n"
    "  including replies where \"ok\" is false. stderr contains client-side failures only.\n"
    "  Local --help prints this text without connecting to a process.\n"
    "  Arguments are joined with single spaces. A pattern such as \"48 8B ?? 48 85 C0\"\n"
    "  may be quoted as one argument or passed as several. Line breaks are rejected.\n"
    "  The command plus its final newline must fit in 64 KiB.\n"
    "\n"
    "target:\n"
    "  --pid <pid>   PID of the process hosting the Cabbird runtime.\n"
    "                The pipe is \\\\.\\pipe\\LOCAL\\Cabbird-<pid>; [Analyzer] PipePrefix\n"
    "                must be Cabbird. Use the same Windows user/logon session as the runtime.\n"
    "                The runtime creates its pipe for an ordinary, non-elevated console.\n"
    "\n"
    "commands:\n"
    "  discovery and prepared snapshots\n"
    "    ping                        liveness: {\"ok\":true,\"pid\":<runtime pid>}\n"
    "    help                        the loaded runtime's command list as JSON\n"
    "    status                      runtime root, game id, platform flags and service health\n"
    "    plugins                     prepared plugin diagnostics from the runtime status snapshot\n"
    "    modules                     loaded module name, path, base address and image size\n"
    "    sections <module|.>         PE section base, size and characteristics\n"
    "    regions <module|.>          memory region state, protection and type\n"
    "    unity                       prepared Unity Profile snapshot; does not rescan\n"
    "    profile                     same prepared snapshot as unity\n"
    "    snapshot [filename]         write modules and the prepared Profile into the runtime root\n"
    "                                No reflection subcommands are currently wired by the host.\n"
    "\n"
    "  explicit scans (require an active Profile and a diagnostic request)\n"
    "    scan <module|.> <section> <pattern>\n"
    "                                scan one PE section; hex bytes accept ?? and nibble wildcards\n"
    "    xrefs <module|.> <target>   executable-section references to an absolute target address\n"
    "    heap-scan <ascii|utf16> <text> [limit]\n"
    "                                scan private memory for one text token; limit defaults to 256,\n"
    "                                maximum 4096; utf16 widens ASCII text to UTF-16LE\n"
    "    il2cpp-objects <namespace|-> <class> [field|-] [limit]\n"
    "                                request an IL2CPP object scan on the Game thread; default 32,\n"
    "                                maximum 256; - means empty namespace or no field\n"
    "\n"
    "  reading memory\n"
    "    read <address> <size>       size <= 1048576; returns raw hex bytes\n"
    "    chain <base> [offset ...]   follow at most 64 pointer offsets and return the end address\n"
    "    ptr <address>               read one pointer\n"
    "    rip <instruction> <disp_offset> <instruction_size>\n"
    "                                resolve a RIP-relative operand to its absolute target\n"
    "    f32 <address> [count]       float32 values; default count 3, maximum 256\n"
    "    f64 <address> [count]       float64 values; default count 3, maximum 256\n"
    "\n"
    "  writing memory (only an explicitly authorized, owned test process)\n"
    "    write <address> <hex bytes>       write into an already writable page\n"
    "    patch <address> <hex bytes>       change protection, write, flush cache, restore protection\n"
    "    protect <address> <size> <r|rw|x|rx|rwx>\n"
    "    alloc <size> [protection]         allocate at most 268435456 bytes; default rw\n"
    "    free <address>                    release an address returned by alloc\n"
    "    Hex bytes are space separated, e.g. \"90 90\". write/patch return the previous bytes;\n"
    "    protect returns the previous page protection; alloc returns the allocation address.\n"
    "\n"
    "arguments:\n"
    "  <module>      . is the main executable; use GameAssembly.dll for the IL2CPP native module\n"
    "  <address>     0x-prefixed hex or decimal; sizes and offsets use the same parser\n"
    "  <section>     PE section name, e.g. .text\n"
    "  <pattern>     hex bytes: ?? matches one byte, 4? fixes the high nibble, ?F the low nibble\n"
    "  protection    r, rw, x, rx, rwx, or a numeric PAGE_* value\n"
    "  <text>        one token without whitespace; this grammar does not preserve quoted spaces\n"
    "\n"
    "exit codes:\n"
    "  0   a reply was received; inspect its JSON \"ok\" field (also returned by local --help)\n"
    "  2   invalid usage, malformed --pid, unencodable argument, line break or oversized request\n"
    "  3   connection failed: 2 = no pipe, 5 = access denied, 231 = all instances busy\n"
    "  4   pipe configuration or request write failed\n"
    "  5   response read, empty response or stdout write failed\n"
    "\n"
    "typical read-only discovery:\n"
    "  cabbird-cli --pid 1234 ping\n"
    "  cabbird-cli --pid 1234 status\n"
    "  cabbird-cli --pid 1234 plugins\n"
    "  cabbird-cli --pid 1234 modules\n"
    "  cabbird-cli --pid 1234 profile\n";
void Usage(std::ostream& stream) { stream << kUsageText; }


bool WriteRequest(const HANDLE pipe, const std::string_view request, DWORD& error) {
    const DWORD request_size = static_cast<DWORD>(request.size());
    DWORD written{};
    if (WriteFile(
            pipe, request.data(), request_size, &written, nullptr) == FALSE) {
        error = GetLastError();
        return false;
    }
    if (written != request_size) {
        error = ERROR_WRITE_FAULT;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool WriteResponseChunk(const char* data, const DWORD size) {
    if (size == 0) return true;
    std::cout.write(data, size);
    return static_cast<bool>(std::cout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2) {
        const std::wstring_view first(argv[1]);
        if (first == L"--help" || first == L"-h" || first == L"/?" || first == L"help") {
            Usage(std::cout);
            return 0;
        }
    }
    if (argc < 4 || std::wstring_view(argv[1]) != L"--pid") {
        Usage(std::cerr);
        return 2;
    }
    const std::wstring_view pid_text(argv[2]);
    if (pid_text.empty() || !std::all_of(pid_text.begin(), pid_text.end(),
            [](wchar_t character) { return character >= L'0' && character <= L'9'; })) {
        Usage(std::cerr);
        return 2;
    }
    errno = 0;
    wchar_t* end{};
    const unsigned long pid = wcstoul(argv[2], &end, 10);
    if (pid == 0 || errno == ERANGE || end == argv[2] || *end != L'\0') {
        Usage(std::cerr);
        return 2;
    }

    std::string request;
    for (int index = 3; index < argc; ++index) {
        const auto argument = Utf8(argv[index]);
        if (!argument) {
            std::cerr << "argument cannot be encoded as UTF-8\n";
            return 2;
        }
        if (argument->find_first_of("\r\n") != std::string::npos) {
            std::cerr << "arguments must not contain line breaks\n";
            return 2;
        }
        const std::size_t separator = request.empty() ? 0U : 1U;
        const std::size_t maximum_command_bytes = kMaximumRequestBytes - 1U;
        if (request.size() > maximum_command_bytes - separator ||
            argument->size() > maximum_command_bytes - request.size() - separator) {
            std::cerr << "request exceeds the 64 KiB diagnostics pipe limit\n";
            return 2;
        }
        if (separator != 0) request.push_back(' ');
        request += *argument;
    }
    if (request.empty()) {
        Usage(std::cerr);
        return 2;
    }
    request.push_back('\n');
    const std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\Cabbird-" + std::to_wstring(pid);
    const std::string pipe_name_utf8 = Utf8(pipe_name).value_or(std::string{});
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD pipe_error = ERROR_FILE_NOT_FOUND;
    for (int attempt = 0; attempt < 50; ++attempt) {
        pipe = CreateFileW(
            pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        pipe_error = GetLastError();
        if (pipe_error == ERROR_PIPE_BUSY) {
            static_cast<void>(WaitNamedPipeW(pipe_name.c_str(), 100));
        } else if (pipe_error == ERROR_FILE_NOT_FOUND) {
            Sleep(100);
        } else {
            break;
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "pipe connection failed: " << pipe_error << " (" << pipe_name_utf8 << ")\n";
        if (pipe_error == ERROR_FILE_NOT_FOUND) {
            std::cerr << "hint: no diagnostics pipe for that PID; confirm the PID is the running "
                         "game process and that the Cabbird runtime is loaded\n";
        } else if (pipe_error == ERROR_ACCESS_DENIED) {
            std::cerr << "hint: access denied; the pipe belongs to a runtime of a higher integrity "
                         "level, or to another Windows user\n";
        }
        return 3;
    }

    DWORD read_mode = PIPE_READMODE_MESSAGE;
    const BOOL mode_configured = SetNamedPipeHandleState(pipe, &read_mode, nullptr, nullptr);
    const DWORD mode_error = mode_configured != FALSE ? ERROR_SUCCESS : GetLastError();
    if (mode_configured == FALSE && mode_error != ERROR_INVALID_PARAMETER) {
        std::cerr << "pipe read-mode configuration failed: " << mode_error << '\n';
        CloseHandle(pipe);
        return 4;
    }

    DWORD write_error{};
    if (!WriteRequest(pipe, request, write_error)) {
        std::cerr << "pipe write failed: " << write_error << '\n';
        CloseHandle(pipe);
        return 4;
    }

    std::array<char, kResponseChunkBytes> buffer{};
    bool received_response{};
    for (;;) {
        DWORD read{};
        const BOOL read_succeeded = ReadFile(
            pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
        const DWORD read_error = read_succeeded != FALSE ? ERROR_SUCCESS : GetLastError();

        if (!WriteResponseChunk(buffer.data(), read)) {
            std::cerr << "stdout write failed\n";
            CloseHandle(pipe);
            return 5;
        }
        received_response = received_response || read != 0;

        if (read_succeeded != FALSE) {
            if (read == 0) break;
            continue;
        }
        if (read_error == ERROR_MORE_DATA && read != 0) continue;
        if (read_error == ERROR_BROKEN_PIPE || read_error == ERROR_NO_DATA ||
            read_error == ERROR_PIPE_NOT_CONNECTED) {
            break;
        }
        std::cerr << "pipe read failed: " << read_error << '\n';
        CloseHandle(pipe);
        return 5;
    }
    CloseHandle(pipe);
    if (!received_response) {
        std::cerr << "pipe closed before returning a response\n";
        return 5;
    }
    return std::cout ? 0 : 5;
}
