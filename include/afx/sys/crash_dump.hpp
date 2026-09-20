#pragma once

// Crash dump — DESIGN.md §21.1, M7-08. On SIGSEGV/SIGABRT/SIGBUS the handler
// writes every registered FlightRecorder ring to afx-crash-<pid>-<sig>.bin
// using only open(2)/write(2)/close(2) — no locks, no allocation, no stdio —
// then re-raises so the normal core-dump path still runs.

#include <string_view>

namespace afx {

class FlightRecorder;

// Called by EventManager's ctor/dtor. Registration is bounded (512 slots);
// overflow is dropped — crash paths never grow.
void register_recorder(const FlightRecorder* r) noexcept;
void unregister_recorder(const FlightRecorder* r) noexcept;

// Install the handlers. Called once from Runtime::start(); standalone
// binaries may call it directly. `dir` is where dumps land.
void install_crash_dump(std::string_view dir = ".");

}  // namespace afx
