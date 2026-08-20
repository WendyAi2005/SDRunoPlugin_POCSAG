#include "RailwayExtDecoder.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
int HexValue(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

char HexDigit(unsigned value)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return kDigits[value & 0x0Fu];
}

std::string TrimAscii(std::string value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\0'))
        value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0'))
        value.pop_back();
    return value;
}

bool DecodeAsciiHex(const std::string& hex, std::string& decoded, bool allowEmpty)
{
    if (hex.empty() || (hex.size() & 1u) != 0)
        return false;
    decoded.clear();
    decoded.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int high = HexValue(hex[i]);
        const int low = HexValue(hex[i + 1]);
        if (high < 0 || low < 0)
            return false;
        const unsigned char ch = static_cast<unsigned char>((high << 4) | low);
        if (ch != 0 && (ch < 0x20 || ch > 0x7E))
            return false;
        decoded.push_back(static_cast<char>(ch));
    }
    decoded = TrimAscii(decoded);
    return allowEmpty || !decoded.empty();
}

bool IsDecimal(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= '0' && ch <= '9';
    });
}

void AddFailure(std::string& failures, const char* failure)
{
    if (!failures.empty())
        failures += ';';
    failures += failure;
}

RailwayCoordinate ParseNmeaCoordinate(const std::string& rawCoordinate, std::size_t degreeDigits,
                                      int maximumDegrees, char hemisphere)
{
    RailwayCoordinate result;
    result.rawCoordinate = rawCoordinate;
    if (rawCoordinate.size() != degreeDigits + 6 || !IsDecimal(rawCoordinate))
        return result;

    result.degrees = std::stoi(rawCoordinate.substr(0, degreeDigits));
    result.minutes = std::stoul(rawCoordinate.substr(degreeDigits)) / 10000.0;
    if (result.degrees > maximumDegrees || result.minutes >= 60.0 ||
        (result.degrees == maximumDegrees && result.minutes > 0.0))
        return result;

    result.decimalDegree = result.degrees + result.minutes / 60.0;
    std::ostringstream display;
    display << result.degrees << "°" << std::fixed << std::setprecision(4)
            << result.minutes << "′ " << hemisphere;
    result.degreeMinute = display.str();
    result.valid = true;
    return result;
}
}

std::string ReverseBitsPerNibble(const std::string& hexNibbles)
{
    std::string result;
    result.reserve(hexNibbles.size());
    for (char ch : hexNibbles)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
            continue;
        const int value = HexValue(ch);
        if (value < 0)
            return {};
        unsigned reversed = 0;
        reversed |= (static_cast<unsigned>(value) & 0x1u) << 3;
        reversed |= (static_cast<unsigned>(value) & 0x2u) << 1;
        reversed |= (static_cast<unsigned>(value) & 0x4u) >> 1;
        reversed |= (static_cast<unsigned>(value) & 0x8u) >> 3;
        result.push_back(HexDigit(reversed));
    }
    return result;
}

std::string NormalizeRailwayExtPayload(const std::string& correctedMessageBits)
{
    std::string result;
    result.reserve(correctedMessageBits.size() / 4);
    for (std::size_t i = 0; i + 3 < correctedMessageBits.size(); i += 4)
    {
        unsigned value = 0;
        for (int bit = 0; bit < 4; ++bit)
        {
            const char ch = correctedMessageBits[i + static_cast<std::size_t>(bit)];
            if (ch != '0' && ch != '1')
                return {};
            if (ch == '1')
                value |= 1u << bit;
        }
        result.push_back(HexDigit(value));
    }
    return result;
}

std::string DecodeRailwayLineName(const std::string& lineHex, bool& valid)
{
    valid = false;
    if (lineHex.empty() || (lineHex.size() & 1u) != 0)
        return {};

    std::string bytes;
    bytes.reserve(lineHex.size() / 2);
    for (std::size_t i = 0; i < lineHex.size(); i += 2)
    {
        const int high = HexValue(lineHex[i]);
        const int low = HexValue(lineHex[i + 1]);
        if (high < 0 || low < 0)
            return {};
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    while (!bytes.empty() && (bytes.back() == '\0' || bytes.back() == ' '))
        bytes.pop_back();
    while (!bytes.empty() && (bytes.front() == '\0' || bytes.front() == ' '))
        bytes.erase(bytes.begin());
    if (bytes.empty())
        return {};

    const int wideCount = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, 0);
    if (wideCount <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    if (MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(),
                            static_cast<int>(bytes.size()), wide.data(), wideCount) != wideCount)
        return {};

    const int utf8Count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount,
                                               nullptr, 0, nullptr, nullptr);
    if (utf8Count <= 0)
        return {};
    std::string utf8(static_cast<std::size_t>(utf8Count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount, utf8.data(), utf8Count,
                            nullptr, nullptr) != utf8Count)
        return {};
    valid = true;
    return utf8;
}

