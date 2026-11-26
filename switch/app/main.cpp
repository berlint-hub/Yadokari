// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/HorizonInputSettings.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Patch.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "common/Horizon/Horizon.h"

#include "HorizonCores.h"
#include "HorizonHost.h"
#include "HorizonHotkeys.h"
#include "HorizonInputMath.h"
#include "HorizonUpdater.h"
#include "HorizonUsbStorage.h"

namespace
{
	constexpr const char* ARMSX2_ROOT = "sdmc:/switch/armsx2";
	constexpr const char* GAMES_DIR = "sdmc:/switch/armsx2/games";
	constexpr const char* LOGS_DIR = "sdmc:/switch/armsx2/logs";
	constexpr const char* LOG_PATH = "sdmc:/switch/armsx2/logs/emulog.txt";
	constexpr const char* USB_SETTINGS_SECTION = "Horizon";
	constexpr const char* USB_GAME_ROOTS_KEY = "ManagedUsbGameRoots";

	constexpr u64 INPUT_POLL_NS = 16'000'000ULL;
	// CPU-thread idle tick while no VM is running
	constexpr u64 IDLE_POLL_NS = 8'000'000ULL;

	constexpr u32 NUM_LOCAL_PLAYERS = 2;
	constexpr const char* MULTIPLAYER_INPUT_VERSION_KEY = "MultiplayerInputVersion";
	constexpr int MULTIPLAYER_INPUT_VERSION = 1;

	// Hold '+' and '-' together to open the FullscreenUI pause menu
	constexpr u64 MENU_COMBO = HidNpadButton_Plus | HidNpadButton_Minus;

	std::unique_ptr<INISettingsInterface> s_settings_interface;

	struct ButtonMap
	{
		u64 nx;
		PadDualshock2::Inputs ps2;
	};
	constexpr ButtonMap BUTTON_MAP[] = {
		{HidNpadButton_Up, PadDualshock2::Inputs::PAD_UP},
		{HidNpadButton_Down, PadDualshock2::Inputs::PAD_DOWN},
		{HidNpadButton_Left, PadDualshock2::Inputs::PAD_LEFT},
		{HidNpadButton_Right, PadDualshock2::Inputs::PAD_RIGHT},
		{HidNpadButton_X, PadDualshock2::Inputs::PAD_TRIANGLE},
		{HidNpadButton_A, PadDualshock2::Inputs::PAD_CIRCLE},
		{HidNpadButton_B, PadDualshock2::Inputs::PAD_CROSS},
		{HidNpadButton_Y, PadDualshock2::Inputs::PAD_SQUARE},
		{HidNpadButton_Minus, PadDualshock2::Inputs::PAD_SELECT},
		{HidNpadButton_Plus, PadDualshock2::Inputs::PAD_START},
		{HidNpadButton_L, PadDualshock2::Inputs::PAD_L1},
		{HidNpadButton_R, PadDualshock2::Inputs::PAD_R1},
		{HidNpadButton_ZL, PadDualshock2::Inputs::PAD_L2},
		{HidNpadButton_ZR, PadDualshock2::Inputs::PAD_R2},
		{HidNpadButton_StickL, PadDualshock2::Inputs::PAD_L3},
		{HidNpadButton_StickR, PadDualshock2::Inputs::PAD_R3},
	};

	// Map by physical position rather than meaning
	struct NavMap
	{
		u64 nx;
		GenericInputBinding generic;
	};
	constexpr NavMap NAV_MAP[] = {
		{HidNpadButton_Up, GenericInputBinding::DPadUp},
		{HidNpadButton_Down, GenericInputBinding::DPadDown},
		{HidNpadButton_Left, GenericInputBinding::DPadLeft},
		{HidNpadButton_Right, GenericInputBinding::DPadRight},
		{HidNpadButton_B, GenericInputBinding::Cross},
		{HidNpadButton_A, GenericInputBinding::Circle},
		{HidNpadButton_Y, GenericInputBinding::Square},
		{HidNpadButton_X, GenericInputBinding::Triangle},
		{HidNpadButton_L, GenericInputBinding::L1},
		{HidNpadButton_R, GenericInputBinding::R1},
		{HidNpadButton_ZL, GenericInputBinding::L2},
		{HidNpadButton_ZR, GenericInputBinding::R2},
		{HidNpadButton_Minus, GenericInputBinding::Select},
		{HidNpadButton_Plus, GenericInputBinding::Start},
	};

