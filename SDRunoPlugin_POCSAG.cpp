#include "SDRunoPlugin_POCSAG.h"

#include <Windows.h>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>

#include <iunoplugincontroller.h>
#include <unoevent.h>

#include "PocsagWindow.h"

SDRunoPlugin_POCSAG::SDRunoPlugin_POCSAG(IUnoPluginController& controller)
    : IUnoPlugin(controller),
      controller_(controller),
      decoder_([this](const PocsagMessage& message) { OnDecodedMessage(message); })
{
    decoder_.SetBaud(1200);
    decoder_.SetSampleRate(controller_.GetAudioSampleRate(kChannel));
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
            decoder_.SetSampleRate(controller_.GetAudioSampleRate(kChannel));
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
    decoder_.ProcessAudio(buffer, length);
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
}

void SDRunoPlugin_POCSAG::SetBaud(int baud)
{
    if (baud != 512 && baud != 1200 && baud != 2400)
        return;
    baud_.store(baud);
    std::lock_guard<std::mutex> lock(decoderMutex_);
    decoder_.SetBaud(baud);
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
         << "  |  输入 " << std::setprecision(1) << (stats.inputPeak * 100.0) << "%"
         << "  |  同步 " << stats.syncs
         << "  |  有效 " << stats.validWords
         << " / 错误 " << stats.invalidWords;
    return text.str();
}

std::deque<PocsagMessage> SDRunoPlugin_POCSAG::DrainMessages()
{
    std::lock_guard<std::mutex> lock(messagesMutex_);
    std::deque<PocsagMessage> result;
    result.swap(pendingMessages_);
    return result;
}

void SDRunoPlugin_POCSAG::RequestClose()
{
    if (!closing_.exchange(true))
        controller_.RequestUnload(this);
}

void SDRunoPlugin_POCSAG::OnDecodedMessage(const PocsagMessage& message)
{
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        pendingMessages_.push_back(message);
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
}
