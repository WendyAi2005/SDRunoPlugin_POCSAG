#include "SDRunoPlugin_POCSAG.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <shellapi.h>

#include <iunoplugincontroller.h>
#include <unoevent.h>

#include "PocsagWindow.h"
#include "RailwayExtDecoder.h"

namespace
{
struct DeferredUnloadContext
{
    IUnoPluginController* controller = nullptr;
    IUnoPlugin* plugin = nullptr;
    HANDLE uiThread = nullptr;
    HMODULE module = nullptr;
};

DWORD WINAPI DeferredUnloadProc(void* parameter)
{
    std::unique_ptr<DeferredUnloadContext> context(
        static_cast<DeferredUnloadContext*>(parameter));
    WaitForSingleObject(context->uiThread, INFINITE);
    CloseHandle(context->uiThread);
    IUnoPluginController* controller = context->controller;
    IUnoPlugin* plugin = context->plugin;
    HMODULE module = context->module;
    context.reset();

    // RequestUnload may synchronously destroy the plugin and release the
    // host's DLL reference. Our extra module reference keeps this worker's
    // return address valid; FreeLibraryAndExitThread then releases it without
    // executing another instruction from the plugin DLL.
    controller->RequestUnload(plugin);
    FreeLibraryAndExitThread(module, 0);
    return 0;
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            else
                out << static_cast<char>(ch);
        }
    }
    return out.str();
}

std::string CsvEscape(const std::string& value)
{
    std::string escaped = value;
    std::size_t position = 0;
    while ((position = escaped.find('"', position)) != std::string::npos)
    {
        escaped.insert(position, 1, '"');
        position += 2;
    }
    return '"' + escaped + '"';
}

std::string Hex32(std::uint32_t value)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string JoinHex(const std::vector<std::uint32_t>& values)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        if (i)
            out << ' ';
        out << Hex32(values[i]);
    }
    return out.str();
}

std::uint64_t NowUnixMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path RuntimeDataDirectory()
{
    wchar_t localAppData[MAX_PATH]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, _countof(localAppData));
    std::filesystem::path path = count > 0 && count < _countof(localAppData)
        ? std::filesystem::path(localAppData) / L"SDRunoPlugin_POCSAG"
        : std::filesystem::temp_directory_path() / L"SDRunoPlugin_POCSAG";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

std::filesystem::path CurrentModuleDirectory()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CurrentModuleDirectory), &module);
    wchar_t path[MAX_PATH]{};
    if (!module || GetModuleFileNameW(module, path, _countof(path)) == 0)
        return {};
    return std::filesystem::path(path).parent_path();
}
}

SDRunoPlugin_POCSAG::SDRunoPlugin_POCSAG(IUnoPluginController& controller)
    : IUnoPlugin(controller),
      controller_(controller),
      decoder_([this](const PocsagMessage& message) { OnDecodedMessage(message); },
               [this](const PocsagTransmission& transmission) { OnDecodedTransmission(transmission); })
{
    decoder_.SetBaud(1200);
    LoadMessageMappings();
    const std::filesystem::path runtimeDirectory = RuntimeDataDirectory();
    railwayStatePath_ = (runtimeDirectory / L"railway_state.json").wstring();
    railwayStateManager_.ConfigureMileageDatabase(
        (runtimeDirectory / L"mileage_positions.json").wstring());
    pluginDirectory_ = CurrentModuleDirectory().wstring();
    PublishRailwayStateSnapshot({});
    const double sampleRate = controller_.GetAudioSampleRate(kChannel);
    audioSampleRate_.store(sampleRate);
    decoder_.SetSampleRate(sampleRate);
    controller_.RegisterAudioObserver(kChannel, this);
    observerRegistered_.store(true);
    uiThread_ = std::thread(&SDRunoPlugin_POCSAG::UiThreadMain, this);
}

SDRunoPlugin_POCSAG::~SDRunoPlugin_POCSAG()
{
    closing_.store(true);
    if (observerRegistered_.exchange(false))
        controller_.UnregisterAudioObserver(kChannel, this);
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (auto* window = window_.load())
            window->Close();
    }
    if (uiThread_.joinable())
    {
        if (uiThread_.get_id() == std::this_thread::get_id())
            uiThread_.detach();
        else
            uiThread_.join();
    }
}

void SDRunoPlugin_POCSAG::HandleEvent(const UnoEvent& event)
{
    if (event.GetChannel() != kChannel && event.GetType() != UnoEvent::ClosingDown)
        return;

    switch (event.GetType())
    {
    case UnoEvent::SampleRateChanged:
    case UnoEvent::DemodulatorChanged:
    case UnoEvent::StreamingStarted:
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            const double sampleRate = controller_.GetAudioSampleRate(kChannel);
            audioSampleRate_.store(sampleRate);
            decoder_.SetSampleRate(sampleRate);
        }
        break;
    case UnoEvent::StreamingStopped:
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            decoder_.Reset();
        }
        break;
    case UnoEvent::ClosingDown:
        closing_.store(true);
        {
            std::lock_guard<std::mutex> lock(windowMutex_);
            if (auto* window = window_.load())
                window->Close();
        }
        break;
    default:
        break;
    }
}

