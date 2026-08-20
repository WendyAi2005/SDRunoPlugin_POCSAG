#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <iunoaudioobserver.h>
#include <iunoplugin.h>

#include "PocsagDecoder.h"
#include "RailwayMessageAssembler.h"
#include "RailwayStateManager.h"

class PocsagWindow;

struct PocsagMessageMapping
{
    std::uint32_t address = 0;
    int function = 0;
    std::string type;
};

class SDRunoPlugin_POCSAG final : public IUnoPlugin, public IUnoAudioObserver
{
public:
    explicit SDRunoPlugin_POCSAG(IUnoPluginController& controller);
    ~SDRunoPlugin_POCSAG() override;

    const char* GetPluginName() const override { return "POCSAG Railway Alert"; }
    void HandleEvent(const UnoEvent& event) override;
    void AudioObserverProcess(channel_t channel, const float* buffer, int length) override;

    void ApplyRailwayPreset();
    void SetBaud(int baud);
    void SetPolarity(PocsagPolarity polarity);
    PocsagPolarity GetPolarity() const { return polarity_.load(); }
    void SetBeepEnabled(bool enabled);
    bool GetBeepEnabled() const { return beepEnabled_.load(); }
    std::string GetStatusText();
    std::deque<PocsagMessage> DrainMessages();
    std::vector<RailwayTarget> GetRailwayTargets();
    void ClearRailwayTargets();
    bool OpenRealtimeMap(std::wstring& error);
    PocsagMessage ApplyMessageMapping(PocsagMessage message) const;
    std::vector<PocsagMessageMapping> GetMessageMappings() const;
    bool SetMessageMapping(std::uint32_t address, int function, const std::string& type);
    bool RemoveMessageMapping(std::uint32_t address, int function);
    std::string GetLatestRawJson() const;
    bool ExportRawLogs(const std::wstring& selectedPath, std::wstring& jsonlPath,
                       std::wstring& csvPath, std::string& error) const;
    std::size_t GetRawTransmissionCount() const;

private:
    static std::uint64_t MappingKey(std::uint32_t address, int function);
    void LoadMessageMappings();
    bool SaveMessageMappingsLocked() const;
    void OnDecodedMessage(const PocsagMessage& message);
    void OnDecodedTransmission(const PocsagTransmission& transmission);
    void PublishRailwayStateSnapshot(const std::vector<RailwayTarget>& targets) const;
    void ScheduleUnloadAfterUiThreadExit();
    std::string SerializeTransmissionJson(const PocsagTransmission& transmission,
                                          double frequency,
                                          const std::vector<RailwayApproachMessage>& railwayMessages) const;
    void UiThreadMain();

    static constexpr channel_t kChannel = 0;
    IUnoPluginController& controller_;
    PocsagDecoder decoder_;
    std::mutex decoderMutex_;
    std::mutex messagesMutex_;
    std::mutex windowMutex_;
    mutable std::mutex mappingsMutex_;
    std::deque<PocsagMessage> pendingMessages_;
    std::map<std::uint64_t, PocsagMessageMapping> messageMappings_;
    mutable std::mutex railwayStateMutex_;
    RailwayStateManager railwayStateManager_;
    struct RawLogEntry
    {
        PocsagTransmission transmission;
        double frequency = 0.0;
        std::vector<RailwayApproachMessage> railwayMessages;
    };
    mutable std::mutex rawLogMutex_;
    std::deque<RawLogEntry> rawLogHistory_;
    std::wstring mappingsPath_;
    std::wstring railwayStatePath_;
    std::wstring pluginDirectory_;
    std::thread uiThread_;
    std::atomic<PocsagWindow*> window_{ nullptr };
    std::atomic<bool> closing_{ false };
    std::atomic<bool> observerRegistered_{ false };
    std::atomic<bool> beepEnabled_{ true };
    std::atomic<int> baud_{ 1200 };
    std::atomic<PocsagPolarity> polarity_{ PocsagPolarity::Auto };
    std::atomic<double> audioSampleRate_{ 0.0 };
    std::vector<float> monoScratch_;
};
