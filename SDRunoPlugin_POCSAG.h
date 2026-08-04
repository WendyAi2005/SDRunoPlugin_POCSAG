#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <iunoaudioobserver.h>
#include <iunoplugin.h>

#include "PocsagDecoder.h"

class PocsagWindow;

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
    void SetBeepEnabled(bool enabled);
    bool GetBeepEnabled() const { return beepEnabled_.load(); }
    std::string GetStatusText();
    std::deque<PocsagMessage> DrainMessages();
    void RequestClose();

private:
    void OnDecodedMessage(const PocsagMessage& message);
    void UiThreadMain();

    static constexpr channel_t kChannel = 0;
    IUnoPluginController& controller_;
    PocsagDecoder decoder_;
    std::mutex decoderMutex_;
    std::mutex messagesMutex_;
    std::mutex windowMutex_;
    std::deque<PocsagMessage> pendingMessages_;
    std::thread uiThread_;
    std::atomic<PocsagWindow*> window_{ nullptr };
    std::atomic<bool> closing_{ false };
    std::atomic<bool> observerRegistered_{ false };
    std::atomic<bool> beepEnabled_{ true };
    std::atomic<int> baud_{ 1200 };
};
