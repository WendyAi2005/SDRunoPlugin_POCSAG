#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "PocsagDecoder.h"

struct RailwayApproachMessage
{
    std::uint64_t transmissionId = 0;
    std::uint64_t timestampUnixMs = 0;
    std::string pairingMethod;
    std::size_t basicMessageIndex = 0;
    std::size_t extensionMessageIndex = 0;
    bool hasBasic = false;
    bool hasExtension = false;
    std::string dataCompleteness;

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
    bool hasLongitude = false;
    double longitudeDeg = 0.0;
    std::string latitudeRaw;
    std::string latitudeDegreeMinute;
    bool hasLatitude = false;
    double latitudeDeg = 0.0;
    std::string railwayAuxRaw;
    std::string confidence;
    int basicCorrectedBits = 0;
    int extensionCorrectedBits = 0;
    bool extensionUncorrectable = false;

    PocsagMessage basic;
    PocsagMessage extension;
};

class RailwayMessageAssembler
{
public:
    static std::vector<RailwayApproachMessage> Assemble(
        std::uint64_t transmissionId,
        std::uint64_t timestampUnixMs,
        const std::vector<PocsagMessage>& interpretedMessages);
};
