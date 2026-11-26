// Prueba de src/lobby.h (lista del lobby y mensaje M_LOBBY), sin juego.
// g++ -std=c++17 -O2 -static -o build\lobbytest.exe tools\lobbytest.cpp -lws2_32 ; imprime "lobby OK" o lo que falla
#include "../src/lobby.h"
#include <cstdio>
#include <cstring>

static int fails;
static void Check(bool ok, const char* what)
{
    if (!ok) printf("lobby FALLA: %s\n", what), ++fails;
}

static char (*back0())[lobby::NAME]
{
    static char b[lobby::MAX_PLAYERS][lobby::NAME];
    return b;
}

static sockaddr_in Addr(int n)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    a.sin_port = htons((u_short)(40000 + n));
    return a;
}

int main()
{
    lobby::List l{};
    Check(l.Touch(Addr(1), "Ana", 1000) && l.Touch(Addr(2), "", 1000) && l.Touch(Addr(3), "Cid", 1000),
          "Touch de 3 distintas da true");
    Check(!l.Touch(Addr(4), "Dan", 1000), "la 4ta no entra");
    Check(l.Touch(Addr(1), nullptr, 3000) && l.n == 3, "Touch de una conocida sigue en 3");
    Check(!strcmp(l.e[0].name, "Ana"), "Touch con nullptr no borra el nombre");
    l.Touch(Addr(2), "Bob", 3000);
    Check(!strcmp(l.e[1].name, "Bob"), "Touch con nombre lo actualiza");

    lobby::List bad{};  // el host guarda nombres que llegan por la red: en CoD2 terminan en "set ..." por consola
    bad.Touch(Addr(9), "x\";quit;\"\n", 1000);
    Check(!strpbrk(bad.e[0].name, "\";\n"), "Touch limpia el nombre que llega por la red");

    l.Expire(1000 + 4999, 5000);  // Cid visto en 1000: le faltan 1 ms
    Check(l.n == 3, "Expire no saca antes de tiempo");
    l.Expire(1000 + 5001, 5000);
    Check(l.n == 2, "Expire saca la vencida");
    l.Remove(Addr(1));
    Check(l.n == 1 && !strcmp(l.e[0].name, "Bob"), "Remove saca esa y deja la otra");
    l.Clear();
    Check(l.n == 0, "Clear");

    char out[lobby::NAME];
    lobby::CleanName("a\"b;c0123456789012345678901234567890123456789", out);
    Check(strlen(out) == 31 && !strncmp(out, "a'b'c", 5), "CleanName corta a 31 y cambia comillas y ;");
    lobby::CleanName("a\nb\rc\td", out);  // va a "set ..." por consola: un salto de linea seria otro comando
    Check(!strcmp(out, "a b c d"), "CleanName cambia los caracteres de control por espacios");
    const uint8_t evil[] = {1, 'x', '\n', 'q', 'u', 'i', 't', 0};
    Check(lobby::Unpack(evil, sizeof evil, back0()) == 1 && !strcmp(back0()[0], "x quit"), "Unpack limpia los nombres");
    lobby::Display("", 0, out);
    Check(!strcmp(out, "Host"), "Display vacio del host");
    lobby::Display("", 2, out);
    Check(!strcmp(out, "Player 3"), "Display vacio de otro");
    lobby::Display("Bob", 2, out);
    Check(!strcmp(out, "Bob"), "Display con nombre");

    char names[lobby::MAX_PLAYERS][lobby::NAME] = {"Host", "Bob"}, back[lobby::MAX_PLAYERS][lobby::NAME];
    uint8_t buf[256];
    int len = lobby::Pack(buf, sizeof buf, names, 2);
    Check(len == 1 + 5 + 4, "Pack: cantidad + nombres con 0");
    Check(lobby::Unpack(buf, len, back) == 2 && !strcmp(back[0], "Host") && !strcmp(back[1], "Bob"),
          "Unpack devuelve lo mismo");
    Check(lobby::Pack(buf, 4, names, 2) == -1, "Pack sin lugar da -1");
    const uint8_t cut[] = {2, 'H', 'o'};
    Check(lobby::Unpack(cut, sizeof cut, back) == -1, "Unpack cortado da -1");
    const uint8_t many[] = {5, 0, 0, 0, 0, 0};
    Check(lobby::Unpack(many, sizeof many, back) == -1, "Unpack con 5 jugadores da -1");

    if (!fails) printf("lobby OK\n");
    return fails ? 1 : 0;
}
