#include "RecentMedia.h"
#include "TestSupport.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"DLSSRecentMediaTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    TempDirectory() { CHECK(std::filesystem::create_directory(path)); }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

RecentMediaEntry Video(char idCharacter)
{
    RecentMediaEntry entry;
    entry.youtube = true;
    entry.id = std::string(11, idCharacter);
    entry.title = L"Example";
    entry.source = L"https://youtu.be/" + std::wstring(11, static_cast<wchar_t>(idCharacter));
    entry.sourceKey = std::string(64, 'a');
    entry.renderKey = std::string(64, 'b');
    entry.sourceQuality = 2;
    return entry;
}

std::string Read(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path& file, std::string_view bytes)
{
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

void persistence_preserves_unicode_and_delimiters()
{
    TempDirectory temp;
    const auto file = temp.path / L"recent.dat";
    RecentMediaHistory history(file);
    CHECK(history.Load());
    auto entry = Video('A');
    entry.title = L"Trailer \"alpha\" | 日本語\nline two \\ backslash";
    CHECK(history.Remember(entry).empty());
    CHECK_EQ(size_t{1}, history.Entries().size());
    CHECK(history.Save());
    RecentMediaHistory loaded(file);
    CHECK(loaded.Load());
    CHECK_EQ(history.Entries(), loaded.Entries());
    CHECK_EQ(size_t{1}, loaded.Entries().size());
    if (!loaded.Entries().empty()) {
        CHECK_EQ(entry.title, loaded.Entries()[0].title);
        CHECK_EQ(L"https://www.youtube.com/watch?v=AAAAAAAAAAA", loaded.Entries()[0].source);
    }
}

void recency_deduplicates_and_reports_sixth_entry()
{
    TempDirectory temp;
    RecentMediaHistory history(temp.path / L"recent.dat");
    for (char id = 'A'; id <= 'E'; ++id) CHECK(history.Remember(Video(id)).empty());
    CHECK_EQ(size_t{5}, history.Entries().size());
    CHECK(history.Remember(Video('B')).empty());
    CHECK_EQ(size_t{5}, history.Entries().size());
    if (!history.Entries().empty()) CHECK_EQ(std::string(11, 'B'), history.Entries()[0].id);
    auto removed = history.Remember(Video('F'));
    CHECK_EQ(size_t{1}, removed.size());
    if (!removed.empty()) CHECK_EQ(std::string(11, 'A'), removed[0].id);
    CHECK_EQ(size_t{5}, history.Entries().size());
    CHECK(history.Save());
    RecentMediaHistory loaded(temp.path / L"recent.dat");
    CHECK(loaded.Load());
    CHECK_EQ(history.Entries(), loaded.Entries());
}

void local_paths_deduplicate_without_touching_user_files()
{
    TempDirectory temp;
    const auto userFile = temp.path / L"日本語.mkv";
    Write(userFile, "user-owned-media");
    RecentMediaHistory history(temp.path / L"recent.dat");
    RecentMediaEntry first;
    first.source = (temp.path / L"unused" / L".." / L"日本語.mkv").wstring();
    first.title = L"Local clip";
    history.Remember(first);
    RecentMediaEntry again;
    again.source = userFile.wstring();
    CharUpperBuffW(again.source.data(), static_cast<DWORD>(again.source.size()));
    again.title = L"Reopened";
    CHECK(history.Remember(again).empty());
    CHECK_EQ(size_t{1}, history.Entries().size());
    if (!history.Entries().empty()) CHECK_EQ(L"Reopened", history.Entries()[0].title);
    CHECK(history.Save());
    RecentMediaHistory loaded(temp.path / L"recent.dat");
    CHECK(loaded.Load());
    CHECK_EQ(history.Entries(), loaded.Entries());
    for (char id = 'A'; id <= 'E'; ++id) history.Remember(Video(id));
    CHECK_EQ("user-owned-media", Read(userFile));
}

void replaced_keys_are_reported_and_invalid_keys_are_ignored()
{
    TempDirectory temp;
    RecentMediaHistory history(temp.path / L"recent.dat");
    auto original = Video('A');
    history.Remember(original);
    auto updated = original;
    updated.renderKey = std::string(64, 'c');
    const auto displaced = history.Remember(updated);
    CHECK_EQ(size_t{1}, displaced.size());
    if (!displaced.empty()) CHECK_EQ(original.renderKey, displaced[0].renderKey);
    CHECK_EQ(size_t{1}, history.Entries().size());
    const auto before = history.Entries();
    auto invalid = Video('B');
    invalid.sourceKey = "..\\user-file";
    CHECK(history.Remember(invalid).empty());
    CHECK_EQ(before, history.Entries());
}

void malformed_history_does_not_replace_existing_entries()
{
    TempDirectory temp;
    const auto file = temp.path / L"recent.dat";
    RecentMediaHistory history(file);
    history.Remember(Video('A'));
    const auto before = history.Entries();
    CHECK(history.Save());
    const std::string valid = Read(file);
    auto tooMany = valid;
    tooMany[valid.find('\n') + 1] = '6';
    for (const std::string& malformed : {
             std::string{}, std::string("wrong format"), valid.substr(0, valid.size() / 2),
             valid + "unexpected trailing content", std::string(1024 * 1024 + 1, 'x'), tooMany}) {
        Write(file, malformed);
        CHECK(!history.Load());
        CHECK_EQ(before, history.Entries());
    }
}

void invalid_records_are_dropped_and_the_rest_kept()
{
    TempDirectory temp;
    const auto file = temp.path / L"recent.dat";
    RecentMediaHistory history(file);
    auto middle = Video('B');
    middle.title = L"Middle";
    for (const auto& entry : {Video('C'), middle, Video('A')}) CHECK(history.Remember(entry).empty());
    CHECK(history.Save());
    const std::string valid = Read(file);
    const auto middleLine = valid.find("1 \"BBBBBBBBBBB\"");
    const auto middleTitle = valid.find("Middle");
    const auto middleKey = valid.find(std::string(64, 'a'), middleLine);
    CHECK(middleLine != std::string::npos);
    CHECK(middleTitle != std::string::npos);
    CHECK(middleKey != std::string::npos);
    if (middleLine == std::string::npos || middleTitle == std::string::npos || middleKey == std::string::npos) return;
    auto badUtf8 = valid;
    badUtf8[middleTitle] = static_cast<char>(0xFF);
    auto badKey = valid;
    badKey.replace(middleKey, 64, "../unsafe");
    auto badFlag = valid;
    badFlag[middleLine] = '2';
    auto shortId = valid;
    shortId.replace(middleLine + 3, 11, std::string(10, 'B'));
    auto duplicate = valid;
    duplicate.replace(middleLine + 3, 11, std::string(11, 'A'));
    for (const auto& damaged : {badUtf8, badKey, badFlag, shortId, duplicate}) {
        Write(file, damaged);
        RecentMediaHistory loaded(file);
        CHECK(loaded.Load());
        CHECK_EQ(size_t{2}, loaded.Entries().size());
        if (loaded.Entries().size() == 2) {
            CHECK_EQ(std::string(11, 'A'), loaded.Entries()[0].id);
            CHECK_EQ(std::string(11, 'C'), loaded.Entries()[1].id);
        }
    }
}

void network_paths_are_remembered_for_the_session_but_never_loaded()
{
    TempDirectory temp;
    const auto file = temp.path / L"recent.dat";
    const auto local = temp.path / L"clip.mkv";
    Write(local, "user-owned-media");
    RecentMediaHistory history(file);
    for (const wchar_t* planted : {L"\\\\server\\share\\video.mkv", L"\\\\?\\UNC\\server\\share\\video.mkv"}) {
        RecentMediaEntry entry;
        entry.source = planted;
        entry.title = L"Planted";
        CHECK(history.Remember(entry).empty());
    }
    RecentMediaEntry kept;
    kept.source = local.wstring();
    kept.title = L"Local clip";
    CHECK(history.Remember(kept).empty());
    CHECK_EQ(size_t{3}, history.Entries().size());
    CHECK(history.Save());
    RecentMediaHistory loaded(file);
    CHECK(loaded.Load());
    CHECK_EQ(size_t{1}, loaded.Entries().size());
    for (const auto& entry : loaded.Entries()) CHECK_EQ(L"Local clip", entry.title);
}

void failed_atomic_replace_preserves_previous_file()
{
    TempDirectory temp;
    const auto file = temp.path / L"recent.dat";
    RecentMediaHistory history(file);
    history.Remember(Video('A'));
    CHECK(history.Save());
    const auto saved = Read(file);
    history.Remember(Video('B'));
    HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    CHECK(!history.Save());
    CHECK_EQ(saved, Read(file));
    for (const auto& item : std::filesystem::directory_iterator(temp.path))
        CHECK_EQ(file, item.path());
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    CHECK(history.Save());
    RecentMediaHistory loaded(file);
    CHECK(loaded.Load());
    CHECK_EQ(history.Entries(), loaded.Entries());
}

} // namespace

int main()
{
    persistence_preserves_unicode_and_delimiters();
    recency_deduplicates_and_reports_sixth_entry();
    local_paths_deduplicate_without_touching_user_files();
    replaced_keys_are_reported_and_invalid_keys_are_ignored();
    malformed_history_does_not_replace_existing_entries();
    invalid_records_are_dropped_and_the_rest_kept();
    network_paths_are_remembered_for_the_session_but_never_loaded();
    failed_atomic_replace_preserves_previous_file();
    if (test_support::failure_count != 0) return 1;
    std::cout << "Recent media tests passed\n";
    return 0;
}
