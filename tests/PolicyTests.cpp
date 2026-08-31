#include "TestSupport.h"

#include "RuntimePolicy.h"
#include "ReShadeConfig.h"
#include "Localization.h"
#include "AppMenu.h"
#include "UiLayout.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::string_view kNeuralAddon = "DLSS 5 Neural Rendering@renodx-dlss5.addon64";
constexpr std::string_view kNeuralAddonName = "DLSS 5 Neural Rendering";
constexpr std::string_view kNeuralAddonFilename = "renodx-dlss5.addon64";

void write_binary_file(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.is_open());
    if (!output.is_open()) return;
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    CHECK(output.good());
    output.flush();
    CHECK(output.good());
    output.close();
    CHECK(!output.fail());
}

std::string read_binary_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input.is_open());
    if (!input.is_open()) return {};
    input.seekg(0, std::ios::end);
    CHECK(input.good());
    const std::streamoff size = input.tellg();
    CHECK(size >= 0);
    if (size < 0) return {};
    input.seekg(0, std::ios::beg);
    CHECK(input.good());
    std::string content(static_cast<size_t>(size), '\0');
    if (!content.empty()) input.read(content.data(), static_cast<std::streamsize>(content.size()));
    CHECK(input.good() || input.eof());
    CHECK_EQ(static_cast<std::streamsize>(content.size()), input.gcount());
    input.close();
    CHECK(!input.fail());
    return content;
}

void remove_file_if_present(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
    CHECK(!std::filesystem::exists(path, error));
    CHECK(!error);
}

struct MenuEntry {
    std::wstring text;
    UINT command;
};

void collect_menu_entries(HMENU menu, std::vector<MenuEntry>& entries)
{
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU;
        GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item);

        std::wstring text(item.cch + 1, L'\0');
        item.dwTypeData = text.data();
        item.cch = static_cast<UINT>(text.size());
        GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item);
        text.resize(item.cch);
        entries.push_back({std::move(text), item.wID});
        if (item.hSubMenu) collect_menu_entries(item.hSubMenu, entries);
    }
}

bool has_menu_entry(const std::vector<MenuEntry>& entries, std::wstring_view text, UINT command)
{
    for (const auto& entry : entries) {
        if (entry.text == text && entry.command == command) return true;
    }
    return false;
}

bool has_menu_text(const std::vector<MenuEntry>& entries, std::wstring_view text)
{
    for (const auto& entry : entries) {
        if (entry.text == text) return true;
    }
    return false;
}

std::vector<ToolbarAction> toolbar_actions(const std::vector<ToolbarItem>& items)
{
    std::vector<ToolbarAction> actions;
    actions.reserve(items.size());
    for (const auto& item : items) actions.push_back(item.action);
    return actions;
}

const ToolbarItem* find_toolbar_item(const std::vector<ToolbarItem>& items, ToolbarAction action)
{
    for (const auto& item : items) {
        if (item.action == action) return &item;
    }
    return nullptr;
}

void check_toolbar_items_do_not_overlap(const std::vector<ToolbarItem>& items)
{
    for (size_t left = 0; left < items.size(); ++left) {
        for (size_t right = left + 1; right < items.size(); ++right) {
            const RECT& a = items[left].bounds;
            const RECT& b = items[right].bounds;
            CHECK(a.right <= b.left || b.right <= a.left ||
                  a.bottom <= b.top || b.bottom <= a.top);
        }
    }
}

void toolbar_layout_selects_stable_action_sets_for_width_modes_test()
{
    const std::vector<ToolbarAction> requiredNarrow{
        ToolbarAction::Open,
        ToolbarAction::PlayPause,
        ToolbarAction::Mute,
        ToolbarAction::ToggleDlss,
        ToolbarAction::Fullscreen,
    };
    const std::vector<ToolbarAction> allActions{
        ToolbarAction::Open,
        ToolbarAction::Back10,
        ToolbarAction::PlayPause,
        ToolbarAction::Stop,
        ToolbarAction::Forward10,
        ToolbarAction::Mute,
        ToolbarAction::ToggleDlss,
        ToolbarAction::Aspect,
        ToolbarAction::Adjustments,
        ToolbarAction::DebugView,
        ToolbarAction::Fullscreen,
    };

    const auto narrow = LayoutToolbar(320, 180, 96);
    CHECK_EQ(requiredNarrow, toolbar_actions(narrow));
    for (const auto& item : narrow) CHECK(item.compact);

    const auto normal = LayoutToolbar(640, 180, 96);
    CHECK_EQ(allActions, toolbar_actions(normal));
    for (const auto& item : normal) CHECK(item.compact);

    const auto wide = LayoutToolbar(1000, 180, 96);
    CHECK_EQ(allActions, toolbar_actions(wide));
    for (const auto& item : wide) CHECK(!item.compact);
}

