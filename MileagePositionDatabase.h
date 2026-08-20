#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

enum class PositionSource
{
    NoPosition,
    RadioGps,
    LocalMileageExact,
    LocalMileageInterpolated,
    OsmMileageExact,
    OsmMileageInterpolated
};

enum class PositionQuality
{
    Invalid,
    Low,
    Medium,
    High
};

enum class PositionComparison
{
    Unavailable,
    Match,
    Warning,
    Mismatch
};

const char* PositionSourceName(PositionSource value);
const char* PositionQualityName(PositionQuality value);
const char* PositionComparisonName(PositionComparison value);

struct MileageAnchor
{
    std::string lineName;
    double kilometerKm = 0.0;
    double latitude = 0.0;
    double longitude = 0.0;
    std::uint64_t sampleCount = 0;
    std::uint64_t rejectedOutlierCount = 0;
    double positionStdMeters = 0.0;
    double varianceAccumulator = 0.0;
    std::uint64_t firstSeenUnixMs = 0;
    std::uint64_t lastSeenUnixMs = 0;
    PositionSource source = PositionSource::LocalMileageExact;
    PositionQuality quality = PositionQuality::Medium;
};

struct MileagePositionEstimate
{
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    PositionSource source = PositionSource::NoPosition;
    PositionQuality quality = PositionQuality::Invalid;
    double confidence = 0.0;
    double lowerKilometerKm = 0.0;
    double upperKilometerKm = 0.0;
    std::string reason;
};

struct MileagePositionConfig
{
    double anchorOutlierMeters = 500.0;
    double maxInterpolationGapKm = 2.0;
    double matchThresholdMeters = 100.0;
    double mismatchThresholdMeters = 500.0;
};

class MileagePositionDatabase
{
public:
    explicit MileagePositionDatabase(std::wstring path = {});

    void SetPath(const std::wstring& path);
    const std::wstring& Path() const { return path_; }
    void SetConfig(const MileagePositionConfig& config) { config_ = config; }
    const MileagePositionConfig& Config() const { return config_; }
    void SetLearningEnabled(bool enabled) { learningEnabled_ = enabled; }
    bool LearningEnabled() const { return learningEnabled_; }

    bool Load();
    bool Save() const;
    bool AddSample(const std::string& lineName, double kilometerKm,
                   double latitude, double longitude, std::uint64_t timestampUnixMs,
                   PositionSource source = PositionSource::RadioGps);
    MileagePositionEstimate Lookup(const std::string& lineName, double kilometerKm) const;
    PositionComparison Compare(double radioLatitude, double radioLongitude,
                               const MileagePositionEstimate& estimate,
                               double* distanceMeters = nullptr) const;
    std::vector<MileageAnchor> Anchors() const;
    void Clear();
    std::uint64_t RejectedOutliers() const { return rejectedOutliers_; }
    std::string SerializeJson() const;
    std::string SerializeCsv() const;

    static double DistanceMeters(double latitudeA, double longitudeA,
                                 double latitudeB, double longitudeB);

private:
    using MileageKey = long long;
    static MileageKey KeyForKilometer(double kilometerKm);
    static double KilometerForKey(MileageKey key);
    static bool ValidCoordinate(double latitude, double longitude);
    static std::string JsonEscape(const std::string& value);
    static PositionQuality QualityFor(const MileageAnchor& anchor);
    bool SaveLocked() const;

    mutable std::mutex mutex_;
    std::map<std::string, std::map<MileageKey, MileageAnchor>> anchors_;
    std::wstring path_;
    MileagePositionConfig config_;
    bool learningEnabled_ = true;
    std::uint64_t rejectedOutliers_ = 0;
};
