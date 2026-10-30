// Instalador del coop: un solo .exe con el mod adentro (installer.rc) que lo copia en la carpeta del juego.
// Uso: "Instalar CoD Coop.exe" [carpeta]  (con carpeta no pregunta donde; asi se relanza como administrador)
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlobj.h>
#include <string>

static const wchar_t* TITLE = L"Call of Duty Coop - Instalador";
static const DWORD CODSP_SIZE = 1716224;  // CoDSP.exe 1.5: el mod usa direcciones fijas de ese exe

struct File { int id; const wchar_t* dest; };
static const File FILES[] = {
    {1, L"ddraw.dll"}, {2, L"codcoop_core.dll"}, {3, L"main\\pakz_coop.pk3"}, {4, L"LEEME CoD Coop.txt"},
    {5, L"steam_api.dll"},
};

static int Msg(UINT flags, const std::wstring& text) { return MessageBoxW(nullptr, text.c_str(), TITLE, flags); }

static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring RegistryDir()
{
    // el instalador es de 32 bits: esto lee WOW6432Node, donde lo pone el instalador del juego
    wchar_t buf[MAX_PATH] = L"";
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Activision\\Call of Duty", L"InstallPath", RRF_RT_REG_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

static std::wstring PickDir()
{
    BROWSEINFOW bi{};
    bi.lpszTitle = L"Elegí la carpeta del Call of Duty (la que tiene CoDSP.exe):";
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dir = argc > 1 ? argv[1] : L"";

    if (dir.empty()) {
        dir = RegistryDir();
        if (!Exists(dir + L"\\CoDSP.exe")) dir.clear();
        else {
            int r = Msg(MB_YESNOCANCEL | MB_ICONQUESTION,
                        L"Se va a instalar el coop en:\n" + dir + L"\n\nSí: instalar ahí\nNo: elegir otra carpeta");
            if (r == IDCANCEL) return 0;
            if (r == IDNO) dir.clear();
        }
        while (dir.empty()) {
            dir = PickDir();
            if (dir.empty()) return 0;
            if (!Exists(dir + L"\\CoDSP.exe")) {
                Msg(MB_ICONWARNING, L"Esa carpeta no tiene CoDSP.exe.\nElegí la carpeta donde está instalado el Call of Duty.");
                dir.clear();
            }
        }
    }
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();  // "C:\" -> "C:" (va entre comillas al relanzar)

    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW((dir + L"\\CoDSP.exe").c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow != CODSP_SIZE &&
        Msg(MB_YESNO | MB_ICONWARNING, L"Tu CoDSP.exe no es el de la versión 1.5 que usa el coop: puede no funcionar."
                                       L"\n\n¿Instalar igual?") != IDYES)
        return 0;

    // un ddraw.dll o steam_api.dll que no es el nuestro (otro mod, arreglo de video) se guarda como .bak
    std::wstring ddraw = dir + L"\\ddraw.dll", steamDll = dir + L"\\steam_api.dll";
    if (Exists(ddraw) && !Exists(ddraw + L".bak") && ReadAll(ddraw).find("codcoop_core.dll") == std::string::npos)
        MoveFileW(ddraw.c_str(), (ddraw + L".bak").c_str());
    if (Exists(steamDll) && !Exists(steamDll + L".bak") && ReadAll(steamDll) != Resource(5))
        MoveFileW(steamDll.c_str(), (steamDll + L".bak").c_str());

    CreateDirectoryW((dir + L"\\main").c_str(), nullptr);
    for (const File& f : FILES) {
        if (WriteResource(f.id, dir + L"\\" + f.dest)) continue;
        if (GetLastError() == ERROR_ACCESS_DENIED && !IsUserAnAdmin()) {
            // carpeta protegida (Archivos de programa): pedir administrador y seguir con la misma carpeta
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            if ((INT_PTR)ShellExecuteW(nullptr, L"runas", self, (L"\"" + dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL) > 32)
                return 0;
        }
        Msg(MB_ICONERROR, std::wstring(L"No se pudo escribir ") + f.dest + L" en\n" + dir +
                              L"\n\nSi el juego está abierto, cerralo y probá de nuevo.");
        return 1;
    }

    Msg(MB_ICONINFORMATION, L"Listo, el coop quedó instalado.\n\nAbrí el Call of Duty como siempre: en el menú principal "
                            L"está el botón \"Cooperativo\".\nLas instrucciones están en \"LEEME CoD Coop.txt\", en la "
                            L"carpeta del juego.");
    return 0;
}