void SDRunoPlugin_POCSAG::AudioObserverProcess(channel_t channel, const float* buffer, int length)
{
    if (channel != kChannel || closing_.load())
        return;
    std::lock_guard<std::mutex> lock(decoderMutex_);

    // In the SDRuno audio API, length is the number of audio frames and the
    // buffer contains two interleaved floats per frame. Community reference
    // plugins iterate buffer[2*i] and buffer[2*i+1] for i < length. Treating
    // length as the total float count discards half of every callback block
    // and introduces discontinuities that prevent POCSAG clock recovery.
    monoScratch_.resize(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i)
        monoScratch_[static_cast<std::size_t>(i)] =
            (buffer[2 * i] + buffer[2 * i + 1]) * 0.5f;
    decoder_.ProcessAudio(monoScratch_.data(), length);
}

void SDRunoPlugin_POCSAG::ApplyRailwayPreset()
{
    constexpr double frequency = 821237500.0;
    controller_.SetCenterFrequency(kChannel, frequency);
    controller_.SetVfoFrequency(kChannel, frequency);
    controller_.SetDemodulatorType(kChannel, IUnoPluginController::DemodulatorNFM);
    controller_.SetFilterBandwidth(kChannel, 15000);
    controller_.SetSquelchEnable(kChannel, false);
    controller_.SetNoiseReductionLevel(kChannel, 0);
    controller_.SetFmNoiseReductionEnable(kChannel, false);
    SetBaud(1200);
    SetPolarity(PocsagPolarity::Inverted);
}

void SDRunoPlugin_POCSAG::SetBaud(int baud)
{
    if (baud != 512 && baud != 1200 && baud != 2400)
        return;
    baud_.store(baud);
    std::lock_guard<std::mutex> lock(decoderMutex_);
    decoder_.SetBaud(baud);
}

void SDRunoPlugin_POCSAG::SetPolarity(PocsagPolarity polarity)
{
    polarity_.store(polarity);
    std::lock_guard<std::mutex> lock(decoderMutex_);
    decoder_.SetPolarity(polarity);
}

void SDRunoPlugin_POCSAG::SetBeepEnabled(bool enabled)
{
    beepEnabled_.store(enabled);
}

std::string SDRunoPlugin_POCSAG::GetStatusText()
{
    PocsagDecoderStats stats;
    {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        stats = decoder_.GetStats();
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(4)
         << controller_.GetVfoFrequency(kChannel) / 1000000.0 << " MHz  |  ";

    const auto demod = controller_.GetDemodulatorType(kChannel);
    text << (demod == IUnoPluginController::DemodulatorNFM ? "NFM" : "请切换 NFM")
         << "  |  BW " << controller_.GetFilterBandwidth(kChannel) / 1000.0 << " kHz"
         << "  |  " << baud_.load() << " baud"
         << "  |  极性 "
         << (polarity_.load() == PocsagPolarity::Auto ? "AUTO" :
             polarity_.load() == PocsagPolarity::Normal ? "NORMAL" : "INVERTED")
         << "  |  SR " << std::setprecision(1) << audioSampleRate_.load() / 1000.0 << " kHz"
         << "  |  双声道帧"
         << "  |  输入 " << std::setprecision(1) << (stats.inputPeak * 100.0) << "%"
         << "  |  跳变 " << stats.transitions
         << "  |  同步 " << stats.syncs
         << "  |  有效 " << stats.validWords
         << " / 错误 " << stats.invalidWords;
    text << "  |  RAW突发 " << GetRawTransmissionCount();
    return text.str();
}

std::size_t SDRunoPlugin_POCSAG::GetRawTransmissionCount() const
{
    std::lock_guard<std::mutex> lock(rawLogMutex_);
    return rawLogHistory_.size();
}

std::deque<PocsagMessage> SDRunoPlugin_POCSAG::DrainMessages()
{
    std::lock_guard<std::mutex> lock(messagesMutex_);
    std::deque<PocsagMessage> result;
    result.swap(pendingMessages_);
    return result;
}

std::vector<RailwayTarget> SDRunoPlugin_POCSAG::GetRailwayTargets()
{
    std::lock_guard<std::mutex> lock(railwayStateMutex_);
    auto result = railwayStateManager_.Snapshot(NowUnixMs());
    PublishRailwayStateSnapshot(result);
    return result;
}

void SDRunoPlugin_POCSAG::ClearRailwayTargets()
{
    std::lock_guard<std::mutex> lock(railwayStateMutex_);
    railwayStateManager_.Clear();
    PublishRailwayStateSnapshot({});
}

bool SDRunoPlugin_POCSAG::OpenRealtimeMap(std::wstring& error)
{
    const std::filesystem::path server = std::filesystem::path(pluginDirectory_) / L"RailwayMapServer.exe";
    if (!std::filesystem::exists(server))
    {
        error = L"未找到 RailwayMapServer.exe：\n" + server.wstring();
        return false;
    }
    std::wstring command = L"\"" + server.wstring() + L"\" --state \"" + railwayStatePath_ + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, pluginDirectory_.c_str(), &startup, &process))
    {
        error = L"无法启动 RailwayMapServer.exe，错误码 " + std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Sleep(250);
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:8765/",
                                                nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
    {
        error = L"地图服务器已启动，但无法打开默认浏览器。请手动访问 http://127.0.0.1:8765/";
        return false;
    }
    return true;
}