	// What the game should see from one controller. Default-constructed means nothing pressed.
	struct GamePadState
	{
		u64 buttons = 0;
		float left_x = 0.0f;
		float left_y = 0.0f;
		float right_x = 0.0f;
		float right_y = 0.0f;
	};
	using GamePadSnapshot = std::array<GamePadState, NUM_LOCAL_PLAYERS>;

	// The input thread only polls HID. Pad state is written on the CPU thread, which pumps its queue every
	// vsync while a game runs (and from the idle loop otherwise). At most one apply task is queued at a time;
	// it always applies the newest snapshot.
	std::mutex s_game_pad_mutex;
	GamePadSnapshot s_pending_game_pads{};
	bool s_game_pad_apply_queued = false;

	// Set after a system applet closes, so buttons still held from it are ignored until they are released.
	std::atomic_bool s_input_resync{false};

	u64 GetHotkeyButton(int button)
	{
		switch (button)
		{
			case HorizonInputSettings::R: return HidNpadButton_R;
			case HorizonInputSettings::L: return HidNpadButton_L;
			case HorizonInputSettings::ZR: return HidNpadButton_ZR;
			case HorizonInputSettings::ZL: return HidNpadButton_ZL;
			case HorizonInputSettings::DPadLeft: return HidNpadButton_Left;
			case HorizonInputSettings::DPadRight: return HidNpadButton_Right;
			case HorizonInputSettings::DPadUp: return HidNpadButton_Up;
			case HorizonInputSettings::DPadDown: return HidNpadButton_Down;
			case HorizonInputSettings::A: return HidNpadButton_A;
			case HorizonInputSettings::B: return HidNpadButton_B;
			case HorizonInputSettings::X: return HidNpadButton_X;
			case HorizonInputSettings::Y: return HidNpadButton_Y;
			case HorizonInputSettings::LeftStick: return HidNpadButton_StickL;
			case HorizonInputSettings::RightStick: return HidNpadButton_StickR;
			default: return 0;
		}
	}

	struct SwitchInputConfig
	{
		HorizonHotkeys::Buttons hotkeys;
		float deadzone;
	};

	SwitchInputConfig ReadSwitchInputConfig()
	{
		auto lock = Host::GetSettingsLock();
		u64 buttons[std::size(HorizonInputSettings::BINDINGS)]{};
		for (size_t i = 0; i < std::size(buttons); i++)
		{
			const auto& binding = HorizonInputSettings::BINDINGS[i];
			buttons[i] = GetHotkeyButton(s_settings_interface->GetIntValue(
				HorizonInputSettings::SECTION, binding.key, binding.default_button));
		}
		const int deadzone = std::clamp(s_settings_interface->GetIntValue(HorizonInputSettings::SECTION,
			HorizonInputSettings::DEADZONE, HorizonInputSettings::DEFAULT_DEADZONE),
			HorizonInputSettings::MIN_DEADZONE, HorizonInputSettings::MAX_DEADZONE);
		return {{HidNpadButton_Minus, HidNpadButton_Plus, buttons[0], buttons[1], buttons[2], buttons[3], buttons[4]},
			static_cast<float>(deadzone) / 100.0f};
	}

	float ApplyDeadzone(s32 raw, float deadzone)
	{
		return HorizonInputMath::ApplyDeadzone(static_cast<float>(raw) / static_cast<float>(JOYSTICK_MAX), deadzone);
	}

	GamePadState ReadGamePad(PadState& pad, u64 buttons, float deadzone)
	{
		const HidAnalogStickState left = padGetStickPos(&pad, 0);
		const HidAnalogStickState right = padGetStickPos(&pad, 1);

		GamePadState state;
		state.buttons = buttons;
		state.left_x = ApplyDeadzone(left.x, deadzone);
		state.left_y = ApplyDeadzone(left.y, deadzone);
		state.right_x = ApplyDeadzone(right.x, deadzone);
		state.right_y = ApplyDeadzone(right.y, deadzone);
		return state;
	}

	void ApplyStick(u32 player, float x, float y, PadDualshock2::Inputs left, PadDualshock2::Inputs right,
		PadDualshock2::Inputs up, PadDualshock2::Inputs down)
	{
		Pad::SetControllerState(player, static_cast<u32>(right), x > 0.0f ? x : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(left), x < 0.0f ? -x : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(up), y > 0.0f ? y : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(down), y < 0.0f ? -y : 0.0f);
	}

