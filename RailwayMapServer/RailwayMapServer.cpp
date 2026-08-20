#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <iomanip>

#include "../MileagePositionDatabase.h"
#include "OsmRailwayMileageProvider.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
constexpr unsigned short kPort = 8765;
std::mutex gClientsMutex;
std::vector<SOCKET> gClients;
std::unique_ptr<MileagePositionDatabase> gMileageDatabase;
std::unique_ptr<OsmRailwayMileageProvider> gOsmProvider;
std::filesystem::path gMileagePath;

std::string ReadFile(const std::filesystem::path& path, const std::string& fallback = {})
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return fallback;
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

bool SendAll(SOCKET socket, const char* data, std::size_t size)
{
    while (size > 0)
    {
        const int sent = send(socket, data, static_cast<int>(std::min<std::size_t>(size, 16384)), 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::string Base64(const unsigned char* data, std::size_t size)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t offset = 0; offset < size; offset += 3)
    {
        const unsigned value = (static_cast<unsigned>(data[offset]) << 16) |
            (offset + 1 < size ? static_cast<unsigned>(data[offset + 1]) << 8 : 0) |
            (offset + 2 < size ? static_cast<unsigned>(data[offset + 2]) : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(offset + 1 < size ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(offset + 2 < size ? alphabet[value & 63] : '=');
    }
    return result;
}

std::string WebSocketAccept(const std::string& key)
{
    const std::string source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    unsigned char digest[20]{};
    DWORD digestSize = sizeof(digest);
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) ||
        !CryptHashData(hash, reinterpret_cast<const BYTE*>(source.data()),
                       static_cast<DWORD>(source.size()), 0) ||
        !CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0))
    {
        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
        return {};
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return Base64(digest, digestSize);
}

std::string HeaderValue(const std::string& request, const std::string& name)
{
    std::string lowerRequest = request;
    std::string lowerName = name;
    const auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::transform(lowerRequest.begin(), lowerRequest.end(), lowerRequest.begin(), lower);
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), lower);
    const std::size_t position = lowerRequest.find(lowerName + ':');
    if (position == std::string::npos)
        return {};
    std::size_t begin = request.find(':', position) + 1;
    while (begin < request.size() && (request[begin] == ' ' || request[begin] == '\t')) ++begin;
    const std::size_t end = request.find("\r\n", begin);
    return request.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool SendWebSocketText(SOCKET socket, const std::string& payload)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (payload.size() <= 125)
        frame.push_back(static_cast<char>(payload.size()));
    else if (payload.size() <= 0xFFFF)
    {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    else
    {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((static_cast<unsigned long long>(payload.size()) >> shift) & 0xFF));
    }
    frame += payload;
    return SendAll(socket, frame.data(), frame.size());
}

void Broadcast(const std::string& payload)
{
    std::lock_guard<std::mutex> lock(gClientsMutex);
    for (auto it = gClients.begin(); it != gClients.end();)
    {
        if (!SendWebSocketText(*it, payload))
        {
            closesocket(*it);
            it = gClients.erase(it);
        }
        else
            ++it;
    }
}

void SendHttp(SOCKET socket, const char* status, const char* contentType,
              const std::string& body, const std::string& extraHeaders = {})
{
    std::ostringstream header;
    header << "HTTP/1.1 " << status << "\r\nContent-Type: " << contentType
           << "\r\nContent-Length: " << body.size()
           << "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: http://127.0.0.1:8765"
           << "\r\n" << extraHeaders << "Connection: close\r\n\r\n";
    const std::string text = header.str();
    SendAll(socket, text.data(), text.size());
    SendAll(socket, body.data(), body.size());
}

std::string UrlDecode(const std::string& value)
{
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '+' ) result.push_back(' ');
        else if (value[i] == '%' && i + 2 < value.size())
        {
            const auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            const int high = hex(value[i + 1]), low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) { result.push_back(static_cast<char>((high << 4) | low)); i += 2; }
            else result.push_back(value[i]);
        }
        else result.push_back(value[i]);
    }
    return result;
}