void toolbar_layout_preserves_group_separation_test()
{
    const auto items = LayoutToolbar(1000, 180, 96);
    const ToolbarItem* back = find_toolbar_item(items, ToolbarAction::Back10);
    const ToolbarItem* play = find_toolbar_item(items, ToolbarAction::PlayPause);
    const ToolbarItem* mute = find_toolbar_item(items, ToolbarAction::Mute);
    const ToolbarItem* dlss = find_toolbar_item(items, ToolbarAction::ToggleDlss);
    const ToolbarItem* adjustments = find_toolbar_item(items, ToolbarAction::Adjustments);
    const ToolbarItem* debug = find_toolbar_item(items, ToolbarAction::DebugView);
    const ToolbarItem* fullscreen = find_toolbar_item(items, ToolbarAction::Fullscreen);
    CHECK(back && play && mute && dlss && adjustments && debug && fullscreen);
    if (!(back && play && mute && dlss && adjustments && debug && fullscreen)) return;

    CHECK_EQ(4L, play->bounds.left - back->bounds.right);
    CHECK_EQ(12L, dlss->bounds.left - mute->bounds.right);
    CHECK_EQ(12L, debug->bounds.left - adjustments->bounds.right);
    CHECK_EQ(4L, fullscreen->bounds.left - debug->bounds.right);
}

void toolbar_layout_scales_hit_height_and_avoids_overlap_test()
{
    const auto narrow = LayoutToolbar(320, 180, 96);
    const auto normal = LayoutToolbar(640, 180, 96);
    const auto wide = LayoutToolbar(1000, 180, 96);
    const auto scaled = LayoutToolbar(960, 300, 144);

    for (const auto* items : {&narrow, &normal, &wide}) {
        CHECK(!items->empty());
        for (const auto& item : *items) {
            CHECK(item.bounds.bottom - item.bounds.top >= 36);
        }
        check_toolbar_items_do_not_overlap(*items);
    }
    CHECK(!scaled.empty());
    for (const auto& item : scaled) {
        CHECK(item.bounds.bottom - item.bounds.top >= 54);
    }
    check_toolbar_items_do_not_overlap(scaled);

    CHECK_EQ(16L, wide.front().bounds.left);
    CHECK(wide.back().bounds.right <= 1000 - 16);
    CHECK_EQ(24L, scaled.front().bounds.left);
    CHECK(scaled.back().bounds.right <= 960 - 24);
}

void toolbar_hit_testing_is_half_open_and_boundary_stable_test()
{
    const auto items = LayoutToolbar(640, 180, 96);
    CHECK(!items.empty());
    for (const auto& item : items) {
        const LONG middleY = item.bounds.top + (item.bounds.bottom - item.bounds.top) / 2;
        CHECK_EQ(item.action, HitTestToolbar(items, POINT{item.bounds.left, middleY}));
        CHECK_EQ(item.action, HitTestToolbar(items, POINT{item.bounds.right - 1, middleY}));
        CHECK_EQ(ToolbarAction::None, HitTestToolbar(items, POINT{item.bounds.right, middleY}));
    }
    CHECK_EQ(ToolbarAction::None, HitTestToolbar(items, POINT{-1, -1}));
}

void player_menu_is_english_only_and_retains_advanced_commands_test()
{
    Localizer localizer;
    const HMENU menu = app_menu::CreateMenuBar(localizer);
    CHECK(menu != nullptr);

    std::vector<MenuEntry> entries;
    if (menu) collect_menu_entries(menu, entries);
    CHECK(!has_menu_text(entries, L"Language"));
    CHECK(has_menu_text(entries, L"Advanced"));
    CHECK(has_menu_entry(entries, L"Restart in DLSS SR safe mode", app_menu::IDM_ADVANCED_SAFE_MODE));
    CHECK(has_menu_entry(entries, L"Recreate NGX / re-hook DLSS 5\tF6", app_menu::IDM_REHOOK));
    for (const auto& entry : entries) {
        CHECK(entry.command < 500 || entry.command >= 600);
    }

    if (menu) DestroyMenu(menu);
}

std::filesystem::path executable_directory()
{
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    return std::filesystem::path(executablePath, executablePath + length).parent_path();
}

struct RestoredFile {
    explicit RestoredFile(std::filesystem::path file)
        : path(std::move(file))
    {
        std::error_code error;
        existed = std::filesystem::exists(path, error);
        CHECK(!error);
        if (existed) original = read_binary_file(path);
    }

    ~RestoredFile()
    {
        Restore();
    }

    void Restore()
    {
        if (restored) return;
        std::error_code error;
        if (existed) {
            write_binary_file(path, original);
            CHECK_EQ(original, read_binary_file(path));
        } else {
            std::filesystem::remove(path, error);
            CHECK(!error);
            CHECK(!std::filesystem::exists(path, error));
            CHECK(!error);
        }
        restored = true;
    }

    std::filesystem::path path;
    bool existed = false;
    std::string original;
    bool restored = false;
};