	void ApplyGamePad(u32 player, const GamePadState& state)
	{
		for (const ButtonMap& m : BUTTON_MAP)
			Pad::SetControllerState(player, static_cast<u32>(m.ps2), (state.buttons & m.nx) ? 1.0f : 0.0f);

		ApplyStick(player, state.left_x, state.left_y, PadDualshock2::Inputs::PAD_L_LEFT,
			PadDualshock2::Inputs::PAD_L_RIGHT, PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN);
		ApplyStick(player, state.right_x, state.right_y, PadDualshock2::Inputs::PAD_R_LEFT,
			PadDualshock2::Inputs::PAD_R_RIGHT, PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_DOWN);
	}

	// CPU thread
	void ApplyPendingGamePads()
	{
		GamePadSnapshot snapshot;
		{
			const std::lock_guard lock(s_game_pad_mutex);
			snapshot = s_pending_game_pads;
			s_game_pad_apply_queued = false;
		}

		if (!VMManager::HasValidVM())
			return;

		for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
			ApplyGamePad(player, snapshot[player]);
	}

	// Input thread
	void PublishGamePads(const GamePadSnapshot& snapshot)
	{
		bool queue_apply;
		{
			const std::lock_guard lock(s_game_pad_mutex);
			s_pending_game_pads = snapshot;
			queue_apply = !s_game_pad_apply_queued;
			s_game_pad_apply_queued = true;
		}

		if (queue_apply)
			Host::RunOnCPUThread(&ApplyPendingGamePads, false);
	}

	// CPU thread. Opens the system controller applet so players can be assigned to controllers.
	void ShowControllerSetup()
	{
		HidLaControllerSupportArg arg;
		hidLaCreateControllerSupportArg(&arg);
		arg.hdr.player_count_max = static_cast<s8>(NUM_LOCAL_PLAYERS);

		HidLaControllerSupportResultInfo info = {};
		const Result result = hidLaShowControllerSupport(&info, &arg);

		// The button that closed the applet is usually still held.
		s_input_resync.store(true, std::memory_order_release);

		if (R_SUCCEEDED(result))
			INFO_LOG("Controller setup finished with {} player(s)", static_cast<int>(info.player_count));
		else
			INFO_LOG("Controller setup closed without changes (0x{:08X})", static_cast<u32>(result));
	}

	void FeedNav(u64 held, u64 changed)
	{
		for (const NavMap& m : NAV_MAP)
		{
			if (changed & m.nx)
				ImGuiManager::ProcessGenericInputEvent(m.generic, InputLayout::Nintendo, (held & m.nx) ? 1.0f : 0.0f);
		}
	}

	void DispatchHotkey(HorizonHotkeys::Action action)
	{
		if (action == HorizonHotkeys::Action::None)
			return;

		Host::RunOnCPUThread([action]() {
			// The VM may have stopped while the command was waiting in the queue.
			if (VMManager::GetState() != VMState::Running)
				return;

			static s32 slot = 1;
			switch (action)
			{
				case HorizonHotkeys::Action::Save:
					VMManager::SaveStateToSlot(slot);
					break;
				case HorizonHotkeys::Action::Load:
					VMManager::LoadStateFromSlot(slot);
					break;
				case HorizonHotkeys::Action::PreviousSlot:
					slot = (slot == 1) ? VMManager::NUM_SAVE_STATE_SLOTS : slot - 1;
					Host::AddKeyedOSDMessage("HorizonSaveSlot", "Save state slot: " + std::to_string(slot));
					break;
				case HorizonHotkeys::Action::NextSlot:
					slot = (slot == VMManager::NUM_SAVE_STATE_SLOTS) ? 1 : slot + 1;
					Host::AddKeyedOSDMessage("HorizonSaveSlot", "Save state slot: " + std::to_string(slot));
					break;
				case HorizonHotkeys::Action::Screenshot:
					MTGS::RunOnGSThread([]() { GSQueueSnapshot(std::string()); });
					break;
				case HorizonHotkeys::Action::None:
					break;
			}
		});
	}

	void SetupSettings()
	{
		const std::string ini_path = Path::Combine(ARMSX2_ROOT, "armsx2.ini");
		s_settings_interface = std::make_unique<INISettingsInterface>(ini_path);
		Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());
		s_settings_interface->Load();

