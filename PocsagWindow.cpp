#include "PocsagWindow.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "PocsagDecoder.h"
#include "MappingWindow.h"
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
constexpr int kMappingButton = 1005;
constexpr int kCopyRawButton = 1006;
constexpr int kExportRawButton = 1007;
constexpr int kPolarityCombo = 1008;
constexpr int kDetailsButton = 1009;
constexpr int kOpenMapButton = 1010;
constexpr int kSetNumeric = 1101;
constexpr int kSetAlpha = 1102;

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

std::wstring FormatKilometer(double value)
{
    std::wostringstream text;
    text << L"K" << std::fixed << std::setprecision(1) << value;
    return text.str();
}

std::wstring FormatCoordinate(double longitude, double latitude)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(6) << longitude << L", " << latitude;
    return text.str();
}

std::wstring JoinHexWide(const std::vector<std::uint32_t>& words)
{
    std::wostringstream text;
    text << std::uppercase << std::hex << std::setfill(L'0');
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (i)
            text << L' ';
        text << std::setw(8) << words[i];
    }
    return text.str();
}
}

PocsagWindow::PocsagWindow(SDRunoPlugin_POCSAG& plugin) : plugin_(plugin)
{
}

PocsagWindow::~PocsagWindow() = default;

void PocsagWindow::Run()
{
    uiThreadId_ = GetCurrentThreadId();
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
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
        CW_USEDEFAULT, CW_USEDEFAULT, 1380, 620,
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
        {
            UpdateStatus();
            SyncRailwayTargets();
        }
        return 0;

    case kMessagesAvailable:
        DrainMessages();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kPresetButton:
            plugin_.ApplyRailwayPreset();
            SendMessageW(baud_, CB_SETCURSEL, 1, 0);
            SendMessageW(polarity_, CB_SETCURSEL, 2, 0);
            UpdateStatus();
            return 0;
        case kClearButton:
            ListView_DeleteAllItems(list_);
            ListView_DeleteAllItems(railwayList_);
            rowMessages_.clear();
            railwayRows_.clear();
            plugin_.ClearRailwayTargets();
            return 0;
        case kMappingButton:
            if (!mappingWindow_)
                mappingWindow_ = std::make_unique<MappingWindow>(plugin_, window_);
            mappingWindow_->Show();
            return 0;
        case kCopyRawButton:
            CopyLatestRaw();
            return 0;
        case kExportRawButton:
            ExportRawLogs();
            return 0;
        case kDetailsButton:
            ShowSelectedRailwayDetails();
            return 0;
        case kOpenMapButton:
            {
                std::wstring error;
                if (!plugin_.OpenRealtimeMap(error))
                    MessageBoxW(window_, error.c_str(), L"打开实时地图", MB_OK | MB_ICONWARNING);
            }
            return 0;
        case kSetNumeric:
        case kSetAlpha:
            ApplyMappingToRow(ListView_GetNextItem(list_, -1, LVNI_SELECTED),
                              LOWORD(wParam) == kSetNumeric ? "NUMERIC" : "ALPHA");
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
        case kPolarityCombo:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                const int selected = static_cast<int>(SendMessageW(polarity_, CB_GETCURSEL, 0, 0));
                const PocsagPolarity modes[] = {
                    PocsagPolarity::Auto, PocsagPolarity::Normal, PocsagPolarity::Inverted
                };
                if (selected >= 0 && selected < 3)
                    plugin_.SetPolarity(modes[selected]);
            }
            return 0;
        case kBeepCheck:
            plugin_.SetBeepEnabled(SendMessageW(beep_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == tab_ &&
            reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE)
        {
            UpdateTabVisibility();
            return 0;
        }
        if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == list_ &&
            reinterpret_cast<NMHDR*>(lParam)->code == NM_RCLICK)
        {
            ShowMessageContextMenu();
            return 0;
        }
        if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == railwayList_ &&
            reinterpret_cast<NMHDR*>(lParam)->code == NM_DBLCLK)
        {
            ShowSelectedRailwayDetails();
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, kStatusTimer);
        if (mappingWindow_)
            mappingWindow_->Close();
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PocsagWindow::CreateControls()
{
    HWND preset = CreateWindowW(L"BUTTON", L"一键设置 821.2375 MHz / NFM / 15 kHz",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 280, 28,
        window_, reinterpret_cast<HMENU>(kPresetButton), nullptr, nullptr);
    SetDefaultFont(preset);

    HWND baudLabel = CreateWindowW(L"STATIC", L"速率:", WS_CHILD | WS_VISIBLE,
        300, 16, 35, 20, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(baudLabel);

    baud_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        335, 10, 70, 200, window_, reinterpret_cast<HMENU>(kBaudCombo), nullptr, nullptr);
    SetDefaultFont(baud_);
    SendMessageW(baud_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"512"));
    SendMessageW(baud_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1200"));
    SendMessageW(baud_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2400"));
    SendMessageW(baud_, CB_SETCURSEL, 1, 0);

    HWND polarityLabel = CreateWindowW(L"STATIC", L"极性:", WS_CHILD | WS_VISIBLE,
        415, 16, 40, 20, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(polarityLabel);

    polarity_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        455, 10, 90, 200, window_, reinterpret_cast<HMENU>(kPolarityCombo), nullptr, nullptr);
    SetDefaultFont(polarity_);
    SendMessageW(polarity_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AUTO"));
    SendMessageW(polarity_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"NORMAL"));
    SendMessageW(polarity_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"INVERTED"));
    SendMessageW(polarity_, CB_SETCURSEL, static_cast<int>(plugin_.GetPolarity()), 0);

    beep_ = CreateWindowW(L"BUTTON", L"收到消息蜂鸣", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        555, 12, 110, 24, window_, reinterpret_cast<HMENU>(kBeepCheck), nullptr, nullptr);
    SetDefaultFont(beep_);
    SendMessageW(beep_, BM_SETCHECK, plugin_.GetBeepEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND clear = CreateWindowW(L"BUTTON", L"清空记录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        675, 10, 75, 28, window_, reinterpret_cast<HMENU>(kClearButton), nullptr, nullptr);
    SetDefaultFont(clear);

    HWND mapping = CreateWindowW(L"BUTTON", L"正文映射", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        760, 10, 90, 28, window_, reinterpret_cast<HMENU>(kMappingButton), nullptr, nullptr);
    SetDefaultFont(mapping);

    HWND copyRaw = CreateWindowW(L"BUTTON", L"复制最近 RAW", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        860, 10, 120, 28, window_, reinterpret_cast<HMENU>(kCopyRawButton), nullptr, nullptr);
    SetDefaultFont(copyRaw);

    HWND exportRaw = CreateWindowW(L"BUTTON", L"导出 RAW 日志", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        990, 10, 125, 28, window_, reinterpret_cast<HMENU>(kExportRawButton), nullptr, nullptr);
    SetDefaultFont(exportRaw);

    HWND details = CreateWindowW(L"BUTTON", L"协议 / RAW 详情", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        1125, 10, 125, 28, window_, reinterpret_cast<HMENU>(kDetailsButton), nullptr, nullptr);
    SetDefaultFont(details);

    HWND openMap = CreateWindowW(L"BUTTON", L"打开实时地图", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        1260, 10, 100, 28, window_, reinterpret_cast<HMENU>(kOpenMapButton), nullptr, nullptr);
    SetDefaultFont(openMap);

    status_ = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        10, 48, 1340, 22, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(status_);

    tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        10, 76, 1340, 490, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(tab_);
    TCITEMW tabItem{};
    tabItem.mask = TCIF_TEXT;
    tabItem.pszText = const_cast<wchar_t*>(L"列车接近预警");
    TabCtrl_InsertItem(tab_, 0, &tabItem);
    tabItem.pszText = const_cast<wchar_t*>(L"POCSAG 原始消息");
    TabCtrl_InsertItem(tab_, 1, &tabItem);

    railwayList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        18, 106, 1324, 450, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(railwayList_);
    ListView_SetExtendedListViewStyle(railwayList_,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const wchar_t* railwayNames[] = {
        L"最后更新", L"车次", L"速度", L"公里标", L"机车号", L"端号", L"线路", L"经纬度", L"数据完整度", L"质量"
    };
    const int railwayWidths[] = { 75, 85, 80, 85, 100, 55, 110, 205, 95, 70 };
    for (int i = 0; i < 10; ++i)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(railwayNames[i]);
        column.cx = railwayWidths[i];
        column.iSubItem = i;
        ListView_InsertColumn(railwayList_, i, &column);
    }

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
        18, 106, 1324, 450, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(list_);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    const wchar_t* names[] = {
        L"时间", L"地址 / RIC", L"功能", L"MsgCW", L"结束原因", L"类型",
        L"车次", L"速度", L"公里标", L"消息", L"纠错"
    };
    const int widths[] = { 70, 90, 45, 55, 95, 105, 70, 65, 85, 390, 50 };
    for (int i = 0; i < 11; ++i)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(names[i]);
        column.cx = widths[i];
        column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }
    UpdateTabVisibility();
}

void PocsagWindow::LayoutControls(int width, int height)
{
    if (status_)
        MoveWindow(status_, 10, 48, std::max(100, width - 20), 22, TRUE);
    if (tab_)
        MoveWindow(tab_, 10, 76, std::max(100, width - 20), std::max(100, height - 86), TRUE);
    if (railwayList_)
        MoveWindow(railwayList_, 18, 106, std::max(100, width - 36), std::max(80, height - 124), TRUE);
    if (list_)
        MoveWindow(list_, 18, 106, std::max(100, width - 36), std::max(80, height - 124), TRUE);
}

void PocsagWindow::DrainMessages()
{
    auto messages = plugin_.DrainMessages();
    for (const auto& message : messages)
        AddMessage(message);
    SyncRailwayTargets();
}

void PocsagWindow::UpdateTabVisibility()
{
    if (!tab_ || !railwayList_ || !list_)
        return;
    const int selected = TabCtrl_GetCurSel(tab_);
    ShowWindow(railwayList_, selected == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(list_, selected == 1 ? SW_SHOW : SW_HIDE);
}

void PocsagWindow::SyncRailwayTargets()
{
    const auto targets = plugin_.GetRailwayTargets();
    for (const auto& target : targets)
    {
        const auto existing = std::find_if(railwayRows_.begin(), railwayRows_.end(),
            [&target](const RailwayTarget& row) { return row.targetUid == target.targetUid; });
        if (existing == railwayRows_.end())
            AddRailwayTarget(target);
        else
        {
            const int row = static_cast<int>(std::distance(railwayRows_.begin(), existing));
            *existing = target;
            UpdateRailwayTargetRow(row, target);
        }
    }
    for (int row = static_cast<int>(railwayRows_.size()) - 1; row >= 0; --row)
    {
        const std::uint64_t uid = railwayRows_[static_cast<std::size_t>(row)].targetUid;
        const bool stillActive = std::any_of(targets.begin(), targets.end(),
            [uid](const RailwayTarget& target) { return target.targetUid == uid; });
        if (!stillActive)
        {
            ListView_DeleteItem(railwayList_, row);
            railwayRows_.erase(railwayRows_.begin() + row);
        }
    }
}

void PocsagWindow::AddRailwayTarget(const RailwayTarget& target)
{
    std::time_t timeValue = static_cast<std::time_t>(target.lastAnyUpdateUnixMs / 1000);
    std::tm local{};
    localtime_s(&local, &timeValue);
    wchar_t timeText[16]{};
    wcsftime(timeText, _countof(timeText), L"%H:%M:%S", &local);

    const int row = ListView_GetItemCount(railwayList_);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = timeText;
    ListView_InsertItem(railwayList_, &item);

    railwayRows_.push_back(target);
    UpdateRailwayTargetRow(row, target);
}

void PocsagWindow::UpdateRailwayTargetRow(int row, const RailwayTarget& target)
{
    std::time_t timeValue = static_cast<std::time_t>(target.lastAnyUpdateUnixMs / 1000);
    std::tm local{};
    localtime_s(&local, &timeValue);
    wchar_t timeText[16]{};
    wcsftime(timeText, _countof(timeText), L"%H:%M:%S", &local);
    ListView_SetItemText(railwayList_, row, 0, timeText);

    const std::wstring train = target.fullTrainNumber.empty() ? L"--" : ToWide(target.fullTrainNumber);
    const std::wstring speed = target.hasSpeed ? std::to_wstring(target.speedKmh) + L" km/h" : L"--";
    const std::wstring kilometer = target.hasKilometer ? FormatKilometer(target.kilometerKm) : L"--";
    const std::wstring locomotive = target.locomotiveId.empty() ? L"--" : ToWide(target.locomotiveId);
    const std::wstring locomotiveEnd = target.locomotiveEnd.empty() ? L"--" : ToWide(target.locomotiveEnd);
    const std::wstring line = target.lineName.empty() ? L"--" : ToWide(target.lineName);
    const std::wstring coordinates = target.hasPosition
        ? FormatCoordinate(target.longitudeDeg, target.latitudeDeg) : L"--";
    const std::wstring completeness = ToWide(target.dataCompleteness);
    const std::wstring confidence = ToWide(target.stale ? "STALE" : target.quality);
    ListView_SetItemText(railwayList_, row, 1, const_cast<wchar_t*>(train.c_str()));
    ListView_SetItemText(railwayList_, row, 2, const_cast<wchar_t*>(speed.c_str()));
    ListView_SetItemText(railwayList_, row, 3, const_cast<wchar_t*>(kilometer.c_str()));
    ListView_SetItemText(railwayList_, row, 4, const_cast<wchar_t*>(locomotive.c_str()));
    ListView_SetItemText(railwayList_, row, 5, const_cast<wchar_t*>(locomotiveEnd.c_str()));
    ListView_SetItemText(railwayList_, row, 6, const_cast<wchar_t*>(line.c_str()));
    ListView_SetItemText(railwayList_, row, 7, const_cast<wchar_t*>(coordinates.c_str()));
    ListView_SetItemText(railwayList_, row, 8, const_cast<wchar_t*>(completeness.c_str()));
    ListView_SetItemText(railwayList_, row, 9, const_cast<wchar_t*>(confidence.c_str()));
}

void PocsagWindow::ShowSelectedRailwayDetails()
{
    const int row = ListView_GetNextItem(railwayList_, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(railwayRows_.size()))
    {
        MessageBoxW(window_, L"请先在“列车接近预警”页选择一条记录。",
                    L"协议 / RAW 详情", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto& message = railwayRows_[static_cast<std::size_t>(row)];
    std::wostringstream details;
    details << L"Target UID: " << message.targetUid
            << L"\n显示 ID: " << ToWide(message.targetId)
            << L"\n创建来源: " << ToWide(message.createdBy)
            << L"\n合并次数: " << message.mergeCount
            << L"\n最近合并原因: " << ToWide(message.lastMergeReason)
            << L"\nTransmission ID: " << message.transmissionId
            << L"\n配对方式: " << ToWide(message.pairingMethod)
            << L"\n数据完整度: " << ToWide(message.dataCompleteness)
            << L"\n\n[BASIC / RIC 1234000]"
            << (message.hasBasic ? L"" : L"\n未收到合法 BASIC")
            << L"\nFunction: " << message.basic.function
            << L"\nNumeric: " << ToWide(message.basic.numericText)
            << L"\nMessage hex: " << ToWide(message.basic.rawHex)
            << L"\nMessage bits: " << ToWide(message.basic.messageBits)
            << L"\nRaw codewords: " << JoinHexWide(message.basic.rawCodewords)
            << L"\nCorrected codewords: " << JoinHexWide(message.basic.correctedCodewords)
            << L"\n纠错 bit: " << message.basic.correctedBits
            << L"\n\n[EXT / RIC 1234002]"
            << (message.hasExtension ? L"" : L"\n未收到合法 EXT")
            << L"\nFunction: " << message.extension.function
            << L"\nNormalized railway hex: " << ToWide(message.extension.railwayExtNormalizedHex)
            << L"\nLon RAW: " << ToWide(message.longitudeRaw)
            << L"\nLat RAW: " << ToWide(message.latitudeRaw)
            << L"\nLon DM: " << ToWide(message.longitudeDegreeMinute)
            << L"\nLat DM: " << ToWide(message.latitudeDegreeMinute)
            << L"\nDecimal: " << std::fixed << std::setprecision(9)
            << message.longitudeDeg << L", " << message.latitudeDeg
            << L"\nMessage hex: " << ToWide(message.extension.rawHex)
            << L"\nMessage bits: " << ToWide(message.extension.messageBits)
            << L"\nRaw codewords: " << JoinHexWide(message.extension.rawCodewords)
            << L"\nCorrected codewords: " << JoinHexWide(message.extension.correctedCodewords)
            << L"\n纠错 bit: " << message.extension.correctedBits
            << L"\n扩展尾字段: " << ToWide(message.railwayAuxRaw);
    MessageBoxW(window_, details.str().c_str(), L"铁路预警协议 / RAW 详情", MB_OK | MB_ICONINFORMATION);
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
    const std::wstring codewordCount = std::to_wstring(message.messageCodewordCount);
    const std::wstring finalizeReason = ToWide(message.finalizeReason);
    const std::wstring type = ToWide(message.type);
    const std::wstring train = ToWide(message.decodedTrain);
    const std::wstring speed = message.hasDecodedSpeed
        ? std::to_wstring(message.decodedSpeedKmh) : L"";
    const std::wstring kilometer = message.hasDecodedKilometer
        ? FormatKilometer(message.decodedKilometer) : L"";
    const std::wstring text = ToWide(message.text);
    const std::wstring corrected = std::to_wstring(message.correctedBits);
    ListView_SetItemText(list_, row, 1, const_cast<wchar_t*>(address.c_str()));
    ListView_SetItemText(list_, row, 2, const_cast<wchar_t*>(function.c_str()));
    ListView_SetItemText(list_, row, 3, const_cast<wchar_t*>(codewordCount.c_str()));
    ListView_SetItemText(list_, row, 4, const_cast<wchar_t*>(finalizeReason.c_str()));
    ListView_SetItemText(list_, row, 5, const_cast<wchar_t*>(type.c_str()));
    ListView_SetItemText(list_, row, 6, const_cast<wchar_t*>(train.c_str()));
    ListView_SetItemText(list_, row, 7, const_cast<wchar_t*>(speed.c_str()));
    ListView_SetItemText(list_, row, 8, const_cast<wchar_t*>(kilometer.c_str()));
    ListView_SetItemText(list_, row, 9, const_cast<wchar_t*>(text.c_str()));
    ListView_SetItemText(list_, row, 10, const_cast<wchar_t*>(corrected.c_str()));
    rowMessages_.push_back(message);
    ListView_EnsureVisible(list_, row, FALSE);
}

void PocsagWindow::ShowMessageContextMenu()
{
    POINT point{};
    GetCursorPos(&point);
    POINT clientPoint = point;
    ScreenToClient(list_, &clientPoint);
    LVHITTESTINFO hit{};
    hit.pt = clientPoint;
    const int row = ListView_HitTest(list_, &hit);
    if (row < 0 || row >= static_cast<int>(rowMessages_.size()) || rowMessages_[row].rawHex.empty())
        return;

    ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kSetNumeric, L"设置正文解码为 NUMERIC");
    AppendMenuW(menu, MF_STRING, kSetAlpha, L"设置正文解码为 ALPHA");
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command == kSetNumeric || command == kSetAlpha)
        SendMessageW(window_, WM_COMMAND, command, 0);
}

void PocsagWindow::ApplyMappingToRow(int row, const char* type)
{
    if (row < 0 || row >= static_cast<int>(rowMessages_.size()))
        return;
    const auto& original = rowMessages_[row];
    if (original.rawHex.empty())
        return;
    if (!plugin_.SetMessageMapping(original.address, original.function, type))
    {
        MessageBoxW(window_, L"映射保存失败，请检查 AppData 目录写入权限。",
                    L"正文映射", MB_OK | MB_ICONERROR);
        return;
    }
    rowMessages_[row] = plugin_.ApplyMessageMapping(original);
    UpdateMessageRow(row, rowMessages_[row]);
}

void PocsagWindow::UpdateMessageRow(int row, const PocsagMessage& message)
{
    const std::wstring type = ToWide(message.type);
    const std::wstring train = ToWide(message.decodedTrain);
    const std::wstring speed = message.hasDecodedSpeed
        ? std::to_wstring(message.decodedSpeedKmh) : L"";
    const std::wstring kilometer = message.hasDecodedKilometer
        ? FormatKilometer(message.decodedKilometer) : L"";
    const std::wstring text = ToWide(message.text);
    ListView_SetItemText(list_, row, 5, const_cast<wchar_t*>(type.c_str()));
    ListView_SetItemText(list_, row, 6, const_cast<wchar_t*>(train.c_str()));
    ListView_SetItemText(list_, row, 7, const_cast<wchar_t*>(speed.c_str()));
    ListView_SetItemText(list_, row, 8, const_cast<wchar_t*>(kilometer.c_str()));
    ListView_SetItemText(list_, row, 9, const_cast<wchar_t*>(text.c_str()));
}

void PocsagWindow::CopyLatestRaw()
{
    const std::string json = plugin_.GetLatestRawJson();
    if (json.empty())
    {
        MessageBoxW(window_, L"当前还没有完成的 POCSAG transmission。", L"复制最近 RAW",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring wide = ToWide(json);
    if (!OpenClipboard(window_))
        return;
    EmptyClipboard();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory)
    {
        void* target = GlobalLock(memory);
        if (target)
        {
            memcpy(target, wide.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        }
        else
        {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void PocsagWindow::ExportRawLogs()
{
    wchar_t path[MAX_PATH] = L"pocsag_raw_log.jsonl";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"JSONL 日志 (*.jsonl)\0*.jsonl\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = _countof(path);
    dialog.lpstrDefExt = L"jsonl";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog))
        return;

    std::wstring jsonlPath;
    std::wstring csvPath;
    std::string error;
    if (!plugin_.ExportRawLogs(path, jsonlPath, csvPath, error))
    {
        MessageBoxW(window_, ToWide(error).c_str(), L"导出 RAW 日志", MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring message = L"已导出：\n" + jsonlPath + L"\n" + csvPath;
    MessageBoxW(window_, message.c_str(), L"导出 RAW 日志", MB_OK | MB_ICONINFORMATION);
}

void PocsagWindow::UpdateStatus()
{
    if (status_)
        SetWindowTextW(status_, ToWide(plugin_.GetStatusText()).c_str());
}
