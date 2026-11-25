// Prueba con el Steam real y una sola cuenta: se manda un mensaje a si mismo (vuelve al mismo proceso) y se muestra
// donde viene el SteamID dentro de SteamNetworkingMessage_t (issue #8: steam.h lo lee en el offset 16).
// g++ -std=c++17 -O2 -static -o build\steamselftest.exe tools\steamselftest.cpp ; correr con steam_api.dll al lado
#include <cstdarg>
#include <windows.h>
#include "../src/steam.h"

static void Print(const char* fmt, ...)
{
    va_list v;
    va_start(v, fmt);
    vprintf(fmt, v);
    va_end(v);
    printf("\n");
}

int main()
{
    steam::log = Print;
    if (!steam::Start()) return printf("Steam: %s\n", steam::error), 1;
    uint64_t me = steam::MyId();
    printf("yo: %llu (%016llx)\n", (unsigned long long)me, (unsigned long long)me);
    for (int i = 0; i < 150; ++i) {
        if (i % 10 == 0) steam::Send(me, "hola", 5);
        steam::Poll(nullptr);
        steam::Message* m;
        if (steam::a.receive(steam::a.msgs, 0, &m, 1) == 1) {
            const uint8_t* b = (const uint8_t*)m;
            printf("mensaje: %d bytes \"%s\"\nprimeros 40 bytes:", m->size, (const char*)m->data);
            for (int k = 0; k < 40; ++k) printf("%s%02x", k % 4 ? "" : " ", b[k]);
            uint64_t at12, at16;
            memcpy(&at12, b + 12 + 8, 8);
            memcpy(&at16, b + 16 + 8, 8);
            printf("\nSteamID si la identidad va en 12: %llu %s\nSteamID si va en 16: %llu %s\n",
                   (unsigned long long)at12, at12 == me ? "<- ES" : "", (unsigned long long)at16, at16 == me ? "<- ES" : "");
            printf("steam.h lee (Message::peer): %llu -> %s\n", (unsigned long long)m->peer.id,
                   m->peer.id == me ? "OK" : "MAL");
            steam::a.release(m);
            return 0;
        }
        Sleep(100);
    }
    printf("no volvio ningun mensaje en 15 s\n");
    return 2;
}
