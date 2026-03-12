#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

namespace {

struct Entry {
    std::wstring source;
    std::wstring link;
    std::wstring kind;
    bool replace_gateway = true;
};

struct Options {
    enum class Command {
        Sync,
        Install,
        Uninstall,
        DetectGateway,
    };

    Command command = Command::Sync;
    fs::path config_path;
    std::wstring task_name = L"LinkSync";
    std::optional<std::wstring> gateway_ip_override;
};

std::wstring ToWide(const std::string& text) {
    if (text.empty()) {
        return L"";
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error("UTF-8 decode failed");
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count) <= 0) {
        throw std::runtime_error("UTF-8 decode failed");
    }
    return result;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return "";
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("UTF-8 encode failed");
    }
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr) <= 0) {
        throw std::runtime_error("UTF-8 encode failed");
    }
    return result;
}

std::wstring Trim(std::wstring value) {
    const auto is_space = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    };
    while (!value.empty() && is_space(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring Lower(std::wstring value) {
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

bool EqualsInsensitive(const std::wstring& left, const std::wstring& right) {
    return Lower(left) == Lower(right);
}

bool ParseBool(const std::wstring& text) {
    const std::wstring value = Lower(Trim(text));
    if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") {
        return true;
    }
    if (value == L"0" || value == L"false" || value == L"no" || value == L"off") {
        return false;
    }
    throw std::runtime_error("invalid boolean value");
}

fs::path GetModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (len < buffer.size() - 1) {
            buffer.resize(len);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

fs::path DefaultConfigPath() {
    return GetModulePath().parent_path() / L"links.conf";
}

std::wstring Quote(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    unsigned slash_count = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slash_count;
            continue;
        }
        if (ch == L'"') {
            result.append(slash_count * 2 + 1, L'\\');
            result.push_back(L'"');
            slash_count = 0;
            continue;
        }
        result.append(slash_count, L'\\');
        slash_count = 0;
        result.push_back(ch);
    }
    result.append(slash_count * 2, L'\\');
    result.push_back(L'"');
    return result;
}

void FinalizeEntry(std::vector<Entry>* entries, Entry* current) {
    if (current->source.empty() && current->link.empty() && current->kind.empty()) {
        return;
    }
    if (current->source.empty() || current->link.empty()) {
        throw std::runtime_error("config entry requires both source and link");
    }
    entries->push_back(*current);
    *current = Entry{};
}

std::vector<Entry> LoadConfig(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open config file");
    }

    std::vector<Entry> entries;
    Entry current;
    std::string raw_line;
    int line_number = 0;
    while (std::getline(input, raw_line)) {
        ++line_number;
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.pop_back();
        }
        if (line_number == 1 && raw_line.size() >= 3 &&
            static_cast<unsigned char>(raw_line[0]) == 0xEF &&
            static_cast<unsigned char>(raw_line[1]) == 0xBB &&
            static_cast<unsigned char>(raw_line[2]) == 0xBF) {
            raw_line.erase(0, 3);
        }

        std::wstring line = Trim(ToWide(raw_line));
        if (line.empty()) {
            FinalizeEntry(&entries, &current);
            continue;
        }
        if (line[0] == L'#' || line[0] == L';') {
            continue;
        }

        const size_t sep = line.find(L'=');
        if (sep == std::wstring::npos) {
            throw std::runtime_error("config line must be key=value");
        }

        const std::wstring key = Lower(Trim(line.substr(0, sep)));
        const std::wstring value = Trim(line.substr(sep + 1));
        if (key == L"source") {
            current.source = value;
        } else if (key == L"link") {
            current.link = value;
        } else if (key == L"kind") {
            current.kind = Lower(value);
        } else if (key == L"replace_gateway") {
            current.replace_gateway = ParseBool(value);
        } else {
            throw std::runtime_error("unknown config key");
        }
    }
    FinalizeEntry(&entries, &current);
    if (entries.empty()) {
        throw std::runtime_error("config contains no entries");
    }
    return entries;
}

Options ParseArgs(int argc, wchar_t** argv) {
    Options options;
    options.config_path = DefaultConfigPath();

    int i = 1;
    if (i < argc) {
        const std::wstring command = Lower(argv[i]);
        if (command == L"sync") {
            options.command = Options::Command::Sync;
            ++i;
        } else if (command == L"install") {
            options.command = Options::Command::Install;
            ++i;
        } else if (command == L"uninstall") {
            options.command = Options::Command::Uninstall;
            ++i;
        } else if (command == L"detect-gateway") {
            options.command = Options::Command::DetectGateway;
            ++i;
        }
    }

    while (i < argc) {
        const std::wstring arg = argv[i];
        if ((arg == L"--config" || arg == L"-c") && i + 1 < argc) {
            options.config_path = argv[++i];
        } else if (arg == L"--task-name" && i + 1 < argc) {
            options.task_name = argv[++i];
        } else if (arg == L"--gateway-ip" && i + 1 < argc) {
            options.gateway_ip_override = argv[++i];
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            std::wcout
                << L"Usage:\n"
                << L"  links.exe [sync] [--config <path>] [--gateway-ip <ipv4>]\n"
                << L"  links.exe install [--config <path>] [--task-name <name>] [--gateway-ip <ipv4>]\n"
                << L"  links.exe uninstall [--task-name <name>]\n"
                << L"  links.exe detect-gateway\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument");
        }
        ++i;
    }
    return options;
}

