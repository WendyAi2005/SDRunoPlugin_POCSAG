#pragma once

#include <Windows.h>
#include <memory>
#include <vector>

class SDRunoPlugin_POCSAG;
class MappingWindow;
struct PocsagMessage;
struct RailwayTarget;

class PocsagWindow
{
public:
    explicit PocsagWindow(SDRunoPlugin_POCSAG& plugin);
    ~PocsagWindow();

    void Run();
    void Close();
    void NotifyMessagesAvailable();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    void LayoutControls(int width, int height);
    void DrainMessages();
    void AddMessage(const PocsagMessage& message);
    void SyncRailwayTargets();
    void AddRailwayTarget(const RailwayTarget& target);
    void UpdateRailwayTargetRow(int row, const RailwayTarget& target);
    void UpdateTabVisibility();
    void ShowSelectedRailwayDetails();
    void ShowMessageContextMenu();
    void ApplyMappingToRow(int row, const char* type);
    void UpdateMessageRow(int row, const PocsagMessage& message);
    void UpdateStatus();
    void CopyLatestRaw();
    void ExportRawLogs();

    SDRunoPlugin_POCSAG& plugin_;
    HWND window_ = nullptr;
    HWND status_ = nullptr;
    HWND tab_ = nullptr;
    HWND railwayList_ = nullptr;
    HWND list_ = nullptr;
    HWND baud_ = nullptr;
    HWND polarity_ = nullptr;
    HWND beep_ = nullptr;
    std::unique_ptr<MappingWindow> mappingWindow_;
    std::vector<PocsagMessage> rowMessages_;
    std::vector<RailwayTarget> railwayRows_;
    DWORD uiThreadId_ = 0;
};
