# Copilot Instructions

## Project Guidelines
- Im Donutz VR HUD Projekt: Der native IPC-Server-Thread (IpcServer.cpp, std::thread) muss beim Prozessende sauber via DllMain(DLL_PROCESS_DETACH) detacht/gestoppt werden, sonst ruft der std::thread-Destruktor std::terminate()/abort() auf, was beim Beenden von iRacing als "Debug Error - abort() has been called" sichtbar wird.
- Im Donutz VR HUD: NamedPipeClientStream-basierte IPC-Reads müssen mit PipeOptions.Asynchronous + ReadAsync/CancellationToken implementiert werden, nicht synchron auf einem Hintergrund-Thread mit Read()/Dispose(). Andernfalls kann Dispose() den aufrufenden Thread (z.B. UI-Thread) blockieren, wenn ein anderer Thread gerade in einem blockierenden synchronen Read steckt.