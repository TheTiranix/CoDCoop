// Instalador del coop: un solo .exe con el mod adentro (installer.rc) que lo copia en la carpeta del juego.
// Mira que juego hay en la carpeta y copia lo de ese: Call of Duty (CoDSP.exe, y United Offensive si esta CoDUOSP.exe)
// o Call of Duty 2 (CoD2SP_s.exe).
// Uso: "Instalar CoD Coop.exe" [carpeta...]  (con carpetas no pregunta donde; asi se relanza como administrador)
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlobj.h>
#include <iterator>
#include <string>
#include <vector>

static const wchar_t* TITLE = L"Call of Duty Coop - Setup";
static const DWORD CODSP_SIZE = 1716224;     // CoDSP.exe 1.5: el mod usa direcciones fijas de ese exe
static const DWORD COD2SP_SIZE = 1761280;    // CoD2SP_s.exe 1.3, idem

struct File { int id; const wchar_t* dest; };
static const File COD1_FILES[] = {
    {1, L"ddraw.dll"}, {2, L"codcoop_core.dll"}, {3, L"main\\pakz_coop.pk3"}, {4, L"CoD Coop README.txt"},
    {5, L"steam_api.dll"},
};
static const File UO_FILES[] = {{6, L"uo\\pakz_coop.pk3"}};
static const File COD2_FILES[] = {{1, L"ddraw.dll"}, {7, L"cod2coop.dll"}, {8, L"main\\zz_coop2.iwd"},
                                  {4, L"CoD Coop README.txt"}};

enum { COD1 = 1, UO = 2, COD2 = 4 };

static int Msg(UINT flags, const std::wstring& text) { return MessageBoxW(nullptr, text.c_str(), TITLE, flags); }

static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

static int GamesIn(const std::wstring& dir)
{
    bool cod1 = Exists(dir + L"\\CoDSP.exe");  // United Offensive es una expansion: va en la misma carpeta
    return (cod1 ? COD1 : 0) | (cod1 && Exists(dir + L"\\CoDUOSP.exe") ? UO : 0) |
           (Exists(dir + L"\\CoD2SP_s.exe") ? COD2 : 0);
}

static std::wstring GameNames(int games)
{
    std::wstring s;
    if (games & COD1) s += L"Call of Duty";
    if (games & UO) s += L" + United Offensive";
    if (games & COD2) s += std::wstring(s.empty() ? L"" : L" + ") + L"Call of Duty 2";
    return s;
}

static std::wstring RegistryDir(const wchar_t* key)
{
    // el instalador es de 32 bits: esto lee WOW6432Node, donde lo pone el instalador del juego
    wchar_t buf[MAX_PATH] = L"";
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    std::wstring dir = buf;
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    return dir;
}

static std::wstring PickDir()
{
    BROWSEINFOW bi{};
    bi.lpszTitle = L"Choose the game folder (the one with CoDSP.exe or CoD2SP_s.exe):";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    wchar_t buf[MAX_PATH];
    bool ok = SHGetPathFromIDListW(pidl, buf);
    CoTaskMemFree(pidl);
    return ok ? buf : L"";
}

static std::string ReadAll(const std::wstring& path)
{
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    data.resize(GetFileSize(f, nullptr));
    DWORD got = 0;
    ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    return data;
}

static std::string Resource(int id)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    return g ? std::string((const char*)LockResource(g), SizeofResource(nullptr, r)) : std::string();
}

// false con GetLastError() del archivo
static bool WriteResource(int id, const std::wstring& path)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    if (!g) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = SizeofResource(nullptr, r), written = 0;
    bool ok = WriteFile(f, LockResource(g), size, &written, nullptr) && written == size;
    DWORD err = GetLastError();
    CloseHandle(f);
    SetLastError(err);
    return ok;
}

static bool WrongSize(const std::wstring& exe, DWORD size)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow != size;
}

enum Result { DONE, FAILED, RELAUNCHED };

