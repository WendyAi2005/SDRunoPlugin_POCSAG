#include "../PocsagDecoder.h"
#include "../RailwayExtDecoder.h"
#include "../RailwayMessageAssembler.h"
#include "../RailwayStateManager.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
constexpr std::uint32_t kSyncWord = 0x7CD215D8u;
constexpr std::uint32_t kIdleWord = 0x7A89C197u;
constexpr std::uint32_t kPolynomial = 0x769u;

int PopCount(std::uint32_t value)
{
    int count = 0;
    while (value)
    {
        value &= value - 1;
        ++count;
    }
    return count;
}

std::uint32_t EncodeInformation(std::uint32_t information21)
{
    std::uint32_t value = information21 << 10;
    std::uint32_t remainder = value;
    for (int bit = 30; bit >= 10; --bit)
        if (remainder & (1u << bit))
            remainder ^= kPolynomial << (bit - 10);
    const std::uint32_t code31 = value | (remainder & 0x3FFu);
    std::uint32_t word = code31 << 1;
    if (PopCount(word) & 1)
        word |= 1u;
    return word;
}

std::uint32_t EncodeAddress(std::uint32_t address, int function)
{
    const std::uint32_t information = ((address >> 3) << 2) | static_cast<std::uint32_t>(function & 3);
    return EncodeInformation(information);
}

std::vector<bool> NumericBits(const std::string& text)
{
    const std::string alphabet = "084 2.6]195-3U7[";
    std::vector<bool> bits;
    for (char ch : text)
    {
        const auto position = alphabet.find(ch);
        assert(position != std::string::npos);
        for (int bit = 3; bit >= 0; --bit)
            bits.push_back((static_cast<int>(position) & (1 << bit)) != 0);
    }
    while (bits.size() % 20 != 0)
        bits.push_back(false);
    return bits;
}

std::vector<bool> HexBits(const std::string& text, std::size_t bitCount)
{
    std::vector<bool> bits;
    for (std::size_t i = 0; i < text.size();)
    {
        while (i < text.size() && text[i] == ' ')
            ++i;
        if (i >= text.size())
            break;
        assert(i + 1 < text.size());
        const auto hexValue = [](char ch) {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };
        const int high = hexValue(text[i++]);
        const int low = hexValue(text[i++]);
        assert(high >= 0 && low >= 0);
        const int byte = (high << 4) | low;
        for (int bit = 7; bit >= 0; --bit)
            bits.push_back((byte & (1 << bit)) != 0);
    }
    assert(bits.size() >= bitCount);
    bits.resize(bitCount);
    return bits;
}

std::string BitsText(const std::vector<bool>& bits)
{
    std::string result;
    result.reserve(bits.size());
    for (bool bit : bits)
        result.push_back(bit ? '1' : '0');
    return result;
}

std::uint32_t EncodeMessageWord(const std::vector<bool>& bits, std::size_t offset)
{
    std::uint32_t information = 1u << 20;
    for (int i = 0; i < 20; ++i)
        if (bits[offset + static_cast<std::size_t>(i)])
            information |= 1u << (19 - i);
    return EncodeInformation(information);
}

void AppendWord(std::vector<bool>& bits, std::uint32_t word)
{
    for (int bit = 31; bit >= 0; --bit)
        bits.push_back(((word >> bit) & 1u) != 0);
}
}

