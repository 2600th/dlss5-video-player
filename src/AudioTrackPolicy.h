#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// Which audio track to play, and how to name the others.
//
// The path here asked ffmpeg for `-map 0:a:0?` - the first audio stream,
// whatever it happens to be - and offered no way to change it. A disc rip
// that lists the director's commentary first therefore played the commentary,
// and a film with an English dub ahead of the original played the dub. Being
// dropped into the commentary with no way out is among the loudest complaints
// there is about media players.
//
// Containers say which tracks are not the feature. MKV and MP4 both carry the
// dispositions below, and ffprobe reports them; nothing here guesses from
// titles, because titles are free text in whatever language the ripper felt
// like.
namespace audio_track {

struct Track {
    // Index among the audio streams, which is what `-map 0:a:N` takes.
    int audioIndex = 0;
    // ISO 639-2 as the container tagged it, empty when untagged.
    std::string language;
    std::string title;
    std::string codec;
    int channels = 0;
    bool isDefault = false;
    // The four dispositions that mean "this is not the feature audio".
    bool comment = false;
    bool visualImpaired = false;
    bool descriptions = false;
    bool hearingImpaired = false;

    bool Supplementary() const
    {
        return comment || visualImpaired || descriptions || hearingImpaired;
    }
};

inline constexpr size_t kNoTrack = static_cast<size_t>(-1);

// The track to start on.
//
// Ordinary tracks first, and among those the one the container marked as its
// default wherever it sits in the list. Only if every track is supplementary
// does one of those get picked - a film still has to have sound, and refusing
// to play any would be a worse answer than playing the wrong one.
inline size_t SelectDefault(std::span<const Track> tracks)
{
    if (tracks.empty()) return kNoTrack;

    size_t firstOrdinary = kNoTrack, defaultOrdinary = kNoTrack;
    size_t firstAny = 0, defaultAny = kNoTrack;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const Track& track = tracks[index];
        if (track.isDefault && defaultAny == kNoTrack) defaultAny = index;
        if (track.Supplementary()) continue;
        if (firstOrdinary == kNoTrack) firstOrdinary = index;
        if (track.isDefault && defaultOrdinary == kNoTrack) defaultOrdinary = index;
    }
    if (defaultOrdinary != kNoTrack) return defaultOrdinary;
    if (firstOrdinary != kNoTrack) return firstOrdinary;
    return defaultAny != kNoTrack ? defaultAny : firstAny;
}

namespace detail {

// The languages a consumer container actually carries. An unlisted tag is
// shown as it was written rather than dropped: "cym" is more use to someone
// than nothing at all.
inline std::string LanguageName(std::string_view tag)
{
    struct Entry { std::string_view tag, name; };
    static constexpr Entry kNames[] = {
        {"eng", "English"},    {"spa", "Spanish"},    {"fra", "French"},
        {"fre", "French"},     {"deu", "German"},     {"ger", "German"},
        {"ita", "Italian"},    {"por", "Portuguese"}, {"rus", "Russian"},
        {"jpn", "Japanese"},   {"kor", "Korean"},     {"zho", "Chinese"},
        {"chi", "Chinese"},    {"cmn", "Mandarin"},   {"yue", "Cantonese"},
        {"ara", "Arabic"},     {"hin", "Hindi"},      {"ben", "Bengali"},
        {"nld", "Dutch"},      {"dut", "Dutch"},      {"swe", "Swedish"},
        {"nor", "Norwegian"},  {"dan", "Danish"},     {"fin", "Finnish"},
        {"isl", "Icelandic"},  {"pol", "Polish"},     {"ces", "Czech"},
        {"cze", "Czech"},      {"slk", "Slovak"},     {"hun", "Hungarian"},
        {"ron", "Romanian"},   {"rum", "Romanian"},   {"ell", "Greek"},
        {"gre", "Greek"},      {"tur", "Turkish"},    {"heb", "Hebrew"},
        {"tha", "Thai"},       {"vie", "Vietnamese"}, {"ind", "Indonesian"},
        {"msa", "Malay"},      {"may", "Malay"},      {"ukr", "Ukrainian"},
        {"bul", "Bulgarian"},  {"hrv", "Croatian"},   {"srp", "Serbian"},
        {"cat", "Catalan"},    {"tam", "Tamil"},      {"tel", "Telugu"},
        {"fil", "Filipino"},   {"tgl", "Tagalog"},    {"per", "Persian"},
        {"fas", "Persian"},    {"und", ""},
    };
    for (const Entry& entry : kNames)
        if (entry.tag == tag) return std::string(entry.name);
    return std::string(tag);
}

inline std::string CodecName(std::string_view codec)
{
    struct Entry { std::string_view codec, name; };
    static constexpr Entry kNames[] = {
        {"aac", "AAC"},     {"ac3", "AC3"},      {"eac3", "E-AC3"},
        {"dts", "DTS"},     {"truehd", "TrueHD"}, {"flac", "FLAC"},
        {"mp3", "MP3"},     {"mp2", "MP2"},      {"opus", "Opus"},
        {"vorbis", "Vorbis"},
    };
    for (const Entry& entry : kNames)
        if (entry.codec == codec) return std::string(entry.name);
    // PCM comes in a dozen spellings and the differences do not help anyone
    // choosing a track.
    if (codec.rfind("pcm_", 0) == 0) return "PCM";
    std::string upper(codec);
    for (char& character : upper)
        if (character >= 'a' && character <= 'z') character = char(character - 'a' + 'A');
    return upper;
}

inline std::string ChannelName(int channels)
{
    switch (channels) {
        case 0: return {};
        case 1: return "mono";
        case 2: return "stereo";
        case 3: return "2.1";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return std::to_string(channels) + "ch";
    }
}

} // namespace detail

// What the menu shows. Enough to tell two English tracks apart, which is the
// whole job: a list of four entries all reading "English" is no better than
// no list.
inline std::string Describe(const Track& track)
{
    std::string label = std::to_string(track.audioIndex + 1) + ".";

    const std::string language = detail::LanguageName(track.language);
    if (!language.empty()) label += " " + language;
    if (!track.title.empty()) label += (language.empty() ? " " : " - ") + track.title;

    std::string format = detail::CodecName(track.codec);
    const std::string channels = detail::ChannelName(track.channels);
    if (!channels.empty()) format += format.empty() ? channels : " " + channels;
    if (!format.empty())
        label += (language.empty() && track.title.empty()) ? " " + format : " - " + format;

    // One suffix, the most specific that applies, so the entry does not turn
    // into a list of flags.
    if (track.comment) label += " (commentary)";
    else if (track.visualImpaired || track.descriptions) label += " (audio description)";
    else if (track.hearingImpaired) label += " (for the hard of hearing)";
    return label;
}

} // namespace audio_track
