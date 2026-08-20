#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

class SDRunoPlugin_POCSAG;

class MappingWindow
{
public:
    MappingWindow(SDRunoPlugin_POCSAG& plugin, HWND owner);
    ~MappingWindow();

    void Show();
    void Close();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    void ReloadMappings();
    void LoadSelectedMapping();
    bool ReadEditor(std::uint32_t& address, int& function, std::string& type) const;

    SDRunoPlugin_POCSAG& plugin_;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HWND list_ = nullptr;
    HWND address_ = nullptr;
    HWND function_ = nullptr;
    HWND type_ = nullptr;
};
