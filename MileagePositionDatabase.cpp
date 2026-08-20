#include "MileagePositionDatabase.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace
{
constexpr double kPi = 3.14159265358979323846;

std::string UnescapeJson(std::string value)
{
    std::string result;
    bool escaped = false;
    for (char ch : value)
    {
        if (escaped)
        {
            if (ch == 'n') result.push_back('\n');
            else if (ch == 'r') result.push_back('\r');
            else if (ch == 't') result.push_back('\t');
            else result.push_back(ch);
            escaped = false;
        }
        else if (ch == '\\') escaped = true;
        else result.push_back(ch);
    }
    return result;
}
}

const char* PositionSourceName(PositionSource value)
{
    switch (value)
    {
    case PositionSource::RadioGps: return "RADIO_GPS";
    case PositionSource::LocalMileageExact: return "LOCAL_MILEAGE_EXACT";
    case PositionSource::LocalMileageInterpolated: return "LOCAL_MILEAGE_INTERPOLATED";
    case PositionSource::OsmMileageExact: return "OSM_MILEAGE_EXACT";
    case PositionSource::OsmMileageInterpolated: return "OSM_MILEAGE_INTERPOLATED";
    default: return "NO_POSITION";
    }
}

const char* PositionQualityName(PositionQuality value)
{
    switch (value)
    {
    case PositionQuality::High: return "HIGH";
    case PositionQuality::Medium: return "MEDIUM";
    case PositionQuality::Low: return "LOW";
    default: return "INVALID";
    }
}

const char* PositionComparisonName(PositionComparison value)
{
    switch (value)
    {
    case PositionComparison::Match: return "MATCH";
    case PositionComparison::Warning: return "WARNING";
    case PositionComparison::Mismatch: return "MISMATCH";
    default: return "UNAVAILABLE";
    }
}

MileagePositionDatabase::MileagePositionDatabase(std::wstring path) : path_(std::move(path))
{
    if (!path_.empty()) Load();
}

void MileagePositionDatabase::SetPath(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
}

MileagePositionDatabase::MileageKey MileagePositionDatabase::KeyForKilometer(double value)
{
    return static_cast<MileageKey>(std::llround(value * 10.0));
}

double MileagePositionDatabase::KilometerForKey(MileageKey key)
{
    return static_cast<double>(key) / 10.0;
}

bool MileagePositionDatabase::ValidCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude) &&
        latitude >= -90.0 && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0;
}