RailwayCoordinate ParseRailwayLongitudeNmea(const std::string& rawCoordinate)
{
    return ParseNmeaCoordinate(rawCoordinate, 3, 180, 'E');
}

RailwayCoordinate ParseRailwayLatitudeNmea(const std::string& rawCoordinate)
{
    return ParseNmeaCoordinate(rawCoordinate, 2, 90, 'N');
}

RailwayExtFields DecodeRailwayExt(const std::string& correctedMessageBits,
                                  int messageCodewordCount,
                                  int correctedBits,
                                  bool hasUncorrectableCodeword)
{
    RailwayExtFields result;
    result.truncated = messageCodewordCount < 10 || correctedMessageBits.size() < 200;
    result.complete = messageCodewordCount == 10 && correctedMessageBits.size() == 200;
    result.normalizedHex = NormalizeRailwayExtPayload(correctedMessageBits);

    if (result.truncated)
        AddFailure(result.failureReason, "truncated");
    else if (!result.complete)
        AddFailure(result.failureReason, "unexpected_length");
    if (hasUncorrectableCodeword)
        AddFailure(result.failureReason, "uncorrectable_codeword");
    if (result.normalizedHex.size() < 50)
    {
        result.confidence = "LOW";
        return result;
    }

    std::string prefix;
    result.trainPrefixValid = DecodeAsciiHex(result.normalizedHex.substr(0, 4), prefix, true);
    result.trainPrefix = prefix;
    if (!result.trainPrefixValid)
        AddFailure(result.failureReason, "invalid_prefix_ascii");

    result.locomotiveId = result.normalizedHex.substr(4, 8);
    result.locomotiveIdValid = IsDecimal(result.locomotiveId);
    if (result.locomotiveIdValid)
    {
        result.locomotiveTypeCode = result.locomotiveId.substr(0, 3);
        result.locomotiveSerial = result.locomotiveId.substr(3, 5);
    }
    else
    {
        AddFailure(result.failureReason, "invalid_locomotive_id");
    }

    result.locomotiveEndValid = DecodeAsciiHex(result.normalizedHex.substr(12, 2),
                                                result.locomotiveEnd, false);
    if (!result.locomotiveEndValid)
        AddFailure(result.failureReason, "invalid_locomotive_end_ascii");

    result.lineNameRawHex = result.normalizedHex.substr(14, 16);
    result.lineName = DecodeRailwayLineName(result.lineNameRawHex, result.lineNameValid);
    if (!result.lineNameValid)
        AddFailure(result.failureReason, "invalid_gbk_line_name");

    result.longitudeRaw = result.normalizedHex.substr(30, 9);
    const RailwayCoordinate longitude = ParseRailwayLongitudeNmea(result.longitudeRaw);
    result.longitudeValid = longitude.valid;
    result.longitudeDegreeMinute = longitude.degreeMinute;
    result.longitudeDeg = longitude.decimalDegree;
    if (!result.longitudeValid)
        AddFailure(result.failureReason, "invalid_longitude");

    result.latitudeRaw = result.normalizedHex.substr(39, 8);
    const RailwayCoordinate latitude = ParseRailwayLatitudeNmea(result.latitudeRaw);
    result.latitudeValid = latitude.valid;
    result.latitudeDegreeMinute = latitude.degreeMinute;
    result.latitudeDeg = latitude.decimalDegree;
    if (!result.latitudeValid)
        AddFailure(result.failureReason, "invalid_latitude");

    result.railwayAuxRaw = result.normalizedHex.substr(47, 3);
    result.valid = result.complete && !hasUncorrectableCodeword && result.trainPrefixValid &&
                   result.locomotiveIdValid && result.locomotiveEndValid && result.lineNameValid &&
                   result.longitudeValid && result.latitudeValid;
    result.confidence = result.valid ? (correctedBits == 0 ? "HIGH" : "MEDIUM") : "LOW";
    return result;
}