void legacy_language_configuration_is_ignored_and_english_lookup_remains_builtin_test()
{
    const std::filesystem::path runtimeDirectory = executable_directory();
    const std::filesystem::path configuration = runtimeDirectory / "DLSSVideoPlayer.ini";
    const std::filesystem::path languageDirectory = runtimeDirectory / "languages";
    const std::filesystem::path portuguesePack = languageDirectory / "pt-BR.lang";
    std::error_code error;
    const bool languageDirectoryExisted = std::filesystem::exists(languageDirectory, error);
    CHECK(!error);
    CHECK(!languageDirectoryExisted);
    std::filesystem::create_directories(languageDirectory, error);
    CHECK(!error);
    RestoredFile restoreConfiguration(configuration);
    RestoredFile restorePortuguesePack(portuguesePack);
    write_binary_file(configuration, "[General]\r\nLanguage=pt-BR\r\n");
    write_binary_file(portuguesePack, "app.title=Leitor em Portugues\r\nmenu.file=Arquivo\r\n");

    Localizer localizer;
    localizer.Initialize();

    CHECK_EQ(std::wstring(L"DLSS Video Player"), localizer.Get(L"app.title"));
    CHECK_EQ(std::wstring(L"File"), localizer.Get(L"menu.file"));

    restorePortuguesePack.Restore();
    restoreConfiguration.Restore();
    if (!languageDirectoryExisted) {
        std::filesystem::remove(languageDirectory, error);
        CHECK(!error);
        CHECK(!std::filesystem::exists(languageDirectory, error));
        CHECK(!error);
    }
}

void harness_sanity_test()
{
    CHECK(true);
    CHECK_EQ(2 + 2, 4);
}

void gpu_classification_table_test()
{
    struct Case {
        uint32_t vendor_id;
        std::wstring_view description;
        GpuGeneration expected;
    };

    constexpr Case cases[] = {
        {0x10DE, L"NVIDIA GeForce RTX 4090", GpuGeneration::Rtx40Ada},
        {0x10DE, L"NVIDIA GeForce RTX 4090 Laptop GPU", GpuGeneration::Rtx40Ada},
        {0x10DE, L"nViDiA gEfOrCe rTx 5090", GpuGeneration::Rtx50Blackwell},
        {0x10DE, L"NVIDIA GeForce RTX 3090", GpuGeneration::OtherNvidia},
        {0x1002, L"AMD Radeon RX 7900 XTX", GpuGeneration::Unsupported},
        {0x8086, L"Intel(R) Arc(TM) A770 Graphics", GpuGeneration::Unsupported},
        {0x10DE, L"", GpuGeneration::OtherNvidia},
        {0, L"GeForce RTX 5090", GpuGeneration::Unsupported},
        {0, L"", GpuGeneration::Unsupported},
    };

    for (const auto& test : cases) {
        CHECK_EQ(test.expected, ClassifyGpu(test.vendor_id, test.description));
    }
}

void neural_addon_policy_test()
{
    CHECK(NeuralAddonDesired(GpuGeneration::Rtx40Ada, false));
    CHECK(NeuralAddonDesired(GpuGeneration::Rtx50Blackwell, false));
    CHECK(!NeuralAddonDesired(GpuGeneration::Rtx40Ada, true));
    CHECK(!NeuralAddonDesired(GpuGeneration::Rtx50Blackwell, true));
    CHECK(!NeuralAddonDesired(GpuGeneration::OtherNvidia, false));
    CHECK(!NeuralAddonDesired(GpuGeneration::Unsupported, false));
}

void bootstrap_action_matrix_test()
{
    struct Case {
        bool desired_enabled;
        bool config_enabled;
        bool already_restarted;
        bool update_succeeded;
        BootstrapAction expected;
    };

    constexpr Case cases[] = {
        {true, true, false, true, BootstrapAction::Continue},
        {false, false, false, true, BootstrapAction::Continue},
        {true, true, true, true, BootstrapAction::Continue},
        {false, false, true, true, BootstrapAction::Continue},
        {true, false, false, true, BootstrapAction::Relaunch},
        {false, true, false, true, BootstrapAction::Relaunch},
        {true, false, true, true, BootstrapAction::Fail},
        {false, true, true, true, BootstrapAction::Fail},
        {true, true, false, false, BootstrapAction::Fail},
        {false, true, false, false, BootstrapAction::Fail},
        {true, false, true, false, BootstrapAction::Fail},
    };

    for (const auto& test : cases) {
        CHECK_EQ(test.expected, DecideBootstrap(
            test.desired_enabled,
            test.config_enabled,
            test.already_restarted,
            test.update_succeeded));
    }
}

