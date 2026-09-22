# Game trailer examples

_Verified against 0.25.0 (1988cac) on 2026-09-22._

The **File > Game trailers** menu contains six official trailers featuring human
characters, selected for face, expression, hair, clothing and lighting comparisons.
The selection includes released AAA games and an upcoming title; every clip is
under three minutes. Titles, uploader channels, runtimes and age limits were
re-checked with the bundled yt-dlp 2026.08.19 on 12 September 2026.

| Game | Official upload | Runtime | `age_limit` | Status when checked |
| --- | --- | --- | --- | --- |
| Senua's Saga: Hellblade II | [XBOX: Launch Trailer](https://www.youtube.com/watch?v=PRbOmIcVXak) | 1:34 | 18 | Released |
| The Last of Us Part II Remastered | [PlayStation: PC Launch Trailer](https://www.youtube.com/watch?v=Tg1oRHd5zlw) | 1:25 | 18 | Released |
| Mafia: The Old Country | [Mafia Game: Family Takes Sacrifice](https://www.youtube.com/watch?v=EAEYZDgHNv8) | 1:44 | 0 | Released |
| Cyberpunk 2077: Phantom Liberty | [Cyberpunk 2077: Launch Trailer](https://www.youtube.com/watch?v=kfX9n_G0N2Y) | 1:35 | 18 | Released expansion |
| Grand Theft Auto VI | [Rockstar Games: Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c) | 2:47 | 0 | Upcoming |
| Death Stranding 2: On the Beach | [PlayStation: Accolades Trailer](https://www.youtube.com/watch?v=od0ULrKzylQ) | 0:30 | 0 | Released |

The Death Stranding 2 clip includes review text overlays. Trailer footage is
illustrative video material, not proof of game performance or native DLSS
integration. Runtime and uploader checks do not establish processed image quality.

## The three age-restricted entries

Hellblade II (`PRbOmIcVXak`), The Last of Us Part II Remastered
(`Tg1oRHd5zlw`) and Cyberpunk 2077: Phantom Liberty (`kfX9n_G0N2Y`) report
`age_limit=18`. The player signs in to nothing, so on those three YouTube
decides what an anonymous request is allowed to see, and it does not decide the
same way every time. yt-dlp says so before it lists a single format:

```
[youtube] kfX9n_G0N2Y: This video is age-restricted; some formats may be
missing without authentication.
```

A call that lands well returns the whole ladder, up to 2160p60. A call that
does not returns exactly one usable format, the legacy progressive
`18  mp4  640x360`, and then 360p is the entire menu: the source, the neural
render done on it, and whatever you export afterwards. The advertised rate of
that single format is a few hundred kbps and is not the same on every call: 451
kbps in the session that prompted this note, 567 kbps for `Tg1oRHd5zlw` and 324
kbps for `kfX9n_G0N2Y` when re-checked on 12 September 2026.

Which of the two you get depends on the client yt-dlp falls back to for that
request, not on anything the player asks for. Three consecutive Auto resolves
of `Tg1oRHd5zlw` on 12 September 2026 returned 1440p at 7854 kbps, 1440p at
7854 kbps, then 640x360 with no advertised video bitrate at all. Opening the
URL again is a reasonable response.

The entries stay in the menu because they still play. When the resolved source
comes back small the status line now names the height and rate that actually
arrived, and adds that the video is age-restricted and that YouTube keeps the
higher rungs for a signed-in session, so a 360p session is something you can
see rather than something you discover in the render.

Refresh the list manually if videos disappear or become unavailable. No API key
or background polling is needed.