		if (s_settings_interface->IsEmpty())
		{
			INFO_LOG("No settings found; writing default settings to {}", ini_path);
			VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);

			s_settings_interface->SetStringValue("Filenames", "Game", "");
			s_settings_interface->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
			s_settings_interface->SetStringValue("EmuCore/GS", "AspectRatio", "4:3");
			s_settings_interface->SetIntValue("EmuCore/GS", "deinterlace_mode", static_cast<int>(GSInterlaceMode::Off));
			s_settings_interface->SetStringValue("SPU2/Output", "Backend", "Horizon");
			s_settings_interface->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
			s_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", 0);
			s_settings_interface->SetBoolValue("UI", "EnableFullscreenUI", true);
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", GAMES_DIR);
			s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
			s_settings_interface->SetBoolValue("InputSources", "SDL", false);
			s_settings_interface->SetBoolValue("Logging", "EnableSystemConsole", true);
			s_settings_interface->SetBoolValue("Logging", "EnableFileLogging", true);
			s_settings_interface->SetBoolValue("Logging", "EnableVerbose", false);
			s_settings_interface->Save();
		}

		if (s_settings_interface->GetIntValue("Horizon", MULTIPLAYER_INPUT_VERSION_KEY, 0) < MULTIPLAYER_INPUT_VERSION)
		{
			s_settings_interface->SetStringValue("Pad2", "Type", "DualShock2");
			s_settings_interface->SetIntValue("Horizon", MULTIPLAYER_INPUT_VERSION_KEY, MULTIPLAYER_INPUT_VERSION);
			s_settings_interface->Save();
		}

		VMManager::Internal::LoadStartupSettings();
		EmuFolders::EnsureFoldersExist();
	}

	bool SyncUsbGameRoots()
	{
		std::vector<std::string> roots;
		for (const HorizonUsbStorage::Volume& volume : HorizonUsbStorage::GetVolumes())
			roots.push_back(volume.root);

		auto lock = Host::GetSettingsLock();
		const std::vector<std::string> old_roots = s_settings_interface->GetStringList(USB_SETTINGS_SECTION, USB_GAME_ROOTS_KEY);
		if (roots == old_roots)
			return false;

		for (const std::string& root : old_roots)
			s_settings_interface->RemoveFromStringList("GameList", "RecursivePaths", root.c_str());
		for (const std::string& root : roots)
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", root.c_str());

		s_settings_interface->SetStringList(USB_SETTINGS_SECTION, USB_GAME_ROOTS_KEY, roots);
		s_settings_interface->Save();
		return true;
	}

	void LogUsbVolumes()
	{
		const std::vector<HorizonUsbStorage::Volume> volumes = HorizonUsbStorage::GetVolumes();
		if (volumes.empty())
		{
			INFO_LOG("USB storage disconnected");
			return;
		}

		for (const HorizonUsbStorage::Volume& volume : volumes)
			INFO_LOG("USB volume '{}' mounted at {} ({})", volume.label, volume.root, volume.filesystem);
	}

	// Boot a disc image directly when passed as argv[1]
	void BootImage(std::string path)
	{
		VMBootParameters params;
		params.filename = std::move(path);

		INFO_LOG("Booting image: {}", params.filename);
		if (VMManager::Initialize(std::move(params)))
			VMManager::SetState(VMState::Running);
		else
			ERROR_LOG("VMManager::Initialize() failed for the launch image");
	}
} // namespace

void Host::CommitBaseSettingChanges()
{
	auto lock = Host::GetSettingsLock();
	if (s_settings_interface)
		s_settings_interface->Save();
}

void Host::OnControllerSetupRequested()
{
	// Called from FullscreenUI on the GS thread. Show the applet on the CPU thread, like the software keyboard.
	Host::RunOnCPUThread(&ShowControllerSetup, false);
}

