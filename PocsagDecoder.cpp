#include "PocsagDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <cctype>

namespace
{
constexpr std::uint32_t kSyncWord = 0x7CD215D8u;
constexpr std::uint32_t kIdleWord = 0x7A89C197u;
constexpr std::uint32_t kBchPolynomial = 0x769u;
constexpr int kPreambleWindowBits = 64;
constexpr int kMinimumPreambleTransitions = 54;

int PopCount64(std::uint64_t value)
{
    int count = 0;
    while (value)
    {
        value &= value - 1;
        ++count;
    }
    return count;
}

std::string TrimField(const std::string& value)
{
    const auto first = value.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(' ');
    return value.substr(first, last - first + 1);
}

bool IsRailwayFieldCharacter(char ch)
{
    return (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-';
}

bool IsDigitsOnly(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}
}

std::string DecodePocsagNumeric(const std::vector<bool>& bits)
{
    // POCSAG numeric alphabet indexed by the transmitted 4-bit nibble.
    static constexpr char kNumericAlphabet[] = "084 2.6]195-3U7[";
    std::string result;
    result.reserve(bits.size() / 4);
    for (std::size_t i = 0; i + 3 < bits.size(); i += 4)
    {
        unsigned value = 0;
        for (int bit = 0; bit < 4; ++bit)
            value = (value << 1) | (bits[i + static_cast<std::size_t>(bit)] ? 1u : 0u);
        result.push_back(kNumericAlphabet[value & 0x0Fu]);
    }
    return result;
}

RailwayFields ParseRailwayNumeric(const std::string& numericText)
{
    RailwayFields result;
    if (numericText.size() < 15 || numericText[5] != ' ' || numericText[9] != ' ')
        return result;

    const std::string layout = numericText.substr(0, 15);
    if (!std::all_of(layout.begin(), layout.end(), IsRailwayFieldCharacter))
        return result;

    result.valid = true;
    const std::string train = TrimField(layout.substr(0, 5));
    const std::string speed = TrimField(layout.substr(6, 3));
    const std::string kilometer = TrimField(layout.substr(10, 5));

    if (IsDigitsOnly(train))
        result.train = train;
    if (IsDigitsOnly(speed))
    {
        result.hasSpeed = true;
        result.speedKmh = std::stoi(speed);
    }
    if (IsDigitsOnly(kilometer))
    {
        result.hasKilometer = true;
        result.kilometer = static_cast<double>(std::stoll(kilometer)) / 10.0;
    }
    return result;
}

bool IsStrictRailwayBasic(const std::string& numericText, int messageCodewordCount,
                          bool hasUncorrectableCodeword)
{
    return messageCodewordCount == 3 && !hasUncorrectableCodeword &&
           numericText.size() == 15 && ParseRailwayNumeric(numericText).valid;
}

PocsagDecoder::PocsagDecoder(MessageHandler handler, TransmissionHandler transmissionHandler)
    : handler_(std::move(handler)), transmissionHandler_(std::move(transmissionHandler))
{
    Reset();
}

void PocsagDecoder::Reset()
{
    FinalizeMessage("SYNC_LOSS");
    FinalizeTransmission("SYNC_LOSS");
    dc_ = 0.0;
    filtered_ = 0.0;
    envelope_ = 0.001;
    phase_ = 0.0;
    haveLevel_ = false;
    samplesSinceEdge_ = 0;
    preambleEdgeCount_ = 0;
    searchRegister_ = 0;
    preambleTransitions_ = 0;
    preambleBits_ = 0;
    preambleWindow_ = 0;
    havePreviousBit_ = false;
    synchronized_ = false;
    inverted_ = false;
    expectingSync_ = false;
    currentWord_ = 0;
    currentWordBits_ = 0;
    batchWordIndex_ = 0;
    batchIndex_ = 0;
    stats_ = {};
}

void PocsagDecoder::SetBaud(int baud)
{
    if (baud == 512 || baud == 1200 || baud == 2400)
    {
        baud_ = baud;
        Reset();
    }
}

void PocsagDecoder::SetSampleRate(double sampleRate)
{
    if (sampleRate >= 8000.0)
    {
        sampleRate_ = sampleRate;
        Reset();
    }
}

void PocsagDecoder::SetPolarity(PocsagPolarity polarity)
{
    if (polarity_ == polarity)
        return;
    polarity_ = polarity;
    Reset();
}

void PocsagDecoder::ProcessAudio(const float* samples, int length)
{
    if (!samples || length <= 0 || sampleRate_ <= 0.0)
        return;

    const double phaseStep = static_cast<double>(baud_) / sampleRate_;
    // Keep the pulse edges sharp. The previous narrow one-pole filter delayed
    // and rounded the 1200-baud transitions enough to corrupt field captures.
    const double lowPassAlpha = std::min(0.45, 18.0 * static_cast<double>(baud_) / sampleRate_);
    double blockPeak = 0.0;

    for (int i = 0; i < length; ++i)
    {
        if (samplesSinceEdge_ < static_cast<int>(sampleRate_))
            ++samplesSinceEdge_;
        const double input = static_cast<double>(samples[i]);
        blockPeak = std::max(blockPeak, std::abs(input));
        dc_ += 0.0005 * (input - dc_);
        const double ac = input - dc_;
        filtered_ += lowPassAlpha * (ac - filtered_);
        envelope_ += 0.002 * (std::abs(filtered_) - envelope_);

        const bool level = filtered_ > 0.0;

        if (!haveLevel_)
        {
            lastLevel_ = level;
            haveLevel_ = true;
        }
        else if (level != lastLevel_)
        {
            ++stats_.transitions;
            const double samplesPerBit = sampleRate_ / static_cast<double>(baud_);
            const double edgeLength = static_cast<double>(samplesSinceEdge_);

            // Like PDW's soundcard slicer, use actual zero-crossing spacing
            // to identify the alternating preamble and place the sampling
            // point in the middle of the following symbol.
            if (edgeLength >= samplesPerBit * 0.70 && edgeLength <= samplesPerBit * 1.30)
                ++preambleEdgeCount_;
            else
                preambleEdgeCount_ = 0;

            // A first-order early/late digital PLL, following the proven
            // multimon-ng POCSAG demodulator. Do not hard-reset the clock on
            // every crossing: ringing and noisy crossings otherwise move the
            // sampling point out of the symbol centre.
            if (phase_ < 0.5 - phaseStep / 2.0)
                phase_ += phaseStep / 8.0;
            else
                phase_ -= phaseStep / 8.0;
            if (phase_ < 0.0)
                phase_ += 1.0;
            else if (phase_ >= 1.0)
                phase_ -= 1.0;

            if (preambleEdgeCount_ >= 40)
                preambleWindow_ = 512;

            samplesSinceEdge_ = 0;
            lastLevel_ = level;
        }

        phase_ += phaseStep;
        if (phase_ >= 1.0)
        {
            phase_ -= 1.0;
            ProcessBit(level);
        }
    }

    stats_.samples += static_cast<std::uint64_t>(length);
    stats_.inputPeak = std::max(blockPeak, stats_.inputPeak * 0.92);
}

void PocsagDecoder::ProcessBit(bool bit)
{
    searchRegister_ = (searchRegister_ << 1) | (bit ? 1u : 0u);

    const bool transition = havePreviousBit_ && bit != previousBit_;
    preambleTransitions_ = (preambleTransitions_ << 1) | (transition ? 1ull : 0ull);
    if (preambleBits_ < kPreambleWindowBits)
        ++preambleBits_;
    previousBit_ = bit;
    havePreviousBit_ = true;

    if (preambleBits_ >= kPreambleWindowBits &&
        PopCount64(preambleTransitions_) >= kMinimumPreambleTransitions)
        preambleWindow_ = 256;
    else if (preambleWindow_ > 0)
        --preambleWindow_;

    if (!synchronized_)
    {
        // A <=2-bit sync match is already highly selective, and the following
        // BCH batch provides another validation layer. Searching continuously
        // avoids rejecting a real sync merely because its own transition
        // pattern has displaced part of the alternating preamble window.
        if (polarity_ != PocsagPolarity::Inverted &&
            HammingDistance(searchRegister_, kSyncWord) <= 2)
        {
            synchronized_ = true;
            inverted_ = false;
            ++stats_.syncs;
        }
        else if (polarity_ != PocsagPolarity::Normal &&
                 HammingDistance(searchRegister_, ~kSyncWord) <= 2)
        {
            synchronized_ = true;
            inverted_ = true;
            ++stats_.syncs;
        }
        else
        {
            return;
        }

        expectingSync_ = false;
        currentWord_ = 0;
        currentWordBits_ = 0;
        batchWordIndex_ = 0;
        batchIndex_ = 0;
        StartTransmission(searchRegister_);
        return;
    }

    const bool decodedBit = inverted_ ? !bit : bit;
    currentWord_ = (currentWord_ << 1) | (decodedBit ? 1u : 0u);
    if (++currentWordBits_ < 32)
        return;

    const std::uint32_t word = currentWord_;
    currentWord_ = 0;
    currentWordBits_ = 0;

    if (expectingSync_)
    {
        if (HammingDistance(word, kSyncWord) <= 2)
        {
            RecordCodeword(inverted_ ? ~word : word, kSyncWord,
                           HammingDistance(word, kSyncWord), -1, "SYNC");
            expectingSync_ = false;
            batchWordIndex_ = 0;
            ++batchIndex_;
        }
        else
        {
            RecordCodeword(inverted_ ? ~word : word, word, -1, -1, "SYNC_MISS");
            FinalizeMessage("SYNC_LOSS");
            FinalizeTransmission("SYNC_LOSS");
            synchronized_ = false;
            preambleWindow_ = 0;
        }
        return;
    }

    ProcessBatchWord(word, batchWordIndex_++);
    if (batchWordIndex_ == 16)
        expectingSync_ = true;
}

void PocsagDecoder::ProcessBatchWord(std::uint32_t word, int index)
{
    const std::uint32_t rawWord = inverted_ ? ~word : word;
    int corrected = 0;
    if (!CorrectCodeword(word, corrected))
    {
        ++stats_.invalidWords;
        RecordCodeword(rawWord, rawWord, -1, index, "UNKNOWN");
        if (messageActive_)
            messageHasUncorrectable_ = true;
        return;
    }
    ++stats_.validWords;

    if (word == kIdleWord)
    {
        RecordCodeword(rawWord, word, corrected, index, "IDLE");
        FinalizeMessage("IDLE");
        return;
    }

    if ((word & 0x80000000u) == 0)
    {
        RecordCodeword(rawWord, word, corrected, index, "ADDRESS");
        FinalizeMessage("NEXT_ADDRESS");
        const std::uint32_t addressHigh = (word >> 13) & 0x3FFFFu;
        const std::uint32_t frame = static_cast<std::uint32_t>(index / 2);
        messageAddress_ = (addressHigh << 3) | frame;
        messageFunction_ = static_cast<int>((word >> 11) & 0x3u);
        messageCorrectedBits_ = corrected;
        messageCodewordCount_ = 0;
        messageHasUncorrectable_ = false;
        messageBits_.clear();
        messageRawCodewords_.clear();
        messageCorrectedCodewords_.clear();
        messageActive_ = true;
        return;
    }

    if (!messageActive_)
    {
        RecordCodeword(rawWord, word, corrected, index, "MESSAGE_ORPHAN");
        return;
    }

    RecordCodeword(rawWord, word, corrected, index, "MESSAGE");
    messageCorrectedBits_ += corrected;
    ++messageCodewordCount_;
    messageRawCodewords_.push_back(rawWord);
    messageCorrectedCodewords_.push_back(word);
    for (int bit = 30; bit >= 11; --bit)
        messageBits_.push_back(((word >> bit) & 1u) != 0);
}

void PocsagDecoder::FinalizeMessage(const char* reason)
{
    if (!messageActive_)
        return;

    PocsagMessage message;
    message.transmissionId = transmission_.transmissionId;
    message.address = messageAddress_;
    message.function = messageFunction_;
    message.correctedBits = messageCorrectedBits_;
    message.messageCodewordCount = messageCodewordCount_;
    message.hasUncorrectableCodeword = messageHasUncorrectable_;
    message.finalizeReason = reason ? reason : "UNKNOWN";
    message.messageBits = EncodeBits(messageBits_);
    message.rawCodewords = messageRawCodewords_;
    message.correctedCodewords = messageCorrectedCodewords_;
    if (messageCodewordCount_ == 0)
    {
        message.type = "TONE";
        message.text = "仅呼叫 / 无文本";
    }
    else
    {
        message.numericText = TrimMessage(DecodePocsagNumeric(messageBits_));
        message.alphaText = TrimMessage(DecodeAlpha(messageBits_));
        message.rawHex = EncodeRawHex(messageBits_);
        message.type = "UNSET";
        message.text = "未设置 | RAW: " + message.rawHex;
    }

    if (transmissionActive_)
        transmission_.messages.push_back(message);
    if (handler_)
        handler_(message);

    messageActive_ = false;
    messageBits_.clear();
    messageCorrectedBits_ = 0;
    messageCodewordCount_ = 0;
    messageHasUncorrectable_ = false;
    messageRawCodewords_.clear();
    messageCorrectedCodewords_.clear();
}

void PocsagDecoder::StartTransmission(std::uint32_t rawSync)
{
    FinalizeTransmission("NEW_SYNC");
    transmission_ = {};
    transmission_.transmissionId = ++nextTransmissionId_;
    transmission_.startedUnixMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    transmission_.baud = baud_;
    transmission_.inverted = inverted_;
    transmissionActive_ = true;
    RecordCodeword(rawSync, kSyncWord, HammingDistance(rawSync, inverted_ ? ~kSyncWord : kSyncWord),
                   -1, "SYNC");
}

void PocsagDecoder::FinalizeTransmission(const char* reason)
{
    if (!transmissionActive_)
        return;
    std::string endReason = reason ? reason : "UNKNOWN";
    if (endReason == "SYNC_LOSS")
    {
        const bool completedAtIdle = std::any_of(
            transmission_.messages.begin(), transmission_.messages.end(),
            [](const PocsagMessage& message) { return message.finalizeReason == "IDLE"; });
        if (completedAtIdle)
            endReason = "BURST_END";
    }
    transmission_.endReason = std::move(endReason);
    if (transmissionHandler_)
        transmissionHandler_(transmission_);
    transmission_ = {};
    transmissionActive_ = false;
}

void PocsagDecoder::RecordCodeword(std::uint32_t raw, std::uint32_t corrected,
                                   int correctedBits, int index, const char* classification)
{
    if (!transmissionActive_)
        return;
    PocsagCodewordRecord record;
    record.raw = raw;
    record.corrected = corrected;
    record.correctedBits = correctedBits;
    record.batch = batchIndex_;
    record.index = index;
    record.classification = classification ? classification : "UNKNOWN";
    transmission_.codewords.push_back(std::move(record));
}

int PocsagDecoder::HammingDistance(std::uint32_t a, std::uint32_t b)
{
    std::uint32_t value = a ^ b;
    int count = 0;
    while (value)
    {
        value &= value - 1;
        ++count;
    }
    return count;
}

bool PocsagDecoder::IsValidCodeword(std::uint32_t word)
{
    if ((HammingDistance(word, 0u) & 1) != 0)
        return false;

    std::uint32_t value = word >> 1;
    for (int bit = 30; bit >= 10; --bit)
    {
        if (value & (1u << bit))
            value ^= kBchPolynomial << (bit - 10);
    }
    return (value & 0x3FFu) == 0;
}

bool PocsagDecoder::CorrectCodeword(std::uint32_t& word, int& correctedBits)
{
    if (IsValidCodeword(word))
    {
        correctedBits = 0;
        return true;
    }

    for (int i = 0; i < 32; ++i)
    {
        const std::uint32_t candidate = word ^ (1u << i);
        if (IsValidCodeword(candidate))
        {
            word = candidate;
            correctedBits = 1;
            return true;
        }
    }

    for (int i = 0; i < 31; ++i)
    {
        for (int j = i + 1; j < 32; ++j)
        {
            const std::uint32_t candidate = word ^ (1u << i) ^ (1u << j);
            if (IsValidCodeword(candidate))
            {
                word = candidate;
                correctedBits = 2;
                return true;
            }
        }
    }
    return false;
}

std::string PocsagDecoder::DecodeAlpha(const std::vector<bool>& bits)
{
    std::string result;
    for (std::size_t i = 0; i + 6 < bits.size(); i += 7)
    {
        int value = 0;
        for (int bit = 0; bit < 7; ++bit)
            if (bits[i + bit])
                value |= 1 << bit;
        result.push_back(static_cast<char>(value));
    }
    return result;
}

std::string PocsagDecoder::EncodeRawHex(const std::vector<bool>& bits)
{
    std::ostringstream result;
    result << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t offset = 0; offset < bits.size(); offset += 8)
    {
        unsigned value = 0;
        const std::size_t count = std::min<std::size_t>(8, bits.size() - offset);
        for (std::size_t bit = 0; bit < count; ++bit)
            value = (value << 1) | (bits[offset + bit] ? 1u : 0u);
        value <<= static_cast<unsigned>(8 - count);
        if (offset != 0)
            result << ' ';
        result << std::setw(2) << value;
    }
    return result.str();
}

std::string PocsagDecoder::EncodeBits(const std::vector<bool>& bits)
{
    std::string result;
    result.reserve(bits.size());
    for (bool bit : bits)
        result.push_back(bit ? '1' : '0');
    return result;
}

std::string PocsagDecoder::TrimMessage(std::string value)
{
    while (!value.empty())
    {
        const unsigned char ch = static_cast<unsigned char>(value.back());
        if (ch == 0 || ch == 3 || ch == 4 || ch == 0x17 || ch == ' ')
            value.pop_back();
        else
            break;
    }
    return value;
}
