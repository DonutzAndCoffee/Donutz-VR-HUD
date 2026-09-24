#include "IpcServer.h"
#include "Logging.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

// See ../Shared/OverlayIpcContract.cs for the message layout this pipe
// server parses. Accepts a single client connection and reads
// length-prefixed messages, updating an in-memory panel table that
// HookedFunctions::Hook_xrEndFrame consults to build composition layer
// quads.
namespace
{
	constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\DonutzVrHudOverlay";

	std::atomic<bool> g_running{ false };
	std::thread g_serverThread;

	// Panels keyed by the raw 16-byte GUID, compared via memcmp through a
	// small helper key type so we can use it in an unordered_map.
	struct GuidKey
	{
		uint8_t bytes[16];
		bool operator==(const GuidKey& other) const
		{
			return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
		}
	};
	struct GuidKeyHash
	{
		size_t operator()(const GuidKey& key) const
		{
			// Simple FNV-1a over the 16 bytes.
			size_t hash = 1469598103934665603ull;
			for (uint8_t b : key.bytes)
			{
				hash ^= b;
				hash *= 1099511628211ull;
			}
			return hash;
		}
	};

	std::mutex g_stateMutex;
	std::unordered_map<GuidKey, IpcServer::PanelState, GuidKeyHash> g_panels;
	std::atomic<bool> g_recenterRequested{ false };
	std::atomic<bool> g_editModeActive{ false };

	// Outgoing (server -> client) message queue, drained by ServerLoop's
	// per-connection loop right after each read attempt. A dedicated write
	// mutex/handle pair is kept separate from the read path since
	// ReadFile/WriteFile on the same duplex pipe handle from different
	// threads is safe, but the current pipe handle must be visible to
	// whichever thread queues a message (ControllerInput.cpp's OpenXR frame
	// thread), not just the pipe server thread itself.
	std::mutex g_writeMutex;
	HANDLE g_activeClientPipe = INVALID_HANDLE_VALUE;

	bool WriteMessage(IpcServer::MessageType type, const void* payload, uint32_t payloadSize)
	{
		std::lock_guard<std::mutex> lock(g_writeMutex);
		if (g_activeClientPipe == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		BYTE header[5];
		header[0] = static_cast<BYTE>(type);
		std::memcpy(header + 1, &payloadSize, sizeof(payloadSize));

		DWORD written = 0;
		if (!WriteFile(g_activeClientPipe, header, sizeof(header), &written, nullptr) || written != sizeof(header))
		{
			return false;
		}

		if (payloadSize > 0)
		{
			if (!WriteFile(g_activeClientPipe, payload, payloadSize, &written, nullptr) || written != payloadSize)
			{
				return false;
			}
		}

		return true;
	}

	GuidKey ToKey(const IpcServer::PanelIdBytes& id)
	{
		GuidKey key;
		std::memcpy(key.bytes, id.bytes, sizeof(key.bytes));
		return key;
	}

	bool ReadExact(HANDLE pipe, void* buffer, DWORD size)
	{
		BYTE* dest = static_cast<BYTE*>(buffer);
		DWORD totalRead = 0;
		while (totalRead < size)
		{
			DWORD bytesRead = 0;
			if (!ReadFile(pipe, dest + totalRead, size - totalRead, &bytesRead, nullptr) || bytesRead == 0)
			{
				return false;
			}
			totalRead += bytesRead;
		}
		return true;
	}

	void HandleSetPanelTransform(const IpcServer::SetPanelTransformPayload& payload)
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		auto key = ToKey(payload.panelId);
		auto& panel = g_panels[key];
		panel.panelId = payload.panelId;
		panel.enabled = payload.enabled != 0;
		panel.positionX = payload.positionX;
		panel.positionY = payload.positionY;
		panel.positionZ = payload.positionZ;
		panel.rotationDegX = payload.rotationDegX;
		panel.rotationDegY = payload.rotationDegY;
		panel.rotationDegZ = payload.rotationDegZ;
		panel.widthMeters = payload.widthMeters;
		panel.heightMeters = payload.heightMeters;
		panel.headLocked = payload.headLocked != 0;
		panel.opaqueBackground = payload.opaqueBackground != 0;
	}