// copia los archivos; sin permiso en una carpeta protegida (Archivos de programa) se relanza como administrador con
// todas las carpetas
static Result Install(const std::wstring& dir, int games, const std::vector<std::wstring>& all)
{
    // un ddraw.dll o steam_api.dll que no es el nuestro (otro mod, arreglo de video) se guarda como .bak
    std::wstring ddraw = dir + L"\\ddraw.dll", steamDll = dir + L"\\steam_api.dll";
    if (Exists(ddraw) && !Exists(ddraw + L".bak") && ReadAll(ddraw).find("codcoop_core.dll") == std::string::npos)
        MoveFileW(ddraw.c_str(), (ddraw + L".bak").c_str());
    if ((games & COD1) && Exists(steamDll) && !Exists(steamDll + L".bak") && ReadAll(steamDll) != Resource(5))
        MoveFileW(steamDll.c_str(), (steamDll + L".bak").c_str());

    std::vector<File> files;
    if (games & COD1) files.insert(files.end(), std::begin(COD1_FILES), std::end(COD1_FILES));
    if (games & UO) files.insert(files.end(), std::begin(UO_FILES), std::end(UO_FILES));
    if (games & COD2) files.insert(files.end(), std::begin(COD2_FILES), std::end(COD2_FILES));
    if (games & (COD1 | COD2)) CreateDirectoryW((dir + L"\\main").c_str(), nullptr);
    if (games & UO) CreateDirectoryW((dir + L"\\uo").c_str(), nullptr);
    for (const File& f : files) {
        if (WriteResource(f.id, dir + L"\\" + f.dest)) continue;
        if (GetLastError() == ERROR_ACCESS_DENIED && !IsUserAnAdmin()) {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring args;
            for (const std::wstring& d : all) args += L"\"" + d + L"\" ";
            if ((INT_PTR)ShellExecuteW(nullptr, L"runas", self, args.c_str(), nullptr, SW_SHOWNORMAL) > 32) return RELAUNCHED;
        }
        Msg(MB_ICONERROR, std::wstring(L"Could not write ") + f.dest + L" to\n" + dir +
                              L"\n\nIf the game is running, close it and try again.");
        return FAILED;
    }
    return DONE;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);

    if (dirs.empty()) {
        // donde los instalo su instalador (Call of Duty y Call of Duty 2 pueden estar los dos)
        std::wstring found;
        for (const wchar_t* key : {L"SOFTWARE\\Activision\\Call of Duty", L"SOFTWARE\\Activision\\Call of Duty 2"}) {
            std::wstring d = RegistryDir(key);
            bool seen = false;
            for (const std::wstring& o : dirs) seen |= !_wcsicmp(o.c_str(), d.c_str());
            if (d.empty() || seen || !GamesIn(d)) continue;
            dirs.push_back(d);
            found += L"\n" + d + L"  (" + GameNames(GamesIn(d)) + L")";
        }
        if (!dirs.empty()) {
            int r = Msg(MB_YESNOCANCEL | MB_ICONQUESTION,
                        L"Co-op will be installed in:" + found + L"\n\nYes: install there\nNo: choose another folder");
            if (r == IDCANCEL) return 0;
            if (r == IDNO) dirs.clear();
        }
        while (dirs.empty()) {
            std::wstring d = PickDir();
            if (d.empty()) return 0;
            if (!GamesIn(d)) {
                Msg(MB_ICONWARNING, L"That folder has no CoDSP.exe or CoD2SP_s.exe.\n"
                                    L"Choose the folder where Call of Duty or Call of Duty 2 is installed.");
                continue;
            }
            dirs.push_back(d);
        }
    }
    for (std::wstring& d : dirs)
        while (!d.empty() && d.back() == L'\\') d.pop_back();  // "C:\" -> "C:" (va entre comillas al relanzar)

    int all = 0;
    for (const std::wstring& dir : dirs) {
        int games = GamesIn(dir);
        if (!games) {
            Msg(MB_ICONERROR, L"No Call of Duty or Call of Duty 2 found in\n" + dir);
            return 1;
        }
        if ((games & COD1) && WrongSize(dir + L"\\CoDSP.exe", CODSP_SIZE) &&
            Msg(MB_YESNO | MB_ICONWARNING, L"Your CoDSP.exe is not version 1.5, the one co-op needs: it may not work."
                                           L"\n\nInstall anyway?") != IDYES)
            return 0;
        if ((games & COD2) && WrongSize(dir + L"\\CoD2SP_s.exe", COD2SP_SIZE) &&
            Msg(MB_YESNO | MB_ICONWARNING, L"Your CoD2SP_s.exe is not version 1.3, the one co-op needs: it may not work."
                                           L"\n\nInstall anyway?") != IDYES)
            return 0;
        Result r = Install(dir, games, dirs);
        if (r == RELAUNCHED) return 0;
        if (r == FAILED) return 1;
        all |= games;
    }

    std::wstring done = L"Done, co-op is installed.\n";
    if (all & COD1)
        done += std::wstring(L"\nCall of Duty: start it as usual, the main menu has a \"Co-op\" button.") +
                (all & UO ? L" United Offensive has it too." : L"");
    if (all & COD2)
        done += L"\nCall of Duty 2: start it as usual, the main menu has a \"Co-op\" button.";
    Msg(MB_ICONINFORMATION, done + L"\n\nInstructions are in \"CoD Coop README.txt\", in the game folder.");
    return 0;
}