std::wstring SockaddrToString(const SOCKADDR* address) {
    if (address->sa_family != AF_INET) {
        throw std::runtime_error("unsupported address family");
    }
    wchar_t buffer[64] = {};
    auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    if (InetNtopW(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), buffer, ARRAYSIZE(buffer)) == nullptr) {
        throw std::runtime_error("InetNtopW failed");
    }
    return buffer;
}

std::wstring DetectGatewayIp() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    while (result == ERROR_BUFFER_OVERFLOW) {
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (result == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(size);
        }
    }
    if (result != NO_ERROR) {
        throw std::runtime_error("GetAdaptersAddresses failed");
    }

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD) {
            continue;
        }
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (adapter->FirstGatewayAddress == nullptr || adapter->FirstGatewayAddress->Address.lpSockaddr == nullptr) {
            continue;
        }
        const std::wstring ip = SockaddrToString(adapter->FirstGatewayAddress->Address.lpSockaddr);
        if (!ip.empty() && ip != L"0.0.0.0") {
            return ip;
        }
    }
    throw std::runtime_error("no active Ethernet IPv4 gateway found");
}

bool IsGatewayPlaceholder(const std::wstring& host) {
    return EqualsInsensitive(host, L"gateway") || EqualsInsensitive(host, L"{gateway}");
}

std::wstring ResolveSource(const Entry& entry, const std::optional<std::wstring>& gateway_ip) {
    if (!entry.replace_gateway || entry.source.rfind(L"\\\\", 0) != 0) {
        return entry.source;
    }
    const size_t sep = entry.source.find(L'\\', 2);
    if (sep == std::wstring::npos) {
        return entry.source;
    }
    const std::wstring host = entry.source.substr(2, sep - 2);
    if (!IsGatewayPlaceholder(host)) {
        return entry.source;
    }
    if (!gateway_ip.has_value()) {
        throw std::runtime_error("gateway replacement needed but no gateway IP is available");
    }
    return L"\\\\" + *gateway_ip + entry.source.substr(sep);
}

bool IsDirectoryTarget(const Entry& entry, const std::wstring& source) {
    if (entry.kind == L"file") {
        return false;
    }
    if (entry.kind == L"directory" || entry.kind == L"dir") {
        return true;
    }
    const DWORD attrs = GetFileAttributesW(source.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("source is not reachable; set kind=file or kind=directory");
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void ValidateReachablePath(const std::wstring& path, bool is_directory, const wchar_t* label) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::wstringstream stream;
        stream << label << L" path is not reachable: " << path << L" (Win32=" << GetLastError() << L")";
        throw std::runtime_error(ToUtf8(stream.str()));
    }

    const bool actual_directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (actual_directory != is_directory) {
        std::wstringstream stream;
        stream << label << L" kind mismatch for path: " << path;
        throw std::runtime_error(ToUtf8(stream.str()));
    }

    const DWORD flags = is_directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        std::wstringstream stream;
        stream << label << L" path exists but could not be opened: " << path << L" (Win32=" << GetLastError() << L")";
        throw std::runtime_error(ToUtf8(stream.str()));
    }
    CloseHandle(handle);
}

void SetReadonly(const fs::path& path, bool readonly) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("GetFileAttributesW failed");
    }
    if (readonly) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    } else {
        attrs &= ~FILE_ATTRIBUTE_READONLY;
    }
    if (!SetFileAttributesW(path.c_str(), attrs)) {
        throw std::runtime_error("SetFileAttributesW failed");
    }
}

bool TryReadSymlinkTarget(const fs::path& path, std::wstring* target) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || status.type() != fs::file_type::symlink) {
        return false;
    }
    const fs::path target_path = fs::read_symlink(path, ec);
    if (ec) {
        throw std::runtime_error("read_symlink failed");
    }
    *target = target_path.native();
    return true;
}

