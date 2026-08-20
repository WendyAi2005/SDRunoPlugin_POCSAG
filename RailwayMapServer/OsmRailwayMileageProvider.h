#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>

struct OsmMileageResult
{
    bool found = false;
    bool pending = false;
    bool ambiguous = false;
    double latitude = 0.0;
    double longitude = 0.0;
    std::string source;
    std::string reason;
};

class OsmRailwayMileageProvider
{
public:
    explicit OsmRailwayMileageProvider(std::wstring cachePath);
    OsmMileageResult Lookup(const std::string& lineName, double kilometerKm);
    void Seed(const std::string& lineName, double kilometerKm,
              double latitude, double longitude);

private:
    struct CachedPoint
    {
        std::string lineName;
        double kilometerKm = 0.0;
        double latitude = 0.0;
        double longitude = 0.0;
    };
    static std::string Key(const std::string& lineName, double kilometerKm);
    static std::wstring Utf8ToWide(const std::string& value);
    static std::wstring UrlEncode(const std::string& value);
    static std::string Download(const std::string& lineName, double kilometerKm);
    static bool ParseSinglePoint(const std::string& json, double& latitude, double& longitude,
                                 bool& ambiguous);
    void Load();
    void SaveLocked() const;
    void FetchAsync(std::string lineName, double kilometerKm);

    std::wstring cachePath_;
    std::mutex mutex_;
    std::map<std::string, CachedPoint> cache_;
    std::set<std::string> pending_;
    std::map<std::string, std::string> failures_;
};