int main(int argc, char** argv)
{
	// Before any thread exists: decides which cores helper threads go to.
	HorizonCores::Initialize();

	const bool have_socket = R_SUCCEEDED(socketInitializeDefault());
	if (have_socket)
		nxlinkStdio();

	mkdir("sdmc:/switch", 0777);
	mkdir(ARMSX2_ROOT, 0777);
	mkdir(GAMES_DIR, 0777);
	mkdir(LOGS_DIR, 0777);

	Log::SetTimestampsEnabled(true);
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);
	VMManager::Internal::SetFileLogPath(LOG_PATH);

	INFO_LOG("================ ARMSX2-NX ================");
	INFO_LOG("Logging to {}", LOG_PATH);
	INFO_LOG("CPU cores: process may use mask 0x{:x}, helper threads go to mask 0x{:x}",
		HorizonCores::GetAllowedCoreMask(), HorizonCores::GetHelperCoreMask());

	// Mount the romfs: it holds the runtime resources (fonts, UI, GameIndex), and the deko3d shaders when
	// that renderer is built.
	const bool have_romfs = R_SUCCEEDED(romfsInit());
	if (!have_romfs)
		ERROR_LOG("romfsInit() failed. Falling back to resources on the SD card.");

	EmuFolders::AppRoot = ARMSX2_ROOT;
	EmuFolders::DataRoot = ARMSX2_ROOT;
	// Resources are baked into the romfs.
	// Fall back to the SD if the romfs failed to mount.
	if (have_romfs)
		EmuFolders::Resources = "romfs:/resources";
	else
		EmuFolders::SetResourcesDirectory();
	INFO_LOG("Resources directory: {}", EmuFolders::Resources);
	SetupSettings();
	HorizonUpdater::Initialize((argc > 0) ? argv[0] : nullptr);
	if (HorizonUsbStorage::Initialize())
	{
		SyncUsbGameRoots();
		LogUsbVolumes();
		HorizonUsbStorage::ConsumeChange();
	}
	else
	{
		SyncUsbGameRoots();
		WARNING_LOG("{}", HorizonUsbStorage::GetError());
	}

	INFO_LOG("BIOS directory: {}", EmuFolders::Bios);
	const std::string bios = s_settings_interface->GetStringValue("Filenames", "BIOS", "");
	if (bios.empty())
		ERROR_LOG("No BIOS configured. Put a dumped BIOS in {} and set [Filenames] BIOS=<file> "
				  "in armsx2.ini", EmuFolders::Bios);
	else
		INFO_LOG("Configured BIOS: {}", bios);

	// Point ImGui at its fonts
	ImGuiManager::SetFontPathAndRange(
		Path::Combine(EmuFolders::Resources, "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf"), {});

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		ERROR_LOG("CPUThreadInitialize() failed. Aborting...");
		VMManager::Internal::CPUThreadShutdown();
		if (have_romfs)
			romfsExit();
		if (have_socket)
			socketExit();
		return 1;
	}

	VMManager::ApplySettings();

	HorizonHost::SetCPUThread();

	if (!MTGS::WaitForOpen())
	{
		ERROR_LOG("Failed to open GS; aborting.");
		VMManager::Internal::CPUThreadShutdown();
		if (have_romfs)
			romfsExit();
		if (have_socket)
			socketExit();
		return 1;
	}

	const bool enable_fsui = s_settings_interface->GetBoolValue("UI", "EnableFullscreenUI", true);
	if (enable_fsui)
	{
		MTGS::RunOnGSThread(&ImGuiManager::InitializeFullscreenUI);
		Host::RefreshGameListAsync(false);
		if (have_socket)
			HorizonUpdater::CheckForUpdates(false);
	}

	padConfigureInput(NUM_LOCAL_PLAYERS, HidNpadStyleSet_NpadStandard);
	std::array<PadState, NUM_LOCAL_PLAYERS> pads;
	padInitializeDefault(&pads[0]);
	padInitialize(&pads[1], HidNpadIdType_No2);

	std::string boot_image;
	if (argc > 1 && argv[1] && argv[1][0])
		boot_image = argv[1];

	if (!boot_image.empty() && FileSystem::FileExists(boot_image.c_str()))
		BootImage(std::move(boot_image));
	else if (!boot_image.empty())
		ERROR_LOG("Launch image not found: {}. Returning to game selector.", boot_image);
	else if (!enable_fsui)
		INFO_LOG("FullscreenUI disabled and no launch image. Please provide a target to display.");

	std::atomic_bool stop_loop{false};
	std::thread input_thread([&]() {
		std::array<u64, NUM_LOCAL_PLAYERS> held{};
		// Buttons in these masks are ignored until they are released.
		// game_block: the menu combo, and buttons that were held while a menu was open.
		// nav_block: buttons already held when a menu opened or when a system applet closed.
		std::array<u64, NUM_LOCAL_PLAYERS> game_block{};
		u64 nav_block = 0;
		u64 prev_nav = 0;
		bool prev_fsui_active = false;
		bool prev_menu_combo = false;
		bool was_feeding_game = false;
		SwitchInputConfig input_config = ReadSwitchInputConfig();
		HorizonHotkeys::Chords hotkeys(input_config.hotkeys);
		while (!stop_loop.load(std::memory_order_relaxed))
		{
			const bool resync = s_input_resync.exchange(false, std::memory_order_acq_rel);
			for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
			{
				padUpdate(&pads[player]);
				held[player] = padGetButtons(&pads[player]);
				game_block[player] = resync ? held[player] : (game_block[player] & held[player]);
			}

			const bool fsui_active = FullscreenUI::HasActiveWindow();
			// The shortcuts and the deadzone can only change in the menu: read them again when it closes.
			if (prev_fsui_active && !fsui_active)
			{
				input_config = ReadSwitchInputConfig();
				hotkeys.Reconfigure(input_config.hotkeys, held[0]);
			}
			const auto hotkey = hotkeys.Update(held[0], !fsui_active && VMManager::GetState() == VMState::Running);
			DispatchHotkey(hotkey.action);

			const bool feed_game = !fsui_active && VMManager::HasValidVM();
			const bool menu_combo = (held[0] & MENU_COMBO) == MENU_COMBO;

			nav_block = (resync || (fsui_active && !prev_fsui_active)) ? held[0] : (nav_block & held[0]);
			if (menu_combo)
				game_block[0] |= MENU_COMBO;
			if (!feed_game)
			{
				for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
					game_block[player] |= held[player];
			}

			// Menu navigation is player 1 only. When the menu closes, nav drops to 0 so every key gets released.
			const u64 nav = fsui_active ? (held[0] & ~nav_block) : 0;
			FeedNav(nav, nav ^ prev_nav);
			prev_nav = nav;

			// Neutral unless the game is being fed. One neutral snapshot goes out when a menu opens,
			// so nothing stays pressed in the game while it is paused.
			GamePadSnapshot game_pads{};
			if (feed_game)
			{
				for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
				{
					// Player 1's shortcut chords (Minus + button) don't reach the game.
					const u64 hidden = game_block[player] | ((player == 0) ? hotkey.consumed : 0);
					game_pads[player] = ReadGamePad(pads[player], held[player] & ~hidden, input_config.deadzone);
				}
			}
			if (feed_game || was_feeding_game)
				PublishGamePads(game_pads);
			was_feeding_game = feed_game;
			prev_fsui_active = fsui_active;

			if (menu_combo && !prev_menu_combo)
			{
				if (FullscreenUI::IsInitialized() && VMManager::HasValidVM())
					FullscreenUI::OpenPauseMenu();
				else if (!FullscreenUI::IsInitialized())
					HorizonHost::RequestExit();
			}
			prev_menu_combo = menu_combo;

			svcSleepThread(INPUT_POLL_NS);
		}
	});

	while (!HorizonHost::IsExitRequested())
	{
		if (!appletMainLoop())
		{
			HorizonHost::RequestExit();
			break;
		}

		Host::PumpMessagesOnCPUThread();
		if (HorizonUsbStorage::ConsumeChange())
		{
			LogUsbVolumes();
			if (SyncUsbGameRoots() && FullscreenUI::IsInitialized())
				Host::RefreshGameListAsync(false);
		}

		switch (VMManager::GetState())
		{
			case VMState::Running:
				VMManager::Execute(); // returns when paused or stopping
				break;

			case VMState::Stopping:
				VMManager::Shutdown(false);
				break;

			default:
				// Idle while the GS thread presents FullscreenUI
				svcSleepThread(IDLE_POLL_NS);
				break;
		}
	}

	stop_loop.store(true, std::memory_order_relaxed);
	input_thread.join();

	HorizonUpdater::Shutdown();

	Host::CancelGameListRefresh();

	if (VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(false);

	HorizonUsbStorage::Shutdown();

	MTGS::WaitForClose();
	VMManager::Internal::CPUThreadShutdown();
	INFO_LOG("================ Exiting ================");

	// patches.zip is read from the romfs and would otherwise be closed at exit, after the romfs is gone:
	// closing its FILE then crashed in lseek ("the software was closed because an error occurred").
	Patch::ClosePatchesZip();

	if (have_romfs)
		romfsExit();

	// After romfsExit(): until then the running .nro is held open for its romfs.
	HorizonUpdater::FinishPendingInstall();

	if (have_socket)
		socketExit();
	return 0;
}
