#pragma once

#include <Windows.h>

class SDRunoPlugin_POCSAG;
struct PocsagMessage;

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
    void UpdateStatus();

    SDRunoPlugin_POCSAG& plugin_;
    HWND window_ = nullptr;
    HWND status_ = nullptr;
    HWND list_ = nullptr;
    HWND baud_ = nullptr;
    HWND beep_ = nullptr;
    DWORD uiThreadId_ = 0;
};
