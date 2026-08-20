#define WIN32_LEAN_AND_MEAN
#include "OsmRailwayMileageProvider.h"

#include <windows.h>
#include <winhttp.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
std::string EscapeJson(const std::string& value)
{
    std::string out;
    for (char ch : value)
    {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}
}

OsmRailwayMileageProvider::OsmRailwayMileageProvider(std::wstring cachePath)
    : cachePath_(std::move(cachePath))
{
    Load();
}

std::string OsmRailwayMileageProvider::Key(const std::string& lineName, double kilometerKm)
{
    std::ostringstream out;
    out << lineName << '|' << std::fixed << std::setprecision(1) << kilometerKm;
    return out.str();
}

std::wstring OsmRailwayMileageProvider::Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::wstring OsmRailwayMileageProvider::UrlEncode(const std::string& value)
{
    static constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result;
    for (unsigned char ch : value)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            result.push_back(static_cast<wchar_t>(ch));
        else
        {
            result.push_back(L'%'); result.push_back(hex[ch >> 4]); result.push_back(hex[ch & 15]);
        }
    }
    return result;
}

std::string OsmRailwayMileageProvider::Download(const std::string& lineName,
                                                double kilometerKm)
{
    std::ostringstream km;
    km << std::fixed << std::setprecision(1) << kilometerKm;
    const std::string escapedLine = std::regex_replace(lineName, std::regex(R"([\"\\])"), R"(\$&)");
    const std::string escapedKm = std::regex_replace(km.str(), std::regex(R"(\.)"), R"(\.)");
    const std::string query = "[out:json][timeout:5];(node[\"railway\"=\"milestone\"]"
        "[\"railway:position\"~\"^" + escapedKm + "$\"]"
        "[~\"^(name|ref|official_name|line|route)$\"~\"" + escapedLine + "\"];"
        "node[\"railway\"=\"milestone\"][\"railway:position:exact\"~\"^" + escapedKm + "$\"]"
        "[~\"^(name|ref|official_name|line|route)$\"~\"" + escapedLine + "\"];);out body;";
    const std::wstring path = L"/api/interpreter?data=" + UrlEncode(query);
    HINTERNET session = WinHttpOpen(L"SDRuno-POCSAG-Mileage/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, L"overpass-api.de",
                                           INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::string result;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
    {
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            result.append(chunk.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

bool OsmRailwayMileageProvider::ParseSinglePoint(const std::string& json,
                                                 double& latitude, double& longitude,
                                                 bool& ambiguous)
{
    const std::regex point(R"(\"lat\"\s*:\s*([-0-9.]+)\s*,\s*\"lon\"\s*:\s*([-0-9.]+))");
    std::sregex_iterator it(json.begin(), json.end(), point), end;
    if (it == end) return false;
    latitude = std::stod((*it)[1].str());
    longitude = std::stod((*it)[2].str());
    ++it;
    ambiguous = it != end;
    return !ambiguous;
}

void OsmRailwayMileageProvider::FetchAsync(std::string lineName, double kilometerKm)
{
    const std::string key = Key(lineName, kilometerKm);
    std::thread([this, key, lineName = std::move(lineName), kilometerKm]() {
        const std::string json = Download(lineName, kilometerKm);
        double latitude = 0.0, longitude = 0.0;
        bool ambiguous = false;
        const bool found = ParseSinglePoint(json, latitude, longitude, ambiguous);
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(key);
        if (found)
        {
            cache_[key] = { lineName, kilometerKm, latitude, longitude };
            failures_.erase(key);
            SaveLocked();
        }
        else failures_[key] = ambiguous ? "AMBIGUOUS_OSM_MILEAGE" :
            json.empty() ? "OSM_NETWORK_UNAVAILABLE" : "NO_MATCHING_OSM_MILESTONE";
    }).detach();
}

OsmMileageResult OsmRailwayMileageProvider::Lookup(const std::string& lineName,
                                                   double kilometerKm)
{
    OsmMileageResult result;
    if (lineName.empty() || !std::isfinite(kilometerKm))
    {
        result.reason = "LINE_OR_MILEAGE_MISSING";
        return result;
    }
    const std::string key = Key(lineName, kilometerKm);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = cache_.find(key);
        if (cached != cache_.end())
        {
            result.found = true;
            result.latitude = cached->second.latitude;
            result.longitude = cached->second.longitude;
            result.source = "OSM_MILEAGE_EXACT";
            result.reason = "OSM_CACHE";
            return result;
        }
        if (pending_.find(key) != pending_.end())
        {
            result.pending = true;
            result.reason = "OSM_QUERY_PENDING";
            return result;
        }
        const auto failure = failures_.find(key);
        if (failure != failures_.end())
        {
            result.ambiguous = failure->second == "AMBIGUOUS_OSM_MILEAGE";
            result.reason = failure->second;
            return result;
        }
        pending_.insert(key);
    }
    FetchAsync(lineName, kilometerKm);
    result.pending = true;
    result.reason = "OSM_QUERY_STARTED";
    return result;
}

void OsmRailwayMileageProvider::Seed(const std::string& lineName, double kilometerKm,
                                     double latitude, double longitude)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[Key(lineName, kilometerKm)] = { lineName, kilometerKm, latitude, longitude };
    SaveLocked();
}

void OsmRailwayMileageProvider::SaveLocked() const
{
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(cachePath_).parent_path(), error);
    std::ofstream out(cachePath_, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "{\"version\":1,\"anchors\":[";
    bool first = true;
    for (const auto& entry : cache_)
    {
        const CachedPoint& p = entry.second;
        if (!first) out << ',';
        first = false;
        out << "{\"line\":\"" << EscapeJson(p.lineName) << "\",\"mileage_km\":"
            << std::fixed << std::setprecision(1) << p.kilometerKm
            << ",\"latitude\":" << std::setprecision(9) << p.latitude
            << ",\"longitude\":" << p.longitude << "}";
    }
    out << "]}";
}

void OsmRailwayMileageProvider::Load()
{
    std::ifstream input(cachePath_, std::ios::binary);
    if (!input) return;
    const std::string json{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    const std::regex point(R"(\{\"line\":\"([^\"]+)\",\"mileage_km\":([-0-9.]+),\"latitude\":([-0-9.]+),\"longitude\":([-0-9.]+)\})");
    for (std::sregex_iterator it(json.begin(), json.end(), point), end; it != end; ++it)
    {
        CachedPoint p;
        p.lineName = (*it)[1].str();
        p.kilometerKm = std::stod((*it)[2].str());
        p.latitude = std::stod((*it)[3].str());
        p.longitude = std::stod((*it)[4].str());
        cache_[Key(p.lineName, p.kilometerKm)] = p;
    }
}