std::uint64_t SDRunoPlugin_POCSAG::MappingKey(std::uint32_t address, int function)
{
    return (static_cast<std::uint64_t>(address) << 2) |
           static_cast<std::uint64_t>(function & 3);
}

void SDRunoPlugin_POCSAG::LoadMessageMappings()
{
    wchar_t appData[MAX_PATH]{};
    const DWORD count = GetEnvironmentVariableW(L"APPDATA", appData, _countof(appData));
    std::filesystem::path directory = count > 0 && count < _countof(appData)
        ? std::filesystem::path(appData) / L"SDRunoPlugin_POCSAG"
        : std::filesystem::temp_directory_path() / L"SDRunoPlugin_POCSAG";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    mappingsPath_ = (directory / L"message_mappings.tsv").wstring();

    std::ifstream input{ std::filesystem::path(mappingsPath_) };
    std::uint32_t address = 0;
    int function = 0;
    std::string type;
    while (input >> address >> function >> type)
    {
        if (function < 0 || function > 3 || (type != "NUMERIC" && type != "ALPHA"))
            continue;
        messageMappings_[MappingKey(address, function)] = { address, function, type };
    }
}

bool SDRunoPlugin_POCSAG::SaveMessageMappingsLocked() const
{
    std::ofstream output(std::filesystem::path(mappingsPath_), std::ios::trunc);
    if (!output)
        return false;
    for (const auto& entry : messageMappings_)
    {
        const auto& mapping = entry.second;
        output << mapping.address << '\t' << mapping.function << '\t' << mapping.type << '\n';
    }
    return output.good();
}

PocsagMessage SDRunoPlugin_POCSAG::ApplyMessageMapping(PocsagMessage message) const
{
    if (message.type == "TONE" || message.rawHex.empty())
        return message;

    if (message.address == 1234000)
    {
        const RailwayFields railway = ParseRailwayNumeric(message.numericText);
        message.type = "铁路 NUMERIC";
        message.text = message.numericText;
        message.railwayValid = IsStrictRailwayBasic(
            message.numericText, message.messageCodewordCount, message.hasUncorrectableCodeword);
        message.decodedTrain = railway.train;
        message.hasDecodedSpeed = railway.hasSpeed;
        message.decodedSpeedKmh = railway.speedKmh;
        message.hasDecodedKilometer = railway.hasKilometer;
        message.decodedKilometer = railway.kilometer;
        if (!message.railwayValid)
            message.decodeConfidence = "LOW";
        else if (message.correctedBits == 0)
            message.decodeConfidence = "HIGH";
        else
            message.decodeConfidence = "MEDIUM";
        return message;
    }

    if (message.address == 1234002)
    {
        const RailwayExtFields extension = DecodeRailwayExt(
            message.messageBits, message.messageCodewordCount, message.correctedBits,
            message.hasUncorrectableCodeword);
        message.railwayExtValid = extension.valid;
        message.railwayExtTruncated = extension.truncated;
        message.railwayExtNormalizedHex = extension.normalizedHex;
        message.trainPrefix = extension.trainPrefix;
        message.locomotiveId = extension.locomotiveId;
        message.locomotiveTypeCode = extension.locomotiveTypeCode;
        message.locomotiveSerial = extension.locomotiveSerial;
        message.locomotiveEnd = extension.locomotiveEnd;
        message.lineName = extension.lineName;
        message.lineNameRawHex = extension.lineNameRawHex;
        message.longitudeRaw = extension.longitudeRaw;
        message.longitudeDegreeMinute = extension.longitudeDegreeMinute;
        message.longitudeValid = extension.longitudeValid;
        message.longitudeDeg = extension.longitudeDeg;
        message.latitudeRaw = extension.latitudeRaw;
        message.latitudeDegreeMinute = extension.latitudeDegreeMinute;
        message.latitudeValid = extension.latitudeValid;
        message.latitudeDeg = extension.latitudeDeg;
        message.railwayAuxRaw = extension.railwayAuxRaw;
        message.railwayExtConfidence = extension.confidence;
        message.railwayExtFailureReason = extension.failureReason;
        message.type = extension.truncated ? "RAILWAY_EXT_TRUNCATED" :
            extension.valid ? "RAILWAY_EXT" : "RAILWAY_EXT_INVALID";
        if (extension.valid)
        {
            std::ostringstream text;
            text << (extension.trainPrefix.empty() ? "无冠字" : extension.trainPrefix)
                 << " | 机车 " << extension.locomotiveId
                 << " | 端 " << extension.locomotiveEnd
                 << " | " << extension.lineName
                 << " | RAW " << extension.longitudeRaw << "," << extension.latitudeRaw
                 << " | DM " << extension.longitudeDegreeMinute << ", "
                 << extension.latitudeDegreeMinute
                 << " | DEC " << std::fixed << std::setprecision(6)
                 << extension.longitudeDeg << ", " << extension.latitudeDeg
                 << " | AUX " << extension.railwayAuxRaw;
            message.text = text.str();
        }
        else
        {
            message.text = message.type + " | " + extension.failureReason + " | RAW: " + message.rawHex;
        }
        return message;
    }

    std::lock_guard<std::mutex> lock(mappingsMutex_);
    const auto found = messageMappings_.find(MappingKey(message.address, message.function));
    if (found == messageMappings_.end())
    {
        message.type = "UNSET";
        message.text = "未设置 | RAW: " + message.rawHex;
        return message;
    }

    message.type = found->second.type;
    message.text = message.type == "NUMERIC" ? message.numericText : message.alphaText;
    if (message.text.empty())
        message.text = "（空正文） | RAW: " + message.rawHex;
    return message;
}

