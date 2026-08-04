#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PocsagMessage
{
    std::uint32_t address = 0;
    int function = 0;
    std::string type;
    std::string text;
    int correctedBits = 0;
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

    explicit PocsagDecoder(MessageHandler handler);

    void Reset();
    void SetBaud(int baud);
    void SetSampleRate(double sampleRate);
    void ProcessAudio(const float* samples, int length);
    PocsagDecoderStats GetStats() const { return stats_; }

private:
    void ProcessBit(bool bit);
    void ProcessBatchWord(std::uint32_t word, int index);
    void FinalizeMessage();

    static int HammingDistance(std::uint32_t a, std::uint32_t b);
    static bool IsValidCodeword(std::uint32_t word);
    static bool CorrectCodeword(std::uint32_t& word, int& correctedBits);
    static std::string DecodeNumeric(const std::vector<bool>& bits);
    static std::string DecodeAlpha(const std::vector<bool>& bits);
    static std::string TrimMessage(std::string value);

    MessageHandler handler_;
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
    bool expectingSync_ = false;
    std::uint32_t currentWord_ = 0;
    int currentWordBits_ = 0;
    int batchWordIndex_ = 0;

    bool messageActive_ = false;
    std::uint32_t messageAddress_ = 0;
    int messageFunction_ = 0;
    int messageCorrectedBits_ = 0;
    std::vector<bool> messageBits_;
    PocsagDecoderStats stats_;
};