std::map<std::string, std::string> QueryValues(const std::string& path)
{
    std::map<std::string, std::string> result;
    const std::size_t question = path.find('?');
    if (question == std::string::npos) return result;
    std::size_t begin = question + 1;
    while (begin < path.size())
    {
        const std::size_t end = path.find('&', begin);
        const std::string pair = path.substr(begin, end == std::string::npos ?
            std::string::npos : end - begin);
        const std::size_t equal = pair.find('=');
        result[UrlDecode(pair.substr(0, equal))] = equal == std::string::npos ?
            std::string() : UrlDecode(pair.substr(equal + 1));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::string MileageLookupJson(const std::string& line, double kilometerKm)
{
    if (!gMileageDatabase || !gOsmProvider) return "{\"valid\":false,\"reason\":\"NOT_READY\"}";
    gMileageDatabase->Load();
    const MileagePositionEstimate local = gMileageDatabase->Lookup(line, kilometerKm);
    std::ostringstream out;
    out << std::fixed << std::setprecision(9);
    if (local.valid)
    {
        out << "{\"valid\":true,\"line\":\"" << line << "\",\"kilometer\":"
            << std::setprecision(1) << kilometerKm << std::setprecision(9)
            << ",\"latitude\":" << local.latitude << ",\"longitude\":" << local.longitude
            << ",\"source\":\"" << PositionSourceName(local.source)
            << "\",\"quality\":\"" << PositionQualityName(local.quality)
            << "\",\"confidence\":" << std::setprecision(3) << local.confidence
            << ",\"lower_km\":" << std::setprecision(1) << local.lowerKilometerKm
            << ",\"upper_km\":" << local.upperKilometerKm << "}";
        return out.str();
    }
    const OsmMileageResult osm = gOsmProvider->Lookup(line, kilometerKm);
    if (osm.found)
    {
        out << "{\"valid\":true,\"line\":\"" << line << "\",\"kilometer\":"
            << std::setprecision(1) << kilometerKm << std::setprecision(9)
            << ",\"latitude\":" << osm.latitude << ",\"longitude\":" << osm.longitude
            << ",\"source\":\"OSM_MILEAGE_EXACT\",\"quality\":\"MEDIUM\","
               "\"confidence\":0.75,\"reason\":\"" << osm.reason << "\"}";
        return out.str();
    }
    out << "{\"valid\":false,\"line\":\"" << line << "\",\"kilometer\":"
        << std::setprecision(1) << kilometerKm << ",\"pending\":"
        << (osm.pending ? "true" : "false") << ",\"ambiguous\":"
        << (osm.ambiguous ? "true" : "false") << ",\"reason\":\""
        << (osm.reason.empty() ? local.reason : osm.reason) << "\"}";
    return out.str();
}

std::vector<std::string> SplitTargetObjects(const std::string& json)
{
    std::vector<std::string> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    std::size_t begin = std::string::npos;
    for (std::size_t i = 0; i < json.size(); ++i)
    {
        const char ch = json[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') { inString = true; continue; }
        if (ch == '{')
        {
            if (depth == 0) begin = i;
            ++depth;
        }
        else if (ch == '}' && depth > 0)
        {
            --depth;
            if (depth == 0 && begin != std::string::npos)
            {
                objects.push_back(json.substr(begin, i - begin + 1));
                begin = std::string::npos;
            }
        }
    }
    return objects;
}

std::string JsonIdentity(const std::string& object)
{
    const std::string uidKey = "\"target_uid\":";
    std::size_t position = object.find(uidKey);
    if (position != std::string::npos)
    {
        position += uidKey.size();
        while (position < object.size() && std::isspace(static_cast<unsigned char>(object[position]))) ++position;
        const std::size_t begin = position;
        while (position < object.size() && std::isdigit(static_cast<unsigned char>(object[position]))) ++position;
        if (position > begin) return object.substr(begin, position - begin);
    }
    const std::string idKey = "\"id\":\"";
    position = object.find(idKey);
    if (position == std::string::npos) return {};
    position += idKey.size();
    const std::size_t end = object.find('"', position);
    return end == std::string::npos ? std::string() : object.substr(position, end - position);
}

std::map<std::string, std::string> IndexTargets(const std::string& json)
{
    std::map<std::string, std::string> result;
    for (const auto& object : SplitTargetObjects(json))
    {
        const std::string uid = JsonIdentity(object);
        if (!uid.empty()) result[uid] = object;
    }
    return result;
}

void HandleClient(SOCKET socket, std::filesystem::path webRoot, std::filesystem::path statePath)
{
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536)
    {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            closesocket(socket);
            return;
        }
        request.append(buffer, buffer + received);
    }
    const std::size_t headerEnd = request.find("\r\n\r\n");
    std::size_t contentLength = 0;
    const std::string contentLengthText = HeaderValue(request, "Content-Length");
    if (!contentLengthText.empty())
    {
        try { contentLength = static_cast<std::size_t>(std::stoull(contentLengthText)); }
        catch (...) { contentLength = 0; }
    }
    const std::size_t bodyBegin = headerEnd == std::string::npos ? request.size() : headerEnd + 4;
    while (request.size() - bodyBegin < contentLength && request.size() < 4 * 1024 * 1024)
    {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        request.append(buffer, buffer + received);
    }
    const std::string body = request.substr(bodyBegin,
        std::min(contentLength, request.size() - bodyBegin));
    std::istringstream firstLine(request);
    std::string method, path, version;
    firstLine >> method >> path >> version;
    if (method != "GET" && method != "POST")
    {
        SendHttp(socket, "405 Method Not Allowed", "text/plain; charset=utf-8", "GET only");
        closesocket(socket);
        return;
    }

    if (path == "/ws" && !HeaderValue(request, "Sec-WebSocket-Key").empty())
    {
        const std::string accept = WebSocketAccept(HeaderValue(request, "Sec-WebSocket-Key"));
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (!SendAll(socket, response.data(), response.size()))
        {
            closesocket(socket);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(gClientsMutex);
            gClients.push_back(socket);
        }
        const std::string snapshot = ReadFile(statePath, "[]");
        SendWebSocketText(socket, "{\"type\":\"snapshot\",\"trains\":" + snapshot + "}");
        while (recv(socket, buffer, sizeof(buffer), 0) > 0) {}
        {
            std::lock_guard<std::mutex> lock(gClientsMutex);
            gClients.erase(std::remove(gClients.begin(), gClients.end(), socket), gClients.end());
        }
        closesocket(socket);
        return;
    }

    const std::size_t question = path.find('?');
    const std::string route = path.substr(0, question);
    const auto query = QueryValues(path);
    if (route == "/api/trains")
        SendHttp(socket, "200 OK", "application/json; charset=utf-8", ReadFile(statePath, "[]"));
    else if (route == "/api/mileage/anchors")
    {
        if (gMileageDatabase) gMileageDatabase->Load();
        SendHttp(socket, "200 OK", "application/json; charset=utf-8",
                 gMileageDatabase ? gMileageDatabase->SerializeJson() : "{\"anchors\":[]}");
    }
    else if (route == "/api/mileage/export")
    {
        if (gMileageDatabase) gMileageDatabase->Load();
        SendHttp(socket, "200 OK", "text/csv; charset=utf-8",
            gMileageDatabase ? gMileageDatabase->SerializeCsv() : std::string(),
            "Content-Disposition: attachment; filename=railway_mileage_positions.csv\r\n");
    }
    else if (route == "/api/mileage/lookup")
    {
        const auto line = query.find("line"), km = query.find("km");
        if (line == query.end() || km == query.end())
            SendHttp(socket, "400 Bad Request", "application/json; charset=utf-8",
                     "{\"valid\":false,\"reason\":\"LINE_OR_MILEAGE_MISSING\"}");
        else
        {
            try { SendHttp(socket, "200 OK", "application/json; charset=utf-8",
                           MileageLookupJson(line->second, std::stod(km->second))); }
            catch (...) { SendHttp(socket, "400 Bad Request", "application/json; charset=utf-8",
                                   "{\"valid\":false,\"reason\":\"INVALID_MILEAGE\"}"); }
        }
    }
    else if (route == "/api/mileage/clear" && method == "POST")
    {
        if (gMileageDatabase) gMileageDatabase->Clear();
        SendHttp(socket, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
    }
    else if (route == "/api/mileage/import" && method == "POST")
    {
        if (body.find("\"anchors\"") == std::string::npos)
            SendHttp(socket, "400 Bad Request", "application/json; charset=utf-8", "{\"ok\":false}");
        else
        {
            const std::filesystem::path temp = gMileagePath.wstring() + L".import.tmp";
            { std::ofstream output(temp, std::ios::binary | std::ios::trunc); output << body; }
            const bool replaced = MoveFileExW(temp.c_str(), gMileagePath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            if (gMileageDatabase) gMileageDatabase->Load();
            SendHttp(socket, replaced ? "200 OK" : "500 Internal Server Error",
                     "application/json; charset=utf-8", replaced ? "{\"ok\":true}" : "{\"ok\":false}");
        }
    }
    else if (route == "/api/mileage/learning" && method == "POST")
    {
        const bool enabled = body.find("false") == std::string::npos;
        if (gMileageDatabase) { gMileageDatabase->SetLearningEnabled(enabled); gMileageDatabase->Save(); }
        { std::ofstream flag(gMileagePath.parent_path() / L"mileage_learning_enabled.flag",
                             std::ios::binary | std::ios::trunc); flag << (enabled ? '1' : '0'); }
        SendHttp(socket, "200 OK", "application/json; charset=utf-8",
                 std::string("{\"ok\":true,\"enabled\":") + (enabled ? "true}" : "false}"));
    }
    else if (route == "/" || route == "/index.html")
        SendHttp(socket, "200 OK", "text/html; charset=utf-8", ReadFile(webRoot / L"index.html", "地图页面文件缺失"));
    else if (route == "/app.js")
        SendHttp(socket, "200 OK", "application/javascript; charset=utf-8", ReadFile(webRoot / L"app.js"));
    else if (route == "/style.css")
        SendHttp(socket, "200 OK", "text/css; charset=utf-8", ReadFile(webRoot / L"style.css"));
    else
        SendHttp(socket, "404 Not Found", "text/plain; charset=utf-8", "Not found");
    closesocket(socket);
}

std::filesystem::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, _countof(path));
    return std::filesystem::path(path).parent_path();
}
}