void windows_command_line_quoting_round_trip_test()
{
    constexpr std::wstring_view executable = L"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe";
    const std::vector<std::wstring> arguments = {
        L"movie.mp4",
        L"C:\\Videos\\clip with spaces.mp4",
        L"",
        L"--future-option=\"quoted value\"",
        L"C:\\trailing slash\\",
        L"plain\\slashes",
        L"embedded\"quote",
    };
    constexpr std::wstring_view expected =
        L"\"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe\" "
        L"\"movie.mp4\" "
        L"\"C:\\Videos\\clip with spaces.mp4\" "
        L"\"\" "
        L"\"--future-option=\\\"quoted value\\\"\" "
        L"\"C:\\trailing slash\\\\\" "
        L"\"plain\\slashes\" "
        L"\"embedded\\\"quote\"";

    const std::wstring commandLine = BuildWindowsCommandLine(executable, arguments);
    CHECK_EQ(std::wstring(expected), commandLine);

    int parsedCount = 0;
    LPWSTR* parsed = CommandLineToArgvW(commandLine.c_str(), &parsedCount);
    CHECK(parsed != nullptr);
    if (parsed) {
        CHECK_EQ(static_cast<int>(arguments.size() + 1), parsedCount);
        if (parsedCount == static_cast<int>(arguments.size() + 1)) {
            CHECK_EQ(std::wstring(executable), std::wstring(parsed[0]));
            for (size_t index = 0; index < arguments.size(); ++index) {
                CHECK_EQ(arguments[index], std::wstring(parsed[index + 1]));
            }
        }
        LocalFree(parsed);
    }
}

void runtime_argument_parsing_preserves_user_arguments_and_strips_markers_test()
{
    const wchar_t* argv[] = {
        L"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe",
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"quoted\"value",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
    };
    const std::vector<std::wstring> expected = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"quoted\"value",
        L"--safe-mode",
    };

    const RuntimeArguments parsed = ParseRuntimeArguments(static_cast<int>(std::size(argv)), argv);
    CHECK(parsed.ok);
    CHECK(parsed.safeMode);
    CHECK(parsed.addonBootstrapRestarted);
    CHECK(parsed.error.empty());
    CHECK_EQ(expected.size(), parsed.userArguments.size());
    if (parsed.userArguments.size() == expected.size()) {
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK_EQ(expected[index], parsed.userArguments[index]);
        }
    }

    const RuntimeArguments failed = ParseRuntimeArguments(0, nullptr);
    CHECK(!failed.ok);
    CHECK(!failed.safeMode);
    CHECK(!failed.addonBootstrapRestarted);
    CHECK(failed.userArguments.empty());
    CHECK(!failed.error.empty());
}

void observed_config_bootstrap_decision_test()
{
    CHECK_EQ(BootstrapAction::Continue,
        DecideBootstrapFromObservedUpdate(true, true, true, false, true));
    CHECK_EQ(BootstrapAction::Continue,
        DecideBootstrapFromObservedUpdate(false, false, false, true, true));
    CHECK_EQ(BootstrapAction::Relaunch,
        DecideBootstrapFromObservedUpdate(true, false, true, false, true));
    CHECK_EQ(BootstrapAction::Fail,
        DecideBootstrapFromObservedUpdate(true, false, true, true, true));
    CHECK_EQ(BootstrapAction::Fail,
        DecideBootstrapFromObservedUpdate(true, false, false, false, true));
    CHECK_EQ(BootstrapAction::Fail,
        DecideBootstrapFromObservedUpdate(false, true, false, false, false));
}

void restart_argument_lifecycle_and_create_process_command_line_test()
{
    const std::vector<std::wstring> contaminated = {
        L"--future-flag",
        L"",
        L"--addon-bootstrap-restarted",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
    };

    const std::vector<std::wstring> bootstrap = BuildBootstrapRelaunchArguments(contaminated);
    const std::vector<std::wstring> expectedBootstrap = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
        L"--addon-bootstrap-restarted",
    };
    CHECK_EQ(expectedBootstrap.size(), bootstrap.size());
    if (bootstrap.size() == expectedBootstrap.size()) {
        for (size_t index = 0; index < bootstrap.size(); ++index) {
            CHECK_EQ(expectedBootstrap[index], bootstrap[index]);
        }
    }

    const std::vector<std::wstring> safeMode = BuildSafeModeRestartArguments(contaminated);
    const std::vector<std::wstring> expectedSafeMode = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
    };
    CHECK_EQ(expectedSafeMode.size(), safeMode.size());
    if (safeMode.size() == expectedSafeMode.size()) {
        for (size_t index = 0; index < safeMode.size(); ++index) {
            CHECK_EQ(expectedSafeMode[index], safeMode[index]);
        }
    }

    constexpr std::wstring_view executable = L"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe";
    constexpr std::wstring_view expectedCommandLine =
        L"\"C:\\Program Files\\DLSS Player\\DLSSVideoPlayer.exe\" "
        L"\"--future-flag\" \"\" "
        L"\"C:\\Videos\\clip with spaces.mp4\" "
        L"\"--safe-mode\" \"--addon-bootstrap-restarted\"";
    CHECK_EQ(std::wstring(expectedCommandLine), BuildWindowsCommandLine(executable, bootstrap));
}