double MileagePositionDatabase::DistanceMeters(double latA, double lonA,
                                               double latB, double lonB)
{
    constexpr double earthMeters = 6371000.0;
    const double rad = kPi / 180.0;
    const double dLat = (latB - latA) * rad;
    const double dLon = (lonB - lonA) * rad;
    const double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
        std::cos(latA * rad) * std::cos(latB * rad) *
        std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
    return earthMeters * 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

PositionQuality MileagePositionDatabase::QualityFor(const MileageAnchor& anchor)
{
    if (anchor.sampleCount >= 3 && anchor.positionStdMeters < 30.0)
        return PositionQuality::High;
    return anchor.sampleCount > 0 ? PositionQuality::Medium : PositionQuality::Invalid;
}

bool MileagePositionDatabase::AddSample(const std::string& lineName, double kilometerKm,
                                        double latitude, double longitude,
                                        std::uint64_t timestampUnixMs, PositionSource source)
{
    if (!path_.empty())
    {
        std::ifstream flag(std::filesystem::path(path_).parent_path() /
                           L"mileage_learning_enabled.flag", std::ios::binary);
        char value = '1';
        if (flag) flag.get(value);
        if (value == '0') return false;
    }
    if (!learningEnabled_ || lineName.empty() || !std::isfinite(kilometerKm) ||
        !ValidCoordinate(latitude, longitude)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const MileageKey key = KeyForKilometer(kilometerKm);
    MileageAnchor& anchor = anchors_[lineName][key];
    if (anchor.sampleCount == 0)
    {
        anchor.lineName = lineName;
        anchor.kilometerKm = KilometerForKey(key);
        anchor.latitude = latitude;
        anchor.longitude = longitude;
        anchor.sampleCount = 1;
        anchor.firstSeenUnixMs = timestampUnixMs;
        anchor.lastSeenUnixMs = timestampUnixMs;
        anchor.source = source == PositionSource::RadioGps
            ? PositionSource::LocalMileageExact : source;
        anchor.quality = PositionQuality::Medium;
        return SaveLocked();
    }
    const double distance = DistanceMeters(anchor.latitude, anchor.longitude, latitude, longitude);
    if (distance > config_.anchorOutlierMeters)
    {
        ++anchor.rejectedOutlierCount;
        ++rejectedOutliers_;
        anchor.lastSeenUnixMs = std::max(anchor.lastSeenUnixMs, timestampUnixMs);
        SaveLocked();
        return false;
    }
    const double previousCount = static_cast<double>(anchor.sampleCount);
    const double newCount = previousCount + 1.0;
    anchor.latitude += (latitude - anchor.latitude) / newCount;
    anchor.longitude += (longitude - anchor.longitude) / newCount;
    anchor.varianceAccumulator += distance * distance * previousCount / newCount;
    ++anchor.sampleCount;
    anchor.positionStdMeters = std::sqrt(anchor.varianceAccumulator /
        static_cast<double>(std::max<std::uint64_t>(1, anchor.sampleCount - 1)));
    anchor.lastSeenUnixMs = std::max(anchor.lastSeenUnixMs, timestampUnixMs);
    anchor.quality = QualityFor(anchor);
    return SaveLocked();
}

MileagePositionEstimate MileagePositionDatabase::Lookup(const std::string& lineName,
                                                        double kilometerKm) const
{
    MileagePositionEstimate result;
    if (lineName.empty() || !std::isfinite(kilometerKm))
    {
        result.reason = "LINE_OR_MILEAGE_MISSING";
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto line = anchors_.find(lineName);
    if (line == anchors_.end() || line->second.empty())
    {
        result.reason = "NO_LINE_ANCHORS";
        return result;
    }
    const MileageKey key = KeyForKilometer(kilometerKm);
    const auto exact = line->second.find(key);
    if (exact != line->second.end())
    {
        const MileageAnchor& anchor = exact->second;
        result.valid = true;
        result.latitude = anchor.latitude;
        result.longitude = anchor.longitude;
        result.source = PositionSource::LocalMileageExact;
        result.quality = QualityFor(anchor);
        result.confidence = result.quality == PositionQuality::High ? 0.98 :
            std::min(0.9, 0.65 + 0.05 * static_cast<double>(anchor.sampleCount));
        result.lowerKilometerKm = result.upperKilometerKm = anchor.kilometerKm;
        result.reason = "EXACT_ANCHOR";
        return result;
    }
    const auto upper = line->second.lower_bound(key);
    if (upper == line->second.begin() || upper == line->second.end())
    {
        result.reason = "ONE_SIDED_ANCHOR_ONLY";
        return result;
    }
    const auto lower = std::prev(upper);
    const double lowerKm = lower->second.kilometerKm;
    const double upperKm = upper->second.kilometerKm;
    const double gap = upperKm - lowerKm;
    if (gap <= 0.0 || gap > config_.maxInterpolationGapKm)
    {
        result.reason = "INTERPOLATION_GAP_TOO_LARGE";
        return result;
    }
    const double ratio = (kilometerKm - lowerKm) / gap;
    result.valid = true;
    result.latitude = lower->second.latitude + ratio *
        (upper->second.latitude - lower->second.latitude);
    result.longitude = lower->second.longitude + ratio *
        (upper->second.longitude - lower->second.longitude);
    result.source = PositionSource::LocalMileageInterpolated;
    result.quality = PositionQuality::Medium;
    result.confidence = std::max(0.55, 0.9 - gap * 0.15);
    result.lowerKilometerKm = lowerKm;
    result.upperKilometerKm = upperKm;
    result.reason = "BRACKETED_LINEAR_INTERPOLATION";
    return result;
}

PositionComparison MileagePositionDatabase::Compare(double radioLatitude, double radioLongitude,
                                                     const MileagePositionEstimate& estimate,
                                                     double* distanceMeters) const
{
    if (!estimate.valid || !ValidCoordinate(radioLatitude, radioLongitude))
        return PositionComparison::Unavailable;
    const double distance = DistanceMeters(radioLatitude, radioLongitude,
                                           estimate.latitude, estimate.longitude);
    if (distanceMeters) *distanceMeters = distance;
    if (distance < config_.matchThresholdMeters) return PositionComparison::Match;
    if (distance <= config_.mismatchThresholdMeters) return PositionComparison::Warning;
    return PositionComparison::Mismatch;
}

std::vector<MileageAnchor> MileagePositionDatabase::Anchors() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MileageAnchor> result;
    for (const auto& line : anchors_)
        for (const auto& entry : line.second) result.push_back(entry.second);
    return result;
}

std::string MileagePositionDatabase::JsonEscape(const std::string& value)
{
    std::string result;
    for (char ch : value)
    {
        if (ch == '\\' || ch == '"') result.push_back('\\');
        if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '\t') result += "\\t";
        else result.push_back(ch);
    }
    return result;
}

std::string MileagePositionDatabase::SerializeJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "{\"version\":1,\"learning_enabled\":" << (learningEnabled_ ? "true" : "false")
        << ",\"rejected_outliers\":" << rejectedOutliers_ << ",\"anchors\":[";
    bool first = true;
    for (const auto& line : anchors_)
        for (const auto& entry : line.second)
        {
            const MileageAnchor& a = entry.second;
            if (!first) out << ',';
            first = false;
            out << "{\"line\":\"" << JsonEscape(a.lineName) << "\",\"mileage_km\":"
                << std::fixed << std::setprecision(1) << a.kilometerKm
                << ",\"latitude\":" << std::setprecision(9) << a.latitude
                << ",\"longitude\":" << a.longitude
                << ",\"samples\":" << a.sampleCount
                << ",\"std_m\":" << std::setprecision(3) << a.positionStdMeters
                << ",\"variance_accumulator\":" << a.varianceAccumulator
                << ",\"outliers\":" << a.rejectedOutlierCount
                << ",\"first_seen\":" << a.firstSeenUnixMs
                << ",\"last_seen\":" << a.lastSeenUnixMs
                << ",\"source\":\"" << PositionSourceName(a.source)
                << "\",\"quality\":\"" << PositionQualityName(a.quality) << "\"}";
        }
    out << "]}";
    return out.str();
}