int wmain(int argc, wchar_t** argv)
{
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\SDRunoPlugin_POCSAG_RailwayMapServer_8765");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (singleton) CloseHandle(singleton);
        return 0;
    }
    std::filesystem::path statePath;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--state")
            statePath = argv[++i];
    if (statePath.empty())
    {
        wchar_t localAppData[MAX_PATH]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, _countof(localAppData));
        statePath = std::filesystem::path(localAppData) / L"SDRunoPlugin_POCSAG" / L"railway_state.json";
    }
    gMileagePath = statePath.parent_path() / L"mileage_positions.json";
    gMileageDatabase = std::make_unique<MileagePositionDatabase>(gMileagePath.wstring());
    gOsmProvider = std::make_unique<OsmRailwayMileageProvider>(
        (statePath.parent_path() / L"osm_mileage_cache.json").wstring());
    const std::filesystem::path webRoot = ExecutableDirectory() / L"web";

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 2;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, SOMAXCONN) == SOCKET_ERROR)
    {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        return 3;
    }

    std::thread watcher([statePath]() {
        std::string previous;
        std::map<std::string, std::string> previousTargets;
        for (;;)
        {
            const std::string current = ReadFile(statePath, "[]");
            if (current != previous)
            {
                const auto currentTargets = IndexTargets(current);
                for (const auto& oldTarget : previousTargets)
                    if (currentTargets.find(oldTarget.first) == currentTargets.end())
                        Broadcast("{\"type\":\"target_remove\",\"target_uid\":\"" +
                                  oldTarget.first + "\"}");
                for (const auto& target : currentTargets)
                {
                    const auto old = previousTargets.find(target.first);
                    if (old == previousTargets.end() || old->second != target.second)
                        Broadcast("{\"type\":\"target_update\",\"target_uid\":\"" +
                                  target.first + "\",\"target\":" + target.second + "}");
                }
                previousTargets = currentTargets;
                previous = current;
                Broadcast("{\"type\":\"snapshot\",\"trains\":" + current + "}");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    watcher.detach();

    for (;;)
    {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client != INVALID_SOCKET)
            std::thread(HandleClient, client, webRoot, statePath).detach();
    }
}