void advanced_safe_mode_normal_invocation_adds_safe_mode_test()
{
    const std::vector<std::wstring> normalArguments = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
    };
    const std::vector<std::wstring> expected = {
        L"--future-flag",
        L"",
        L"C:\\Videos\\clip with spaces.mp4",
        L"--safe-mode",
    };
    int launchCalls = 0;
    std::vector<std::wstring> launchedArguments;

    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        true,
        normalArguments,
        [&](const std::vector<std::wstring>& arguments) {
            ++launchCalls;
            launchedArguments = arguments;
            return true;
        });

    CHECK_EQ(SafeModeRestartOutcome::CloseCurrent, outcome);
    CHECK_EQ(1, launchCalls);
    CHECK_EQ(expected.size(), launchedArguments.size());
    if (launchedArguments.size() == expected.size()) {
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK_EQ(expected[index], launchedArguments[index]);
        }
    }
}

void advanced_safe_mode_cancel_keeps_current_open_without_launch_test()
{
    int launchCalls = 0;
    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        false,
        {L"--future-flag"},
        [&](const std::vector<std::wstring>&) {
            ++launchCalls;
            return true;
        });

    CHECK_EQ(SafeModeRestartOutcome::Cancelled, outcome);
    CHECK_EQ(0, launchCalls);
}

void advanced_safe_mode_launch_failure_keeps_current_open_test()
{
    int launchCalls = 0;
    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        true,
        {L"--future-flag"},
        [&](const std::vector<std::wstring>& arguments) {
            ++launchCalls;
            CHECK_EQ(2u, arguments.size());
            if (arguments.size() == 2) {
                CHECK_EQ(std::wstring(L"--future-flag"), arguments[0]);
                CHECK_EQ(std::wstring(L"--safe-mode"), arguments[1]);
            }
            return false;
        });

    CHECK_EQ(SafeModeRestartOutcome::LaunchFailed, outcome);
    CHECK_EQ(1, launchCalls);
}

void advanced_safe_mode_launch_success_closes_with_sanitized_arguments_test()
{
    const std::vector<std::wstring> contaminated = {
        L"--addon-bootstrap-restarted",
        L"--safe-mode",
        L"--future-flag",
        L"--safe-mode",
    };
    const std::vector<std::wstring> expected = {
        L"--safe-mode",
        L"--future-flag",
    };
    int launchCalls = 0;
    std::vector<std::wstring> launchedArguments;

    const SafeModeRestartOutcome outcome = ExecuteAdvancedSafeModeRestart(
        true,
        contaminated,
        [&](const std::vector<std::wstring>& arguments) {
            ++launchCalls;
            launchedArguments = arguments;
            return true;
        });

    CHECK_EQ(SafeModeRestartOutcome::CloseCurrent, outcome);
    CHECK_EQ(1, launchCalls);
    CHECK_EQ(expected.size(), launchedArguments.size());
    if (launchedArguments.size() == expected.size()) {
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK_EQ(expected[index], launchedArguments[index]);
        }
    }
}

void disabled_addons_creates_missing_addon_section_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "PresetPath=C:\\Games\\Player\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "PresetPath=C:\\Games\\Player\r\n"
        "[ADDON]\r\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_updates_empty_and_populated_lists_test()
{
    constexpr std::string_view emptyInput =
        "[ADDON]\n"
        "DisabledAddons=\n"
        "[INPUT]\n"
        "KeyMenu=36\n";
    constexpr std::string_view emptyExpected =
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[INPUT]\n"
        "KeyMenu=36\n";
    constexpr std::string_view populatedInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,third-party.addon64\n";
    constexpr std::string_view populatedExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,third-party.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";

    CHECK_EQ(std::string(emptyExpected), UpdateDisabledAddonsIni(emptyInput, kNeuralAddon, true));
    CHECK_EQ(std::string(populatedExpected), UpdateDisabledAddonsIni(populatedInput, kNeuralAddon, true));
}

void disabled_addons_preserves_mixed_line_endings_and_unrelated_sections_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "NoReloadOnInit=1\n"
        "[ADDON]\r"
        "DisabledAddons=legacy.addon64\r"
        "[OVERLAY]\n"
        "TutorialProgress=3\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "NoReloadOnInit=1\n"
        "[ADDON]\r"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\r"
        "[OVERLAY]\n"
        "TutorialProgress=3\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_removes_only_exact_target_entries_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,renodx-dlss5.addon64.bak,DLSS 5 Neural Rendering@renodx-dlss5.addon64,other.addon64\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64.bak,other.addon64\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
}