std::string MileagePositionDatabase::SerializeCsv() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "line,mileage_km,lat,lon,samples,std_m,outliers,first_seen,last_seen,source,quality\r\n";
    for (const auto& line : anchors_)
        for (const auto& entry : line.second)
        {
            const MileageAnchor& a = entry.second;
            out << '"' << a.lineName << "\"," << std::fixed << std::setprecision(1)
                << a.kilometerKm << ',' << std::setprecision(9) << a.latitude << ','
                << a.longitude << ',' << a.sampleCount << ',' << std::setprecision(3)
                << a.positionStdMeters << ',' << a.rejectedOutlierCount << ','
                << a.firstSeenUnixMs << ',' << a.lastSeenUnixMs << ','
                << PositionSourceName(a.source) << ',' << PositionQualityName(a.quality) << "\r\n";
        }
    return out.str();
}

bool MileagePositionDatabase::SaveLocked() const
{
    if (path_.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), error);
    std::ostringstream out;
    out << "{\"version\":1,\"learning_enabled\":" << (learningEnabled_ ? "true" : "false")
        << ",\"rejected_outliers\":" << rejectedOutliers_ << ",\"anchors\":[";
    bool first = true;
    for (const auto& line : anchors_)
        for (const auto& entry : line.second)
        {
            const MileageAnchor& a = entry.second;
            if (!first) out << ',';
            first = false;
            out << "{\"line\":\"" << JsonEscape(a.lineName) << "\",\"mileage_km\":"
                << std::fixed << std::setprecision(1) << a.kilometerKm
                << ",\"latitude\":" << std::setprecision(9) << a.latitude
                << ",\"longitude\":" << a.longitude << ",\"samples\":" << a.sampleCount
                << ",\"std_m\":" << std::setprecision(3) << a.positionStdMeters
                << ",\"variance_accumulator\":" << a.varianceAccumulator
                << ",\"outliers\":" << a.rejectedOutlierCount
                << ",\"first_seen\":" << a.firstSeenUnixMs << ",\"last_seen\":" << a.lastSeenUnixMs
                << ",\"source\":\"" << PositionSourceName(a.source)
                << "\",\"quality\":\"" << PositionQualityName(a.quality) << "\"}";
        }
    out << "]}";
    const std::filesystem::path destination(path_);
    const std::filesystem::path temporary = destination.wstring() + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << out.str();
    file.close();
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    return !error;
}

