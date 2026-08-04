#include "PocsagDecoder.h"

#include <algorithm>
#include <cmath>

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
}

PocsagDecoder::PocsagDecoder(MessageHandler handler) : handler_(std::move(handler))
{
    Reset();
}

void PocsagDecoder::Reset()
{
    FinalizeMessage();
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

void PocsagDecoder::ProcessAudio(const float* samples, int length)
{
    if (!samples || length <= 0 || sampleRate_ <= 0.0)
        return;

    const double phaseStep = static_cast<double>(baud_) / sampleRate_;
    const double lowPassAlpha = std::min(0.45, 6.0 * static_cast<double>(baud_) / sampleRate_);
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

        const double threshold = std::max(0.00002, envelope_ * 0.08);
        bool level = lastLevel_;
        if (filtered_ > threshold)
            level = true;
        else if (filtered_ < -threshold)
            level = false;

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

            if (edgeLength >= samplesPerBit * 0.32 && edgeLength <= samplesPerBit * 1.90)
                phase_ = 0.0;

            if (preambleEdgeCount_ >= 40)
                preambleWindow_ = 512;

            samplesSinceEdge_ = 0;
            lastLevel_ = level;
        }

        const double oldPhase = phase_;
        phase_ += phaseStep;
        if (phase_ >= 1.0)
            phase_ -= 1.0;

        if (oldPhase < 0.5 && phase_ >= 0.5)
            ProcessBit(level);
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
        if (preambleWindow_ > 0 && HammingDistance(searchRegister_, kSyncWord) <= 2)
        {
            synchronized_ = true;
            inverted_ = false;
            ++stats_.syncs;
        }
        else if (preambleWindow_ > 0 && HammingDistance(searchRegister_, ~kSyncWord) <= 2)
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
            expectingSync_ = false;
            batchWordIndex_ = 0;
        }
        else
        {
            FinalizeMessage();
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
    int corrected = 0;
    if (!CorrectCodeword(word, corrected))
    {
        ++stats_.invalidWords;
        FinalizeMessage();
        return;
    }
    ++stats_.validWords;

    if (word == kIdleWord)
    {
        FinalizeMessage();
        return;
    }

    if ((word & 0x80000000u) == 0)
    {
        FinalizeMessage();
        const std::uint32_t addressHigh = (word >> 13) & 0x3FFFFu;
        const std::uint32_t frame = static_cast<std::uint32_t>(index / 2);
        messageAddress_ = (addressHigh << 3) | frame;
        messageFunction_ = static_cast<int>((word >> 11) & 0x3u);
        messageCorrectedBits_ = corrected;
        messageBits_.clear();
        messageActive_ = true;
        return;
    }

    if (!messageActive_)
        return;

    messageCorrectedBits_ += corrected;
    for (int bit = 30; bit >= 11; --bit)
        messageBits_.push_back(((word >> bit) & 1u) != 0);
}

void PocsagDecoder::FinalizeMessage()
{
    if (!messageActive_)
        return;

    const std::string numeric = TrimMessage(DecodeNumeric(messageBits_));
    const std::string alpha = TrimMessage(DecodeAlpha(messageBits_));

    int alphaPrintable = 0;
    int alphaLetters = 0;
    for (unsigned char ch : alpha)
    {
        if (ch >= 0x20 && ch <= 0x7E)
            ++alphaPrintable;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
            ++alphaLetters;
    }

    PocsagMessage message;
    message.address = messageAddress_;
    message.function = messageFunction_;
    message.correctedBits = messageCorrectedBits_;
    if (messageBits_.empty())
    {
        message.type = "TONE";
        message.text = "仅呼叫 / 无文本";
    }
    else if (!alpha.empty() && alphaLetters > 0 && alphaPrintable * 10 >= static_cast<int>(alpha.size()) * 9)
    {
        message.type = "ALPHA";
        message.text = alpha;
    }
    else
    {
        message.type = "NUMERIC";
        message.text = numeric;
    }

    if (message.text.empty())
    {
        message.type = "RAW";
        message.text = "收到数据，但文本不可显示";
    }

    if (handler_)
        handler_(message);

    messageActive_ = false;
    messageBits_.clear();
    messageCorrectedBits_ = 0;
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

std::string PocsagDecoder::DecodeNumeric(const std::vector<bool>& bits)
{
    static constexpr char kNumericAlphabet[] = "084 2.6]195-3U7[";
    std::string result;
    for (std::size_t i = 0; i + 3 < bits.size(); i += 4)
    {
        int value = 0;
        for (int bit = 0; bit < 4; ++bit)
            if (bits[i + bit])
                value |= 1 << bit;
        result.push_back(kNumericAlphabet[value & 0x0F]);
    }
    return result;
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
