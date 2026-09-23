#pragma once

#include <windows.h>
#include <mmreg.h>
// Before ks.h: ks.h defines GUID_NULL as a macro, and a cguid.h that arrives
// after it (shlobj.h brings one) then fails to declare the variable.
#include <cguid.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Bitstream passthrough: an AC-3, E-AC-3 or DTS track handed to a receiver
// as it is, instead of decoded here and sent as PCM.
//
// The bitstream is FFmpeg's `spdif` muxer output - IEC 61937 bursts in 16-bit
// stereo frames - played through a WASAPI EXCLUSIVE stream whose format says
// what the bursts carry. Shared mode cannot do it: the engine mixes in float
// and would turn the bursts into noise. Everything here is the part of that
// which can be argued with in a test: which tracks qualify, the format the
// endpoint is asked for, what the viewer is told, and when a display-mode
// change has to reopen the link.
//
// Off by default. A refusal is never silence: the player opens the ordinary
// PCM stream instead and says why in the status line.
namespace audio_passthrough {

enum class Codec : uint8_t { None, Ac3, Eac3, Dts };

// ffprobe's codec_name. DTS-HD and DTS:X report "dts" too; the spdif muxer
// sends their core, which every DTS receiver decodes.
inline Codec CodecFromName(std::string_view name)
{
    if (name == "ac3") return Codec::Ac3;
    if (name == "eac3") return Codec::Eac3;
    if (name == "dts") return Codec::Dts;
    return Codec::None;
}

inline const wchar_t* CodecLabel(Codec codec)
{
    switch (codec) {
        case Codec::Ac3: return L"AC-3";
        case Codec::Eac3: return L"E-AC-3";
        case Codec::Dts: return L"DTS";
        default: return L"";
    }
}

// What goes over the link, and what the receiver decodes it to.
struct Link {
    Codec codec = Codec::None;
    GUID subFormat{};
    // The rate the endpoint is opened at and the spdif muxer writes at, and
    // therefore the rate the clock divides played frames by. Always 16-bit
    // stereo frames: IEC 61937 over S/PDIF or HDMI's two-channel layout.
    uint32_t transportRate = 0;
    uint32_t encodedRate = 0;
    uint16_t encodedChannels = 0;
};

inline constexpr uint16_t kTransportChannels = 2;
inline constexpr uint16_t kTransportBits = 16;
inline constexpr uint32_t kTransportBytesPerFrame = kTransportChannels * kTransportBits / 8;

// Nothing when the track cannot be passed through: another codec, or a rate
// IEC 61937 has no carriage for. E-AC-3 travels at four times its sample rate
// (IEC 61937-3: one 6144-frame burst period per 1536-sample frame, at 192 kHz
// for a 48 kHz track); AC-3 and the DTS core travel at their own rate.
inline std::optional<Link> PlanLink(Codec codec, uint32_t sampleRate, int channels)
{
    const bool standardRate = sampleRate == 32000 || sampleRate == 44100 || sampleRate == 48000;
    if (!standardRate) return std::nullopt;
    Link link;
    link.codec = codec;
    link.encodedRate = sampleRate;
    // The receiver decodes the channel count, not this; it is informational
    // in the format, and a track that did not say is described as stereo.
    link.encodedChannels = uint16_t(channels > 0 && channels <= 8 ? channels : 2);
    switch (codec) {
        case Codec::Ac3:
            link.subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL;
            link.transportRate = sampleRate;
            return link;
        case Codec::Eac3:
            link.subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS;
            link.transportRate = sampleRate * 4;
            return link;
        case Codec::Dts:
            // DTS has no 32 kHz carriage in practice.
            if (sampleRate == 32000) return std::nullopt;
            link.subFormat = KSDATAFORMAT_SUBTYPE_IEC61937_DTS;
            link.transportRate = sampleRate;
            return link;
        default:
            return std::nullopt;
    }
}

// The format an exclusive stream is opened with. Microsoft's "Representing
// Formats for IEC 61937 Transmissions" asks for WAVEFORMATEXTENSIBLE_IEC61937,
// which adds what the bursts decode to; some drivers only recognise the plain
// WAVEFORMATEXTENSIBLE, so the renderer offers this first and the plain form
// second (`plain`). The leading WAVEFORMATEXTENSIBLE is identical in both.
inline WAVEFORMATEXTENSIBLE_IEC61937 WaveFormat(const Link& link, bool plain = false)
{
    WAVEFORMATEXTENSIBLE_IEC61937 format{};
    WAVEFORMATEX& wave = format.FormatExt.Format;
    wave.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.nChannels = kTransportChannels;
    wave.nSamplesPerSec = link.transportRate;
    wave.wBitsPerSample = kTransportBits;
    wave.nBlockAlign = WORD(kTransportBytesPerFrame);
    wave.nAvgBytesPerSec = link.transportRate * kTransportBytesPerFrame;
    wave.cbSize = WORD((plain ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEXTENSIBLE_IEC61937)) -
                       sizeof(WAVEFORMATEX));
    format.FormatExt.Samples.wValidBitsPerSample = kTransportBits;
    format.FormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    format.FormatExt.SubFormat = link.subFormat;
    if (!plain) {
        format.dwEncodedSamplesPerSec = link.encodedRate;
        format.dwEncodedChannelCount = link.encodedChannels;
        // "Can be 0": the content's own byte rate varies with the bitrate and
        // nothing downstream needs it.
        format.dwAverageBytesPerSec = 0;
    }
    return format;
}

