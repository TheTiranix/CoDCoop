// Lobby de la ventana Co-op, el mismo en CoD1/UO (core.cpp) y CoD2 (cod2.cpp). El host anota a los que piden entrar
// (M_JOIN) mientras esta en el menu y les manda a todos la lista de nombres (M_LOBBY); cuando el host ya tiene nivel,
// el reintento de M_JOIN de cada uno sigue el camino de siempre (cargar el mapa del host y entrar).
// Prueba: tools/lobbytest.cpp.
#pragma once
#include <winsock2.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace lobby {

constexpr uint8_t M_LOBBY = 'L';  // host -> jugadores: cantidad (1 byte) + nombres terminados en 0, el host primero
constexpr int MAX_OTHERS = 3, MAX_PLAYERS = 4, NAME = 32;

inline bool Same(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// los nombres terminan en "set ..." por consola: sin comillas, ";" ni caracteres de control (otro comando)
inline void CleanName(const char* in, char out[NAME])
{
    int k = 0;
    for (; in && in[k] && k < NAME - 1; ++k) {
        char c = in[k];
        out[k] = c == '"' || c == ';' ? '\'' : (unsigned char)c < 32 ? ' ' : c;
    }
    out[k] = 0;
}

struct Entry {
    sockaddr_in addr;
    char name[NAME];
    DWORD seen;
};

// los que no son el host: en el menu esperando o ya dentro del nivel
struct List {
    Entry e[MAX_OTHERS];
    int n;

    // anota que llego algo de `a` (con su nombre si viene uno); false si es nuevo y no hay lugar
    bool Touch(const sockaddr_in& a, const char* name, DWORD now)
    {
        int i = 0;
        while (i < n && !Same(e[i].addr, a)) ++i;
        if (i == n) {
            if (n == MAX_OTHERS) return false;
            e[n++] = Entry{a, "", now};
        }
        e[i].seen = now;
        if (name && *name) CleanName(name, e[i].name);  // llega por la red: cualquiera puede mandar un M_JOIN
        return true;
    }
    void RemoveAt(int i)
    {
        memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof e[0]);  // el orden queda: es el de la lista en pantalla
        --n;
    }
    void Remove(const sockaddr_in& a)
    {
        for (int i = 0; i < n; ++i)
            if (Same(e[i].addr, a)) return RemoveAt(i);
    }
    void Expire(DWORD now, DWORD keepMs)
    {
        for (int i = n - 1; i >= 0; --i)
            if (now - e[i].seen > keepMs) RemoveAt(i);
    }
    void Clear() { n = 0; }
};

// nombre vacio (sin Steam): "Host" el primero, "Player N" los demas
inline void Display(const char* name, int index, char out[NAME])
{
    if (name && *name) snprintf(out, NAME, "%s", name);
    else if (!index) snprintf(out, NAME, "Host");
    else snprintf(out, NAME, "Player %d", index + 1);
}

// cuerpo de M_LOBBY; bytes escritos o -1
inline int Pack(uint8_t* out, int max, const char (*names)[NAME], int count)
{
    if (count < 0 || count > MAX_PLAYERS || max < 1) return -1;
    int len = 0;
    out[len++] = (uint8_t)count;
    for (int i = 0; i < count; ++i) {
        int l = (int)strnlen(names[i], NAME - 1);
        if (len + l + 1 > max) return -1;
        memcpy(out + len, names[i], l);
        out[len + l] = 0;
        len += l + 1;
    }
    return len;
}

// cantidad de jugadores (los nombres ya limpios) o -1 si el mensaje esta mal armado
inline int Unpack(const uint8_t* p, int len, char (*names)[NAME])
{
    if (len < 1 || p[0] > MAX_PLAYERS) return -1;
    int count = p[0], at = 1;
    for (int i = 0; i < count; ++i) {
        const uint8_t* z = at < len ? (const uint8_t*)memchr(p + at, 0, len - at) : nullptr;
        if (!z) return -1;
        CleanName((const char*)p + at, names[i]);
        at = int(z - p) + 1;
    }
    return count;
}

}  // namespace lobby