	void HandleUpdateFrame(const IpcServer::UpdateFramePayload& payload)
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		auto key = ToKey(payload.panelId);
		auto it = g_panels.find(key);
		if (it == g_panels.end())
		{
			return;
		}
		// The name array may not be null-terminated if the sender filled it
		// completely; clamp defensively to kSharedHandleNameLength.
		size_t nameLen = 0;
		while (nameLen < IpcServer::kSharedHandleNameLength && payload.sharedHandleName[nameLen] != L'\0')
		{
			++nameLen;
		}
		it->second.sharedHandleName.assign(payload.sharedHandleName, nameLen);
		it->second.frameWidth = payload.width;
		it->second.frameHeight = payload.height;
		it->second.hasFrame = true;
	}

	void HandleRemovePanel(const IpcServer::RemovePanelPayload& payload)
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_panels.erase(ToKey(payload.panelId));
	}

	void HandleMessage(IpcServer::MessageType type, const std::vector<BYTE>& payload)
	{
		switch (type)
		{
		case IpcServer::MessageType::SetPanelTransform:
			if (payload.size() >= sizeof(IpcServer::SetPanelTransformPayload))
			{
				IpcServer::SetPanelTransformPayload msg;
				std::memcpy(&msg, payload.data(), sizeof(msg));
				HandleSetPanelTransform(msg);
			}
			break;
		case IpcServer::MessageType::UpdateFrame:
			if (payload.size() >= sizeof(IpcServer::UpdateFramePayload))
			{
				IpcServer::UpdateFramePayload msg;
				std::memcpy(&msg, payload.data(), sizeof(msg));
				HandleUpdateFrame(msg);
			}
			break;
		case IpcServer::MessageType::RemovePanel:
			if (payload.size() >= sizeof(IpcServer::RemovePanelPayload))
			{
				IpcServer::RemovePanelPayload msg;
				std::memcpy(&msg, payload.data(), sizeof(msg));
				HandleRemovePanel(msg);
			}
			break;
		case IpcServer::MessageType::Recenter:
			g_recenterRequested = true;
			break;
		case IpcServer::MessageType::SetEditModeActive:
			if (payload.size() >= sizeof(IpcServer::SetEditModeActivePayload))
			{
				IpcServer::SetEditModeActivePayload msg;
				std::memcpy(&msg, payload.data(), sizeof(msg));
				g_editModeActive = msg.active != 0;
			}
			break;
		default:
			break;
		}
	}

	void ServerLoop()
	{
		Logging::Log("IpcServer::ServerLoop: starting, pipe name \\\\.\\pipe\\DonutzVrHudOverlay");
		while (g_running)
		{
			HANDLE pipe = CreateNamedPipeW(
				kPipeName,
				PIPE_ACCESS_DUPLEX,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
				1,
				64 * 1024,
				64 * 1024,
				0,
				nullptr);

			if (pipe == INVALID_HANDLE_VALUE)
			{
				Logging::Log("IpcServer::ServerLoop: CreateNamedPipeW failed with GetLastError=" + std::to_string(GetLastError()));
				break;
			}

			Logging::Log("IpcServer::ServerLoop: pipe created, waiting for client connection.");
			const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
			if (connected)
			{
				Logging::Log("IpcServer::ServerLoop: client connected.");
				{
					std::lock_guard<std::mutex> lock(g_writeMutex);
					g_activeClientPipe = pipe;
				}
				while (g_running)
				{
					BYTE header[5]; // 1 byte type + 4 byte length
					if (!ReadExact(pipe, header, sizeof(header)))
					{
						break;
					}

					const auto type = static_cast<IpcServer::MessageType>(header[0]);
					int32_t length = 0;
					std::memcpy(&length, header + 1, sizeof(length));

					if (length < 0 || length > (16 * 1024 * 1024))
					{
						break;
					}

					std::vector<BYTE> payload(static_cast<size_t>(length));
					if (length > 0 && !ReadExact(pipe, payload.data(), static_cast<DWORD>(length)))
					{
						break;
					}

					HandleMessage(type, payload);
				}
				{
					std::lock_guard<std::mutex> lock(g_writeMutex);
					g_activeClientPipe = INVALID_HANDLE_VALUE;
				}
			}

			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
		}
	}
}

