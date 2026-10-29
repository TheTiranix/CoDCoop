#pragma once
// Contrato entre el cargador fijo (ddraw.dll) y el nucleo recargable (codcoop_core.dll).
// El nucleo exporta (extern "C", cdecl):
//   uint32_t Coop_StateVersion(); int Coop_StateSize();
//   void Coop_Init(const CoreApi*, void* state, int fresh);   fresh = 1: estado recien puesto en cero
//   void Coop_Shutdown();  const char* Coop_Commands();  void Coop_Command(const char* name);
//   void Coop_SendPacket(int sock, const void* data, int len, const void* to);
//   int  Coop_GetPacket(int sock, void* msg, void* from);
//   void Coop_HostFrame();  int Coop_FrozenFrame(int msec);
struct CoreApi {
    void (*logLine)(const char* line);
};
