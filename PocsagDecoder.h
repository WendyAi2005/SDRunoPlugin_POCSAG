#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct RailwayFields
{
    bool valid = false;
    std::string train;
    bool hasSpeed = false;
    int speedKmh = 0;
    bool hasKilometer = false;
    double kilometer = 0.0;
};

// Final interpretation helpers. They deliberately do not participate in
// synchronization, BCH correction, RIC extraction or message aggregation.
std::string DecodePocsagNumeric(const std::vector<bool>& bits);
RailwayFields ParseRailwayNumeric(const std::string& numericText);
bool IsStrictRailwayBasic(const std::string& numericText, int messageCodewordCount,
                          bool hasUncorrectableCodeword);

enum class PocsagPolarity
{
    Auto,
    Normal,
    Inverted
};

struct PocsagMessage
{
    std::uint64_t transmissionId = 0;
    std::uint32_t address = 0;
    int function = 0;
    std::string type;
    std::string text;
    std::string numericText;
    std::string alphaText;
    std::string rawHex;
    int correctedBits = 0;
    int messageCodewordCount = 0;
    bool hasUncorrectableCodeword = false;
    std::string finalizeReason;
    std::string messageBits;
    std::vector<std::uint32_t> rawCodewords;
    std::vector<std::uint32_t> correctedCodewords;
    bool railwayValid = false;
    std::string decodedTrain;
    bool hasDecodedSpeed = false;
    int decodedSpeedKmh = 0;
    bool hasDecodedKilometer = false;
    double decodedKilometer = 0.0;
    std::string decodeConfidence;
    bool railwayExtValid = false;
    bool railwayExtTruncated = false;
    std::string railwayExtNormalizedHex;
    std::string trainPrefix;
    std::string locomotiveId;
    std::string locomotiveTypeCode;
    std::string locomotiveSerial;
    std::string locomotiveEnd;
    std::string lineName;
    std::string lineNameRawHex;
    std::string longitudeRaw;
    std::string longitudeDegreeMinute;
    bool longitudeValid = false;
    double longitudeDeg = 0.0;
    std::string latitudeRaw;
    std::string latitudeDegreeMinute;
    bool latitudeValid = false;
    double latitudeDeg = 0.0;
    std::string railwayAuxRaw;
    std::string railwayExtConfidence;
    std::string railwayExtFailureReason;
};

struct PocsagCodewordRecord
{
    std::uint32_t raw = 0;
    std::uint32_t corrected = 0;
    int correctedBits = 0; // -1 means uncorrectable.
    int batch = 0;
    int index = -1;
    std::string classification;
};

struct PocsagTransmission
{
    std::uint64_t transmissionId = 0;
    std::uint64_t startedUnixMs = 0;
    int baud = 1200;
    bool inverted = false;
    std::string endReason;
    std::vector<PocsagCodewordRecord> codewords;
    std::vector<PocsagMessage> messages;
};

struct PocsagDecoderStats
{
    double inputPeak = 0.0;
    std::uint64_t samples = 0;
    std::uint64_t transitions = 0;
    std::uint64_t syncs = 0;
    std::uint64_t validWords = 0;
    std::uint64_t invalidWords = 0;
};

class PocsagDecoder
{
public:
    using MessageHandler = std::function<void(const PocsagMessage&)>;
    using TransmissionHandler = std::function<void(const PocsagTransmission&)>;

    explicit PocsagDecoder(MessageHandler handler, TransmissionHandler transmissionHandler = {});

    void Reset();
    void SetBaud(int baud);
    void SetSampleRate(double sampleRate);
    void SetPolarity(PocsagPolarity polarity);
    PocsagPolarity GetPolarity() const { return polarity_; }
    void ProcessAudio(const float* samples, int length);
    PocsagDecoderStats GetStats() const { return stats_; }

private:
    void ProcessBit(bool bit);
    void ProcessBatchWord(std::uint32_t word, int index);
    void FinalizeMessage(const char* reason);
    void StartTransmission(std::uint32_t rawSync);
    void FinalizeTransmission(const char* reason);
    void RecordCodeword(std::uint32_t raw, std::uint32_t corrected, int correctedBits,
                        int index, const char* classification);

    static int HammingDistance(std::uint32_t a, std::uint32_t b);
    static bool IsValidCodeword(std::uint32_t word);
    static bool CorrectCodeword(std::uint32_t& word, int& correctedBits);
    static std::string DecodeAlpha(const std::vector<bool>& bits);
    static std::string EncodeRawHex(const std::vector<bool>& bits);
    static std::string EncodeBits(const std::vector<bool>& bits);
    static std::string TrimMessage(std::string value);

    MessageHandler handler_;
    TransmissionHandler transmissionHandler_;
    int baud_ = 1200;
    double sampleRate_ = 48000.0;

    double dc_ = 0.0;
    double filtered_ = 0.0;
    double envelope_ = 0.001;
    double phase_ = 0.0;
    bool lastLevel_ = false;
    bool haveLevel_ = false;
    int samplesSinceEdge_ = 0;
    int preambleEdgeCount_ = 0;

    std::uint32_t searchRegister_ = 0;
    std::uint64_t preambleTransitions_ = 0;
    int preambleBits_ = 0;
    int preambleWindow_ = 0;
    bool previousBit_ = false;
    bool havePreviousBit_ = false;

    bool synchronized_ = false;
    bool inverted_ = false;
    PocsagPolarity polarity_ = PocsagPolarity::Auto;
    bool expectingSync_ = false;
    std::uint32_t currentWord_ = 0;
    int currentWordBits_ = 0;
    int batchWordIndex_ = 0;
    int batchIndex_ = 0;

    bool messageActive_ = false;
    std::uint32_t messageAddress_ = 0;
    int messageFunction_ = 0;
    int messageCorrectedBits_ = 0;
    int messageCodewordCount_ = 0;
    bool messageHasUncorrectable_ = false;
    std::vector<bool> messageBits_;
    std::vector<std::uint32_t> messageRawCodewords_;
    std::vector<std::uint32_t> messageCorrectedCodewords_;
    PocsagTransmission transmission_;
    bool transmissionActive_ = false;
    std::uint64_t nextTransmissionId_ = 0;
    PocsagDecoderStats stats_;
};