namespace IpcServer
{
	void EnsureStarted()
	{
		bool expected = false;
		if (g_running.compare_exchange_strong(expected, true))
		{
			g_serverThread = std::thread(ServerLoop);
		}
	}

	void Stop()
	{
		if (g_running.exchange(false))
		{
			// Wake up a blocked ConnectNamedPipe/ReadFile by connecting a
			// dummy client, then join.
			HANDLE dummy = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (dummy != INVALID_HANDLE_VALUE)
			{
				CloseHandle(dummy);
			}

			if (g_serverThread.joinable())
			{
				g_serverThread.join();
			}
		}
	}

	void DetachForProcessExit()
	{
		// At DLL_PROCESS_DETACH the process is unwinding; other threads may
		// already have been terminated by the OS, and calling into
		// synchronization primitives/joining here can deadlock. Just flag
		// the loop to stop (best-effort) and detach so the std::thread
		// destructor (which runs at static/global teardown) doesn't call
		// std::terminate() for a still-joinable thread -- that call to
		// std::terminate() is what surfaces as "abort() has been called"
		// when the host application (iRacing) shuts down with this layer
		// loaded.
		g_running = false;
		if (g_serverThread.joinable())
		{
			g_serverThread.detach();
		}
	}

	std::vector<PanelState> GetPanelsSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		std::vector<PanelState> result;
		result.reserve(g_panels.size());
		for (const auto& [key, panel] : g_panels)
		{
			result.push_back(panel);
		}
		return result;
	}

	bool ConsumeRecenterRequested()
	{
		return g_recenterRequested.exchange(false);
	}

	bool IsEditModeActive()
	{
		return g_editModeActive.load();
	}

	void ApplyControllerTransform(
		const PanelIdBytes& panelId,
		float positionX, float positionY, float positionZ,
		float rotationDegX, float rotationDegY, float rotationDegZ)
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		auto it = g_panels.find(ToKey(panelId));
		if (it == g_panels.end())
		{
			return;
		}
		it->second.positionX = positionX;
		it->second.positionY = positionY;
		it->second.positionZ = positionZ;
		it->second.rotationDegX = rotationDegX;
		it->second.rotationDegY = rotationDegY;
		it->second.rotationDegZ = rotationDegZ;
	}

	void QueuePanelTransformUpdated(const PanelState& panel)
	{
		PanelTransformUpdatedPayload payload{};
		payload.panelId = panel.panelId;
		payload.positionX = panel.positionX;
		payload.positionY = panel.positionY;
		payload.positionZ = panel.positionZ;
		payload.rotationDegX = panel.rotationDegX;
		payload.rotationDegY = panel.rotationDegY;
		payload.rotationDegZ = panel.rotationDegZ;
		WriteMessage(MessageType::PanelTransformUpdated, &payload, sizeof(payload));
	}

	void QueueControllerNudgeAction(uint8_t nudgeAction)
	{
		ControllerNudgeActionPayload payload{};
		payload.nudgeAction = nudgeAction;
		WriteMessage(MessageType::ControllerNudgeAction, &payload, sizeof(payload));
	}

	void QueueControllerGrabStarted(const PanelIdBytes& panelId)
	{
		ControllerGrabStartedPayload payload{};
		payload.panelId = panelId;
		WriteMessage(MessageType::ControllerGrabStarted, &payload, sizeof(payload));
	}
}

extern "C" BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID /*reserved*/)
{
	if (reason == DLL_PROCESS_DETACH)
	{
		// The host application (e.g. iRacing.exe) is exiting. If the IPC
		// server's std::thread is still joinable when its destructor runs
		// during static/global object teardown, the C++ runtime calls
		// std::terminate(), which aborts the process -- visible to the
		// user as a "Debug Error! ... abort() has been called" dialog on
		// exit. Detach it here instead so no code assumes it's still safe
		// to join a thread this late in process teardown.
		IpcServer::DetachForProcessExit();
	}
	return TRUE;
}
