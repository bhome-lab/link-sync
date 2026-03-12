#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winnetwk.h>
#include <ws2tcpip.h>

#include "../third_party/json11/json11.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;
using json11::Json;

namespace {

struct LinkConfig {
    size_t index = 0;
    std::wstring source;
    std::wstring link;
    std::wstring kind;
    std::optional<std::wstring> drive;
    std::optional<std::wstring> remote;
    std::optional<std::wstring> username;
    std::optional<std::wstring> password;
    std::optional<bool> persist;
    std::optional<bool> replace_gateway;
    std::wstring parse_error;
};

struct MappingDefaults {
    std::wstring drive = L"Z:";
    std::wstring remote;
    std::wstring username = L"guest";
    std::wstring password;
    bool persist = true;
    bool replace_gateway = true;
};

struct Config {
    MappingDefaults defaults;
    std::vector<LinkConfig> links;
};

struct EffectiveSettings {
    std::wstring drive;
    std::wstring remote;
    std::wstring username;
    std::wstring password;
    bool persist = true;
    bool replace_gateway = true;
};

struct ParsedUnc {
    std::wstring host;
    std::vector<std::wstring> segments;
};

struct MappingAssignment {
    std::wstring drive;
    std::wstring remote_root;
    std::wstring remote_host;
    std::wstring username;
    std::wstring password;
    bool persist = true;

    auto Tie() const {
        return std::tie(drive, remote_root, remote_host, username, password, persist);
    }

    bool operator<(const MappingAssignment& other) const {
        return Tie() < other.Tie();
    }