void disabled_addons_collapses_only_exact_target_duplicates_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,renodx-dlss5.addon64.bak\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,renodx-dlss5.addon64.bak\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64, DLSS 5 Neural Rendering@renodx-dlss5.addon64,  other.addon64\n";
    constexpr std::string_view enabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,  other.addon64\n";

    CHECK_EQ(std::string(enabledExpected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
    CHECK_EQ(std::string(input), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
    CHECK_EQ(std::string(input), UpdateDisabledAddonsIni(
        UpdateDisabledAddonsIni(input, kNeuralAddon, true), kNeuralAddon, true));
}

void reshade_68_disabled_addon_token_conformance_test()
{
    const std::string canonical = "[ADDON]\nDisabledAddons=" + std::string(kNeuralAddon) + "\n";
    const std::string registeredName = "[ADDON]\nDisabledAddons=" + std::string(kNeuralAddonName) + "\n";
    const std::string atFilename = "[ADDON]\nDisabledAddons=@" + std::string(kNeuralAddonFilename) + "\n";
    const std::string legacyBareFilename = "[ADDON]\nDisabledAddons=" + std::string(kNeuralAddonFilename) + "\n";
    const std::string wrongCaseCanonical =
        "[ADDON]\nDisabledAddons=dlss 5 neural rendering@RENODX-DLSS5.ADDON64\n";

    const ConfigUpdate canonicalState = EvaluateNeuralAddonConfigUpdate(canonical, canonical, false, false);
    CHECK(canonicalState.ok);
    CHECK(!canonicalState.addonEnabled);
    const ConfigUpdate nameState = EvaluateNeuralAddonConfigUpdate(registeredName, registeredName, false, false);
    CHECK(nameState.ok);
    CHECK(!nameState.addonEnabled);
    const ConfigUpdate filenameState = EvaluateNeuralAddonConfigUpdate(atFilename, atFilename, false, false);
    CHECK(filenameState.ok);
    CHECK(!filenameState.addonEnabled);
    const ConfigUpdate legacyState = EvaluateNeuralAddonConfigUpdate(
        legacyBareFilename, legacyBareFilename, false, true);
    CHECK(legacyState.ok);
    CHECK(legacyState.addonEnabled);
    const ConfigUpdate wrongCaseState = EvaluateNeuralAddonConfigUpdate(
        wrongCaseCanonical, wrongCaseCanonical, false, true);
    CHECK(wrongCaseState.ok);
    CHECK(wrongCaseState.addonEnabled);
}

void reshade_68_aliases_migrate_to_one_canonical_token_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering,@renodx-dlss5.addon64,renodx-dlss5.addon64,dlss 5 neural rendering@RENODX-DLSS5.ADDON64,other.addon64\n";
    constexpr std::string_view disabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64,other.addon64\n";
    constexpr std::string_view enabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,other.addon64\n";

    const std::string disabled = UpdateDisabledAddonsIni(input, kNeuralAddon, true);
    CHECK_EQ(std::string(disabledExpected), disabled);
    CHECK_EQ(disabled, UpdateDisabledAddonsIni(disabled, kNeuralAddon, true));
    CHECK_EQ(std::string(enabledExpected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
}

void reshade_68_section_and_key_lookup_are_case_sensitive_test()
{
    constexpr std::string_view wrongCaseSection =
        "[addon]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    constexpr std::string_view wrongCaseSectionExpected =
        "[addon]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    constexpr std::string_view wrongCaseKey =
        "[ADDON]\r\n"
        "disabledaddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n"
        "KeyOverlay=36\r\n";
    constexpr std::string_view wrongCaseKeyExpected =
        "[ADDON]\r\n"
        "disabledaddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n"
        "KeyOverlay=36\r\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n";

    CHECK_EQ(std::string(wrongCaseSectionExpected),
        UpdateDisabledAddonsIni(wrongCaseSection, kNeuralAddon, true));
    CHECK_EQ(std::string(wrongCaseKeyExpected),
        UpdateDisabledAddonsIni(wrongCaseKey, kNeuralAddon, true));
}

void reshade_68_utf8_bom_is_ignored_for_lookup_and_preserved_test()
{
    const std::string input =
        std::string("\xEF\xBB\xBF") + "[ADDON]\r\nDisabledAddons=" + std::string(kNeuralAddon) + "\r\n";
    const std::string enabledExpected =
        std::string("\xEF\xBB\xBF") + "[ADDON]\r\nDisabledAddons=\r\n";

    CHECK_EQ(input, UpdateDisabledAddonsIni(input, kNeuralAddon, true));
    CHECK_EQ(enabledExpected, UpdateDisabledAddonsIni(input, kNeuralAddon, false));
    const ConfigUpdate state = EvaluateNeuralAddonConfigUpdate(input, input, false, false);
    CHECK(state.ok);
    CHECK(!state.addonEnabled);
}

void disabled_addons_insertion_uses_target_section_line_ending_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "PresetPath=.\\ReShadePreset.ini\r\n"
        "[ADDON]\n"
        "KeyOverlay=36\n"
        "[INPUT]\r\n"
        "KeyMenu=36\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "PresetPath=.\\ReShadePreset.ini\r\n"
        "[ADDON]\n"
        "KeyOverlay=36\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n"
        "[INPUT]\r\n"
        "KeyMenu=36\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void configure_neural_addon_is_idempotent_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade.ini";
    remove_file_if_present(path);
    constexpr std::string_view input =
        "[GENERAL]\n"
        "PresetPath=C:\\Games\\Player\n"
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    constexpr std::string_view expected =
        "[GENERAL]\n"
        "PresetPath=C:\\Games\\Player\n"
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n";
    write_binary_file(path, input);

    const ConfigUpdate first = ConfigureNeuralAddon(path, true);
    CHECK(first.ok);
    CHECK(first.changed);
    CHECK(first.addonEnabled);
    CHECK(first.error.empty());
    CHECK_EQ(std::string(expected), read_binary_file(path));

    const std::string afterFirst = read_binary_file(path);
    const ConfigUpdate second = ConfigureNeuralAddon(path, true);
    CHECK(second.ok);
    CHECK(!second.changed);
    CHECK(second.previousAddonEnabled);
    CHECK(second.addonEnabled);
    CHECK(second.error.empty());
    CHECK_EQ(afterFirst, read_binary_file(path));

    remove_file_if_present(path);
}

void configure_neural_addon_reports_semantic_state_across_text_canonicalization_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-semantic.ini";
    remove_file_if_present(path);

    constexpr std::string_view missingAddonInput =
        "[GENERAL]\n"
        "PresetPath=.\\ReShadePreset.ini\n";
    write_binary_file(path, missingAddonInput);
    const ConfigUpdate missingAddon = ConfigureNeuralAddon(path, true);
    CHECK(missingAddon.ok);
    CHECK(missingAddon.changed);
    CHECK(missingAddon.previousAddonEnabled);
    CHECK(missingAddon.addonEnabled);

    constexpr std::string_view duplicateTargetInput =
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64,legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    write_binary_file(path, duplicateTargetInput);
    const ConfigUpdate duplicateTarget = ConfigureNeuralAddon(path, false);
    CHECK(duplicateTarget.ok);
    CHECK(duplicateTarget.changed);
    CHECK(!duplicateTarget.previousAddonEnabled);
    CHECK(!duplicateTarget.addonEnabled);

    remove_file_if_present(path);
}

void configure_neural_addon_safe_then_normal_observes_reshade_state_test()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "PolicyTests-ReShade-safe-normal.ini";
    remove_file_if_present(path);
    constexpr std::string_view legacyInput =
        "[ADDON]\r\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64\r\n";
    constexpr std::string_view safeExpected =
        "[ADDON]\r\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\r\n";
    constexpr std::string_view normalExpected =
        "[ADDON]\r\n"
        "DisabledAddons=legacy.addon64\r\n";
    write_binary_file(path, legacyInput);

    const ConfigUpdate safe = ConfigureNeuralAddon(path, false);
    CHECK(safe.ok);
    CHECK(safe.changed);
    CHECK(safe.previousAddonEnabled);
    CHECK(!safe.addonEnabled);
    CHECK_EQ(std::string(safeExpected), read_binary_file(path));

    const ConfigUpdate safeAgain = ConfigureNeuralAddon(path, false);
    CHECK(safeAgain.ok);
    CHECK(!safeAgain.changed);
    CHECK(!safeAgain.previousAddonEnabled);
    CHECK(!safeAgain.addonEnabled);
    CHECK_EQ(std::string(safeExpected), read_binary_file(path));

    const ConfigUpdate normal = ConfigureNeuralAddon(path, true);
    CHECK(normal.ok);
    CHECK(normal.changed);
    CHECK(!normal.previousAddonEnabled);
    CHECK(normal.addonEnabled);
    CHECK_EQ(std::string(normalExpected), read_binary_file(path));

    const ConfigUpdate normalAgain = ConfigureNeuralAddon(path, true);
    CHECK(normalAgain.ok);
    CHECK(!normalAgain.changed);
    CHECK(normalAgain.previousAddonEnabled);
    CHECK(normalAgain.addonEnabled);
    CHECK_EQ(std::string(normalExpected), read_binary_file(path));

    remove_file_if_present(path);
}

