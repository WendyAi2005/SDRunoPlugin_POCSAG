#pragma once

#include <string>

struct RailwayCoordinate
{
    std::string rawCoordinate;
    std::string degreeMinute;
    bool valid = false;
    int degrees = 0;
    double minutes = 0.0;
    double decimalDegree = 0.0;
};

struct RailwayExtFields
{
    bool complete = false;
    bool valid = false;
    bool truncated = false;
    std::string normalizedHex;

    std::string trainPrefix;
    bool trainPrefixValid = false;
    std::string locomotiveId;
    std::string locomotiveTypeCode;
    std::string locomotiveSerial;
    bool locomotiveIdValid = false;
    std::string locomotiveEnd;
    bool locomotiveEndValid = false;
    std::string lineName;
    std::string lineNameRawHex;
    bool lineNameValid = false;

    std::string longitudeRaw;
    std::string longitudeDegreeMinute;
    bool longitudeValid = false;
    double longitudeDeg = 0.0;
    std::string latitudeRaw;
    std::string latitudeDegreeMinute;
    bool latitudeValid = false;
    double latitudeDeg = 0.0;

    std::string railwayAuxRaw;
    std::string confidence;
    std::string failureReason;
};

// Reverses the four bits inside every hexadecimal nibble independently.
// The operation is self-inverse: 0123456789ABCDEF -> 084C2A6E195D3B7F.
std::string ReverseBitsPerNibble(const std::string& hexNibbles);

// Builds normalized railway hex directly from BCH-corrected POCSAG message bits.
std::string NormalizeRailwayExtPayload(const std::string& correctedMessageBits);

// Decodes the fixed eight-byte railway line field as Windows code page 936 (GBK).
std::string DecodeRailwayLineName(const std::string& lineHex, bool& valid);

RailwayCoordinate ParseRailwayLongitudeNmea(const std::string& rawCoordinate);
RailwayCoordinate ParseRailwayLatitudeNmea(const std::string& rawCoordinate);

RailwayExtFields DecodeRailwayExt(const std::string& correctedMessageBits,
                                  int messageCodewordCount,
                                  int correctedBits,
                                  bool hasUncorrectableCodeword);