bool MileagePositionDatabase::Save() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return SaveLocked();
}

bool MileagePositionDatabase::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    anchors_.clear(); rejectedOutliers_ = 0;
    std::ifstream input(path_, std::ios::binary);
    if (!input) return false;
    const std::string json{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    const auto stringField = [](const std::string& object, const char* name) {
        const std::regex pattern(std::string("\\\"") + name + "\\\":\\\"((?:\\\\.|[^\\\"])*)\\\"");
        std::smatch match;
        return std::regex_search(object, match, pattern) ? UnescapeJson(match[1].str()) : std::string();
    };
    const auto numberField = [](const std::string& object, const char* name, double fallback = 0.0) {
        const std::regex pattern(std::string("\\\"") + name + "\\\":([-+0-9.eE]+)");
        std::smatch match;
        return std::regex_search(object, match, pattern) ? std::stod(match[1].str()) : fallback;
    };
    const std::size_t anchorsBegin = json.find("\"anchors\"");
    std::size_t cursor = anchorsBegin == std::string::npos ? json.size() :
        json.find('[', anchorsBegin);
    while (cursor < json.size())
    {
        const std::size_t begin = json.find('{', cursor);
        if (begin == std::string::npos) break;
        bool inString = false, escaped = false;
        int depth = 0;
        std::size_t end = begin;
        for (; end < json.size(); ++end)
        {
            const char ch = json[end];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') inString = false;
                continue;
            }
            if (ch == '"') inString = true;
            else if (ch == '{') ++depth;
            else if (ch == '}' && --depth == 0) break;
        }
        if (end >= json.size()) break;
        const std::string object = json.substr(begin, end - begin + 1);
        MileageAnchor a;
        a.lineName = stringField(object, "line");
        a.kilometerKm = numberField(object, "mileage_km");
        a.latitude = numberField(object, "latitude");
        a.longitude = numberField(object, "longitude");
        a.sampleCount = static_cast<std::uint64_t>(numberField(object, "samples"));
        a.positionStdMeters = numberField(object, "std_m");
        a.varianceAccumulator = numberField(object, "variance_accumulator");
        a.rejectedOutlierCount = static_cast<std::uint64_t>(numberField(object, "outliers"));
        a.firstSeenUnixMs = static_cast<std::uint64_t>(numberField(object, "first_seen"));
        a.lastSeenUnixMs = static_cast<std::uint64_t>(numberField(object, "last_seen"));
        a.source = PositionSource::LocalMileageExact;
        a.quality = stringField(object, "quality") == "HIGH" ? PositionQuality::High : PositionQuality::Medium;
        if (!a.lineName.empty() && a.sampleCount > 0 && ValidCoordinate(a.latitude, a.longitude))
        {
            anchors_[a.lineName][KeyForKilometer(a.kilometerKm)] = a;
            rejectedOutliers_ += a.rejectedOutlierCount;
        }
        cursor = end + 1;
    }
    learningEnabled_ = json.find("\"learning_enabled\":false") == std::string::npos;
    return true;
}

void MileagePositionDatabase::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    anchors_.clear(); rejectedOutliers_ = 0;
    SaveLocked();
}
