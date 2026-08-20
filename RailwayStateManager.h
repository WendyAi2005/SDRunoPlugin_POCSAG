#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "RailwayMessageAssembler.h"
#include "MileagePositionDatabase.h"

struct RailwayTrackPoint
{
    std::string latitudeRaw;
    std::string longitudeRaw;
    std::string latitudeDegreeMinute;
    std::string longitudeDegreeMinute;
    double latitude = 0.0;
    double longitude = 0.0;
    std::uint64_t timestampUnixMs = 0;
    PositionSource source = PositionSource::RadioGps;
    PositionQuality quality = PositionQuality::High;
};

struct RailwayTarget
{
    std::uint64_t targetUid = 0;
    std::string targetId; // Human-readable label, never the container key.
    std::string createdBy;
    std::uint64_t mergeCount = 0;
    std::string lastMergeReason;
    std::uint64_t transmissionId = 0;
    std::vector<std::uint64_t> transmissionAliases;
    std::vector<std::string> trainNumberAliases;
    std::vector<std::string> locomotiveIdAliases;
    std::vector<std::string> locomotiveEndHistory;
    std::uint64_t lastBasicUpdateUnixMs = 0;
    std::uint64_t lastExtensionUpdateUnixMs = 0;
    std::uint64_t lastAnyUpdateUnixMs = 0;
    std::string pairingMethod;
    bool hasBasic = false;
    bool hasExtension = false;
    std::string trainPrefix;
    std::string trainNumber;
    std::string fullTrainNumber;
    bool hasSpeed = false;
    int speedKmh = 0;
    bool hasKilometer = false;
    double kilometerKm = 0.0;
    std::string locomotiveId;
    std::string locomotiveEnd;
    std::string lineName;
    std::string longitudeRaw;
    std::string longitudeDegreeMinute;
    std::string latitudeRaw;
    std::string latitudeDegreeMinute;
    bool hasPosition = false; // Original radio GPS only; retained for compatibility.
    bool radioGpsFresh = false;
    double longitudeDeg = 0.0;
    double latitudeDeg = 0.0;
    bool hasDisplayedPosition = false;
    double displayedLongitudeDeg = 0.0;
    double displayedLatitudeDeg = 0.0;
    PositionSource positionSource = PositionSource::NoPosition;
    PositionQuality positionQuality = PositionQuality::Invalid;
    double positionConfidence = 0.0;
    bool hasMileageEstimate = false;
    double mileageLongitudeDeg = 0.0;
    double mileageLatitudeDeg = 0.0;
    PositionSource mileagePositionSource = PositionSource::NoPosition;
    PositionQuality mileagePositionQuality = PositionQuality::Invalid;
    double mileagePositionConfidence = 0.0;
    PositionComparison gpsMileageComparison = PositionComparison::Unavailable;
    double gpsVsMileageDistanceMeters = 0.0;
    std::string railwayAuxRaw;
    std::string dataCompleteness;
    std::string quality;
    bool stale = false;
    std::vector<RailwayTrackPoint> track;
    PocsagMessage basic;
    PocsagMessage extension;
};

class RailwayStateManager
{
public:
    static constexpr std::uint64_t kFallbackWindowMs = 5000;
    static constexpr int kCandidateMergeThreshold = 60;
    static constexpr std::uint64_t kStaleAfterMs = 120000;
    static constexpr std::uint64_t kRemoveAfterMs = 300000;
    static constexpr std::size_t kMaxTrackPoints = 100;

    void Apply(const RailwayApproachMessage& update);
    std::vector<RailwayTarget> Snapshot(std::uint64_t nowUnixMs);
    void Clear();
    std::uint64_t TotalMergeCount() const { return totalMergeCount_; }
    void ConfigureMileageDatabase(const std::wstring& path);
    MileagePositionDatabase& MileageDatabase() { return mileageDatabase_; }
    const MileagePositionDatabase& MileageDatabase() const { return mileageDatabase_; }

private:
    using TargetUid = std::uint64_t;
    using TargetMap = std::map<TargetUid, RailwayTarget>;
    TargetUid CreateTarget(const RailwayApproachMessage& update);
    std::vector<TargetUid> DirectCandidates(const RailwayApproachMessage& update) const;
    TargetUid FindBestScoredCandidate(const RailwayApproachMessage& update) const;
    int CandidateScore(const RailwayApproachMessage& update, const RailwayTarget& target) const;
    void MergeTargets(TargetUid primaryUid, TargetUid secondaryUid, const std::string& reason);
    void MergeIdentityConflicts(TargetUid primaryUid);
    void MergeInto(RailwayTarget& target, const RailwayApproachMessage& update);
    void RebuildIndexes();
    static void RefreshDerivedFields(RailwayTarget& target);
    void RefreshPosition(RailwayTarget& target, bool allowLearning);
    bool PassesLearningQuality(const RailwayTarget& target) const;
    bool PassesMotionContinuity(const RailwayTarget& target) const;
    static void NormalizeTrack(std::vector<RailwayTrackPoint>& track);

    TargetMap targets_;
    std::map<std::string, TargetUid> trainNumberIndex_;
    std::map<std::string, TargetUid> locomotiveEndIndex_;
    std::map<std::string, TargetUid> locomotiveIndex_;
    std::map<std::uint64_t, TargetUid> transmissionIndex_;
    TargetUid nextTargetUid_ = 0;
    std::uint64_t totalMergeCount_ = 0;
    MileagePositionDatabase mileageDatabase_;
    std::map<TargetUid, std::pair<double, std::uint64_t>> lastLearnedMileage_;
};