// What the viewer is told. Every state but Off is shown, because each is an
// answer to a toggle the viewer turned on.
enum class State : uint8_t {
    Off,            // not asked for: PCM, nothing to say
    NotApplicable,  // asked for, but this track is not AC-3/E-AC-3/DTS at a carried rate
    Active,         // the bitstream is going to the receiver
    Refused,        // the endpoint said no to the format: PCM instead
    Unavailable,    // the format was fine but an exclusive stream would not start: PCM instead
};

struct Status {
    State state = State::Off;
    Codec codec = Codec::None;
    // ffprobe's codec_name for the track, so NotApplicable can say what the
    // track is rather than only what it is not. Empty when nothing described it.
    std::string trackCodec;
    friend bool operator==(const Status&, const Status&) = default;
};

// Localization keys for the status line; each takes the codec, or the
// track's format when it is not one of ours, as its one %s.
inline const wchar_t* StatusKey(State state)
{
    switch (state) {
        case State::NotApplicable: return L"audio.passthrough.not_applicable";
        case State::Active: return L"audio.passthrough.active";
        case State::Refused: return L"audio.passthrough.refused";
        case State::Unavailable: return L"audio.passthrough.unavailable";
        default: return nullptr;
    }
}

// Kodi #18453: a display-mode change - a refresh switch for frame generation
// is one this player makes itself - retrains the HDMI link, and the receiver
// drops the audio sink with it. A shared stream is the engine's problem and
// survives; an exclusive IEC stream keeps writing into a link the receiver is
// no longer locked to, and the film plays on in silence with every call
// succeeding. So an active passthrough stream is reopened once the display has
// settled: straight away would land in the middle of the retrain, find the
// endpoint gone or refusing, and fall back to PCM for the rest of the film.
inline constexpr double kDisplaySettleSeconds = 1.5;

struct DisplayChangeReopen {
    double dueAt = 0.0;
    bool pending = false;
};

// Called on every display change. Only an active passthrough stream needs
// reopening; a burst of changes (a mode set sends several) moves the deadline
// rather than queuing several reopens.
inline void NoteDisplayChange(DisplayChangeReopen& reopen, double nowSeconds, State state)
{
    if (state != State::Active) return;
    reopen.pending = true;
    reopen.dueAt = nowSeconds + kDisplaySettleSeconds;
}

// True once, when the settle time has passed.
inline bool ReopenDue(DisplayChangeReopen& reopen, double nowSeconds)
{
    if (!reopen.pending || nowSeconds < reopen.dueAt) return false;
    reopen.pending = false;
    return true;
}

} // namespace audio_passthrough
