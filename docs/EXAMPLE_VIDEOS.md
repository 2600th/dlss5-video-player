# Game trailer examples

_Verified against 0.26.1 (d9c7b51) on 2026-09-25._

**File > Game trailers** and the start screen's trailer row offer seven official
trailers, picked for what DLSS 5 changes most: faces, skin, hair, cloth and
cinematic lighting, in real-engine footage rather than pre-rendered CG. Each
one is under three minutes, carries little on-screen text, and is open to an
anonymous session at up to 2160p. The start screen shows as many as fit its
width, in this order, so the strongest go first, each with its YouTube
thumbnail (fetched at run time, not shipped; see
[Usage](USAGE.md#open-render-and-compare) for what that request is and how to
turn it off). A tile's title is the game's name, which fits its width; the
table below names each trailer in full.

Every figure below was read on 24 September 2026 with the bundled yt-dlp
2026.08.19, run with the player's own flags (`--no-config --no-cache-dir
--no-plugin-dirs --no-playlist`, the Auto format selector and
`-S height,vbr,abr`) and `-J`. "Auto" is the rung the player's default
YouTube quality picks: the tallest up to 1440p, then the highest bitrate.

| # | Trailer | Channel | Runtime | Max anonymous | Auto picks | Views | Uploaded |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | [Grand Theft Auto VI Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c) | Rockstar Games | 2:47 | 2160p30, 18.97 Mbps | 1440p30, 9.28 Mbps | 183.2 M | 2025-05-06 |
| 2 | [Resident Evil Requiem - 2nd Trailer](https://www.youtube.com/watch?v=0wFNN1f6hF8) | Resident Evil (Capcom) | 2:34 | 2160p60, 27.61 Mbps | 1440p60, 11.96 Mbps | 2.18 M | 2025-09-12 |
| 3 | [007 First Light - Story Trailer](https://www.youtube.com/watch?v=trvIyyFt_MM) | PlayStation | 1:37 | 2160p60, 27.81 Mbps | 1440p60, 13.75 Mbps | 0.87 M | 2026-02-12 |
| 4 | [Tomb Raider: Legacy of Atlantis - Announcement Trailer](https://www.youtube.com/watch?v=1lJBNZ1Dk6k) | PlayStation | 1:42 | 2160p30, 18.90 Mbps | 1440p30, 9.48 Mbps | 2.83 M | 2025-12-12 |
| 5 | [Assassin's Creed Shadows: Story Trailer](https://www.youtube.com/watch?v=SHaN4MIGqpo) | Assassin's Creed (Ubisoft) | 2:31 | 2160p30, 19.00 Mbps | 1440p30, 9.47 Mbps | 1.05 M | 2025-01-23 |
| 6 | [Mafia: The Old Country - "Family Takes Sacrifice" Launch Trailer](https://www.youtube.com/watch?v=EAEYZDgHNv8) | Mafia Game (2K) | 1:44 | 2160p30, 19.04 Mbps | 1440p30, 9.38 Mbps | 0.88 M | 2025-08-07 |
| 7 | [Kingdom Come: Deliverance II Official Story Trailer](https://www.youtube.com/watch?v=vPS-pgg3scE) | Warhorse Studios | 2:47 | 2160p30, 18.08 Mbps | 1440p30, 7.47 Mbps | 1.23 M | 2024-12-05 |

All seven report `age_limit=0` and `availability=public`. View counts move
daily; they are here to show the scale, not as a ranking.

## Why each one

The on-screen text was judged from a contact sheet of every fourth second at
360p. Every trailer opens with a rating card and closes on logo, date or
store cards; what differs is how much sits in between.

1. **GTA VI Trailer 2.** 183 million views, captured in-game on PS5. Jason
   and Lucia in close-up under Florida sun, neon and dusk: skin, stubble,
   hair and sweat across every kind of light.
   *Text:* a rating card, a "Rockstar Games presents" card, and the logo and
   date at the end. Street signs and shop fronts are part of the world.
2. **Resident Evil Requiem, 2nd Trailer.** Capcom's RE Engine. Requiem is on
   NVIDIA's announced DLSS 5 list, and a Requiem character's face was
   NVIDIA's own DLSS 5 demonstration at GTC 2026, so this is the comparison
   people already argue about. Most of it is dark, lit by torchlight: a hard
   low-light case. 2160p60. *Text:* a rating card, then logo, date and
   platform cards for the last 12 s or so. None over the footage.
3. **007 First Light, Story Trailer.** IO Interactive's Glacier engine.
   Conversations shot as film close-ups: a young Bond and the people who
   recruit him, in suits and tuxedos, in lamp-lit interiors. 2160p60 at the
   highest bitrate of the seven.
   *Text:* a rating card, a small "captured on PS5" line under two shots,
   and a title card plus about 8 s of pre-order art at the end.
4. **Tomb Raider: Legacy of Atlantis, Announcement Trailer.** Unreal Engine 5.
   By one count the most-viewed reveal of The Game Awards 2025. Lara's face
   in daylight jungle, then wet stone, fire and ice: the brightest, most
   varied materials in the list. The PlayStation upload has 2.5x the views
   of the publisher's and a shorter end screen. *Text:* a corner rating icon
   and about 12 s of logo, date and platform cards at the end.
5. **Assassin's Creed Shadows, Story Trailer.** Ubisoft's Anvil engine, and on
   NVIDIA's DLSS 5 list. The most faces per minute of the seven: Naoe,
   Yasuke and a large cast, lit by candle, fire and moonlight, with armour
   and silk. *Text:* a corner rating icon, one tagline card and about 10 s
   of pre-order art at the end.
6. **Mafia: The Old Country, "Family Takes Sacrifice".** Unreal Engine 5.
   1900s Sicily: weathered faces, period suits, sun-baked stone. Kept from
   the previous list. *Text:* the heaviest here: a rating card, then roughly
   the last 20 s of key art, "available now" and subscribe cards.
7. **Kingdom Come: Deliverance II, Story Trailer.** CryEngine. Medieval
   Bohemia in overcast daylight and firelight: beards, mail, cloth and
   leather. *Text:* a rating card, a publisher logo, two single-word cards
   ("Fortune", "the Brave") and about 10 s of pre-order art at the end.

The seven come from six publishers (Take-Two twice, through Rockstar and 2K)
and six engines, and cover crime, horror, spy, adventure, stealth and
historical role-play.

## Candidates rejected

Age-restricted videos were dropped whatever their merit. yt-dlp reports
`age_limit=18` and `availability=needs_auth` for them, and an anonymous
session can then be served a single 640x360 format. The previous list kept
three of these, and they produced exactly that; see below.

| Candidate | Why not |
| --- | --- |
| [GTA VI Trailer 1](https://www.youtube.com/watch?v=QdBZY2fkU-0) (296 M views, 1:30, 2160p, age 0) | Several shots are framed as social-media clips, with usernames and captions over the footage. One GTA entry keeps the publisher spread, and Trailer 2 has more close-up faces |
| [Senua's Saga: Hellblade II - Launch Trailer](https://www.youtube.com/watch?v=PRbOmIcVXak) and its [TGA 2023 trailer](https://www.youtube.com/watch?v=3VYGOkMnGCE) | age_limit 18 (previous list) |
| [The Last of Us Part II Remastered - PC Launch Trailer](https://www.youtube.com/watch?v=Tg1oRHd5zlw) | age_limit 18 (previous list) |
| [Cyberpunk 2077: Phantom Liberty - Launch Trailer](https://www.youtube.com/watch?v=kfX9n_G0N2Y) | age_limit 18 (previous list) |
| [Death Stranding 2 - Accolades Trailer](https://www.youtube.com/watch?v=od0ULrKzylQ) | 0:30, and mostly review quotes over the footage (previous list). Most other official DS2 trailers run 4 to 10 minutes |
| Resident Evil Requiem [4th Trailer](https://www.youtube.com/watch?v=fXVy4mALHLY), [Reveal](https://www.youtube.com/watch?v=POz1-EmLsTY), [Launch](https://www.youtube.com/watch?v=9lrThxCoznw) | age_limit 18; 3:39; 0:30 |
| [Oblivion Remastered - Official Trailer](https://www.youtube.com/watch?v=wFJ3PZuAjK4) | age_limit 18 (on NVIDIA's DLSS 5 list, otherwise a strong pick) |
| [Battlefield 6 Reveal](https://www.youtube.com/watch?v=pgNCgJG0vnY) (19.8 M views) | age_limit 18 |
| Marvel's Wolverine [Gameplay](https://www.youtube.com/watch?v=s3pDMUWlA6I) (13.4 M), [Launch](https://www.youtube.com/watch?v=G62QQ42Ewwg), [Story](https://www.youtube.com/watch?v=3Z42tBfBLJY) | age_limit 18 |
| Phantom Blade Zero [gameplay](https://www.youtube.com/watch?v=b68WE-eZ-Cc), Directive 8020 [story](https://www.youtube.com/watch?v=4a_VXgNSfME), Stranger Than Heaven [reveal](https://www.youtube.com/watch?v=5L3XVVzsmHE), Clair Obscur [reveal](https://www.youtube.com/watch?v=-qgOZDRDynw), Onimusha [1st](https://www.youtube.com/watch?v=W9Ct29FeMak) and [launch](https://www.youtube.com/watch?v=pfMJY5qOVPg), Blood of Dawnwalker [launch](https://www.youtube.com/watch?v=FC_bDk-I7F4), Alien: Isolation 2 [PlayStation reveal](https://www.youtube.com/watch?v=ddrUq4s5xQk) | age_limit 18 |
| [Alien: Isolation 2 SGF 2026 premiere](https://www.youtube.com/watch?v=lUfKBrUgiS0) | Age 0, but half is the stage presentation, with lower thirds and subtitles; almost no faces |
| [Marvel 1943: Rise of Hydra - Story Trailer](https://www.youtube.com/watch?v=Lb2wwEx6DVw) (9.2 M views) | 2.39:1 scope at 3840x1634 at most, so Auto gets 2560x1090. Mostly dark masked heroes, and it ends on "Coming 2025" for a game that did not ship then |
| [Control Resonant - Launch Trailer](https://www.youtube.com/watch?v=SqvAvOAd1VA) | About a third is title, date and pre-order cards, and few faces |
| [Ghost of Yotei - Launch Trailer](https://www.youtube.com/watch?v=sLcksHR30UA) | Masks and wide landscape shots; 1:00 |
| [Crimson Desert - Launch Trailer](https://www.youtube.com/watch?v=YHhwdyWkwTQ) | Effects-heavy combat with few faces |
| 007 First Light [Launch](https://www.youtube.com/watch?v=gDvbGANDH4E) and [Gameplay](https://www.youtube.com/watch?v=nTUoIyTMw0Q) | 1080p at most; action-heavy with kinetic text and two pre-order cards |
| [Kingdom Come: Deliverance II - Launch Trailer](https://www.youtube.com/watch?v=7ynJN-HejlY) | 1080p at most |
| [Tomb Raider channel upload](https://www.youtube.com/watch?v=n6JxVxSHjqE) of the chosen trailer | Same footage, fewer views and a longer end screen |
| Mafia: The Old Country [Initiation](https://www.youtube.com/watch?v=lkdV6NxPOLc) and [Teaser](https://www.youtube.com/watch?v=crDUx5suLm4) | Darker, with the same end cards; the teaser is 24 fps at 2.1 Mbps at 1440p |
| [NBA 2K27 - NVIDIA DLSS 5 Launch Trailer](https://www.youtube.com/watch?v=LHUzFnq70b8) | Already DLSS 5 output, with comparison captions: the player would be comparing the model with itself |
| Intergalactic [announcement](https://www.youtube.com/watch?v=VLGy63pt9vA), Resident Evil Veronica [announcement](https://www.youtube.com/watch?v=uxUMXniTfjM), AC Shadows [world premiere](https://www.youtube.com/watch?v=vovkzbtYBC8) | Over three minutes (4:34, 4:00, 3:49) |

## When YouTube serves less

An age-restricted video, pasted by hand, can still arrive at 640x360. yt-dlp
says so before it lists a format (`This video is age-restricted; some formats
may be missing without authentication`), and the status line then names the
height and rate that actually arrived and says why. None of the seven above is
age-restricted, so this should not happen with the built-in list. If one of
them is later restricted, removed or re-uploaded, re-run the check above and
replace it. No API key or background polling is involved.

Trailer footage is illustrative video material. It is not evidence of game
performance or of native DLSS integration, and runtime and uploader checks do
not establish processed image quality.
