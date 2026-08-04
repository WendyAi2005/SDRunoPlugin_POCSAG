#include "../PocsagDecoder.h"

#include <cassert>
#include <cstdint>
#include <iostream>
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
        for (int bit = 0; bit < 4; ++bit)
            bits.push_back((static_cast<int>(position) & (1 << bit)) != 0);
    }
    while (bits.size() % 20 != 0)
        bits.push_back(false);
    return bits;
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

int main()
{
    std::vector<PocsagMessage> decoded;
    PocsagDecoder decoder([&](const PocsagMessage& message) { decoded.push_back(message); });
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
    assert(decoded[0].type == "NUMERIC");
    assert(decoded[0].text.find("6934 103 465") == 0);

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
    std::cout << "Decoder test passed: " << decoded[0].address << " " << decoded[0].text << "\n";
    return 0;
}
