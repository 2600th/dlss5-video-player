#include "TestSupport.h"

#include "RuntimePolicy.h"
#include "ReShadeConfig.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::string_view kNeuralAddon = "renodx-dlss5.addon64";

void write_binary_file(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_binary_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

void disabled_addons_creates_missing_addon_section_test()
{
    constexpr std::string_view input =
        "[GENERAL]\r\n"
        "PresetPath=C:\\Games\\Player\r\n";
    constexpr std::string_view expected =
        "[GENERAL]\r\n"
        "PresetPath=C:\\Games\\Player\r\n"
        "[ADDON]\r\n"
        "DisabledAddons=renodx-dlss5.addon64\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_updates_empty_and_populated_lists_test()
{
    constexpr std::string_view emptyInput =
        "[aDdOn]\n"
        "dIsAbLeDaDdOnS=\n"
        "[INPUT]\n"
        "KeyMenu=36\n";
    constexpr std::string_view emptyExpected =
        "[aDdOn]\n"
        "dIsAbLeDaDdOnS=renodx-dlss5.addon64\n"
        "[INPUT]\n"
        "KeyMenu=36\n";
    constexpr std::string_view populatedInput =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,third-party.addon64\n";
    constexpr std::string_view populatedExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,third-party.addon64,renodx-dlss5.addon64\n";

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
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64\r"
        "[OVERLAY]\n"
        "TutorialProgress=3\r\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_removes_only_exact_target_entries_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64,renodx-dlss5.addon64.bak,renodx-dlss5.addon64,other.addon64\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64.bak,other.addon64\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
}

void disabled_addons_collapses_only_exact_target_duplicates_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64,renodx-dlss5.addon64,renodx-dlss5.addon64.bak\n";
    constexpr std::string_view expected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64,renodx-dlss5.addon64.bak\n";

    CHECK_EQ(std::string(expected), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
}

void disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test()
{
    constexpr std::string_view input =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64, renodx-dlss5.addon64,  other.addon64\n";
    constexpr std::string_view enabledExpected =
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,  other.addon64\n";

    CHECK_EQ(std::string(enabledExpected), UpdateDisabledAddonsIni(input, kNeuralAddon, false));
    CHECK_EQ(std::string(input), UpdateDisabledAddonsIni(input, kNeuralAddon, true));
    CHECK_EQ(std::string(input), UpdateDisabledAddonsIni(
        UpdateDisabledAddonsIni(input, kNeuralAddon, true), kNeuralAddon, true));
}

void configure_neural_addon_is_idempotent_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade.ini";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    constexpr std::string_view input =
        "[GENERAL]\n"
        "PresetPath=C:\\Games\\Player\n"
        "[ADDON]\n"
        "DisabledAddons=legacy.addon64,renodx-dlss5.addon64\n";
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
    CHECK(second.addonEnabled);
    CHECK(second.error.empty());
    CHECK_EQ(afterFirst, read_binary_file(path));

    std::filesystem::remove(path, removeError);
}

void configure_neural_addon_fails_closed_for_malformed_ini_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-malformed.ini";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
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
        "DisabledAddons=renodx-dlss5.addon64\n";
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
        "[aDdOn]\n"
        "DisabledAddons=renodx-dlss5.addon64\n";
    write_binary_file(path, crossSectionDuplicateInput);
    const ConfigUpdate crossSectionResult = ConfigureNeuralAddon(path, true);
    CHECK(!crossSectionResult.ok);
    CHECK(!crossSectionResult.changed);
    CHECK(!crossSectionResult.addonEnabled);
    CHECK(!crossSectionResult.error.empty());
    CHECK_EQ(std::string(crossSectionDuplicateInput), read_binary_file(path));

    std::filesystem::remove(path, removeError);
}

void configure_neural_addon_rejects_non_regular_path_before_replacement_test()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "PolicyTests-ReShade-directory";
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
    std::filesystem::create_directory(path, removeError);

    const ConfigUpdate result = ConfigureNeuralAddon(path, true);
    CHECK(!result.ok);
    CHECK(!result.changed);
    CHECK(!result.addonEnabled);
    CHECK(!result.error.empty());
    CHECK(std::filesystem::is_directory(path));

    std::filesystem::remove_all(path, removeError);
}

} // namespace

int main()
{
    harness_sanity_test();
    gpu_classification_table_test();
    neural_addon_policy_test();
    bootstrap_action_matrix_test();
    windows_command_line_quoting_round_trip_test();
    disabled_addons_creates_missing_addon_section_test();
    disabled_addons_updates_empty_and_populated_lists_test();
    disabled_addons_preserves_mixed_line_endings_and_unrelated_sections_test();
    disabled_addons_removes_only_exact_target_entries_test();
    disabled_addons_collapses_only_exact_target_duplicates_test();
    disabled_addons_matches_trimmed_tokens_without_changing_retained_whitespace_test();
    configure_neural_addon_is_idempotent_test();
    configure_neural_addon_fails_closed_for_malformed_ini_test();
    configure_neural_addon_rejects_non_regular_path_before_replacement_test();

    if (test_support::failure_count != 0) {
        std::cerr << test_support::failure_count << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "PolicyTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