std::vector<PocsagMessageMapping> SDRunoPlugin_POCSAG::GetMessageMappings() const
{
    std::lock_guard<std::mutex> lock(mappingsMutex_);
    std::vector<PocsagMessageMapping> result;
    result.reserve(messageMappings_.size());
    for (const auto& entry : messageMappings_)
        result.push_back(entry.second);
    return result;
}

bool SDRunoPlugin_POCSAG::SetMessageMapping(std::uint32_t address, int function, const std::string& type)
{
    if (function < 0 || function > 3 || (type != "NUMERIC" && type != "ALPHA"))
        return false;
    std::lock_guard<std::mutex> lock(mappingsMutex_);
    messageMappings_[MappingKey(address, function)] = { address, function, type };
    return SaveMessageMappingsLocked();
}

bool SDRunoPlugin_POCSAG::RemoveMessageMapping(std::uint32_t address, int function)
{
    std::lock_guard<std::mutex> lock(mappingsMutex_);
    messageMappings_.erase(MappingKey(address, function));
    return SaveMessageMappingsLocked();
}

void SDRunoPlugin_POCSAG::OnDecodedMessage(const PocsagMessage& message)
{
    const auto mappedMessage = ApplyMessageMapping(message);
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        pendingMessages_.push_back(mappedMessage);
        while (pendingMessages_.size() > 1000)
            pendingMessages_.pop_front();
    }
    if (beepEnabled_.load())
        MessageBeep(MB_ICONEXCLAMATION);
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (auto* window = window_.load())
            window->NotifyMessagesAvailable();
    }
}

void SDRunoPlugin_POCSAG::OnDecodedTransmission(const PocsagTransmission& transmission)
{
    RawLogEntry entry;
    entry.transmission = transmission;
    entry.frequency = controller_.GetVfoFrequency(kChannel);
    std::vector<PocsagMessage> interpretedMessages;
    interpretedMessages.reserve(transmission.messages.size());
    for (const auto& message : transmission.messages)
        interpretedMessages.push_back(ApplyMessageMapping(message));
    entry.railwayMessages = RailwayMessageAssembler::Assemble(
        transmission.transmissionId, transmission.startedUnixMs, interpretedMessages);

    if (!entry.railwayMessages.empty())
    {
        std::lock_guard<std::mutex> lock(railwayStateMutex_);
        for (const auto& message : entry.railwayMessages)
            railwayStateManager_.Apply(message);
        PublishRailwayStateSnapshot(railwayStateManager_.Snapshot(NowUnixMs()));
    }
    {
        std::lock_guard<std::mutex> lock(rawLogMutex_);
        rawLogHistory_.push_back(std::move(entry));
        while (rawLogHistory_.size() > 5000)
            rawLogHistory_.pop_front();
    }
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (auto* window = window_.load())
            window->NotifyMessagesAvailable();
    }
}