int Sync(const Options& options) {
    const std::vector<Entry> entries = LoadConfig(options.config_path);
    std::optional<std::wstring> gateway_ip = options.gateway_ip_override;
    if (!gateway_ip.has_value()) {
        bool needs_gateway = false;
        for (const Entry& entry : entries) {
            if (entry.source.rfind(L"\\\\", 0) != 0 || !entry.replace_gateway) {
                continue;
            }
            const size_t sep = entry.source.find(L'\\', 2);
            if (sep != std::wstring::npos && IsGatewayPlaceholder(entry.source.substr(2, sep - 2))) {
                needs_gateway = true;
                break;
            }
        }
        if (needs_gateway) {
            gateway_ip = DetectGatewayIp();
        }
    }

    if (gateway_ip.has_value()) {
        std::wcout << L"Gateway IP: " << *gateway_ip << std::endl;
    }

    int failures = 0;
    for (const Entry& entry : entries) {
        try {
            const std::wstring source = ResolveSource(entry, gateway_ip);
            const bool is_directory = IsDirectoryTarget(entry, source);
            const fs::path link = entry.link;
            ValidateReachablePath(source, is_directory, L"Source");

            std::error_code ec;
            fs::create_directories(link.parent_path(), ec);
            if (ec) {
                throw std::runtime_error("failed to create parent directory");
            }

            std::wstring existing_target;
            if (TryReadSymlinkTarget(link, &existing_target)) {
                if (EqualsInsensitive(existing_target, source)) {
                    ValidateReachablePath(link.native(), is_directory, L"Existing link target");
                    SetReadonly(link, true);
                    std::wcout << L"OK    " << link.native() << L" -> " << source << std::endl;
                    continue;
                }
                SetReadonly(link, false);
                fs::remove(link, ec);
                if (ec) {
                    throw std::runtime_error("failed to remove stale link");
                }
            } else if (fs::exists(link, ec)) {
                throw std::runtime_error("refusing to replace a non-link path");
            }

            DWORD flags = is_directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
            flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
            if (!CreateSymbolicLinkW(link.c_str(), source.c_str(), flags)) {
                std::ostringstream msg;
                msg << "CreateSymbolicLinkW failed. Win32=" << GetLastError();
                throw std::runtime_error(msg.str());
            }
            try {
                ValidateReachablePath(link.native(), is_directory, L"Created link target");
            } catch (...) {
                SetReadonly(link, false);
                fs::remove(link, ec);
                throw;
            }
            SetReadonly(link, true);
            std::wcout << L"LINK  " << link.native() << L" -> " << source << std::endl;
        } catch (const std::exception& ex) {
            ++failures;
            std::wcerr << L"FAIL  " << entry.link << L": " << ToWide(ex.what()) << std::endl;
        }
    }
    return failures == 0 ? 0 : 1;
}

int RunProcess(const std::wstring& command_line) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring mutable_cmd = command_line;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::ostringstream msg;
        msg << "CreateProcessW failed. Win32=" << GetLastError();
        throw std::runtime_error(msg.str());
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
}

std::wstring SchtasksPath() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(buffer, ARRAYSIZE(buffer));
    if (len == 0 || len >= ARRAYSIZE(buffer)) {
        throw std::runtime_error("GetSystemDirectoryW failed");
    }
    return std::wstring(buffer) + L"\\schtasks.exe";
}

int Install(const Options& options) {
    static_cast<void>(LoadConfig(options.config_path));
    std::wstring tr = Quote(GetModulePath().native()) + L" sync --config " + Quote(fs::absolute(options.config_path).native());
    if (options.gateway_ip_override.has_value()) {
        tr += L" --gateway-ip " + Quote(*options.gateway_ip_override);
    }
    const std::wstring cmd =
        Quote(SchtasksPath()) + L" /Create /SC ONLOGON /RL HIGHEST /TN " + Quote(options.task_name) +
        L" /TR " + Quote(tr) + L" /F";
    const int code = RunProcess(cmd);
    if (code != 0) {
        throw std::runtime_error("schtasks create failed");
    }
    std::wcout << L"Installed scheduled task '" << options.task_name << L"'." << std::endl;
    return 0;
}

int Uninstall(const Options& options) {
    const std::wstring cmd =
        Quote(SchtasksPath()) + L" /Delete /TN " + Quote(options.task_name) + L" /F";
    const int code = RunProcess(cmd);
    if (code != 0) {
        throw std::runtime_error("schtasks delete failed");
    }
    std::wcout << L"Removed scheduled task '" << options.task_name << L"'." << std::endl;
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const Options options = ParseArgs(argc, argv);
        switch (options.command) {
        case Options::Command::Sync:
            return Sync(options);
        case Options::Command::Install:
            return Install(options);
        case Options::Command::Uninstall:
            return Uninstall(options);
        case Options::Command::DetectGateway:
            std::wcout << DetectGatewayIp() << std::endl;
            return 0;
        }
    } catch (const std::exception& ex) {
        std::wcerr << L"Error: " << ToWide(ex.what()) << std::endl;
        return 1;
    }
    return 1;
}
