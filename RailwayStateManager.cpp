#include "RailwayStateManager.h"

#include <algorithm>
#include <cmath>

namespace
{
template <typename T>
void AddUnique(std::vector<T>& values, const T& value)
{
    if (value != T{} && std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
}

std::uint64_t AbsoluteDifference(std::uint64_t a, std::uint64_t b)
{
    return a >= b ? a - b : b - a;
}

std::string LocomotiveEndKey(const std::string& id, const std::string& end)
{
    return id.empty() || end.empty() ? std::string() : id + ":" + end;
}

double DistanceKm(double lon1, double lat1, double lon2, double lat2)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kEarthKm = 6371.0;
    const double toRadians = kPi / 180.0;
    const double dLat = (lat2 - lat1) * toRadians;
    const double dLon = (lon2 - lon1) * toRadians;
    const double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
        std::cos(lat1 * toRadians) * std::cos(lat2 * toRadians) *
        std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
    return kEarthKm * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

bool SameTrackPoint(const RailwayTrackPoint& a, const RailwayTrackPoint& b)
{
    return a.timestampUnixMs == b.timestampUnixMs &&
        std::abs(a.latitude - b.latitude) < 1e-10 &&
        std::abs(a.longitude - b.longitude) < 1e-10;
}

}

void RailwayStateManager::ConfigureMileageDatabase(const std::wstring& path)
{
    mileageDatabase_.SetPath(path);
    mileageDatabase_.Load();
}

void RailwayStateManager::Apply(const RailwayApproachMessage& update)
{
    if (!update.hasBasic && !update.hasExtension)
        return;
    for (auto it = targets_.begin(); it != targets_.end();)
    {
        if (update.timestampUnixMs > it->second.lastAnyUpdateUnixMs &&
            update.timestampUnixMs - it->second.lastAnyUpdateUnixMs > kRemoveAfterMs)
            it = targets_.erase(it);
        else
            ++it;
    }
    RebuildIndexes();

    std::vector<TargetUid> direct = DirectCandidates(update);
    TargetUid primaryUid = 0;
    if (!direct.empty())
    {
        // Preserve the oldest object so an alias change never changes marker identity.
        primaryUid = *std::min_element(direct.begin(), direct.end());
        for (TargetUid uid : direct)
            if (uid != primaryUid)
                MergeTargets(primaryUid, uid, "IDENTITY_INDEX");
    }
    else
        primaryUid = FindBestScoredCandidate(update);
    if (primaryUid == 0)
        primaryUid = CreateTarget(update);

    auto target = targets_.find(primaryUid);
    if (target == targets_.end())
        return;
    RailwayApproachMessage effective = update;
    if (target->second.lastAnyUpdateUnixMs != 0 && update.pairingMethod == "INDEPENDENT")
        effective.pairingMethod = direct.empty() ? "SCORED_FALLBACK" : "TARGET_STATE";
    if (direct.empty() && target->second.lastAnyUpdateUnixMs != 0)
        target->second.lastMergeReason = "SCORED_FALLBACK";
    MergeInto(target->second, effective);
    RefreshDerivedFields(target->second);
    RefreshPosition(target->second, true);
    RebuildIndexes();
    MergeIdentityConflicts(primaryUid);
    RebuildIndexes();
}

RailwayStateManager::TargetUid RailwayStateManager::CreateTarget(
    const RailwayApproachMessage& update)
{
    const TargetUid uid = ++nextTargetUid_;
    RailwayTarget target;
    target.targetUid = uid;
    target.createdBy = update.hasBasic && update.hasExtension ? "FULL" :
        update.hasBasic ? "BASIC_ONLY" : "EXT_ONLY";
    targets_.emplace(uid, std::move(target));
    return uid;
}

std::vector<RailwayStateManager::TargetUid> RailwayStateManager::DirectCandidates(
    const RailwayApproachMessage& update) const
{
    std::vector<TargetUid> result;
    const auto add = [&result](TargetUid uid) {
        if (uid != 0 && std::find(result.begin(), result.end(), uid) == result.end())
            result.push_back(uid);
    };
    if (update.transmissionId != 0)
    {
        const auto found = transmissionIndex_.find(update.transmissionId);
        if (found != transmissionIndex_.end()) add(found->second);
    }
    if (!update.trainNumber.empty())
    {
        const auto found = trainNumberIndex_.find(update.trainNumber);
        if (found != trainNumberIndex_.end()) add(found->second);
    }
    if (!update.locomotiveId.empty())
    {
        const auto exact = locomotiveEndIndex_.find(
            LocomotiveEndKey(update.locomotiveId, update.locomotiveEnd));
        if (exact != locomotiveEndIndex_.end()) add(exact->second);
        const auto anyEnd = locomotiveIndex_.find(update.locomotiveId);
        if (anyEnd != locomotiveIndex_.end()) add(anyEnd->second);
    }
    return result;
}

RailwayStateManager::TargetUid RailwayStateManager::FindBestScoredCandidate(
    const RailwayApproachMessage& update) const
{
    TargetUid bestUid = 0;
    int bestScore = kCandidateMergeThreshold - 1;
    std::uint64_t bestTime = 0;
    for (const auto& entry : targets_)
    {
        const int score = CandidateScore(update, entry.second);
        if (score > bestScore || (score == bestScore &&
            entry.second.lastAnyUpdateUnixMs > bestTime))
        {
            bestUid = entry.first;
            bestScore = score;
            bestTime = entry.second.lastAnyUpdateUnixMs;
        }
    }
    return bestUid;
}

int RailwayStateManager::CandidateScore(const RailwayApproachMessage& update,
                                        const RailwayTarget& target) const
{
    const bool complementary =
        (update.hasBasic && !update.hasExtension && !target.hasBasic && target.hasExtension) ||
        (update.hasExtension && !update.hasBasic && target.hasBasic && !target.hasExtension);
    // Heuristics may pair a BASIC_ONLY with an EXT_ONLY target. They must never
    // collapse two unrelated BASIC or two unrelated EXT targets merely because
    // both were observed close together on the same line.
    if (!complementary)
        return 0;
    int score = 0;
    if (update.transmissionId != 0 &&
        std::find(target.transmissionAliases.begin(), target.transmissionAliases.end(),
                  update.transmissionId) != target.transmissionAliases.end()) score += 100;
    if (!update.trainNumber.empty() &&
        std::find(target.trainNumberAliases.begin(), target.trainNumberAliases.end(),
                  update.trainNumber) != target.trainNumberAliases.end()) score += 100;
    if (!update.locomotiveId.empty() &&
        std::find(target.locomotiveIdAliases.begin(), target.locomotiveIdAliases.end(),
                  update.locomotiveId) != target.locomotiveIdAliases.end()) score += 100;
    if (complementary) score += 20;
    const std::uint64_t difference = AbsoluteDifference(update.timestampUnixMs,
                                                        target.lastAnyUpdateUnixMs);
    if (difference < 1000) score += 40;
    else if (difference < 5000) score += 20;
    if (!update.lineName.empty() && !target.lineName.empty() &&
        update.lineName == target.lineName) score += 20;
    if (update.hasLongitude && update.hasLatitude && target.hasPosition &&
        DistanceKm(update.longitudeDeg, update.latitudeDeg,
                   target.longitudeDeg, target.latitudeDeg) <= 20.0) score += 20;
    if (update.hasKilometer && target.hasKilometer &&
        std::abs(update.kilometerKm - target.kilometerKm) <= 20.0) score += 10;
    return score;
}

void RailwayStateManager::MergeTargets(TargetUid primaryUid, TargetUid secondaryUid,
                                       const std::string& reason)
{
    if (primaryUid == secondaryUid) return;
    auto primaryIt = targets_.find(primaryUid);
    auto secondaryIt = targets_.find(secondaryUid);
    if (primaryIt == targets_.end() || secondaryIt == targets_.end()) return;
    RailwayTarget& primary = primaryIt->second;
    const RailwayTarget secondary = secondaryIt->second;
    for (auto value : secondary.transmissionAliases) AddUnique(primary.transmissionAliases, value);
    for (const auto& value : secondary.trainNumberAliases) AddUnique(primary.trainNumberAliases, value);
    for (const auto& value : secondary.locomotiveIdAliases) AddUnique(primary.locomotiveIdAliases, value);
    for (const auto& value : secondary.locomotiveEndHistory) AddUnique(primary.locomotiveEndHistory, value);

    if (secondary.hasBasic && (!primary.hasBasic ||
        secondary.lastBasicUpdateUnixMs >= primary.lastBasicUpdateUnixMs))
    {
        primary.hasBasic = true;
        primary.lastBasicUpdateUnixMs = secondary.lastBasicUpdateUnixMs;
        primary.trainNumber = secondary.trainNumber;
        primary.hasSpeed = secondary.hasSpeed;
        primary.speedKmh = secondary.speedKmh;
        primary.hasKilometer = secondary.hasKilometer;
        primary.kilometerKm = secondary.kilometerKm;
        primary.basic = secondary.basic;
    }
    if (secondary.hasExtension && (!primary.hasExtension ||
        secondary.lastExtensionUpdateUnixMs >= primary.lastExtensionUpdateUnixMs))
    {
        primary.hasExtension = true;
        primary.lastExtensionUpdateUnixMs = secondary.lastExtensionUpdateUnixMs;
        primary.trainPrefix = secondary.trainPrefix;
        primary.locomotiveId = secondary.locomotiveId;
        primary.locomotiveEnd = secondary.locomotiveEnd;
        primary.lineName = secondary.lineName;
        primary.longitudeRaw = secondary.longitudeRaw;
        primary.longitudeDegreeMinute = secondary.longitudeDegreeMinute;
        primary.latitudeRaw = secondary.latitudeRaw;
        primary.latitudeDegreeMinute = secondary.latitudeDegreeMinute;
        primary.hasPosition = secondary.hasPosition;
        primary.longitudeDeg = secondary.longitudeDeg;
        primary.latitudeDeg = secondary.latitudeDeg;
        primary.railwayAuxRaw = secondary.railwayAuxRaw;
        primary.extension = secondary.extension;
    }
    if (secondary.lastAnyUpdateUnixMs >= primary.lastAnyUpdateUnixMs)
    {
        primary.transmissionId = secondary.transmissionId;
        primary.pairingMethod = secondary.pairingMethod;
    }
    primary.lastAnyUpdateUnixMs = std::max(primary.lastAnyUpdateUnixMs,
                                           secondary.lastAnyUpdateUnixMs);
    primary.track.insert(primary.track.end(), secondary.track.begin(), secondary.track.end());
    NormalizeTrack(primary.track);
    primary.mergeCount += secondary.mergeCount + 1;
    primary.lastMergeReason = reason;
    ++totalMergeCount_;
    targets_.erase(secondaryIt);
    RefreshDerivedFields(primary);
    RefreshPosition(primary, false);
}

void RailwayStateManager::MergeIdentityConflicts(TargetUid primaryUid)
{
    auto primaryIt = targets_.find(primaryUid);
    if (primaryIt == targets_.end()) return;
    std::vector<TargetUid> conflicts;
    for (const auto& entry : targets_)
    {
        if (entry.first == primaryUid) continue;
        const bool sameTrain = !primaryIt->second.trainNumber.empty() &&
            entry.second.trainNumber == primaryIt->second.trainNumber;
        const bool sameLocomotive = !primaryIt->second.locomotiveId.empty() &&
            entry.second.locomotiveId == primaryIt->second.locomotiveId;
        if (sameTrain || sameLocomotive) conflicts.push_back(entry.first);
    }
    for (TargetUid uid : conflicts)
        MergeTargets(primaryUid, uid, "IDENTITY_CONFLICT");
}

void RailwayStateManager::MergeInto(RailwayTarget& target,
                                    const RailwayApproachMessage& update)
{
    target.transmissionId = update.transmissionId;
    AddUnique(target.transmissionAliases, update.transmissionId);
    target.lastAnyUpdateUnixMs = std::max(target.lastAnyUpdateUnixMs, update.timestampUnixMs);
    target.pairingMethod = update.pairingMethod;
    target.stale = false;
    if (update.hasBasic && update.timestampUnixMs >= target.lastBasicUpdateUnixMs)
    {
        target.hasBasic = true;
        target.lastBasicUpdateUnixMs = update.timestampUnixMs;
        target.trainNumber = update.trainNumber;
        AddUnique(target.trainNumberAliases, update.trainNumber);
        target.hasSpeed = update.hasSpeed;
        target.speedKmh = update.speedKmh;
        target.hasKilometer = update.hasKilometer;
        target.kilometerKm = update.kilometerKm;
        target.basic = update.basic;
    }
    if (update.hasExtension && update.timestampUnixMs >= target.lastExtensionUpdateUnixMs)
    {
        target.hasExtension = true;
        target.lastExtensionUpdateUnixMs = update.timestampUnixMs;
        target.trainPrefix = update.trainPrefix;
        target.locomotiveId = update.locomotiveId;
        target.locomotiveEnd = update.locomotiveEnd;
        AddUnique(target.locomotiveIdAliases, update.locomotiveId);
        AddUnique(target.locomotiveEndHistory, update.locomotiveEnd);
        target.lineName = update.lineName;
        target.longitudeRaw = update.longitudeRaw;
        target.longitudeDegreeMinute = update.longitudeDegreeMinute;
        target.latitudeRaw = update.latitudeRaw;
        target.latitudeDegreeMinute = update.latitudeDegreeMinute;
        target.hasPosition = update.hasLongitude && update.hasLatitude;
        target.longitudeDeg = update.longitudeDeg;
        target.latitudeDeg = update.latitudeDeg;
        target.railwayAuxRaw = update.railwayAuxRaw;
        target.extension = update.extension;
        if (target.hasPosition)
        {
            target.track.push_back({ target.latitudeRaw, target.longitudeRaw,
                target.latitudeDegreeMinute, target.longitudeDegreeMinute,
                target.latitudeDeg, target.longitudeDeg, update.timestampUnixMs,
                PositionSource::RadioGps, PositionQuality::High });
            NormalizeTrack(target.track);
        }
    }
}

bool RailwayStateManager::PassesLearningQuality(const RailwayTarget& target) const
{
    const std::uint64_t pairDifference = AbsoluteDifference(
        target.lastBasicUpdateUnixMs, target.lastExtensionUpdateUnixMs);
    return target.hasBasic && target.hasExtension && target.hasKilometer &&
        !target.lineName.empty() && target.hasPosition &&
        pairDifference <= kFallbackWindowMs &&
        target.basic.railwayValid && target.basic.messageCodewordCount == 3 &&
        !target.basic.hasUncorrectableCodeword && target.basic.decodeConfidence == "HIGH" &&
        target.extension.railwayExtValid && target.extension.messageCodewordCount == 10 &&
        !target.extension.hasUncorrectableCodeword &&
        target.extension.railwayExtConfidence == "HIGH";
}

bool RailwayStateManager::PassesMotionContinuity(const RailwayTarget& target) const
{
    const auto previous = lastLearnedMileage_.find(target.targetUid);
    if (previous == lastLearnedMileage_.end()) return true;
    const std::uint64_t dtMs = target.lastAnyUpdateUnixMs >= previous->second.second
        ? target.lastAnyUpdateUnixMs - previous->second.second : 0;
    if (dtMs == 0) return true;
    const double elapsedHours = static_cast<double>(dtMs) / 3600000.0;
    const double expectedKm = target.hasSpeed ? std::abs(target.speedKmh) * elapsedHours : 0.0;
    const double allowedKm = std::max(1.0, expectedKm * 3.0 + 0.5);
    return std::abs(target.kilometerKm - previous->second.first) <= allowedKm;
}

void RailwayStateManager::RefreshPosition(RailwayTarget& target, bool allowLearning)
{
    if (allowLearning && PassesLearningQuality(target) && PassesMotionContinuity(target))
    {
        if (mileageDatabase_.AddSample(target.lineName, target.kilometerKm,
                                       target.latitudeDeg, target.longitudeDeg,
                                       target.lastAnyUpdateUnixMs))
            lastLearnedMileage_[target.targetUid] = { target.kilometerKm,
                                                      target.lastAnyUpdateUnixMs };
    }
    const MileagePositionEstimate estimate = target.hasKilometer && !target.lineName.empty()
        ? mileageDatabase_.Lookup(target.lineName, target.kilometerKm)
        : MileagePositionEstimate{};
    target.hasMileageEstimate = estimate.valid;
    target.mileageLatitudeDeg = estimate.latitude;
    target.mileageLongitudeDeg = estimate.longitude;
    target.mileagePositionSource = estimate.source;
    target.mileagePositionQuality = estimate.quality;
    target.mileagePositionConfidence = estimate.confidence;
    target.gpsMileageComparison = PositionComparison::Unavailable;
    target.gpsVsMileageDistanceMeters = 0.0;
    target.radioGpsFresh = target.hasPosition &&
        target.lastAnyUpdateUnixMs >= target.lastExtensionUpdateUnixMs &&
        target.lastAnyUpdateUnixMs - target.lastExtensionUpdateUnixMs <= 15000;
    if (target.radioGpsFresh && estimate.valid)
        target.gpsMileageComparison = mileageDatabase_.Compare(
            target.latitudeDeg, target.longitudeDeg, estimate,
            &target.gpsVsMileageDistanceMeters);

    if (target.radioGpsFresh)
    {
        target.hasDisplayedPosition = true;
        target.displayedLatitudeDeg = target.latitudeDeg;
        target.displayedLongitudeDeg = target.longitudeDeg;
        target.positionSource = PositionSource::RadioGps;
        target.positionQuality = PositionQuality::High;
        target.positionConfidence = 1.0;
    }
    else if (estimate.valid)
    {
        target.hasDisplayedPosition = true;
        target.displayedLatitudeDeg = estimate.latitude;
        target.displayedLongitudeDeg = estimate.longitude;
        target.positionSource = estimate.source;
        target.positionQuality = estimate.quality;
        target.positionConfidence = estimate.confidence;
        const bool duplicate = !target.track.empty() &&
            target.track.back().timestampUnixMs == target.lastAnyUpdateUnixMs &&
            target.track.back().source == estimate.source;
        if (!duplicate)
        {
            target.track.push_back({ {}, {}, {}, {}, estimate.latitude,
                estimate.longitude, target.lastAnyUpdateUnixMs,
                estimate.source, estimate.quality });
            NormalizeTrack(target.track);
        }
    }
    else
    {
        target.hasDisplayedPosition = false;
        target.positionSource = PositionSource::NoPosition;
        target.positionQuality = PositionQuality::Invalid;
        target.positionConfidence = 0.0;
    }
}

void RailwayStateManager::RebuildIndexes()
{
    trainNumberIndex_.clear(); locomotiveEndIndex_.clear();
    locomotiveIndex_.clear(); transmissionIndex_.clear();
    for (const auto& entry : targets_)
    {
        const TargetUid uid = entry.first;
        const RailwayTarget& target = entry.second;
        for (const auto& train : target.trainNumberAliases)
            if (!train.empty()) trainNumberIndex_[train] = uid;
        for (const auto& locomotive : target.locomotiveIdAliases)
            if (!locomotive.empty()) locomotiveIndex_[locomotive] = uid;
        for (const auto& locomotive : target.locomotiveIdAliases)
            for (const auto& end : target.locomotiveEndHistory)
            {
                const std::string key = LocomotiveEndKey(locomotive, end);
                if (!key.empty()) locomotiveEndIndex_[key] = uid;
            }
        for (std::uint64_t transmission : target.transmissionAliases)
            if (transmission != 0) transmissionIndex_[transmission] = uid;
    }
}

void RailwayStateManager::RefreshDerivedFields(RailwayTarget& target)
{
    target.fullTrainNumber = target.trainNumber.empty() ? std::string() :
        target.trainPrefix + target.trainNumber;
    target.targetId = !target.fullTrainNumber.empty() ? target.fullTrainNumber :
        !target.locomotiveId.empty() ? "LOCO:" + target.locomotiveId :
        "TARGET:" + std::to_string(target.targetUid);
    target.dataCompleteness = target.hasBasic && target.hasExtension ? "FULL" :
        target.hasBasic ? "BASIC ONLY" : "EXT ONLY";
    const bool highBasic = !target.hasBasic || target.basic.decodeConfidence == "HIGH";
    const bool highExtension = !target.hasExtension ||
        target.extension.railwayExtConfidence == "HIGH";
    target.quality = highBasic && highExtension ? "HIGH" : "MEDIUM";
}

void RailwayStateManager::NormalizeTrack(std::vector<RailwayTrackPoint>& track)
{
    std::sort(track.begin(), track.end(), [](const RailwayTrackPoint& a,
                                             const RailwayTrackPoint& b) {
        if (a.timestampUnixMs != b.timestampUnixMs) return a.timestampUnixMs < b.timestampUnixMs;
        if (a.latitude != b.latitude) return a.latitude < b.latitude;
        return a.longitude < b.longitude;
    });
    track.erase(std::unique(track.begin(), track.end(), SameTrackPoint), track.end());
    if (track.size() > kMaxTrackPoints)
        track.erase(track.begin(), track.end() - static_cast<std::ptrdiff_t>(kMaxTrackPoints));
}

std::vector<RailwayTarget> RailwayStateManager::Snapshot(std::uint64_t nowUnixMs)
{
    for (auto it = targets_.begin(); it != targets_.end();)
    {
        const std::uint64_t age = nowUnixMs >= it->second.lastAnyUpdateUnixMs
            ? nowUnixMs - it->second.lastAnyUpdateUnixMs : 0;
        if (age > kRemoveAfterMs) it = targets_.erase(it);
        else { it->second.stale = age > kStaleAfterMs; ++it; }
    }
    RebuildIndexes();
    std::vector<RailwayTarget> result;
    result.reserve(targets_.size());
    for (const auto& entry : targets_) result.push_back(entry.second);
    std::sort(result.begin(), result.end(), [](const RailwayTarget& a,
                                               const RailwayTarget& b) {
        return a.lastAnyUpdateUnixMs > b.lastAnyUpdateUnixMs;
    });
    return result;
}

void RailwayStateManager::Clear()
{
    targets_.clear(); trainNumberIndex_.clear(); locomotiveEndIndex_.clear();
    locomotiveIndex_.clear(); transmissionIndex_.clear(); nextTargetUid_ = 0;
    totalMergeCount_ = 0;
    lastLearnedMileage_.clear();
}