    bool operator==(const MappingAssignment& other) const {
        return Tie() == other.Tie();
    }
};

struct PlannedLink {
    size_t index = 0;
    std::wstring display_name;
    std::wstring kind;
    fs::path link;
    std::wstring target;
    MappingAssignment mapping;
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

struct SyncSummary {
    int mappings_ok = 0;
    int mappings_failed = 0;
    int links_ok = 0;
    int links_failed = 0;
};

struct EnsureOutcome {
    bool success = false;
    bool needs_host_reset = false;
    std::wstring error;
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
    if (!value.empty()) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

bool EqualsInsensitive(const std::wstring& left, const std::wstring& right) {
    return Lower(left) == Lower(right);
}

std::wstring NormalizeDriveLetter(std::wstring value) {
    value = Trim(value);
    if (value.size() == 1 &&
        ((value[0] >= L'a' && value[0] <= L'z') || (value[0] >= L'A' && value[0] <= L'Z'))) {
        value.push_back(L':');
    }

    if (value.size() != 2 || value[1] != L':' ||
        !((value[0] >= L'a' && value[0] <= L'z') || (value[0] >= L'A' && value[0] <= L'Z'))) {
        throw std::runtime_error("drive must be a single letter like Z:");
    }

    value[0] = static_cast<wchar_t>(towupper(value[0]));
    return value;
}

std::wstring Quote(const std::wstring& value) {
    if (value.empty()) {
        return L"\"\"";
    }
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

std::string ReadFileUtf8Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open config file");
    }

    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return bytes;
}

const Json* FindMember(const Json::object& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

const Json::object& EnsureObject(const Json& value, const char* label) {
    if (!value.is_object()) {
        throw std::runtime_error(std::string(label) + " must be a JSON object");
    }
    return value.object_items();
}

const Json::array& EnsureArray(const Json& value, const char* label) {
    if (!value.is_array()) {
        throw std::runtime_error(std::string(label) + " must be a JSON array");
    }
    return value.array_items();
}

std::optional<std::wstring> GetOptionalString(const Json::object& object, const char* key) {
    const Json* value = FindMember(object, key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (!value->is_string()) {
        throw std::runtime_error(std::string(key) + " must be a string");
    }
    return ToWide(value->string_value());
}

std::optional<bool> GetOptionalBool(const Json::object& object, const char* key) {
    const Json* value = FindMember(object, key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (!value->is_bool()) {
        throw std::runtime_error(std::string(key) + " must be a boolean");
    }
    return value->bool_value();
}

void RejectUnknownKeys(const Json::object& object, const std::set<std::string>& allowed, const char* label) {
    for (const auto& item : object) {
        if (allowed.find(item.first) == allowed.end()) {
            throw std::runtime_error(std::string(label) + " contains unknown key: " + item.first);
        }
    }
}

MappingDefaults ParseDefaults(const Json& value) {
    const Json::object& object = EnsureObject(value, "defaults");
    RejectUnknownKeys(object, {"drive", "remote", "username", "password", "persist", "replace_gateway"}, "defaults");

    MappingDefaults defaults;
    if (const auto drive = GetOptionalString(object, "drive")) {
        defaults.drive = NormalizeDriveLetter(*drive);
    }
    if (const auto remote = GetOptionalString(object, "remote")) {
        defaults.remote = *remote;
    }
    if (const auto username = GetOptionalString(object, "username")) {
        defaults.username = username->empty() ? L"guest" : *username;
    }
    if (const auto password = GetOptionalString(object, "password")) {
        defaults.password = *password;
    }
    if (const auto persist = GetOptionalBool(object, "persist")) {
        defaults.persist = *persist;
    }
    if (const auto replace_gateway = GetOptionalBool(object, "replace_gateway")) {
        defaults.replace_gateway = *replace_gateway;
    }
    return defaults;
}

LinkConfig ParseLinkObject(size_t index, const Json& value) {
    const Json::object& object = EnsureObject(value, "links[]");
    RejectUnknownKeys(object, {"source", "link", "kind", "drive", "remote", "username", "password", "persist", "replace_gateway"}, "links[]");

    LinkConfig entry;
    entry.index = index;
    if (const auto source = GetOptionalString(object, "source")) {
        entry.source = *source;
    }
    if (const auto link = GetOptionalString(object, "link")) {
        entry.link = *link;
    }
    if (const auto kind = GetOptionalString(object, "kind")) {
        entry.kind = *kind;
    }
    entry.drive = GetOptionalString(object, "drive");
    entry.remote = GetOptionalString(object, "remote");
    entry.username = GetOptionalString(object, "username");
    entry.password = GetOptionalString(object, "password");
    entry.persist = GetOptionalBool(object, "persist");
    entry.replace_gateway = GetOptionalBool(object, "replace_gateway");
    return entry;
}

Config LoadConfig(const fs::path& path) {
    std::string error;
    const Json root = Json::parse(ReadFileUtf8Bytes(path), error);
    if (!error.empty()) {
        throw std::runtime_error("JSON parse error: " + error);
    }

    const Json::object& object = EnsureObject(root, "config");
    RejectUnknownKeys(object, {"defaults", "links"}, "config");

    Config config;
    if (const Json* defaults = FindMember(object, "defaults")) {
        config.defaults = ParseDefaults(*defaults);
    }

    const Json* links_value = FindMember(object, "links");
    if (links_value == nullptr) {
        throw std::runtime_error("config must contain a links array");
    }

    const Json::array& links = EnsureArray(*links_value, "links");
    if (links.empty()) {
        throw std::runtime_error("config contains no links");
    }

    config.links.reserve(links.size());
    for (size_t i = 0; i < links.size(); ++i) {
        try {
            config.links.push_back(ParseLinkObject(i + 1, links[i]));
        } catch (const std::exception& ex) {
            LinkConfig entry;
            entry.index = i + 1;
            entry.parse_error = ToWide(ex.what());
            config.links.push_back(std::move(entry));
        }
    }

    config.defaults.drive = NormalizeDriveLetter(config.defaults.drive);
    if (config.defaults.username.empty()) {
        config.defaults.username = L"guest";
    }
    return config;
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
    return GetModulePath().parent_path() / L"links.json";
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
    if (address == nullptr || address->sa_family != AF_INET) {
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

class LazyGatewayResolver {
public:
    explicit LazyGatewayResolver(std::optional<std::wstring> override_ip)
        : override_ip_(std::move(override_ip)) {}

    std::wstring Resolve() {
        if (override_ip_.has_value()) {
            return *override_ip_;
        }
        if (!detected_ip_.has_value()) {
            detected_ip_ = DetectGatewayIp();
        }
        return *detected_ip_;
    }

    std::optional<std::wstring> Current() const {
        if (override_ip_.has_value()) {
            return override_ip_;
        }
        return detected_ip_;
    }

private:
    std::optional<std::wstring> override_ip_;
    std::optional<std::wstring> detected_ip_;
};

bool IsGatewayPlaceholder(const std::wstring& host) {
    return EqualsInsensitive(host, L"gateway") || EqualsInsensitive(host, L"{gateway}");
}

void NormalizePathSlashes(std::wstring* value) {
    for (wchar_t& ch : *value) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
}

ParsedUnc ParseUnc(std::wstring path) {
    NormalizePathSlashes(&path);
    if (path.rfind(L"\\\\", 0) != 0) {
        throw std::runtime_error("UNC path must start with \\\\");
    }

    const size_t host_end = path.find(L'\\', 2);
    ParsedUnc result;
    if (host_end == std::wstring::npos) {
        result.host = path.substr(2);
        if (result.host.empty()) {
            throw std::runtime_error("UNC path must contain a host");
        }
        return result;
    }

    result.host = path.substr(2, host_end - 2);
    if (result.host.empty()) {
        throw std::runtime_error("UNC path must contain a host");
    }

    size_t pos = host_end + 1;
    while (pos < path.size()) {
        const size_t next = path.find(L'\\', pos);
        const std::wstring segment = path.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        if (!segment.empty()) {
            result.segments.push_back(segment);
        }
        if (next == std::wstring::npos) {
            break;
        }
        pos = next + 1;
    }
    return result;
}

std::vector<std::wstring> SplitRelativeSegments(std::wstring path) {
    NormalizePathSlashes(&path);
    std::vector<std::wstring> segments;
    size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == L'\\') {
            ++pos;
        }
        if (pos >= path.size()) {
            break;
        }

        const size_t next = path.find(L'\\', pos);
        const std::wstring segment = path.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
        if (segment == L"." || segment == L"..") {
            throw std::runtime_error("relative source segments may not contain . or ..");
        }
        segments.push_back(segment);
        if (next == std::wstring::npos) {
            break;
        }
        pos = next + 1;
    }
    return segments;
}

std::wstring JoinSegments(const std::vector<std::wstring>& segments, size_t start = 0) {
    std::wstring result;
    for (size_t i = start; i < segments.size(); ++i) {
        if (!result.empty()) {
            result.append(L"\\");
        }
        result.append(segments[i]);
    }
    return result;
}

std::wstring BuildDriveTarget(const std::wstring& drive, const std::vector<std::wstring>& segments) {
    if (segments.empty()) {
        return drive + L"\\";
    }
    return drive + L"\\" + JoinSegments(segments);
}

std::wstring BuildRemoteRoot(const std::wstring& host, const std::wstring& share) {
    if (host.empty() || share.empty()) {
        throw std::runtime_error("remote root requires host and share");
    }
    return L"\\\\" + host + L"\\" + share;
}

std::wstring ResolveGatewayPath(const std::wstring& value, bool replace_gateway, LazyGatewayResolver* resolver) {
    if (!replace_gateway || value.rfind(L"\\\\", 0) != 0) {
        return value;
    }

    ParsedUnc unc = ParseUnc(value);
    if (!IsGatewayPlaceholder(unc.host)) {
        return value;
    }

    std::wstring result = L"\\\\" + resolver->Resolve();
    if (!unc.segments.empty()) {
        result.append(L"\\");
        result.append(JoinSegments(unc.segments));
    }
    return result;
}

EffectiveSettings ResolveSettings(const MappingDefaults& defaults, const LinkConfig& entry) {
    EffectiveSettings settings;
    settings.drive = entry.drive.has_value() ? NormalizeDriveLetter(*entry.drive) : defaults.drive;
    settings.remote = entry.remote.value_or(defaults.remote);
    settings.username = entry.username.has_value() ? *entry.username : defaults.username;
    settings.password = entry.password.value_or(defaults.password);
    settings.persist = entry.persist.value_or(defaults.persist);
    settings.replace_gateway = entry.replace_gateway.value_or(defaults.replace_gateway);
    if (settings.username.empty()) {
        settings.username = L"guest";
    }
    return settings;
}

std::wstring DisplayLabel(const LinkConfig& entry) {
    if (!entry.link.empty()) {
        return entry.link;
    }
    return L"link[" + std::to_wstring(entry.index) + L"]";
}

PlannedLink PrepareLink(const MappingDefaults& defaults, const LinkConfig& entry, LazyGatewayResolver* gateway) {
    if (!entry.parse_error.empty()) {
        throw std::runtime_error(ToUtf8(entry.parse_error));
    }
    if (entry.source.empty()) {
        throw std::runtime_error("source is required");
    }
    if (entry.link.empty()) {
        throw std::runtime_error("link is required");
    }

    PlannedLink planned;
    planned.index = entry.index;
    planned.display_name = DisplayLabel(entry);
    planned.link = fs::path(entry.link);
    planned.kind = Lower(Trim(entry.kind));
    if (!planned.kind.empty() && planned.kind != L"file" && planned.kind != L"directory" && planned.kind != L"dir") {
        throw std::runtime_error("kind must be file or directory");
    }

    const EffectiveSettings settings = ResolveSettings(defaults, entry);
    planned.mapping.drive = settings.drive;
    planned.mapping.username = settings.username;
    planned.mapping.password = settings.password;
    planned.mapping.persist = settings.persist;

    const std::wstring source = ResolveGatewayPath(entry.source, settings.replace_gateway, gateway);
    const std::wstring remote = settings.remote.empty() ? L"" : ResolveGatewayPath(settings.remote, settings.replace_gateway, gateway);

    std::vector<std::wstring> target_segments;
    if (source.rfind(L"\\\\", 0) == 0) {
        const ParsedUnc source_unc = ParseUnc(source);
        if (source_unc.segments.empty()) {
            throw std::runtime_error("source UNC path must include a share");
        }

        if (!remote.empty()) {
            const ParsedUnc remote_unc = ParseUnc(remote);
            if (!EqualsInsensitive(remote_unc.host, source_unc.host)) {
                throw std::runtime_error("remote host does not match source host");
            }
            if (!remote_unc.segments.empty()) {
                if (!EqualsInsensitive(remote_unc.segments.front(), source_unc.segments.front())) {
                    throw std::runtime_error("remote share does not match source share");
                }
                if (remote_unc.segments.size() > source_unc.segments.size()) {
                    throw std::runtime_error("source is not under the configured remote path");
                }
                for (size_t i = 0; i < remote_unc.segments.size(); ++i) {
                    if (!EqualsInsensitive(remote_unc.segments[i], source_unc.segments[i])) {
                        throw std::runtime_error("source is not under the configured remote path");
                    }
                }
            }
        }

        planned.mapping.remote_host = source_unc.host;
        planned.mapping.remote_root = BuildRemoteRoot(source_unc.host, source_unc.segments.front());
        target_segments.assign(source_unc.segments.begin() + 1, source_unc.segments.end());
    } else {
        if (remote.empty()) {
            throw std::runtime_error("relative source requires remote in defaults or the link object");
        }

        const ParsedUnc remote_unc = ParseUnc(remote);
        const std::vector<std::wstring> source_segments = SplitRelativeSegments(source);
        if (remote_unc.segments.empty()) {
            if (source_segments.empty()) {
                throw std::runtime_error("relative source must include a share when remote is host-only");
            }
            planned.mapping.remote_host = remote_unc.host;
            planned.mapping.remote_root = BuildRemoteRoot(remote_unc.host, source_segments.front());
            target_segments.assign(source_segments.begin() + 1, source_segments.end());
        } else {
            planned.mapping.remote_host = remote_unc.host;
            planned.mapping.remote_root = BuildRemoteRoot(remote_unc.host, remote_unc.segments.front());
            target_segments.assign(remote_unc.segments.begin() + 1, remote_unc.segments.end());
            target_segments.insert(target_segments.end(), source_segments.begin(), source_segments.end());
        }
    }

    planned.target = BuildDriveTarget(planned.mapping.drive, target_segments);
    return planned;
}

std::wstring SystemToolPath(const wchar_t* exe_name) {
    wchar_t buffer[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(buffer, ARRAYSIZE(buffer));
    if (len == 0 || len >= ARRAYSIZE(buffer)) {
        throw std::runtime_error("GetSystemDirectoryW failed");
    }
    return std::wstring(buffer) + L"\\" + exe_name;
}

int RunProcess(const std::wstring& command_line) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring mutable_command = command_line;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
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

bool EnsureLinkedConnectionsEnabled() {
    HKEY key = nullptr;
    const LONG open_result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (open_result != ERROR_SUCCESS) {
        std::ostringstream msg;
        msg << "RegCreateKeyExW failed. Win32=" << open_result;
        throw std::runtime_error(msg.str());
    }

    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG query_result = RegQueryValueExW(key, L"EnableLinkedConnections", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    if (query_result == ERROR_SUCCESS && type == REG_DWORD && value == 1) {
        RegCloseKey(key);
        return false;
    }

    value = 1;
    const LONG set_result = RegSetValueExW(key, L"EnableLinkedConnections", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (set_result != ERROR_SUCCESS) {
        std::ostringstream msg;
        msg << "RegSetValueExW failed. Win32=" << set_result;
        throw std::runtime_error(msg.str());
    }
    return true;
}

void DeleteWindowsCredential(const std::wstring& host) {
    const std::wstring delete_host =
        Quote(SystemToolPath(L"cmdkey.exe")) + L" /delete:" + Quote(host);
    if (RunProcess(delete_host) == 0) {
        return;
    }

    static_cast<void>(RunProcess(
        Quote(SystemToolPath(L"cmdkey.exe")) + L" /delete:" + Quote(L"Domain:target=" + host)));
}

void PersistWindowsCredential(const std::wstring& host, const std::wstring& username, const std::wstring& password) {
    DeleteWindowsCredential(host);
    const std::wstring add_host =
        Quote(SystemToolPath(L"cmdkey.exe")) + L" /add:" + Quote(host) +
        L" /user:" + Quote(username) +
        L" /pass:" + Quote(password);
    if (RunProcess(add_host) != 0) {
        throw std::runtime_error("cmdkey failed to persist SMB credentials");
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
                const ParsedUnc unc = ParseUnc(resource.lpRemoteName);
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

EnsureOutcome EnsureMappingOnce(const MappingAssignment& mapping) {
    try {
        const std::wstring drive_root = mapping.drive + L"\\";
        const UINT drive_type = GetDriveTypeW(drive_root.c_str());
        if (drive_type != DRIVE_NO_ROOT_DIR && drive_type != DRIVE_REMOTE) {
            return {false, false, L"configured drive letter is already used by a local device"};
        }

        const std::wstring existing_remote = GetMappedRemotePath(mapping.drive);
        if (!existing_remote.empty() && EqualsInsensitive(existing_remote, mapping.remote_root)) {
            return {true, false, L""};
        }
        if (!existing_remote.empty()) {
            CancelNetworkConnection(mapping.drive);
        }

        NETRESOURCEW resource = {};
        resource.dwType = RESOURCETYPE_DISK;
        resource.lpLocalName = const_cast<LPWSTR>(mapping.drive.c_str());
        resource.lpRemoteName = const_cast<LPWSTR>(mapping.remote_root.c_str());

        DWORD flags = mapping.persist ? CONNECT_UPDATE_PROFILE : 0;
        const DWORD error = WNetAddConnection2W(
            &resource,
            mapping.password.c_str(),
            mapping.username.c_str(),
            flags);
        if (error == NO_ERROR) {
            return {true, false, L""};
        }
        if (error == ERROR_SESSION_CREDENTIAL_CONFLICT) {
            return {false, true, L""};
        }

        std::wstringstream stream;
        stream << L"WNetAddConnection2W failed. Win32=" << error;
        return {false, false, stream.str()};
    } catch (const std::exception& ex) {
        return {false, false, ToWide(ex.what())};
    }
}

bool IsDirectoryTarget(const PlannedLink& planned) {
    if (planned.kind == L"file") {
        return false;
    }
    if (planned.kind == L"directory" || planned.kind == L"dir") {
        return true;
    }

    const DWORD attrs = GetFileAttributesW(planned.target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("target is not reachable; set kind=file or kind=directory");
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

void SyncLink(const PlannedLink& planned) {
    const bool is_directory = IsDirectoryTarget(planned);
    ValidateReachablePath(planned.target, is_directory, L"Mapped target");

    std::error_code ec;
    const fs::path parent = planned.link.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("failed to create link parent directory");
        }
    }

    std::wstring existing_target;
    if (TryReadSymlinkTarget(planned.link, &existing_target)) {
        if (EqualsInsensitive(existing_target, planned.target)) {
            ValidateReachablePath(planned.link.native(), is_directory, L"Existing link target");
            SetReadonly(planned.link, true);
            std::wcout << L"OK    " << planned.link.native() << L" -> " << planned.target << std::endl;
            return;
        }
        SetReadonly(planned.link, false);
        fs::remove(planned.link, ec);
        if (ec) {
            throw std::runtime_error("failed to remove stale link");
        }
    } else if (fs::exists(planned.link, ec)) {
        throw std::runtime_error("refusing to replace a non-link path");
    }

    DWORD flags = is_directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#ifdef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
    flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
    if (!CreateSymbolicLinkW(planned.link.c_str(), planned.target.c_str(), flags)) {
        std::ostringstream msg;
        msg << "CreateSymbolicLinkW failed. Win32=" << GetLastError();
        throw std::runtime_error(msg.str());
    }

    try {
        ValidateReachablePath(planned.link.native(), is_directory, L"Created link target");
    } catch (...) {
        SetReadonly(planned.link, false);
        fs::remove(planned.link, ec);
        throw;
    }

    SetReadonly(planned.link, true);
    std::wcout << L"LINK  " << planned.link.native() << L" -> " << planned.target << std::endl;
}

std::wstring SchtasksPath() {
    return SystemToolPath(L"schtasks.exe");
}

int Install(const Options& options) {
    static_cast<void>(LoadConfig(options.config_path));
    const bool linked_connections_changed = EnsureLinkedConnectionsEnabled();

    std::wstring task_command = Quote(GetModulePath().native()) + L" sync --config " + Quote(fs::absolute(options.config_path).native());
    if (options.gateway_ip_override.has_value()) {
        task_command += L" --gateway-ip " + Quote(*options.gateway_ip_override);
    }

    const std::wstring command =
        Quote(SchtasksPath()) + L" /Create /SC ONLOGON /RL HIGHEST /TN " + Quote(options.task_name) +
        L" /TR " + Quote(task_command) + L" /F";
    if (RunProcess(command) != 0) {
        throw std::runtime_error("schtasks create failed");
    }

    std::wcout << L"Installed scheduled task '" << options.task_name << L"'." << std::endl;
    if (linked_connections_changed) {
        std::wcout << L"EnableLinkedConnections was enabled. Sign out and sign back in if Explorer does not show mapped drives yet." << std::endl;
    }
    return 0;
}

int Uninstall(const Options& options) {
    const std::wstring command =
        Quote(SchtasksPath()) + L" /Delete /TN " + Quote(options.task_name) + L" /F";
    if (RunProcess(command) != 0) {
        throw std::runtime_error("schtasks delete failed");
    }
    std::wcout << L"Removed scheduled task '" << options.task_name << L"'." << std::endl;
    return 0;
}

int Sync(const Options& options) {
    const Config config = LoadConfig(options.config_path);

    const bool linked_connections_changed = EnsureLinkedConnectionsEnabled();
    if (linked_connections_changed) {
        std::wcout << L"INFO  EnableLinkedConnections was enabled. Explorer visibility may require a fresh sign-in." << std::endl;
    }

    LazyGatewayResolver gateway(options.gateway_ip_override);
    std::map<MappingAssignment, std::vector<PlannedLink>> mapping_links;
    SyncSummary summary;

    for (const LinkConfig& entry : config.links) {
        try {
            PlannedLink planned = PrepareLink(config.defaults, entry, &gateway);
            mapping_links[planned.mapping].push_back(std::move(planned));
        } catch (const std::exception& ex) {
            ++summary.links_failed;
            std::wcerr << L"FAIL  " << DisplayLabel(entry) << L": " << ToWide(ex.what()) << std::endl;
        }
    }

    if (const auto gateway_ip = gateway.Current()) {
        std::wcout << L"Gateway IP: " << *gateway_ip << std::endl;
    }

    std::map<MappingAssignment, std::wstring> mapping_errors;
    std::map<std::wstring, MappingAssignment> desired_by_drive;
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> host_credentials;
    std::set<std::wstring> conflicted_hosts;

    for (const auto& item : mapping_links) {
        const MappingAssignment& mapping = item.first;

        const std::wstring drive_key = Lower(mapping.drive);
        const auto drive_it = desired_by_drive.find(drive_key);
        if (drive_it == desired_by_drive.end()) {
            desired_by_drive.emplace(drive_key, mapping);
        } else if (!(drive_it->second == mapping)) {
            mapping_errors.emplace(drive_it->second, L"multiple mappings requested the same drive letter with different settings");
            mapping_errors.emplace(mapping, L"multiple mappings requested the same drive letter with different settings");
        }

        const std::wstring host_key = Lower(mapping.remote_host);
        const std::pair<std::wstring, std::wstring> credential(mapping.username, mapping.password);
        const auto host_it = host_credentials.find(host_key);
        if (host_it == host_credentials.end()) {
            host_credentials.emplace(host_key, credential);
        } else if (!EqualsInsensitive(host_it->second.first, credential.first) || host_it->second.second != credential.second) {
            conflicted_hosts.insert(host_key);
        }
    }

    for (const auto& item : mapping_links) {
        if (conflicted_hosts.find(Lower(item.first.remote_host)) != conflicted_hosts.end()) {
            mapping_errors.emplace(item.first, L"multiple mappings for the same host use different credentials");
        }
    }

    std::map<std::wstring, std::vector<MappingAssignment>> host_groups;
    for (const auto& item : mapping_links) {
        if (mapping_errors.find(item.first) == mapping_errors.end()) {
            host_groups[Lower(item.first.remote_host)].push_back(item.first);
        }
    }

    std::set<MappingAssignment> successful_mappings;
    for (auto& host_item : host_groups) {
        std::vector<MappingAssignment>& mappings = host_item.second;
        std::sort(mappings.begin(), mappings.end());
        const std::wstring host = mappings.front().remote_host;

        try {
            PersistWindowsCredential(host, mappings.front().username, mappings.front().password);
        } catch (const std::exception& ex) {
            for (const MappingAssignment& mapping : mappings) {
                mapping_errors.emplace(mapping, L"credential update failed: " + ToWide(ex.what()));
            }
            continue;
        }

        auto attempt_host = [&](bool allow_host_reset, std::set<MappingAssignment>* host_successes, bool* requested_reset) {
            for (const MappingAssignment& mapping : mappings) {
                if (mapping_errors.find(mapping) != mapping_errors.end()) {
                    continue;
                }

                const EnsureOutcome outcome = EnsureMappingOnce(mapping);
                if (outcome.success) {
                    host_successes->insert(mapping);
                    continue;
                }
                if (outcome.needs_host_reset && allow_host_reset) {
                    *requested_reset = true;
                    return;
                }
                mapping_errors.emplace(mapping, outcome.needs_host_reset
                    ? L"another SMB connection already uses different credentials for this host"
                    : outcome.error);
            }
        };

        std::set<MappingAssignment> host_successes;
        bool requested_reset = false;
        attempt_host(true, &host_successes, &requested_reset);
        if (requested_reset) {
            try {
                CancelHostConnections(host);
            } catch (const std::exception& ex) {
                for (const MappingAssignment& mapping : mappings) {
                    mapping_errors.emplace(mapping, L"failed to reconcile host connections: " + ToWide(ex.what()));
                }
                continue;
            }

            host_successes.clear();
            requested_reset = false;
            for (const MappingAssignment& mapping : mappings) {
                mapping_errors.erase(mapping);
            }
            attempt_host(false, &host_successes, &requested_reset);
            if (requested_reset) {
                for (const MappingAssignment& mapping : mappings) {
                    if (host_successes.find(mapping) == host_successes.end()) {
                        mapping_errors.emplace(mapping, L"another SMB connection already uses different credentials for this host");
                    }
                }
            }
        }

        successful_mappings.insert(host_successes.begin(), host_successes.end());
    }

    for (const auto& item : mapping_links) {
        const MappingAssignment& mapping = item.first;
        const auto error_it = mapping_errors.find(mapping);
        if (error_it != mapping_errors.end()) {
            ++summary.mappings_failed;
            for (const PlannedLink& link : item.second) {
                ++summary.links_failed;
                std::wcerr << L"FAIL  " << link.display_name << L": " << error_it->second << std::endl;
            }
            continue;
        }

        if (successful_mappings.find(mapping) == successful_mappings.end()) {
            ++summary.mappings_failed;
            for (const PlannedLink& link : item.second) {
                ++summary.links_failed;
                std::wcerr << L"FAIL  " << link.display_name << L": internal mapping state error" << std::endl;
            }
            continue;
        }

        ++summary.mappings_ok;
        for (const PlannedLink& link : item.second) {
            try {
                SyncLink(link);
                ++summary.links_ok;
            } catch (const std::exception& ex) {
                ++summary.links_failed;
                std::wcerr << L"FAIL  " << link.display_name << L": " << ToWide(ex.what()) << std::endl;
            }
        }
    }

    std::wcout
        << L"SUMMARY mappings_ok=" << summary.mappings_ok
        << L" mappings_failed=" << summary.mappings_failed
        << L" links_ok=" << summary.links_ok
        << L" links_failed=" << summary.links_failed
        << std::endl;
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