void SDRunoPlugin_POCSAG::PublishRailwayStateSnapshot(
    const std::vector<RailwayTarget>& targets) const
{
    if (railwayStatePath_.empty())
        return;
    std::ostringstream out;
    out << '[';
    const std::uint64_t now = NowUnixMs();
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const auto& target = targets[i];
        if (i)
            out << ',';
        const std::uint64_t ageSeconds = now >= target.lastAnyUpdateUnixMs
            ? (now - target.lastAnyUpdateUnixMs) / 1000 : 0;
        out << "{\"id\":\"" << target.targetUid
            << "\",\"target_uid\":" << target.targetUid
            << ",\"display_id\":\"" << JsonEscape(target.targetId)
            << "\",\"created_by\":\"" << JsonEscape(target.createdBy)
            << "\",\"merge_count\":" << target.mergeCount
            << ",\"last_merge_reason\":\"" << JsonEscape(target.lastMergeReason)
            << "\",\"train\":" << (target.fullTrainNumber.empty() ? "null" : "\"" + JsonEscape(target.fullTrainNumber) + "\"")
            << ",\"speed_kmh\":";
        if (target.hasSpeed) out << target.speedKmh; else out << "null";
        out << ",\"kilometer_km\":";
        if (target.hasKilometer) out << std::fixed << std::setprecision(1) << target.kilometerKm; else out << "null";
        out << ",\"locomotive_id\":" << (target.locomotiveId.empty() ? "null" : "\"" + JsonEscape(target.locomotiveId) + "\"")
            << ",\"locomotive_end\":" << (target.locomotiveEnd.empty() ? "null" : "\"" + JsonEscape(target.locomotiveEnd) + "\"")
            << ",\"line_name\":" << (target.lineName.empty() ? "null" : "\"" + JsonEscape(target.lineName) + "\"")
            << ",\"longitude_raw\":" << (target.longitudeRaw.empty() ? "null" : "\"" + JsonEscape(target.longitudeRaw) + "\"")
            << ",\"latitude_raw\":" << (target.latitudeRaw.empty() ? "null" : "\"" + JsonEscape(target.latitudeRaw) + "\"")
            << ",\"longitude_degree_minute\":" << (target.longitudeDegreeMinute.empty() ? "null" : "\"" + JsonEscape(target.longitudeDegreeMinute) + "\"")
            << ",\"latitude_degree_minute\":" << (target.latitudeDegreeMinute.empty() ? "null" : "\"" + JsonEscape(target.latitudeDegreeMinute) + "\"")
            << ",\"radio_longitude\":";
        if (target.hasPosition) out << std::fixed << std::setprecision(9) << target.longitudeDeg; else out << "null";
        out << ",\"radio_latitude\":";
        if (target.hasPosition) out << std::fixed << std::setprecision(9) << target.latitudeDeg; else out << "null";
        out << ",\"radio_gps_fresh\":" << (target.radioGpsFresh ? "true" : "false")
            << ",\"longitude\":";
        if (target.hasDisplayedPosition) out << std::fixed << std::setprecision(9) << target.displayedLongitudeDeg; else out << "null";
        out << ",\"latitude\":";
        if (target.hasDisplayedPosition) out << std::fixed << std::setprecision(9) << target.displayedLatitudeDeg; else out << "null";
        out << ",\"display_longitude\":";
        if (target.hasDisplayedPosition) out << std::fixed << std::setprecision(9) << target.displayedLongitudeDeg; else out << "null";
        out << ",\"display_latitude\":";
        if (target.hasDisplayedPosition) out << std::fixed << std::setprecision(9) << target.displayedLatitudeDeg; else out << "null";
        out << ",\"position_source\":\"" << PositionSourceName(target.positionSource)
            << "\",\"position_quality\":\"" << PositionQualityName(target.positionQuality)
            << "\",\"position_confidence\":" << std::fixed << std::setprecision(3)
            << target.positionConfidence
            << ",\"mileage_longitude\":";
        if (target.hasMileageEstimate) out << std::fixed << std::setprecision(9) << target.mileageLongitudeDeg; else out << "null";
        out << ",\"mileage_latitude\":";
        if (target.hasMileageEstimate) out << std::fixed << std::setprecision(9) << target.mileageLatitudeDeg; else out << "null";
        out << ",\"mileage_position_source\":\"" << PositionSourceName(target.mileagePositionSource)
            << "\",\"mileage_position_quality\":\"" << PositionQualityName(target.mileagePositionQuality)
            << "\",\"mileage_position_confidence\":" << std::fixed << std::setprecision(3)
            << target.mileagePositionConfidence
            << ",\"gps_mileage_comparison\":\"" << PositionComparisonName(target.gpsMileageComparison)
            << "\",\"gps_vs_mileage_distance_m\":";
        if (target.gpsMileageComparison != PositionComparison::Unavailable)
            out << std::fixed << std::setprecision(1) << target.gpsVsMileageDistanceMeters;
        else out << "null";
        out << ",\"data_completeness\":\"" << JsonEscape(target.dataCompleteness)
            << "\",\"quality\":\"" << JsonEscape(target.quality)
            << "\",\"stale\":" << (target.stale ? "true" : "false")
            << ",\"last_update_unix_ms\":" << target.lastAnyUpdateUnixMs
            << ",\"age_seconds\":" << ageSeconds << ",\"track\":[";
        for (std::size_t pointIndex = 0; pointIndex < target.track.size(); ++pointIndex)
        {
            if (pointIndex)
                out << ',';
            const auto& point = target.track[pointIndex];
            out << "{\"latitude_raw\":\"" << JsonEscape(point.latitudeRaw)
                << "\",\"longitude_raw\":\"" << JsonEscape(point.longitudeRaw)
                << "\",\"latitude_degree_minute\":\"" << JsonEscape(point.latitudeDegreeMinute)
                << "\",\"longitude_degree_minute\":\"" << JsonEscape(point.longitudeDegreeMinute)
                << "\",\"latitude\":" << std::fixed << std::setprecision(9) << point.latitude
                << ",\"longitude\":" << point.longitude
                << ",\"source\":\"" << PositionSourceName(point.source)
                << "\",\"quality\":\"" << PositionQualityName(point.quality)
                << "\",\"timestamp_unix_ms\":" << point.timestampUnixMs << '}';
        }
        out << "]}";
    }
    out << ']';

    const std::filesystem::path destination(railwayStatePath_);
    const std::filesystem::path temporary = destination.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file << out.str();
    }
    MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::string SDRunoPlugin_POCSAG::SerializeTransmissionJson(
    const PocsagTransmission& transmission, double frequency,
    const std::vector<RailwayApproachMessage>& railwayMessages) const
{
    std::ostringstream out;
    out << "{\"transmission_id\":" << transmission.transmissionId
        << ",\"timestamp_unix_ms\":" << transmission.startedUnixMs
        << ",\"frequency_hz\":" << std::fixed << std::setprecision(0) << frequency
        << ",\"baud\":" << transmission.baud
        << ",\"polarity\":\"" << (transmission.inverted ? "INVERTED" : "NORMAL") << "\""
        << ",\"end_reason\":\"" << JsonEscape(transmission.endReason) << "\""
        << ",\"codewords\":[";
    for (std::size_t i = 0; i < transmission.codewords.size(); ++i)
    {
        const auto& word = transmission.codewords[i];
        if (i)
            out << ',';
        out << "{\"sequence\":" << i
            << ",\"batch\":" << word.batch
            << ",\"index\":" << word.index
            << ",\"raw\":\"" << Hex32(word.raw) << "\""
            << ",\"corrected\":\"" << Hex32(word.corrected) << "\""
            << ",\"corrected_bits\":" << word.correctedBits
            << ",\"uncorrectable\":" << (word.correctedBits < 0 ? "true" : "false")
            << ",\"classification\":\"" << JsonEscape(word.classification) << "\"}";
    }
    out << "],\"messages\":[";
    for (std::size_t i = 0; i < transmission.messages.size(); ++i)
    {
        if (i)
            out << ',';
        const auto mapped = ApplyMessageMapping(transmission.messages[i]);
        out << "{\"transmission_id\":" << mapped.transmissionId
            << ",\"ric\":" << mapped.address
            << ",\"function\":" << mapped.function
            << ",\"message_codeword_count\":" << mapped.messageCodewordCount
            << ",\"finalize_reason\":\"" << JsonEscape(mapped.finalizeReason) << "\""
            << ",\"corrected_bits_total\":" << mapped.correctedBits
            << ",\"has_uncorrectable_codeword\":"
            << (mapped.hasUncorrectableCodeword ? "true" : "false")
            << ",\"raw_codewords\":\"" << JoinHex(mapped.rawCodewords) << "\""
            << ",\"corrected_codewords\":\"" << JoinHex(mapped.correctedCodewords) << "\""
            << ",\"message_bits\":\"" << mapped.messageBits << "\""
            << ",\"raw_message_bits\":\"" << mapped.messageBits << "\""
            << ",\"message_hex\":\"" << JsonEscape(mapped.rawHex) << "\""
            << ",\"numeric_decode\":\"" << JsonEscape(mapped.numericText) << "\""
            << ",\"alpha_decode\":\"" << JsonEscape(mapped.alphaText) << "\""
            << ",\"mapped_type\":\"" << JsonEscape(mapped.type) << "\""
            << ",\"mapped_decode\":\"" << JsonEscape(mapped.text) << "\""
            << ",\"decoded_train\":"
            << (mapped.decodedTrain.empty() ? "null" : "\"" + JsonEscape(mapped.decodedTrain) + "\"")
            << ",\"decoded_speed_kmh\":";
        if (mapped.hasDecodedSpeed)
            out << mapped.decodedSpeedKmh;
        else
            out << "null";
        out << ",\"decoded_km\":";
        if (mapped.hasDecodedKilometer)
            out << std::fixed << std::setprecision(1) << mapped.decodedKilometer;
        else
            out << "null";
        out << ",\"railway_valid\":" << (mapped.railwayValid ? "true" : "false")
            << ",\"decode_confidence\":\"" << JsonEscape(mapped.decodeConfidence) << "\""
            << ",\"railway_ext_normalized_hex\":\"" << JsonEscape(mapped.railwayExtNormalizedHex) << "\""
            << ",\"train_prefix\":\"" << JsonEscape(mapped.trainPrefix) << "\""
            << ",\"locomotive_id\":\"" << JsonEscape(mapped.locomotiveId) << "\""
            << ",\"locomotive_type_code\":\"" << JsonEscape(mapped.locomotiveTypeCode) << "\""
            << ",\"locomotive_serial\":\"" << JsonEscape(mapped.locomotiveSerial) << "\""
            << ",\"locomotive_end\":\"" << JsonEscape(mapped.locomotiveEnd) << "\""
            << ",\"line_name\":\"" << JsonEscape(mapped.lineName) << "\""
            << ",\"line_name_raw_hex\":\"" << JsonEscape(mapped.lineNameRawHex) << "\""
            << ",\"longitude_raw\":\"" << JsonEscape(mapped.longitudeRaw) << "\""
            << ",\"latitude_raw\":\"" << JsonEscape(mapped.latitudeRaw) << "\""
            << ",\"longitude_degree_minute\":\"" << JsonEscape(mapped.longitudeDegreeMinute) << "\""
            << ",\"latitude_degree_minute\":\"" << JsonEscape(mapped.latitudeDegreeMinute) << "\""
            << ",\"longitude_deg\":";
        if (mapped.longitudeValid)
            out << std::fixed << std::setprecision(9) << mapped.longitudeDeg;
        else
            out << "null";
        out << ",\"latitude_deg\":";
        if (mapped.latitudeValid)
            out << std::fixed << std::setprecision(9) << mapped.latitudeDeg;
        else
            out << "null";
        out << ",\"railway_aux_raw\":\"" << JsonEscape(mapped.railwayAuxRaw) << "\""
            << ",\"railway_ext_valid\":" << (mapped.railwayExtValid ? "true" : "false")
            << ",\"railway_ext_truncated\":" << (mapped.railwayExtTruncated ? "true" : "false")
            << ",\"railway_ext_confidence\":\"" << JsonEscape(mapped.railwayExtConfidence) << "\""
            << ",\"railway_ext_failure_reason\":\"" << JsonEscape(mapped.railwayExtFailureReason) << "\"}";
    }
    out << "],\"railway_combined\":[";
    for (std::size_t i = 0; i < railwayMessages.size(); ++i)
    {
        const auto& combined = railwayMessages[i];
        if (i)
            out << ',';
        out << "{\"transmission_id\":" << combined.transmissionId
            << ",\"pairing_method\":\"" << JsonEscape(combined.pairingMethod) << "\""
            << ",\"data_completeness\":\"" << JsonEscape(combined.dataCompleteness) << "\""
            << ",\"paired_basic_ric\":" << (combined.hasBasic ? "1234000" : "null")
            << ",\"paired_extension_ric\":" << (combined.hasExtension ? "1234002" : "null")
            << ",\"full_train_number\":\"" << JsonEscape(combined.fullTrainNumber) << "\""
            << ",\"speed_kmh\":";
        if (combined.hasSpeed) out << combined.speedKmh; else out << "null";
        out << ",\"kilometer_km\":";
        if (combined.hasKilometer) out << std::fixed << std::setprecision(1) << combined.kilometerKm;
        else out << "null";
        out << ",\"locomotive_id\":\"" << JsonEscape(combined.locomotiveId) << "\""
            << ",\"locomotive_end\":\"" << JsonEscape(combined.locomotiveEnd) << "\""
            << ",\"line_name\":\"" << JsonEscape(combined.lineName) << "\""
            << ",\"longitude_raw\":\"" << JsonEscape(combined.longitudeRaw) << "\""
            << ",\"latitude_raw\":\"" << JsonEscape(combined.latitudeRaw) << "\""
            << ",\"longitude_degree_minute\":\"" << JsonEscape(combined.longitudeDegreeMinute) << "\""
            << ",\"latitude_degree_minute\":\"" << JsonEscape(combined.latitudeDegreeMinute) << "\""
            << ",\"longitude_deg\":";
        if (combined.hasLongitude) out << std::fixed << std::setprecision(9) << combined.longitudeDeg;
        else out << "null";
        out << ",\"latitude_deg\":";
        if (combined.hasLatitude) out << std::fixed << std::setprecision(9) << combined.latitudeDeg;
        else out << "null";
        out
            << ",\"railway_aux_raw\":\"" << JsonEscape(combined.railwayAuxRaw) << "\""
            << ",\"confidence\":\"" << JsonEscape(combined.confidence) << "\"}"
            ;
    }
    out << "]}";
    return out.str();
}

