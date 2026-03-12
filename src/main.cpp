#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winnetwk.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

namespace {

struct Entry {
    std::wstring source;
    std::wstring link;
    std::wstring kind;
    bool replace_gateway = true;
};

struct MappingSettings {
    std::wstring drive = L"Z:";
    std::wstring username = L"guest";
    std::wstring password;
    bool persist = true;
    bool default_replace_gateway = true;
};

struct Config {
    MappingSettings mapping;
    std::vector<Entry> entries;
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

int RunProcess(const std::wstring& command_line);

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

std::wstring NormalizeDriveLetter(std::wstring value) {
    value = Trim(value);
    if (value.size() == 1 && ((value[0] >= L'a' && value[0] <= L'z') || (value[0] >= L'A' && value[0] <= L'Z'))) {
        value.push_back(L':');
    }
    if (value.size() != 2 || value[1] != L':' || !((value[0] >= L'a' && value[0] <= L'z') || (value[0] >= L'A' && value[0] <= L'Z'))) {
        throw std::runtime_error("drive must be a single letter like Z:");
    }
    value[0] = static_cast<wchar_t>(towupper(value[0]));
    return value;
}

void ApplySettingsBlock(const std::vector<std::pair<std::wstring, std::wstring>>& block, Config* config) {
    for (const auto& item : block) {
        const std::wstring& key = item.first;
        const std::wstring& value = item.second;
        if (key == L"drive") {
            config->mapping.drive = NormalizeDriveLetter(value);
        } else if (key == L"username") {
            config->mapping.username = value.empty() ? L"guest" : value;
        } else if (key == L"password") {
            config->mapping.password = value;
        } else if (key == L"persist") {
            config->mapping.persist = ParseBool(value);
        } else if (key == L"replace_gateway") {
            config->mapping.default_replace_gateway = ParseBool(value);
        } else {
            throw std::runtime_error("unknown config settings key");
        }
    }
}

Entry ParseEntryBlock(const std::vector<std::pair<std::wstring, std::wstring>>& block, const Config& config) {
    Entry entry;
    entry.replace_gateway = config.mapping.default_replace_gateway;
    for (const auto& item : block) {
        const std::wstring& key = item.first;
        const std::wstring& value = item.second;
        if (key == L"source") {
            entry.source = value;
        } else if (key == L"link") {
            entry.link = value;
        } else if (key == L"kind") {
            entry.kind = Lower(value);
        } else if (key == L"replace_gateway") {
            entry.replace_gateway = ParseBool(value);
        } else if (key == L"drive" || key == L"username" || key == L"password" || key == L"persist") {
            throw std::runtime_error("settings keys must be in their own block before link entries");
        } else {
            throw std::runtime_error("unknown config entry key");
        }
    }
    if (entry.source.empty() || entry.link.empty()) {
        throw std::runtime_error("config entry requires both source and link");
    }
    return entry;
}

void FinalizeBlock(const std::vector<std::pair<std::wstring, std::wstring>>& block, Config* config) {
    if (block.empty()) {
        return;
    }

    bool has_entry_key = false;
    for (const auto& item : block) {
        if (item.first == L"source" || item.first == L"link" || item.first == L"kind") {
            has_entry_key = true;
            break;
        }
    }

    if (has_entry_key) {
        config->entries.push_back(ParseEntryBlock(block, *config));
    } else {
        ApplySettingsBlock(block, config);
    }
}

Config LoadConfig(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open config file");
    }

