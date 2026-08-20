#include "MappingWindow.h"

#include <CommCtrl.h>
#include <cwchar>
#include <string>

#include "SDRunoPlugin_POCSAG.h"

namespace
{
constexpr wchar_t kMappingWindowClass[] = L"SDRunoPocsagMappingWindow";
constexpr int kMappingList = 2001;
constexpr int kAddressEdit = 2002;
constexpr int kFunctionCombo = 2003;
constexpr int kTypeCombo = 2004;
constexpr int kSaveButton = 2005;
constexpr int kDeleteButton = 2006;
constexpr int kCloseButton = 2007;

void SetDefaultFont(HWND control)
{
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

std::string ToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
}

MappingWindow::MappingWindow(SDRunoPlugin_POCSAG& plugin, HWND owner)
    : plugin_(plugin), owner_(owner)
{
}

MappingWindow::~MappingWindow()
{
    Close();
}

void MappingWindow::Show()
{
    if (window_)
    {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        ReloadMappings();
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kMappingWindowClass;
    RegisterClassExW(&windowClass);

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kMappingWindowClass, L"POCSAG 正文解码映射",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 430,
        owner_, nullptr, windowClass.hInstance, this);
    if (window_)
    {
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }
}

void MappingWindow::Close()
{
    if (window_)
        DestroyWindow(window_);
}

LRESULT CALLBACK MappingWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    MappingWindow* self = reinterpret_cast<MappingWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MappingWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(window, message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT MappingWindow::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        CreateControls();
        ReloadMappings();
        return 0;

    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == kMappingList &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED)
            LoadSelectedMapping();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kSaveButton:
            {
                std::uint32_t address = 0;
                int function = 0;
                std::string type;
                if (!ReadEditor(address, function, type))
                {
                    MessageBoxW(window, L"请输入有效的 RIC（0-2097151）、功能（0-3）和解码类型。",
                                L"正文映射", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                if (!plugin_.SetMessageMapping(address, function, type))
                    MessageBoxW(window, L"映射保存失败，请检查 AppData 目录写入权限。",
                                L"正文映射", MB_OK | MB_ICONERROR);
                ReloadMappings();
                return 0;
            }
        case kDeleteButton:
            {
                const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
                if (row < 0)
                    return 0;
                wchar_t addressText[32]{};
                wchar_t functionText[8]{};
                ListView_GetItemText(list_, row, 0, addressText, _countof(addressText));
                ListView_GetItemText(list_, row, 1, functionText, _countof(functionText));
                plugin_.RemoveMessageMapping(static_cast<std::uint32_t>(wcstoul(addressText, nullptr, 10)),
                                             static_cast<int>(wcstol(functionText, nullptr, 10)));
                ReloadMappings();
                return 0;
            }
        case kCloseButton:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_NCDESTROY:
        window_ = nullptr;
        list_ = nullptr;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void MappingWindow::CreateControls()
{
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        12, 12, 578, 280, window_, reinterpret_cast<HMENU>(kMappingList), nullptr, nullptr);
    SetDefaultFont(list_);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    const wchar_t* names[] = { L"地址 / RIC", L"功能", L"正文解码" };
    const int widths[] = { 250, 100, 180 };
    for (int index = 0; index < 3; ++index)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(names[index]);
        column.cx = widths[index];
        column.iSubItem = index;
        ListView_InsertColumn(list_, index, &column);
    }

    HWND addressLabel = CreateWindowW(L"STATIC", L"RIC:", WS_CHILD | WS_VISIBLE,
        12, 308, 35, 22, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(addressLabel);
    address_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
        50, 304, 130, 25, window_, reinterpret_cast<HMENU>(kAddressEdit), nullptr, nullptr);
    SetDefaultFont(address_);

    HWND functionLabel = CreateWindowW(L"STATIC", L"功能:", WS_CHILD | WS_VISIBLE,
        194, 308, 40, 22, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(functionLabel);
    function_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        238, 304, 75, 120, window_, reinterpret_cast<HMENU>(kFunctionCombo), nullptr, nullptr);
    SetDefaultFont(function_);
    for (int value = 0; value < 4; ++value)
    {
        const std::wstring text = std::to_wstring(value);
        SendMessageW(function_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(function_, CB_SETCURSEL, 0, 0);

    HWND typeLabel = CreateWindowW(L"STATIC", L"解码:", WS_CHILD | WS_VISIBLE,
        328, 308, 40, 22, window_, nullptr, nullptr, nullptr);
    SetDefaultFont(typeLabel);
    type_ = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        372, 304, 120, 100, window_, reinterpret_cast<HMENU>(kTypeCombo), nullptr, nullptr);
    SetDefaultFont(type_);
    SendMessageW(type_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"NUMERIC"));
    SendMessageW(type_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ALPHA"));
    SendMessageW(type_, CB_SETCURSEL, 0, 0);

    HWND save = CreateWindowW(L"BUTTON", L"添加 / 更新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        12, 346, 120, 30, window_, reinterpret_cast<HMENU>(kSaveButton), nullptr, nullptr);
    HWND remove = CreateWindowW(L"BUTTON", L"删除选中", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        144, 346, 105, 30, window_, reinterpret_cast<HMENU>(kDeleteButton), nullptr, nullptr);
    HWND close = CreateWindowW(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        485, 346, 105, 30, window_, reinterpret_cast<HMENU>(kCloseButton), nullptr, nullptr);
    SetDefaultFont(save);
    SetDefaultFont(remove);
    SetDefaultFont(close);
}

void MappingWindow::ReloadMappings()
{
    if (!list_)
        return;
    ListView_DeleteAllItems(list_);
    const auto mappings = plugin_.GetMessageMappings();
    for (const auto& mapping : mappings)
    {
        const int row = ListView_GetItemCount(list_);
        const std::wstring address = std::to_wstring(mapping.address);
        const std::wstring function = std::to_wstring(mapping.function);
        const std::wstring type(mapping.type.begin(), mapping.type.end());
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(address.c_str());
        ListView_InsertItem(list_, &item);
        ListView_SetItemText(list_, row, 1, const_cast<wchar_t*>(function.c_str()));
        ListView_SetItemText(list_, row, 2, const_cast<wchar_t*>(type.c_str()));
    }
}

void MappingWindow::LoadSelectedMapping()
{
    const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (row < 0)
        return;
    wchar_t addressText[32]{};
    wchar_t functionText[8]{};
    wchar_t typeText[16]{};
    ListView_GetItemText(list_, row, 0, addressText, _countof(addressText));
    ListView_GetItemText(list_, row, 1, functionText, _countof(functionText));
    ListView_GetItemText(list_, row, 2, typeText, _countof(typeText));
    SetWindowTextW(address_, addressText);
    SendMessageW(function_, CB_SETCURSEL, wcstol(functionText, nullptr, 10), 0);
    SendMessageW(type_, CB_SETCURSEL, wcscmp(typeText, L"ALPHA") == 0 ? 1 : 0, 0);
}

bool MappingWindow::ReadEditor(std::uint32_t& address, int& function, std::string& type) const
{
    wchar_t addressText[32]{};
    GetWindowTextW(address_, addressText, _countof(addressText));
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(addressText, &end, 10);
    if (addressText[0] == L'\0' || (end && *end != L'\0') || value > 2097151ul)
        return false;
    address = static_cast<std::uint32_t>(value);
    function = static_cast<int>(SendMessageW(function_, CB_GETCURSEL, 0, 0));
    const int typeIndex = static_cast<int>(SendMessageW(type_, CB_GETCURSEL, 0, 0));
    if (function < 0 || function > 3 || typeIndex < 0 || typeIndex > 1)
        return false;
    type = typeIndex == 0 ? "NUMERIC" : "ALPHA";
    return true;
}