std::string SDRunoPlugin_POCSAG::GetLatestRawJson() const
{
    std::lock_guard<std::mutex> lock(rawLogMutex_);
    if (rawLogHistory_.empty())
        return {};
    const auto& entry = rawLogHistory_.back();
    return SerializeTransmissionJson(entry.transmission, entry.frequency, entry.railwayMessages);
}

bool SDRunoPlugin_POCSAG::ExportRawLogs(const std::wstring& selectedPath,
                                        std::wstring& jsonlPath, std::wstring& csvPath,
                                        std::string& error) const
{
    std::vector<RawLogEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(rawLogMutex_);
        snapshot.assign(rawLogHistory_.begin(), rawLogHistory_.end());
    }
    if (snapshot.empty())
    {
        error = "当前还没有完成的 POCSAG transmission 可导出。";
        return false;
    }

    std::filesystem::path base(selectedPath);
    base.replace_extension();
    jsonlPath = (base.wstring() + L".jsonl");
    csvPath = (base.wstring() + L".csv");
    std::ofstream jsonl(std::filesystem::path(jsonlPath), std::ios::binary | std::ios::trunc);
    std::ofstream csv(std::filesystem::path(csvPath), std::ios::binary | std::ios::trunc);
    if (!jsonl || !csv)
    {
        error = "无法创建导出文件。";
        return false;
    }
    csv << "timestamp_unix_ms,frequency_hz,baud,polarity,transmission_end_reason,ric,function,"
           "message_codeword_count,finalize_reason,corrected_bits_total,has_uncorrectable_codeword,"
           "raw_codewords,corrected_codewords,message_bits,message_hex,numeric_decode,alpha_decode,"
           "mapped_type,mapped_decode,decoded_train,decoded_speed_kmh,decoded_km,railway_valid,"
           "decode_confidence,railway_ext_normalized_hex,train_prefix,locomotive_id,"
           "locomotive_end,line_name,longitude_raw,latitude_raw,longitude_degree_minute,"
           "latitude_degree_minute,longitude_deg,latitude_deg,railway_aux_raw,railway_ext_valid,"
           "railway_ext_confidence,railway_ext_truncated,railway_ext_failure_reason,"
           "line_name_raw_hex,locomotive_type_code,locomotive_serial,raw_message_bits,"
           "transmission_id,pairing_method,data_completeness,paired_basic_ric,"
           "paired_extension_ric,full_train_number,speed_kmh,kilometer_km\r\n";
    for (const auto& entry : snapshot)
    {
        jsonl << SerializeTransmissionJson(entry.transmission, entry.frequency, entry.railwayMessages) << '\n';
        for (std::size_t messageIndex = 0; messageIndex < entry.transmission.messages.size(); ++messageIndex)
        {
            const auto& sourceMessage = entry.transmission.messages[messageIndex];
            const auto message = ApplyMessageMapping(sourceMessage);
            const RailwayApproachMessage* paired = nullptr;
            for (const auto& combined : entry.railwayMessages)
            {
                if ((combined.hasBasic && combined.basicMessageIndex == messageIndex) ||
                    (combined.hasExtension && combined.extensionMessageIndex == messageIndex))
                {
                    paired = &combined;
                    break;
                }
            }
            csv << entry.transmission.startedUnixMs << ',' << std::fixed << std::setprecision(0)
                << entry.frequency << ',' << entry.transmission.baud << ','
                << (entry.transmission.inverted ? "INVERTED" : "NORMAL") << ','
                << CsvEscape(entry.transmission.endReason) << ',' << message.address << ','
                << message.function << ',' << message.messageCodewordCount << ','
                << CsvEscape(message.finalizeReason) << ',' << message.correctedBits << ','
                << (message.hasUncorrectableCodeword ? "true" : "false") << ','
                << CsvEscape(JoinHex(message.rawCodewords)) << ','
                << CsvEscape(JoinHex(message.correctedCodewords)) << ','
                << CsvEscape(message.messageBits) << ',' << CsvEscape(message.rawHex) << ','
                << CsvEscape(message.numericText) << ',' << CsvEscape(message.alphaText) << ','
                << CsvEscape(message.type) << ',' << CsvEscape(message.text) << ','
                << CsvEscape(message.decodedTrain) << ',';
            if (message.hasDecodedSpeed)
                csv << message.decodedSpeedKmh;
            csv << ',';
            if (message.hasDecodedKilometer)
                csv << std::fixed << std::setprecision(1) << message.decodedKilometer;
            csv << ',' << (message.railwayValid ? "true" : "false") << ','
                << CsvEscape(message.decodeConfidence) << ','
                << CsvEscape(message.railwayExtNormalizedHex) << ','
                << CsvEscape(message.trainPrefix) << ',' << CsvEscape(message.locomotiveId) << ','
                << CsvEscape(message.locomotiveEnd) << ',' << CsvEscape(message.lineName) << ','
                << CsvEscape(message.longitudeRaw) << ',' << CsvEscape(message.latitudeRaw) << ','
                << CsvEscape(message.longitudeDegreeMinute) << ','
                << CsvEscape(message.latitudeDegreeMinute) << ',';
            if (message.longitudeValid)
                csv << std::fixed << std::setprecision(9) << message.longitudeDeg;
            csv << ',';
            if (message.latitudeValid)
                csv << std::fixed << std::setprecision(9) << message.latitudeDeg;
            csv << ',' << CsvEscape(message.railwayAuxRaw) << ','
                << (message.railwayExtValid ? "true" : "false") << ','
                << CsvEscape(message.railwayExtConfidence) << ','
                << (message.railwayExtTruncated ? "true" : "false") << ','
                << CsvEscape(message.railwayExtFailureReason) << ','
                << CsvEscape(message.lineNameRawHex) << ','
                << CsvEscape(message.locomotiveTypeCode) << ','
                << CsvEscape(message.locomotiveSerial) << ','
                << CsvEscape(message.messageBits) << ',' << message.transmissionId << ',';
            if (paired)
            {
                csv << CsvEscape(paired->pairingMethod) << ',' << CsvEscape(paired->dataCompleteness) << ',';
                if (paired->hasBasic) csv << "1234000";
                csv << ',';
                if (paired->hasExtension) csv << "1234002";
                csv << ','
                    << CsvEscape(paired->fullTrainNumber) << ',';
                if (paired->hasSpeed) csv << paired->speedKmh;
                csv << ',';
                if (paired->hasKilometer)
                    csv << std::fixed << std::setprecision(1) << paired->kilometerKm;
            }
            else
            {
                csv << ",,,,,,";
            }
            csv << "\r\n";
        }
    }
    if (!jsonl.good() || !csv.good())
    {
        error = "写入 RAW 日志时发生错误。";
        return false;
    }
    return true;
}

void SDRunoPlugin_POCSAG::UiThreadMain()
{
    auto window = std::make_unique<PocsagWindow>(*this);
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        window_.store(window.get());
    }
    window->Run();
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        window_.store(nullptr);
    }
    if (!closing_.exchange(true))
        ScheduleUnloadAfterUiThreadExit();
}

void SDRunoPlugin_POCSAG::ScheduleUnloadAfterUiThreadExit()
{
    HANDLE uiThread = OpenThread(SYNCHRONIZE, FALSE, GetCurrentThreadId());
    HMODULE module = nullptr;
    if (!uiThread || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&DeferredUnloadProc), &module))
    {
        if (uiThread)
            CloseHandle(uiThread);
        return;
    }

    auto context = std::make_unique<DeferredUnloadContext>();
    context->controller = &controller_;
    context->plugin = this;
    context->uiThread = uiThread;
    context->module = module;
    HANDLE worker = CreateThread(nullptr, 0, DeferredUnloadProc, context.get(), 0, nullptr);
    if (!worker)
    {
        CloseHandle(uiThread);
        FreeLibrary(module);
        return;
    }
    context.release();
    CloseHandle(worker);
}
