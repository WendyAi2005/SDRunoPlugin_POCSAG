#include "RailwayMessageAssembler.h"

#include <algorithm>

namespace
{
RailwayApproachMessage MakeBasic(std::uint64_t transmissionId, std::uint64_t timestampUnixMs,
                                 std::size_t index, const PocsagMessage& basic)
{
    RailwayApproachMessage result;
    result.transmissionId = transmissionId;
    result.timestampUnixMs = timestampUnixMs;
    result.pairingMethod = "INDEPENDENT";
    result.basicMessageIndex = index;
    result.hasBasic = true;
    result.dataCompleteness = "BASIC ONLY";
    result.basic = basic;
    result.trainNumber = basic.decodedTrain;
    result.fullTrainNumber = basic.decodedTrain;
    result.hasSpeed = basic.hasDecodedSpeed;
    result.speedKmh = basic.decodedSpeedKmh;
    result.hasKilometer = basic.hasDecodedKilometer;
    result.kilometerKm = basic.decodedKilometer;
    result.basicCorrectedBits = basic.correctedBits;
    result.confidence = basic.decodeConfidence;
    return result;
}

RailwayApproachMessage MakeExtension(std::uint64_t transmissionId, std::uint64_t timestampUnixMs,
                                     std::size_t index, const PocsagMessage& extension)
{
    RailwayApproachMessage result;
    result.transmissionId = transmissionId;
    result.timestampUnixMs = timestampUnixMs;
    result.pairingMethod = "INDEPENDENT";
    result.extensionMessageIndex = index;
    result.hasExtension = true;
    result.dataCompleteness = "EXT ONLY";
    result.extension = extension;
    result.trainPrefix = extension.trainPrefix;
    result.locomotiveId = extension.locomotiveId;
    result.locomotiveEnd = extension.locomotiveEnd;
    result.lineName = extension.lineName;
    result.longitudeRaw = extension.longitudeRaw;
    result.longitudeDegreeMinute = extension.longitudeDegreeMinute;
    result.hasLongitude = extension.longitudeValid;
    result.longitudeDeg = extension.longitudeDeg;
    result.latitudeRaw = extension.latitudeRaw;
    result.latitudeDegreeMinute = extension.latitudeDegreeMinute;
    result.hasLatitude = extension.latitudeValid;
    result.latitudeDeg = extension.latitudeDeg;
    result.railwayAuxRaw = extension.railwayAuxRaw;
    result.extensionCorrectedBits = extension.correctedBits;
    result.extensionUncorrectable = extension.hasUncorrectableCodeword;
    result.confidence = extension.railwayExtConfidence;
    return result;
}
}

std::vector<RailwayApproachMessage> RailwayMessageAssembler::Assemble(
    std::uint64_t transmissionId,
    std::uint64_t timestampUnixMs,
    const std::vector<PocsagMessage>& interpretedMessages)
{
    std::vector<std::size_t> basics;
    std::vector<std::size_t> extensions;
    for (std::size_t i = 0; i < interpretedMessages.size(); ++i)
    {
        const auto& message = interpretedMessages[i];
        if (message.address == 1234000 && message.railwayValid)
            basics.push_back(i);
        else if (message.address == 1234002 && message.railwayExtValid)
            extensions.push_back(i);
    }

    const std::size_t pairCount = std::min(basics.size(), extensions.size());
    std::vector<RailwayApproachMessage> result;
    result.reserve(basics.size() + extensions.size() - pairCount);
    for (std::size_t i = 0; i < pairCount; ++i)
    {
        RailwayApproachMessage combined = MakeBasic(
            transmissionId, timestampUnixMs, basics[i], interpretedMessages[basics[i]]);
        const RailwayApproachMessage extension = MakeExtension(
            transmissionId, timestampUnixMs, extensions[i], interpretedMessages[extensions[i]]);
        combined.pairingMethod = "TRANSMISSION_ID";
        combined.hasExtension = true;
        combined.dataCompleteness = "FULL";
        combined.extensionMessageIndex = extensions[i];
        combined.extension = extension.extension;

        combined.trainPrefix = combined.extension.trainPrefix;
        combined.trainNumber = combined.basic.decodedTrain;
        combined.fullTrainNumber = combined.trainPrefix + combined.trainNumber;
        combined.hasSpeed = combined.basic.hasDecodedSpeed;
        combined.speedKmh = combined.basic.decodedSpeedKmh;
        combined.hasKilometer = combined.basic.hasDecodedKilometer;
        combined.kilometerKm = combined.basic.decodedKilometer;
        combined.locomotiveId = combined.extension.locomotiveId;
        combined.locomotiveEnd = combined.extension.locomotiveEnd;
        combined.lineName = combined.extension.lineName;
        combined.longitudeRaw = combined.extension.longitudeRaw;
        combined.longitudeDegreeMinute = combined.extension.longitudeDegreeMinute;
        combined.hasLongitude = combined.extension.longitudeValid;
        combined.longitudeDeg = combined.extension.longitudeDeg;
        combined.latitudeRaw = combined.extension.latitudeRaw;
        combined.latitudeDegreeMinute = combined.extension.latitudeDegreeMinute;
        combined.hasLatitude = combined.extension.latitudeValid;
        combined.latitudeDeg = combined.extension.latitudeDeg;
        combined.railwayAuxRaw = combined.extension.railwayAuxRaw;
        combined.basicCorrectedBits = combined.basic.correctedBits;
        combined.extensionCorrectedBits = combined.extension.correctedBits;
        combined.extensionUncorrectable = combined.extension.hasUncorrectableCodeword;
        combined.confidence = combined.basic.decodeConfidence == "HIGH" &&
                              combined.extension.railwayExtConfidence == "HIGH"
            ? "HIGH" : "MEDIUM";
        result.push_back(std::move(combined));
    }
    for (std::size_t i = pairCount; i < basics.size(); ++i)
        result.push_back(MakeBasic(transmissionId, timestampUnixMs, basics[i], interpretedMessages[basics[i]]));
    for (std::size_t i = pairCount; i < extensions.size(); ++i)
        result.push_back(MakeExtension(transmissionId, timestampUnixMs, extensions[i], interpretedMessages[extensions[i]]));
    return result;
}