    Config config;
    std::string raw_line;
    int line_number = 0;
    std::vector<std::pair<std::wstring, std::wstring>> current_block;
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
            FinalizeBlock(current_block, &config);
            current_block.clear();
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
        current_block.emplace_back(key, value);
    }
    FinalizeBlock(current_block, &config);
    if (config.entries.empty()) {
        throw std::runtime_error("config contains no entries");
    }
    config.mapping.drive = NormalizeDriveLetter(config.mapping.drive);
    if (config.mapping.username.empty()) {
        config.mapping.username = L"guest";
    }
    return config;
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

struct UncPath {
    std::wstring host;
    std::wstring root;
    std::wstring relative;
};

UncPath SplitUncPath(const std::wstring& path) {
    if (path.rfind(L"\\\\", 0) != 0) {
        throw std::runtime_error("source must be a UNC path");
    }

    const size_t host_sep = path.find(L'\\', 2);
    if (host_sep == std::wstring::npos || host_sep == 2) {
        throw std::runtime_error("UNC path must contain a host and share");
    }

    const size_t share_start = host_sep + 1;
    const size_t share_sep = path.find(L'\\', share_start);
    const std::wstring host = path.substr(2, host_sep - 2);
    if (share_sep == std::wstring::npos) {
        const std::wstring share = path.substr(share_start);
        if (share.empty()) {
            throw std::runtime_error("UNC path must contain a share");
        }
        return UncPath{host, path, L""};
    }

    const std::wstring share = path.substr(share_start, share_sep - share_start);
    if (share.empty()) {
        throw std::runtime_error("UNC path must contain a share");
    }

    return UncPath{
        host,
        path.substr(0, share_sep),
        path.substr(share_sep + 1),
    };
}

std::wstring BuildDriveTarget(const std::wstring& drive, const UncPath& unc) {
    if (unc.relative.empty()) {
        return drive + L"\\";
    }
    return drive + L"\\" + unc.relative;
}

void ValidateSingleShareRoot(const Config& config, const std::optional<std::wstring>& gateway_ip, std::wstring* remote_root, std::wstring* remote_host) {
    for (const Entry& entry : config.entries) {
        const std::wstring source = ResolveSource(entry, gateway_ip);
        const UncPath unc = SplitUncPath(source);
        if (remote_root->empty()) {
            *remote_root = unc.root;
            *remote_host = unc.host;
            continue;
        }
        if (!EqualsInsensitive(*remote_root, unc.root)) {
            throw std::runtime_error("all sources must be under the same UNC share when using one mapped drive");
        }
    }
}

std::wstring GetMappedRemotePath(const std::wstring& drive) {
    DWORD size = 0;
    DWORD error = WNetGetConnectionW(drive.c_str(), nullptr, &size);
    if (error == ERROR_NOT_CONNECTED || error == ERROR_BAD_DEVICE) {
        return L"";
    }
    if (error != ERROR_MORE_DATA) {
        std::ostringstream msg;
        msg << "WNetGetConnectionW failed. Win32=" << error;
        throw std::runtime_error(msg.str());
    }

    std::wstring buffer(size, L'\0');
    error = WNetGetConnectionW(drive.c_str(), buffer.data(), &size);
    if (error != NO_ERROR) {
        std::ostringstream msg;
        msg << "WNetGetConnectionW failed. Win32=" << error;
        throw std::runtime_error(msg.str());
    }

    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

void CancelNetworkConnection(const std::wstring& name) {
    const DWORD error = WNetCancelConnection2W(name.c_str(), CONNECT_UPDATE_PROFILE, TRUE);
    if (error == NO_ERROR || error == ERROR_NOT_CONNECTED || error == ERROR_BAD_DEVICE || error == ERROR_CONNECTION_UNAVAIL) {
        return;
    }
    std::ostringstream msg;
    msg << "WNetCancelConnection2W failed. Win32=" << error;
    throw std::runtime_error(msg.str());
}

void CancelHostConnections(const std::wstring& host) {
    HANDLE enumeration = nullptr;
    DWORD error = WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &enumeration);
    if (error == ERROR_NO_NETWORK || error == ERROR_EXTENDED_ERROR) {
        return;
    }
    if (error != NO_ERROR) {
        std::ostringstream msg;
        msg << "WNetOpenEnumW failed. Win32=" << error;
        throw std::runtime_error(msg.str());
    }

    std::vector<unsigned char> buffer(16 * 1024);
    for (;;) {
        DWORD count = 0xFFFFFFFF;
        DWORD size = static_cast<DWORD>(buffer.size());
        error = WNetEnumResourceW(enumeration, &count, buffer.data(), &size);
        if (error == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (error == ERROR_MORE_DATA) {
            buffer.resize(size);
            continue;
        }
        if (error != NO_ERROR) {
            WNetCloseEnum(enumeration);
            std::ostringstream msg;
            msg << "WNetEnumResourceW failed. Win32=" << error;
            throw std::runtime_error(msg.str());
        }

        auto* resources = reinterpret_cast<NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            const NETRESOURCEW& resource = resources[i];
            if (resource.lpRemoteName == nullptr) {
                continue;
            }

            try {
                const UncPath unc = SplitUncPath(resource.lpRemoteName);
                if (!EqualsInsensitive(unc.host, host)) {
                    continue;
                }
            } catch (...) {
                continue;
            }

            if (resource.lpLocalName != nullptr && resource.lpLocalName[0] != L'\0') {
                CancelNetworkConnection(resource.lpLocalName);
            } else {
                CancelNetworkConnection(resource.lpRemoteName);
            }
        }
    }

    WNetCloseEnum(enumeration);
}

std::wstring SystemToolPath(const wchar_t* exe_name) {
    wchar_t buffer[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(buffer, ARRAYSIZE(buffer));
    if (len == 0 || len >= ARRAYSIZE(buffer)) {
        throw std::runtime_error("GetSystemDirectoryW failed");
    }
    return std::wstring(buffer) + L"\\" + exe_name;
}

void DeleteWindowsCredential(const std::wstring& host) {
    const std::wstring cmd =
        Quote(SystemToolPath(L"cmdkey.exe")) + L" /delete:" + Quote(host);

    const int code = RunProcess(cmd);
    if (code == 0) {
        return;
    }

    const int generic_target_code = RunProcess(
        Quote(SystemToolPath(L"cmdkey.exe")) + L" /delete:" + Quote(L"Domain:target=" + host));
    if (generic_target_code == 0) {
        return;
    }
}

void PersistWindowsCredential(const std::wstring& host, const MappingSettings& settings) {
    DeleteWindowsCredential(host);

    const std::wstring cmd =
        Quote(SystemToolPath(L"cmdkey.exe")) + L" /add:" + Quote(host) +
        L" /user:" + Quote(settings.username) +
        L" /pass:" + Quote(settings.password);

    const int code = RunProcess(cmd);
    if (code != 0) {
        throw std::runtime_error("cmdkey failed to persist SMB credentials");
    }
}

void MapDrive(const Config& config, const std::wstring& remote_root, const std::wstring& remote_host) {
    const std::wstring drive = config.mapping.drive;
    const std::wstring drive_root = drive + L"\\";

    const UINT drive_type = GetDriveTypeW(drive_root.c_str());
    if (drive_type != DRIVE_NO_ROOT_DIR && drive_type != DRIVE_REMOTE) {
        throw std::runtime_error("configured drive letter is already used by a local device");
    }

    const std::wstring existing_remote = GetMappedRemotePath(drive);
    if (!existing_remote.empty() && !EqualsInsensitive(existing_remote, remote_root)) {
        CancelNetworkConnection(drive);
    } else if (!existing_remote.empty()) {
        CancelNetworkConnection(drive);
    }

    CancelHostConnections(remote_host);
    CancelNetworkConnection(remote_root);
    PersistWindowsCredential(remote_host, config.mapping);

    NETRESOURCEW resource = {};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpLocalName = const_cast<LPWSTR>(drive.c_str());
    resource.lpRemoteName = const_cast<LPWSTR>(remote_root.c_str());

    DWORD flags = config.mapping.persist ? CONNECT_UPDATE_PROFILE : 0;
    DWORD error = WNetAddConnection2W(
        &resource,
        config.mapping.password.c_str(),
        config.mapping.username.c_str(),
        flags);

    if (error == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        CancelNetworkConnection(remote_root);
        error = WNetAddConnection2W(
            &resource,
            config.mapping.password.c_str(),
            config.mapping.username.c_str(),
            flags);
    }

    if (error != NO_ERROR) {
        std::ostringstream msg;
        msg << "WNetAddConnection2W failed. Win32=" << error;
        throw std::runtime_error(msg.str());
    }
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
    const Config config = LoadConfig(options.config_path);
    std::optional<std::wstring> gateway_ip = options.gateway_ip_override;
    if (!gateway_ip.has_value()) {
        bool needs_gateway = false;
        for (const Entry& entry : config.entries) {
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

    std::wstring remote_root;
    std::wstring remote_host;
    ValidateSingleShareRoot(config, gateway_ip, &remote_root, &remote_host);
    MapDrive(config, remote_root, remote_host);
    std::wcout << L"Drive  " << config.mapping.drive << L" -> " << remote_root << std::endl;

    int failures = 0;
    for (const Entry& entry : config.entries) {
        try {
            const std::wstring source = ResolveSource(entry, gateway_ip);
            const UncPath unc = SplitUncPath(source);
            if (!EqualsInsensitive(unc.root, remote_root)) {
                throw std::runtime_error("source does not match mapped drive root");
            }

            const std::wstring target = BuildDriveTarget(config.mapping.drive, unc);
            const bool is_directory = IsDirectoryTarget(entry, target);
            const fs::path link = entry.link;
            ValidateReachablePath(target, is_directory, L"Mapped target");

            std::error_code ec;
            fs::create_directories(link.parent_path(), ec);
            if (ec) {
                throw std::runtime_error("failed to create parent directory");
            }

            std::wstring existing_target;
            if (TryReadSymlinkTarget(link, &existing_target)) {
                if (EqualsInsensitive(existing_target, target)) {
                    ValidateReachablePath(link.native(), is_directory, L"Existing link target");
                    SetReadonly(link, true);
                    std::wcout << L"OK    " << link.native() << L" -> " << target << std::endl;
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
            if (!CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) {
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
            std::wcout << L"LINK  " << link.native() << L" -> " << target << std::endl;
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
    return SystemToolPath(L"schtasks.exe");
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