void evaluated_config_update_observes_actual_final_bytes_test()
{
    constexpr std::string_view enabledIni =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n";
    constexpr std::string_view disabledIni =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";

    const ConfigUpdate mismatch = EvaluateNeuralAddonConfigUpdate(enabledIni, disabledIni, true, true);
    CHECK(!mismatch.ok);
    CHECK(mismatch.changed);
    CHECK(mismatch.previousAddonEnabled);
    CHECK(!mismatch.addonEnabled);
    CHECK(!mismatch.error.empty());

    const ConfigUpdate observed = EvaluateNeuralAddonConfigUpdate(disabledIni, enabledIni, true, true);
    CHECK(observed.ok);
    CHECK(observed.changed);
    CHECK(!observed.previousAddonEnabled);
    CHECK(observed.addonEnabled);
    CHECK(observed.error.empty());
}

void configure_neural_addon_fails_closed_for_malformed_ini_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-malformed.ini";
    remove_file_if_present(path);
    constexpr char nulContent[] = "[ADDON]\nDisabledAddons=legacy.addon64\0tail";
    const std::string nulInput{nulContent, sizeof(nulContent) - 1};
    write_binary_file(path, nulInput);

    const ConfigUpdate nulResult = ConfigureNeuralAddon(path, false);
    CHECK(!nulResult.ok);
    CHECK(!nulResult.changed);
    CHECK(!nulResult.addonEnabled);
    CHECK(!nulResult.error.empty());
    CHECK_EQ(nulInput, read_binary_file(path));

    constexpr std::string_view duplicateInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    write_binary_file(path, duplicateInput);
    const ConfigUpdate duplicateResult = ConfigureNeuralAddon(path, true);
    CHECK(!duplicateResult.ok);
    CHECK(!duplicateResult.changed);
    CHECK(!duplicateResult.addonEnabled);
    CHECK(!duplicateResult.error.empty());
    CHECK_EQ(std::string(duplicateInput), read_binary_file(path));

    constexpr std::string_view crossSectionDuplicateInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64\n"
        "[ADDON]\n"
        "DisabledAddons=DLSS 5 Neural Rendering@renodx-dlss5.addon64\n";
    write_binary_file(path, crossSectionDuplicateInput);
    const ConfigUpdate crossSectionResult = ConfigureNeuralAddon(path, true);
    CHECK(!crossSectionResult.ok);
    CHECK(!crossSectionResult.changed);
    CHECK(!crossSectionResult.addonEnabled);
    CHECK(!crossSectionResult.error.empty());
    CHECK_EQ(std::string(crossSectionDuplicateInput), read_binary_file(path));

    remove_file_if_present(path);
}

