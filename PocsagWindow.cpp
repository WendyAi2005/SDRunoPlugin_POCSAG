#include "PocsagWindow.h"

#include <CommCtrl.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>

#include "PocsagDecoder.h"
#include "SDRunoPlugin_POCSAG.h"

namespace
{
constexpr wchar_t kWindowClass[] = L"SDRunoPocsagRailwayAlertWindow";
constexpr UINT kMessagesAvailable = WM_APP + 42;
constexpr UINT_PTR kStatusTimer = 1;
constexpr int kPresetButton = 1001;
constexpr int kClearButton = 1002;
constexpr int kBaudCombo = 1003;
constexpr int kBeepCheck = 1004;

std::wstring ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

void SetDefaultFont(HWND control)
{
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}
}

PocsagWindow::PocsagWindow(SDRunoPlugin_POCSAG& plugin) : plugin_(plugin)
{
}

PocsagWindow::~PocsagWindow() = default;

void PocsagWindow::Run()
{
    uiThreadId_ = GetCurrentThreadId();
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    window_ = CreateWindowExW(
        0, kWindowClass, L"SDRuno POCSAG - 列车接近预警接收",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 880, 500,
        nullptr, nullptr, windowClass.hInstance, this);

    if (!window_)
        return;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window_ = nullptr;
}

void PocsagWindow::Close()
{
    const HWND window = window_;
    if (window)
        PostMessageW(window, WM_CLOSE, 0, 0);
    else if (uiThreadId_)
        PostThreadMessageW(uiThreadId_, WM_QUIT, 0, 0);
}

void PocsagWindow::NotifyMessagesAvailable()
{
    const HWND window = window_;
    if (window)
        PostMessageW(window, kMessagesAvailable, 0, 0);
}

LRESULT CALLBACK PocsagWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    PocsagWindow* self = reinterpret_cast<PocsagWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PocsagWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT PocsagWindow::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        CreateControls();
        SetTimer(window, kStatusTimer, 1000, nullptr);
        UpdateStatus();
        return 0;

    case WM_SIZE:
        LayoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_TIMER:
        if (wParam == kStatusTimer)
            UpdateStatus();
        return 0;

    case kMessagesAvailable:
        DrainMessages();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kPresetButton:
            plugin_.ApplyRailwayPreset();
            UpdateStatus();
            return 0;
        case kClearButton:
            ListView_DeleteAllItems(list_);
            return 0;
        case kBaudCombo:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                const int selected = static_cast<int>(SendMessageW(baud_, CB_GETCURSEL, 0, 0));
                const int rates[] = { 512, 1200, 2400 };
                if (selected >= 0 && selected < 3)
                    plugin_.SetBaud(rates[selected]);
            }
            return 0;
        case kBeepCheck:
            plugin_.SetBeepEnabled(SendMessageW(beep_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, kStatusTimer);
        window_ = nullptr;
        PostQuitMessage(0);
        plugin_.RequestClose();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PocsagWindow::CreateControls()
{
    HWND preset = CreateWindowW(L"BUTTON", L"一键设置 821.2375 MHz / NFM / 15 kHz",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 300, 28,
        window_, reinterpret_cast<HMENU>(kPresetButton), nullptr, nullptr);
    SetDefaultFont(preset);

    HWND baudLabel = CreateWindowW(L"STATIC", L"速率:", WS_CHILD | WS_VISIBLE,
        325, 16, 40, 20, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(baudLabel);

    baud_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        368, 10, 100, 200, window_, reinterpret_cast<HMENU>(kBaudCombo), nullptr, nullptr);
    SetDefaultFont(baud_);
    SendMessageW(baud_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"512"));
    SendMessageW(baud_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1200"));
    SendMessageW(baud_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2400"));
    SendMessageW(baud_, CB_SETCURSEL, 1, 0);

    beep_ = CreateWindowW(L"BUTTON", L"收到消息蜂鸣", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        480, 12, 125, 24, window_, reinterpret_cast<HMENU>(kBeepCheck), nullptr, nullptr);
    SetDefaultFont(beep_);
    SendMessageW(beep_, BM_SETCHECK, plugin_.GetBeepEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND clear = CreateWindowW(L"BUTTON", L"清空记录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        620, 10, 90, 28, window_, reinterpret_cast<HMENU>(kClearButton), nullptr, nullptr);
    SetDefaultFont(clear);

    status_ = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        10, 48, 830, 22, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(status_);

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        10, 76, 840, 370, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(list_);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    const wchar_t* names[] = { L"时间", L"地址 / RIC", L"功能", L"类型", L"消息", L"纠错" };
    const int widths[] = { 86, 100, 55, 80, 430, 55 };
    for (int i = 0; i < 6; ++i)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(names[i]);
        column.cx = widths[i];
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }
}

void PocsagWindow::LayoutControls(int width, int height)
{
    if (status_)
        MoveWindow(status_, 10, 48, std::max(100, width - 20), 22, TRUE);
    if (list_)
        MoveWindow(list_, 10, 76, std::max(100, width - 20), std::max(80, height - 86), TRUE);
}

void PocsagWindow::DrainMessages()
{
    auto messages = plugin_.DrainMessages();
    for (const auto& message : messages)
        AddMessage(message);
}

void PocsagWindow::AddMessage(const PocsagMessage& message)
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t timeText[16]{};
    wcsftime(timeText, _countof(timeText), L"%H:%M:%S", &local);

    const int row = ListView_GetItemCount(list_);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = timeText;
    ListView_InsertItem(list_, &item);

    const std::wstring address = std::to_wstring(message.address);
    const std::wstring function = std::to_wstring(message.function);
    const std::wstring type = ToWide(message.type);
    const std::wstring text = ToWide(message.text);
    const std::wstring corrected = std::to_wstring(message.correctedBits);
    ListView_SetItemText(list_, row, 1, const_cast<wchar_t*>(address.c_str()));
    ListView_SetItemText(list_, row, 2, const_cast<wchar_t*>(function.c_str()));
    ListView_SetItemText(list_, row, 3, const_cast<wchar_t*>(type.c_str()));
    ListView_SetItemText(list_, row, 4, const_cast<wchar_t*>(text.c_str()));
    ListView_SetItemText(list_, row, 5, const_cast<wchar_t*>(corrected.c_str()));
    ListView_EnsureVisible(list_, row, FALSE);
}

void PocsagWindow::UpdateStatus()
{
    if (status_)
        SetWindowTextW(status_, ToWide(plugin_.GetStatusText()).c_str());
}