int main(int argc, char** argv)
{
    // Test A: standard numeric nibble mapping plus railway field extraction.
    const std::string numericA = DecodePocsagNumeric(
        HexBits("36 96 23 80 C3 33 26 A0", 60));
    assert(numericA == " 6964 103   465");
    const RailwayFields railwayA = ParseRailwayNumeric(numericA);
    assert(railwayA.valid && railwayA.train == "6964");
    assert(railwayA.hasSpeed && railwayA.speedKmh == 103);
    assert(railwayA.hasKilometer && std::abs(railwayA.kilometer - 46.5) < 0.001);
    std::cout << "Test A passed\n";

    // Test B.
    const RailwayFields railwayB = ParseRailwayNumeric(DecodePocsagNumeric(
        HexBits("89 00 83 3C C3 88 4A 00", 60)));
    assert(railwayB.valid && railwayB.train == "19001");
    assert(railwayB.hasSpeed && railwayB.speedKmh == 33);
    assert(railwayB.hasKilometer && std::abs(railwayB.kilometer - 1125.0) < 0.001);
    std::cout << "Test B passed\n";

    // Test C.
    const RailwayFields railwayC = ParseRailwayNumeric(DecodePocsagNumeric(
        HexBits("89 00 83 3C 13 88 4A 40", 60)));
    assert(railwayC.valid && railwayC.train == "19001");
    assert(railwayC.hasSpeed && railwayC.speedKmh == 38);
    assert(railwayC.hasKilometer && std::abs(railwayC.kilometer - 1125.2) < 0.001);
    std::cout << "Test C passed\n";

    // Test D: two updates for the same train remain two independent records.
    const std::vector<RailwayFields> continuousRecords{ railwayB, railwayC };
    assert(continuousRecords.size() == 2);
    assert(continuousRecords[0].train == continuousRecords[1].train);
    assert(continuousRecords[0].speedKmh == 33 && continuousRecords[1].speedKmh == 38);
    std::cout << "Test D passed\n";

    // Test E: dashes are valid placeholders, not garbled text.
    const RailwayFields railwayE = ParseRailwayNumeric("----- --- -----");
    assert(railwayE.valid && railwayE.train.empty());
    assert(!railwayE.hasSpeed && !railwayE.hasKilometer);
    std::cout << "Test E passed\n";

    // Test F: a train can coexist with unknown speed and kilometer fields.
    const RailwayFields railwayF = ParseRailwayNumeric("86813 --- -----");
    assert(railwayF.valid && railwayF.train == "86813");
    assert(!railwayF.hasSpeed && !railwayF.hasKilometer);
    std::cout << "Test F passed\n";

    // EXT-1: reverse every nibble independently, never the full byte.
    assert(ReverseBitsPerNibble("0123456789ABCDEF") == "084C2A6E195D3B7F");
    std::cout << "Test EXT-1 passed\n";

    // EXT-2: complete real railway extension for S6964.
    const std::string normalizedExt =
        "20533460701130B3A4D6EACCB6CFDF11256080127521574000";
    const std::string transmittedExtHex = ReverseBitsPerNibble(normalizedExt);
    const std::string extBits = BitsText(HexBits(transmittedExtHex, 200));
    const RailwayExtFields railwayExt = DecodeRailwayExt(extBits, 10, 0, false);
    assert(railwayExt.valid && railwayExt.complete && !railwayExt.truncated);
    assert(railwayExt.normalizedHex == normalizedExt);
    assert(railwayExt.trainPrefix == "S");
    assert(railwayExt.locomotiveId == "34607011");
    assert(railwayExt.locomotiveTypeCode == "346");
    assert(railwayExt.locomotiveSerial == "07011");
    assert(railwayExt.locomotiveEnd == "0");
    assert(railwayExt.lineName == "长株潭线");
    assert(railwayExt.longitudeRaw == "112560801");
    assert(railwayExt.latitudeRaw == "27521574");
    assert(railwayExt.longitudeDegreeMinute == "112°56.0801′ E");
    assert(railwayExt.latitudeDegreeMinute == "27°52.1574′ N");
    assert(std::abs(railwayExt.longitudeDeg - 112.934668333333) < 0.000000001);
    assert(std::abs(railwayExt.latitudeDeg - 27.86929) < 0.000000001);
    assert(railwayExt.railwayAuxRaw == "000");
    assert(railwayExt.confidence == "HIGH");
    std::cout << "Test EXT-2 passed\n";

    // Explicit field example: dddmm.mmmm / ddmm.mmmm, not raw / 1e6.
    const RailwayCoordinate exampleLongitude = ParseRailwayLongitudeNmea("112546296");
    const RailwayCoordinate exampleLatitude = ParseRailwayLatitudeNmea("27527550");
    assert(exampleLongitude.valid && exampleLatitude.valid);
    assert(exampleLongitude.degreeMinute == "112°54.6296′ E");
    assert(exampleLatitude.degreeMinute == "27°52.7550′ N");
    assert(exampleLongitude.minutes < 60.0 && exampleLatitude.minutes < 60.0);
    assert(std::abs(exampleLongitude.decimalDegree - 112.910493333333) < 0.000000001);
    assert(std::abs(exampleLatitude.decimalDegree - 27.87925) < 0.000000001);
    assert(!ParseRailwayLongitudeNmea("112606296").valid);
    assert(!ParseRailwayLatitudeNmea("27607550").valid);
    std::cout << "Test EXT NMEA coordinate parsing passed\n";

    // A 9-CW extension is debug-visible but never a valid combined record.
    const RailwayExtFields truncatedExt = DecodeRailwayExt(extBits.substr(0, 180), 9, 0, false);
    assert(!truncatedExt.valid && truncatedExt.truncated);
    assert(truncatedExt.confidence == "LOW");
    std::cout << "Test EXT truncated passed\n";

    // An uncorrectable CW always prevents formal extension validity.
    const RailwayExtFields damagedExt = DecodeRailwayExt(extBits, 10, 0, true);
    assert(!damagedExt.valid && damagedExt.failureReason.find("uncorrectable_codeword") != std::string::npos);
    std::cout << "Test EXT uncorrectable passed\n";

    // Invalid GBK and invalid decimal coordinates are rejected without losing RAW.
    std::string invalidGbkHex = normalizedExt;
    invalidGbkHex.replace(14, 16, "8130000000000000");
    const RailwayExtFields invalidGbk = DecodeRailwayExt(
        BitsText(HexBits(ReverseBitsPerNibble(invalidGbkHex), 200)), 10, 0, false);
    assert(!invalidGbk.valid && !invalidGbk.lineNameValid);
    assert(invalidGbk.lineNameRawHex == "8130000000000000");
    std::cout << "Test EXT invalid GBK passed\n";

    std::string invalidCoordinateHex = normalizedExt;
    invalidCoordinateHex[30] = 'A';
    const RailwayExtFields invalidCoordinate = DecodeRailwayExt(
        BitsText(HexBits(ReverseBitsPerNibble(invalidCoordinateHex), 200)), 10, 0, false);
    assert(!invalidCoordinate.valid && !invalidCoordinate.longitudeValid);
    assert(invalidCoordinate.railwayAuxRaw == "000");
    std::cout << "Test EXT invalid coordinate and aux preservation passed\n";

    // Same-transmission pairing must not depend on the POCSAG Function value.
    PocsagMessage basicMessage;
    basicMessage.transmissionId = 42;
    basicMessage.address = 1234000;
    basicMessage.function = 3;
    basicMessage.railwayValid = true;
    basicMessage.decodedTrain = railwayA.train;
    basicMessage.hasDecodedSpeed = railwayA.hasSpeed;
    basicMessage.decodedSpeedKmh = railwayA.speedKmh;
    basicMessage.hasDecodedKilometer = railwayA.hasKilometer;
    basicMessage.decodedKilometer = railwayA.kilometer;
    basicMessage.decodeConfidence = "HIGH";

    PocsagMessage extMessage;
    extMessage.transmissionId = 42;
    extMessage.address = 1234002;
    extMessage.function = 1;
    extMessage.railwayExtValid = railwayExt.valid;
    extMessage.trainPrefix = railwayExt.trainPrefix;
    extMessage.locomotiveId = railwayExt.locomotiveId;
    extMessage.locomotiveEnd = railwayExt.locomotiveEnd;
    extMessage.lineName = railwayExt.lineName;
    extMessage.longitudeValid = railwayExt.longitudeValid;
    extMessage.longitudeDeg = railwayExt.longitudeDeg;
    extMessage.latitudeValid = railwayExt.latitudeValid;
    extMessage.latitudeDeg = railwayExt.latitudeDeg;
    extMessage.railwayAuxRaw = railwayExt.railwayAuxRaw;
    extMessage.railwayExtConfidence = railwayExt.confidence;
    const auto combined = RailwayMessageAssembler::Assemble(42, 123456789,
                                                             { basicMessage, extMessage });
    assert(combined.size() == 1);
    assert(combined[0].fullTrainNumber == "S6964");
    assert(combined[0].locomotiveId == "34607011");
    assert(combined[0].lineName == "长株潭线");
    assert(combined[0].pairingMethod == "TRANSMISSION_ID");
    assert(combined[0].hasBasic && combined[0].hasExtension);
    assert(combined[0].dataCompleteness == "FULL");
    assert(combined[0].confidence == "HIGH");
    std::cout << "Test railway merge passed\n";

    // P1: a valid BASIC becomes a current target without waiting for EXT.
    const auto basicOnly = RailwayMessageAssembler::Assemble(100, 100000, { basicMessage });
    assert(basicOnly.size() == 1 && basicOnly[0].hasBasic && !basicOnly[0].hasExtension);
    RailwayStateManager state;
    state.Apply(basicOnly[0]);
    auto targets = state.Snapshot(100000);
    assert(targets.size() == 1 && targets[0].dataCompleteness == "BASIC ONLY");
    assert(targets[0].trainNumber == "6964" && !targets[0].hasPosition);
    std::cout << "Test P1 passed\n";

    // P2: a matching EXT 500 ms later enriches the same row and upgrades its ID.
    extMessage.transmissionId = 101;
    const auto extOnly = RailwayMessageAssembler::Assemble(101, 100500, { extMessage });
    assert(extOnly.size() == 1 && !extOnly[0].hasBasic && extOnly[0].hasExtension);
    state.Apply(extOnly[0]);
    targets = state.Snapshot(100500);
    assert(targets.size() == 1 && targets[0].dataCompleteness == "FULL");
    assert(targets[0].targetId == "S6964" && targets[0].locomotiveId == "34607011");
    assert(targets[0].pairingMethod == "SCORED_FALLBACK");
    std::cout << "Test P2 passed\n";

    // P3: an EXT-only target is immediately map eligible and identified by locomotive/end.
    RailwayStateManager extOnlyState;
    extOnlyState.Apply(extOnly[0]);
    auto extTargets = extOnlyState.Snapshot(100500);
    assert(extTargets.size() == 1 && extTargets[0].dataCompleteness == "EXT ONLY");
    assert(extTargets[0].fullTrainNumber.empty() && extTargets[0].hasPosition);
    assert(extTargets[0].targetUid != 0 && extTargets[0].targetId == "LOCO:34607011");
    std::cout << "Test P3 passed\n";

    // P4: dashed BASIC placeholders are valid but do not invent train/speed/km fields.
    PocsagMessage placeholder = basicMessage;
    placeholder.decodedTrain.clear();
    placeholder.hasDecodedSpeed = false;
    placeholder.hasDecodedKilometer = false;
    const auto placeholderPair = RailwayMessageAssembler::Assemble(102, 101000,
                                                                    { placeholder, extMessage });
    assert(placeholderPair.size() == 1);
    RailwayStateManager placeholderState;
    placeholderState.Apply(placeholderPair[0]);
    const auto placeholderTargets = placeholderState.Snapshot(101000);
    assert(placeholderTargets.size() == 1 && placeholderTargets[0].dataCompleteness == "FULL");
    assert(placeholderTargets[0].fullTrainNumber.empty() && !placeholderTargets[0].hasSpeed &&
           !placeholderTargets[0].hasKilometer && placeholderTargets[0].hasPosition);
    std::cout << "Test P4 passed\n";

    // P5: invalid BASIC never reaches the state layer even when text fields look plausible.
    PocsagMessage rejectedBasic = basicMessage;
    rejectedBasic.railwayValid = false;
    assert(RailwayMessageAssembler::Assemble(103, 102000, { rejectedBasic }).empty());
    assert(IsStrictRailwayBasic("19028  70 31015", 3, false));
    assert(!IsStrictRailwayBasic("19028  70 31015", 2, false));
    assert(!IsStrictRailwayBasic("19028  70 31015", 3, true));
    assert(!IsStrictRailwayBasic("19028  70 31015X", 3, false));
    std::cout << "Test P5 passed\n";

    // P6: repeated updates remain one target and append a bounded trajectory.
    RailwayStateManager repeatedState;
    for (int i = 0; i < 10; ++i)
    {
        auto update = combined[0];
        update.timestampUnixMs = 200000 + static_cast<std::uint64_t>(i * 1000);
        update.longitudeDeg += i * 0.0001;
        update.latitudeDeg += i * 0.0001;
        repeatedState.Apply(update);
    }
    const auto repeatedTargets = repeatedState.Snapshot(209000);
    assert(repeatedTargets.size() == 1 && repeatedTargets[0].targetId == "S6964");
    assert(repeatedTargets[0].track.size() == 10);
    std::cout << "Test P6 passed\n";

    // P7: targets become stale after 120 s and disappear after 300 s.
    assert(!repeatedState.Snapshot(209000 + RailwayStateManager::kStaleAfterMs).front().stale);
    assert(repeatedState.Snapshot(209001 + RailwayStateManager::kStaleAfterMs).front().stale);
    assert(repeatedState.Snapshot(209001 + RailwayStateManager::kRemoveAfterMs).empty());
    std::cout << "Test P7 passed\n";

    // M1: EXT first and BASIC second in the same transmission share one UID.
    RailwayStateManager m1;
    auto m1Ext = extOnly[0];
    m1Ext.transmissionId = 500;
    m1Ext.timestampUnixMs = 300000;
    m1.Apply(m1Ext);
    const auto m1Uid = m1.Snapshot(300000).front().targetUid;
    auto m1Basic = basicOnly[0];
    m1Basic.transmissionId = 500;
    m1Basic.timestampUnixMs = 300100;
    m1.Apply(m1Basic);
    auto m1Targets = m1.Snapshot(300100);
    assert(m1Targets.size() == 1 && m1Targets[0].targetUid == m1Uid &&
           m1Targets[0].hasBasic && m1Targets[0].hasExtension);
    std::cout << "Test M1 passed\n";

    // M2: BASIC first and a short-window EXT fallback enrich the same UID.
    RailwayStateManager m2;
    auto m2Basic = m1Basic;
    m2Basic.transmissionId = 600;
    m2Basic.timestampUnixMs = 310000;
    m2.Apply(m2Basic);
    const auto m2Uid = m2.Snapshot(310000).front().targetUid;
    auto m2Ext = m1Ext;
    m2Ext.transmissionId = 601;
    m2Ext.timestampUnixMs = 310500;
    m2.Apply(m2Ext);
    auto m2Targets = m2.Snapshot(310500);
    assert(m2Targets.size() == 1 && m2Targets[0].targetUid == m2Uid &&
           m2Targets[0].dataCompleteness == "FULL");
    std::cout << "Test M2 passed\n";

    // M3: ten BASIC broadcasts for one train update one target.
    RailwayStateManager m3;
    for (int i = 0; i < 10; ++i)
    {
        auto update = m2Basic;
        update.transmissionId = 700 + i;
        update.timestampUnixMs = 320000 + i * 1000;
        update.speedKmh = 70 + i;
        m3.Apply(update);
    }
    assert(m3.Snapshot(329000).size() == 1);
    std::cout << "Test M3 passed\n";

    // M4: ten EXT broadcasts for one locomotive update one target.
    RailwayStateManager m4;
    for (int i = 0; i < 10; ++i)
    {
        auto update = m2Ext;
        update.transmissionId = 800 + i;
        update.timestampUnixMs = 330000 + i * 1000;
        update.longitudeDeg += i * 0.0001;
        m4.Apply(update);
    }
    assert(m4.Snapshot(339000).size() == 1);
    std::cout << "Test M4 passed\n";

    // M5: locomotive end changes are history on one locomotive target.
    RailwayStateManager m5;
    auto endOne = m2Ext;
    endOne.transmissionId = 900;
    endOne.timestampUnixMs = 340000;
    endOne.locomotiveEnd = "1";
    m5.Apply(endOne);
    const auto m5Uid = m5.Snapshot(340000).front().targetUid;
    auto endTwo = endOne;
    endTwo.transmissionId = 901;
    endTwo.timestampUnixMs = 341000;
    endTwo.locomotiveEnd = "2";
    m5.Apply(endTwo);
    auto m5Targets = m5.Snapshot(341000);
    assert(m5Targets.size() == 1 && m5Targets[0].targetUid == m5Uid &&
           m5Targets[0].locomotiveEnd == "2" &&
           m5Targets[0].locomotiveEndHistory.size() == 2);
    std::cout << "Test M5 passed\n";

    // A FULL update that bridges separate train and locomotive objects merges both.
    RailwayStateManager bridge;
    auto bridgeBasic = m2Basic;
    bridgeBasic.transmissionId = 1000;
    bridgeBasic.timestampUnixMs = 350000;
    bridge.Apply(bridgeBasic);
    auto bridgeExt = m2Ext;
    bridgeExt.transmissionId = 1001;
    bridgeExt.timestampUnixMs = 360000; // Outside fallback: deliberately creates EXT_ONLY.
    bridge.Apply(bridgeExt);
    assert(bridge.Snapshot(360000).size() == 2);
    auto bridgeFull = combined[0];
    bridgeFull.transmissionId = 1002;
    bridgeFull.timestampUnixMs = 361000;
    bridge.Apply(bridgeFull);
    const auto bridged = bridge.Snapshot(361000);
    assert(bridged.size() == 1 && bridged[0].hasBasic && bridged[0].hasExtension &&
           bridge.TotalMergeCount() == 1);
    std::cout << "Test identity bridge merge passed\n";

    // KM1/KM2: same line+mileage aggregates rather than creating duplicate anchors.
    const auto mileagePath = std::filesystem::temp_directory_path() /
        "pocsag_mileage_position_tests.json";
    std::error_code mileageError;
    std::filesystem::remove(mileagePath, mileageError);
    MileagePositionDatabase mileageDb(mileagePath.wstring());
    assert(mileageDb.AddSample("京广线", 1130.1, 27.879250, 112.910493, 1000));
    for (int i = 1; i < 20; ++i)
        assert(mileageDb.AddSample("京广线", 1130.1,
            27.879250 + i * 0.000001, 112.910493 - i * 0.000001, 1000 + i));
    auto anchors = mileageDb.Anchors();
    assert(anchors.size() == 1 && anchors[0].sampleCount == 20);
    std::cout << "Test KM1/KM2 passed\n";

    // KM3: interpolation only occurs between two nearby anchors.
    MileagePositionDatabase interpolationDb;
    assert(!interpolationDb.AddSample("京广线", 1129.8, 27.870000, 112.900000, 2000));
    assert(!interpolationDb.AddSample("京广线", 1130.4, 27.876000, 112.906000, 2000));
    const auto km3 = interpolationDb.Lookup("京广线", 1130.1);
    assert(km3.valid && km3.source == PositionSource::LocalMileageInterpolated);
    assert(std::abs(km3.latitude - 27.873000) < 1e-8);
    std::cout << "Test KM3 passed\n";

    // KM4: a 30 km bracket is not reliable.
    MileagePositionDatabase wideGap;
    assert(!wideGap.AddSample("京广线", 1100.0, 27.0, 112.0, 1)); // no path still updates memory
    assert(!wideGap.AddSample("京广线", 1130.0, 28.0, 113.0, 2));
    const auto km4 = wideGap.Lookup("京广线", 1115.0);
    assert(!km4.valid && km4.reason == "INTERPOLATION_GAP_TOO_LARGE");
    std::cout << "Test KM4 passed\n";

    // KM5/KM6: cross-validation never rewrites radio GPS.
    MileagePositionEstimate comparisonAnchor;
    comparisonAnchor.valid = true;
    comparisonAnchor.latitude = 27.879250;
    comparisonAnchor.longitude = 112.910493;
    comparisonAnchor.source = PositionSource::LocalMileageExact;
    double comparisonDistance = 0.0;
    assert(mileageDb.Compare(27.8792501, 112.9104931, comparisonAnchor,
                             &comparisonDistance) == PositionComparison::Match);
    assert(mileageDb.Compare(27.890050, 112.910493, comparisonAnchor,
                             &comparisonDistance) == PositionComparison::Mismatch);
    std::cout << "Test KM5/KM6 passed\n";

    // KM7: mileage without a line is not a coordinate.
    const auto km7 = mileageDb.Lookup("", 1130.1);
    assert(!km7.valid && km7.reason == "LINE_OR_MILEAGE_MISSING");
    std::cout << "Test KM7 passed\n";

    // Geographic range checks are global WGS84 bounds, not a China-only box.
    std::string outOfRangeHex = normalizedExt;
    outOfRangeHex.replace(30, 9, "999000000");
    const RailwayExtFields outOfRange = DecodeRailwayExt(
        BitsText(HexBits(ReverseBitsPerNibble(outOfRangeHex), 200)), 10, 0, false);
    assert(!outOfRange.valid && !outOfRange.longitudeValid);
    std::cout << "Test EXT coordinate range passed\n";

    std::vector<PocsagMessage> decoded;
    std::vector<PocsagTransmission> transmissions;
    PocsagDecoder decoder([&](const PocsagMessage& message) { decoded.push_back(message); },
                          [&](const PocsagTransmission& transmission) { transmissions.push_back(transmission); });
    decoder.SetBaud(1200);
    decoder.SetSampleRate(48000.0);

    const std::uint32_t address = 123456;
    const int frame = static_cast<int>(address & 7u);
    const auto payload = NumericBits("6934 103 465");

    std::vector<std::uint32_t> batch(16, kIdleWord);
    int index = frame * 2;
    batch[static_cast<std::size_t>(index++)] = EncodeAddress(address, 0);
    for (std::size_t offset = 0; offset < payload.size() && index < 16; offset += 20)
        batch[static_cast<std::size_t>(index++)] = EncodeMessageWord(payload, offset);

    std::vector<bool> bits;
    for (int i = 0; i < 576; ++i)
        bits.push_back((i & 1) == 0);
    AppendWord(bits, kSyncWord);
    for (std::uint32_t word : batch)
        AppendWord(bits, word);
    AppendWord(bits, kSyncWord);

    std::vector<float> audio;
    audio.reserve(bits.size() * 40);
    for (bool bit : bits)
        for (int sample = 0; sample < 40; ++sample)
            audio.push_back(bit ? 0.8f : -0.8f);

    decoder.ProcessAudio(audio.data(), static_cast<int>(audio.size()));
    decoder.Reset();

    assert(decoded.size() == 1);
    assert(decoded[0].address == address);
    assert(decoded[0].type == "UNSET");
    assert(decoded[0].text.find("RAW:") != std::string::npos);
    assert(decoded[0].numericText.find("6934 103 465") == 0);
    assert(!decoded[0].rawHex.empty());
    assert(decoded[0].messageCodewordCount == 3);
    assert(decoded[0].finalizeReason == "IDLE");
    assert(!decoded[0].messageBits.empty());
    assert(transmissions.size() == 1);
    assert(decoded[0].transmissionId != 0);
    assert(decoded[0].transmissionId == transmissions[0].transmissionId);
    assert(!transmissions[0].codewords.empty());
    assert(transmissions[0].messages.size() == 1);
    assert(transmissions[0].endReason == "BURST_END");

    std::vector<PocsagMessage> invertedDecoded;
    PocsagDecoder invertedDecoder(
        [&](const PocsagMessage& message) { invertedDecoded.push_back(message); });
    invertedDecoder.SetBaud(1200);
    invertedDecoder.SetSampleRate(48000.0);
    invertedDecoder.SetPolarity(PocsagPolarity::Inverted);
    std::vector<float> invertedAudio = audio;
    for (float& sample : invertedAudio)
        sample = -sample;
    invertedDecoder.ProcessAudio(invertedAudio.data(), static_cast<int>(invertedAudio.size()));
    invertedDecoder.Reset();
    assert(invertedDecoded.size() == 1);
    assert(invertedDecoded[0].numericText.find("6934 103 465") == 0);

    std::vector<PocsagMessage> toneDecoded;
    PocsagDecoder toneDecoder([&](const PocsagMessage& message) { toneDecoded.push_back(message); });
    toneDecoder.SetBaud(1200);
    toneDecoder.SetSampleRate(48000.0);
    std::vector<std::uint32_t> toneBatch(16, kIdleWord);
    toneBatch[static_cast<std::size_t>(frame * 2)] = EncodeAddress(address, 1);
    std::vector<bool> toneBits;
    for (int i = 0; i < 576; ++i)
        toneBits.push_back((i & 1) == 0);
    AppendWord(toneBits, kSyncWord);
    for (std::uint32_t word : toneBatch)
        AppendWord(toneBits, word);
    std::vector<float> toneAudio;
    for (bool bit : toneBits)
        for (int sample = 0; sample < 40; ++sample)
            toneAudio.push_back(bit ? 0.7f : -0.7f);
    toneDecoder.ProcessAudio(toneAudio.data(), static_cast<int>(toneAudio.size()));
    toneDecoder.Reset();
    assert(toneDecoded.size() == 1);
    assert(toneDecoded[0].address == address);
    assert(toneDecoded[0].type == "TONE");
    assert(toneDecoded[0].messageCodewordCount == 0);
    assert(toneDecoded[0].finalizeReason == "IDLE");

    // A message may continue through the next batch sync. SYNC must not
    // finalize it: an address at frame 7 leaves one message word in batch 0.
    const std::uint32_t crossAddress = 123463; // frame 7
    std::vector<PocsagMessage> crossDecoded;
    PocsagDecoder crossDecoder([&](const PocsagMessage& message) { crossDecoded.push_back(message); });
    crossDecoder.SetBaud(1200);
    crossDecoder.SetSampleRate(48000.0);
    std::vector<std::uint32_t> crossBatch0(16, kIdleWord);
    std::vector<std::uint32_t> crossBatch1(16, kIdleWord);
    crossBatch0[14] = EncodeAddress(crossAddress, 1);
    crossBatch0[15] = EncodeMessageWord(payload, 0);
    crossBatch1[0] = EncodeMessageWord(payload, 20);
    crossBatch1[1] = EncodeMessageWord(payload, 40);
    std::vector<bool> crossBits;
    for (int i = 0; i < 576; ++i)
        crossBits.push_back((i & 1) == 0);
    AppendWord(crossBits, kSyncWord);
    for (auto word : crossBatch0)
        AppendWord(crossBits, word);
    AppendWord(crossBits, kSyncWord);
    for (auto word : crossBatch1)
        AppendWord(crossBits, word);
    std::vector<float> crossAudio;
    for (bool bit : crossBits)
        for (int sample = 0; sample < 40; ++sample)
            crossAudio.push_back(bit ? 0.8f : -0.8f);
    crossDecoder.ProcessAudio(crossAudio.data(), static_cast<int>(crossAudio.size()));
    crossDecoder.Reset();
    assert(crossDecoded.size() == 1);
    assert(crossDecoded[0].address == crossAddress);
    assert(crossDecoded[0].messageCodewordCount == 3);
    assert(crossDecoded[0].numericText.find("6934 103 465") == 0);
    assert(crossDecoded[0].finalizeReason == "IDLE");

    // One uncorrectable message codeword must mark the active message as
    // damaged, not prematurely emit a false TONE.
    std::vector<PocsagMessage> damagedDecoded;
    PocsagDecoder damagedDecoder([&](const PocsagMessage& message) { damagedDecoded.push_back(message); });
    damagedDecoder.SetBaud(1200);
    damagedDecoder.SetSampleRate(48000.0);
    std::vector<std::uint32_t> damagedBatch(16, kIdleWord);
    damagedBatch[0] = EncodeAddress(123456, 2);
    damagedBatch[1] = EncodeMessageWord(payload, 0) ^ 0x7u;
    damagedBatch[2] = EncodeMessageWord(payload, 20);
    damagedBatch[3] = EncodeMessageWord(payload, 40);
    std::vector<bool> damagedBits;
    for (int i = 0; i < 576; ++i)
        damagedBits.push_back((i & 1) == 0);
    AppendWord(damagedBits, kSyncWord);
    for (auto word : damagedBatch)
        AppendWord(damagedBits, word);
    std::vector<float> damagedAudio;
    for (bool bit : damagedBits)
        for (int sample = 0; sample < 40; ++sample)
            damagedAudio.push_back(bit ? 0.8f : -0.8f);
    damagedDecoder.ProcessAudio(damagedAudio.data(), static_cast<int>(damagedAudio.size()));
    damagedDecoder.Reset();
    assert(damagedDecoded.size() == 1);
    assert(damagedDecoded[0].type == "UNSET");
    assert(damagedDecoded[0].messageCodewordCount == 2);
    assert(damagedDecoded[0].hasUncorrectableCodeword);
    assert(damagedDecoded[0].finalizeReason == "IDLE");

    if (argc > 1)
    {
        std::ifstream wav(argv[1], std::ios::binary);
        assert(wav);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(wav)), {});
        assert(bytes.size() > 44);
        const auto read16 = [&](std::size_t offset) {
            return static_cast<unsigned>(bytes[offset]) |
                   (static_cast<unsigned>(bytes[offset + 1]) << 8);
        };
        const auto read32 = [&](std::size_t offset) {
            return read16(offset) | (read16(offset + 2) << 16);
        };
        assert(std::string(reinterpret_cast<const char*>(bytes.data()), 4) == "RIFF");
        const unsigned channels = read16(22);
        const unsigned sampleRate = read32(24);
        const unsigned bitsPerSample = read16(34);
        assert(channels >= 1 && bitsPerSample == 16);
        std::size_t dataOffset = 12;
        while (dataOffset + 8 <= bytes.size() &&
               std::string(reinterpret_cast<const char*>(bytes.data() + dataOffset), 4) != "data")
            dataOffset += 8 + read32(dataOffset + 4) + (read32(dataOffset + 4) & 1u);
        assert(dataOffset + 8 <= bytes.size());
        const std::size_t dataSize = std::min<std::size_t>(read32(dataOffset + 4), bytes.size() - dataOffset - 8);
        const auto* pcm = reinterpret_cast<const std::int16_t*>(bytes.data() + dataOffset + 8);
        const std::size_t frames = dataSize / (channels * sizeof(std::int16_t));
        std::vector<float> mono;
        mono.reserve(frames);
        for (std::size_t frameIndex = 0; frameIndex < frames; ++frameIndex)
            mono.push_back(static_cast<float>(pcm[frameIndex * channels]) / 32768.0f);

        std::vector<PocsagMessage> fieldDecoded;
        PocsagDecoder fieldDecoder([&](const PocsagMessage& message) { fieldDecoded.push_back(message); });
        fieldDecoder.SetBaud(1200);
        fieldDecoder.SetSampleRate(sampleRate);
        fieldDecoder.ProcessAudio(mono.data(), static_cast<int>(mono.size()));
        const auto stats = fieldDecoder.GetStats();
        fieldDecoder.Reset();
        std::cout << "Field replay: sync=" << stats.syncs
                  << " valid=" << stats.validWords
                  << " invalid=" << stats.invalidWords
                  << " messages=" << fieldDecoded.size() << "\n";
        for (const auto& message : fieldDecoded)
            std::cout << "  " << message.address << " " << message.type << " " << message.text << "\n";
        assert(stats.syncs >= 3);
        assert(stats.validWords >= 40);
        assert(!fieldDecoded.empty());
    }
    std::cout << "Decoder test passed: " << decoded[0].address << " " << decoded[0].numericText << "\n";
    return 0;
}