void configure_neural_addon_rejects_non_regular_path_before_replacement_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-directory";
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
    CHECK(!removeError);
    std::filesystem::create_directory(path, removeError);
    CHECK(!removeError);
    CHECK(std::filesystem::is_directory(path, removeError));
    CHECK(!removeError);

    const ConfigUpdate result = ConfigureNeuralAddon(path, true);
    CHECK(!result.ok);
    CHECK(!result.changed);
    CHECK(!result.addonEnabled);
    CHECK(!result.error.empty());
    CHECK(std::filesystem::is_directory(path));

    std::filesystem::remove_all(path, removeError);
    CHECK(!removeError);
    CHECK(!std::filesystem::exists(path, removeError));
    CHECK(!removeError);
}

} // namespace

int main()
{
    harness_sanity_test();
    toolbar_layout_selects_stable_action_sets_for_width_modes_test();
    toolbar_layout_preserves_group_separation_test();
    toolbar_layout_scales_hit_height_and_avoids_overlap_test();
    toolbar_hit_testing_is_half_open_and_boundary_stable_test();
    player_menu_is_english_only_and_retains_advanced_commands_test();
    legacy_language_configuration_is_ignored_and_english_lookup_remains_builtin_test();
    gpu_classification_table_test();
    neural_addon_policy_test();
    bootstrap_action_matrix_test();
    windows_command_line_quoting_round_trip_test();
    runtime_argument_parsing_preserves_user_arguments_and_strips_markers_test();
    observed_config_bootstrap_decision_test();
    restart_argument_lifecycle_and_create_process_command_line_test();
    advanced_safe_mode_normal_invocation_adds_safe_mode_test();
    advanced_safe_mode_cancel_keeps_current_open_without_launch_test();
    advanced_safe_mode_launch_failure_keeps_current_open_test();
    advanced_safe_mode_launch_success_closes_with_sanitized_arguments_test();
    disabled_addons_creates_missing_addon_section_test();
    disabled_addons_updates_empty_and_populated_lists_test();
    disabled_addons_preserves_mixed_line_endings_and_unrelated_sections_test();
    disabled_addons_removes_only_exact_target_entries_test();
    disabled_addons_collapses_only_exact_target_duplicates_test();
    disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test();
    reshade_68_disabled_addon_token_conformance_test();
    reshade_68_aliases_migrate_to_one_canonical_token_test();
    reshade_68_section_and_key_lookup_are_case_sensitive_test();
    reshade_68_utf8_bom_is_ignored_for_lookup_and_preserved_test();
    disabled_addons_insertion_uses_target_section_line_ending_test();
    configure_neural_addon_is_idempotent_test();
    configure_neural_addon_reports_semantic_state_across_text_canonicalization_test();
    configure_neural_addon_safe_then_normal_observes_reshade_state_test();
    evaluated_config_update_observes_actual_final_bytes_test();
    configure_neural_addon_fails_closed_for_malformed_ini_test();
    configure_neural_addon_rejects_non_regular_path_before_replacement_test();

    if (test_support::failure_count != 0) {
        std::cerr << test_support::failure_count << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "PolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
