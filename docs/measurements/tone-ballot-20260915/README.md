# The `NRLocalTone` blind ballot (built 2026-09-15, unscored)

`docs/measurements/art-defaults-20260914/REPORT.md` closes with the one thing that
report could not do: *"A human blind A/B remains the only instrument that could
overturn a look decision, and none was run."* This is that ballot, built and sealed.
It is the last thing standing between Q7's `NRLocalTone` candidacy and a closed item,
and nothing here can score it - a person has to look.

## What is being asked

Shipped `NRLocalTone=1.000000` against the candidate `0.500000`, every other art key
at its shipped value, `NRAutoMask=1` (the shipped mask state, not the harness
`baseline`), on **camera-original material** - footage cut from the publishers' own
releases, not the NR-processed captures every earlier art verdict rests on.

The metrics have already answered and their answer is *no change*: on a real graded
clip the knob's whole range is 0.77 dE, against 4.02-8.11 on fractals, and the NVENC
carrier alone accounts for 1.26 dE of the 2.48 measured. The ballot exists because a
sub-dE difference no metric here distinguishes from the carrier may still be a *look*
decision, and a look decision is not settled by dE.

## Arms, and the proof each one took effect

The project's rule from the P3 near-miss: a policy A/B needs a per-arm assertion that
the arm took effect, not a per-arm label.

| arm | profile | `NRLocalTone` in that run's `pass1-ReShade.ini` |
|---|---|---|
| shipped | `shipped-tone-100` | `NRLocalTone=1.000000` |
| candidate | `shipped-tone-050` | `NRLocalTone=0.500000` |

Both written explicitly from `tone.profile.json` beside this file, and confirmed read
back out of each run directory. The ReShade receipt line cannot serve as the
assertion here: it echoes `global_tone`, which stays `1.000000` under both arms
(`art-defaults-20260914/REPORT.md:95-100`), so the ini plus the output digest is the
evidence.

Decoded-frame sequence digests, shipped against candidate - the arms are not secretly
one:

| clip | tone 1.0 | tone 0.5 |
|---|---|---|
| `orig-faces` | `71ca84c3db1aab69` | `1a2b7c1f22708b58` |
| `orig-film-cuts-a` | `b73f2d214fcd3ca9` | `b41eaea5848ef423` |
| `orig-game-motion` | `d2d0079d0501d3da` | `7a0afe60ff5467b0` |
| `orig-dissolve` | `ead6be3c37ad0fe8` | `d81a6e81d4e4229c` |

How large the difference is, per pair, so nobody hunts for an invisible one - mean
absolute 8-bit RGB difference between the two sides, the share of pixels differing by
more than 2 levels, and PSNR of one side against the other:

| pair | mean abs | max abs | pixels > 2 levels | A-vs-B PSNR |
|---|---|---|---|---|
| `ede7a4` | 4.45 | 54 | 89.0 % | 32.65 dB |
| `57a8cd` | 3.35 | 39 | 76.7 % | 35.55 dB |
| `c6169a` | 3.24 | 40 | 77.3 % | 36.00 dB |
| `02a108` | 2.42 | 34 | 43.8 % | 35.49 dB |
| `c7f2fe` | 1.86 | 19 | 64.9 % | 40.26 dB |
| `be86c3` | 1.70 | 68 | 30.3 % | 38.65 dB |
| `547411` | 1.68 | 52 | 43.4 % | 40.38 dB |
| `641352` | 1.63 | 24 | 37.3 % | 40.28 dB |

Which side is which is not stated anywhere outside the sealed key, and the key is not
read while the ballot is open.

## How it was built

```
cd tools/benchmark
python run.py --corpus <repo>/build-upscaling/benchmark-corpus \
  --clips orig-faces orig-film-cuts-a orig-game-motion orig-dissolve \
  --profile-file ../../docs/measurements/tone-ballot-20260915/tone.profile.json \
  --profiles shipped-tone-100 shipped-tone-050 --repeats 1 --fresh-profiles
python blind.py --single shipped-tone-100 --double shipped-tone-050 \
  --pairs-per-clip 2 --seconds 3.0 --seed 1
```

8 pairs, 5 shots reported as too short rather than silently skipped, 0 clips skipped.
`--seed 1` reproduces the frame choice; the pair ids are fresh random each build, so
a rebuild retires the ballot below.

## Provenance of the renders

Both arms were rendered by one worker binary, recorded here because
`NeuralWorker.exe` joined the hashed identity files only this wave and a rebuild of
`build-upscaling/Release` later the same day would otherwise make this ballot
unreproducible without anyone noticing:

`run.py` copies a runtime per profile, lazily, so the binary that matters is the one
inside each arm's profile clone - not the one in `build-upscaling/Release`, which was
rebuilt twice by concurrent work after these renders finished (it now hashes
`79dc0433...`, which is why it is the wrong file to cite):

| arm | binary | sha256 |
|---|---|---|
| shipped | `benchmark-work/profiles/shipped-tone-100/neural-runtime/NeuralWorker.exe` | `729836f0ab7c866e...` |
| candidate | `benchmark-work/profiles/shipped-tone-050/neural-runtime/NeuralWorker.exe` | `729836f0ab7c866e...` |

Equal, so the two arms did share a worker build. Card and driver: RTX 4080 SUPER,
610.47 (32.0.16.1047).

A later rebuild does not invalidate the eight pairs - they are finished files - but
re-running the two `run.py` lines above would use whatever is in `Release` then, and
the digests in the table above would move.

## How to score it

The artefacts are under `build-upscaling/benchmark-work/blind/` (untracked - renders,
not documentation):

- `pairs/<id>-A.png`, `pairs/<id>-B.png` - matched stills, same frame index
- `pairs/<id>-A.mp4`, `pairs/<id>-B.mp4` - the excerpt from that frame, clipped to
  the shot so it never crosses an edit. **These carry the question the stills cannot**:
  local tone is a temporal-stability risk as much as a look choice. They run
  0.54-1.43 s, not the 3.0 s the `--seconds` cap above asks for: every excerpt is
  bounded by its own shot, and on this corpus the shots are shorter than the cap.
- `pairs/<id>.txt` - what to judge, with no identities in it
- `view/<id>-full.png`, `view/<id>-crop.png` - viewing aids added by this session: A
  left, B right, and a 2x blow-up of the 480x270 tile where the two arms differ most.
  The tile is chosen by difference magnitude alone, which says where to look and
  nothing about which side is better.
- `ballot.csv` - one row per pair: `preferred` = `A`, `B` or `tie`, `confidence` an
  integer 1-5. A blank confidence is accepted and scores preference-only, with the
  weighting withheld rather than defaulted; anything that is not 1-5 fails the run,
  because the scale has five points and `0` or `9` is a data error, not a weak
  preference
- `key.json` - sealed. Do not open it before the ballot is filled in.

Then:

```
python tools/benchmark/blind.py --score build-upscaling/benchmark-work/blind/ballot.csv
```

**Read the roles, not the habit.** `blind.py` was written for one-pass versus
two-pass, so its votes are keyed `single` and `double`. For this ballot
`single` = `shipped-tone-100` (the shipped 1.0) and `double` = `shipped-tone-050`
(the candidate) - the scorer prints that mapping with the result, in the JSON as
`profiles` and in prose on stderr, because "double wins" read as "two-pass wins"
would invert this verdict.

## The decision rule, fixed before anyone scores

Eight pairs and one judge is a small instrument, so the threshold is pre-registered
here rather than chosen once the votes are in. 5 of 8 is chance; nothing below 7 is
worth changing a shipped default on.

| Outcome | Reading |
|---|---|
| candidate preferred on **>= 7 of 8** pairs at confidence >= 3 | a *look* result overturns a sub-dE metric wash. The only way this default changes, and it would be the first verdict in this project decided by eye |
| shipped preferred on **>= 7 of 8** | Q7 closes as it stands, and with a stronger reason than the metrics gave it |
| anything between, ties included | **the knob is indistinguishable at 0.5**, which is a result about the ballot's power, not about the knob. See the note below before reaching for a bigger sample |

**If it lands in the middle, do not re-run this ballot with more pairs - re-run it
against `shipped-tone-0`.** On real footage the knob's *entire* range (1.0 -> 0.0) is
0.77 dE, so half the range is around 0.4 dE on average - below a 1 dE just-noticeable
difference. What makes this ballot worth scoring anyway is that the average is not
what a judge sees: the measured per-pixel differences above reach 19-68 levels
locally, on 30-89 % of the frame. But if the answer is "cannot tell", the honest next
experiment is the maximum-signal pair (1.0 against 0.0), where a null *does* close
the item - `shipped-tone-0` already exists in
`../art-defaults-20260914/shipped-state.profile.json`.

The key stays sealed until the ballot is filled in, either way round.

## Result of ballot 1 (1.0 against 0.5): scored, and the default does not change

Filled in by a human judge on 2026-09-15, all 8 pairs, no ties. **Confidence was left
blank on every row, so this ballot has no weighting** - the scorer used to turn a blank
into 1.0 and print a confidence-weighted total that was arithmetic on a default; it now
names the rows and withholds the figure. The pre-registered rule's confidence clause
therefore cannot be checked against this ballot, and does not need to be: the vote
count alone falls short of the bar.

| | pairs | confidence-weighted |
|---|---|---|
| `shipped-tone-100` (shipped 1.0) | 2 | not recorded |
| `shipped-tone-050` (candidate 0.5) | **6** | not recorded |

**6 of 8 is below the pre-registered bar of 7, so the default stays at 1.0.** This is
exactly the case the threshold was fixed in advance for: 6 of 8 looks like a 75 %
preference and is not one. Under pure chance, 6 or more of 8 falls one way 14.5 % of
the time one-sided, 28.9 % two-sided; the bar of 7 sits at 3.5 %. Nothing here beats
the metric wash that the art-defaults report already recorded.

Per pair, once unsealed:

| pair | clip | frame | judged | that side was |
|---|---|---|---|---|
| `ede7a4` | `orig-dissolve` | 58 | B | candidate 0.5 |
| `c7f2fe` | `orig-dissolve` | 5 | A | candidate 0.5 |
| `02a108` | `orig-faces` | 65 | A | candidate 0.5 |
| `547411` | `orig-faces` | 42 | A | **shipped 1.0** |
| `641352` | `orig-film-cuts-a` | 53 | B | candidate 0.5 |
| `be86c3` | `orig-film-cuts-a` | 87 | B | **shipped 1.0** |
| `57a8cd` | `orig-game-motion` | 40 | A | candidate 0.5 |
| `c6169a` | `orig-game-motion` | 23 | A | candidate 0.5 |

**The pattern is worth more than the tally.** The candidate took both
`orig-dissolve` pairs and both `orig-game-motion` pairs - 4 of 4 on the continuous
motion and the cross-dissolve - and split 1-1 on `orig-faces` and
`orig-film-cuts-a`. So the one hypothesis this ballot generated is that the local
tone term costs something on moving material and is neutral on static or
cut-bearing material. Eight pairs cannot test that; it is a reason to keep asking,
not a finding.

Artefacts: `ballot-1/ballot.csv`, `ballot-1/key.json` (now unsealed),
`ballot-1/result.json` and `result.txt`. The pair media is not committed - it is
renders, and `--seed 1` against the same two runs reproduces the frame choice.

## Ballot 2 (1.0 against 0.0): built, unscored

The pre-registered follow-up, because a middling result at half the range does not
close the item while a null at the *full* range does. Same four clips, same shipped
mask state, `shipped-tone-000` rendered 2026-09-15 and asserted the same way (the
ini reads `NRLocalTone=0.000000` and all four clips' decoded-frame digests differ
from the 1.0 arm).

**Both arms share one worker build, checked rather than assumed.** `run.py` clones a
runtime per profile, so the 1.0 arm had been rendered by `729836f0` (before the
source-tag probe landed) and the 0.0 arm by `0fc948df` (after) - two arms differing by
the knob *and* the binary. The 1.0 arm was therefore re-rendered under `0fc948df` and
its four decoded-frame digests reproduce the earlier ones exactly
(`71ca84c3db1aab69`, `b73f2d214fcd3ca9`, `d2d0079d0501d3da`, `ead6be3c37ad0fe8`), so
the worker change is pixel-neutral on the default path and the pairs already cut are
sound. Both profile clones now hash `0fc948df03d1b967`.

**12 pairs, and the bar is 10.** Three pairs per clip rather than two, because the
question this time is about power: under chance, 10 or more of 12 falls one way
1.9 % of the time, 9 or more 7.3 %. So 10 decides it either way, and anything below
means the knob is indistinguishable across its whole range on real footage - which
closes Q7 for good rather than leaving it open.

The differences are larger than ballot 1's, as the full range should be: mean
absolute 1.82-7.58 eight-bit levels against 1.63-4.45, up to 98.5 % of the frame
moving.

**Please record confidence 1-5 this time.** With the bar at 10 of 12 the
confidence-weighted figure is what separates a reluctant sweep from a confident
one, and ballot 1 arrived without it.

## Result of ballot 2 (1.0 against 0.0): the bar as written was not met, and the
## rule as written was defective

Scored 2026-09-15, same judge, 12 pairs. Confidence blank again, so the weighting is
withheld again.

| | pairs |
|---|---|
| `shipped-tone-100` (shipped 1.0) | 1 |
| `shipped-tone-000` (knob off) | **8** |
| ties | 3 |

**Two readings, and I have to give both, because the pre-registration did not say
which:**

| reading | test | result |
|---|---|---|
| **A** - literal: "10 or more of 12" | 8 of 12 | **bar not met** (chance gives 8+ of 12 19.4 % of the time) |
| **B** - ties carry no preference, so n is the decided pairs | 8 of 9 | **P = 0.0195**, which clears the ~2 % the bar of 10 was chosen to encode |

The rule said "10 of 12" and never defined tie handling. That omission is mine, and
it is exactly the kind of latitude a pre-registration exists to remove: reading A
keeps the default, reading B changes it, and the honest position is that a rule
which cannot answer its own data does not get to be read whichever way suits the
outcome. **So the default stays at 1.0 for now** - not because the evidence is
weak, but because the instrument that was supposed to make the decision unarguable
turned out to be arguable, and picking reading B after seeing that it wins is the
behaviour the whole pre-registration was set up to prevent.

**What the two ballots agree on, which is more interesting than either tally.**
Pooling the decided pairs across both rounds, *less* local tone was preferred on
**14 of 17** (P = 0.0064 one-sided). Pooling was not pre-registered either, so that
number is exploratory - but the per-clip pattern reproduced independently, with
different arms, on the second run:

| clip | ballot 1 (1.0 vs 0.5) | ballot 2 (1.0 vs 0.0) |
|---|---|---|
| `orig-dissolve` | less tone **2/2** | less tone **3/3** |
| `orig-game-motion` | less tone **2/2** | less tone **3/3** |
| `orig-faces` | 1-1 | 1 shipped, 1 less, 1 tie |
| `orig-film-cuts-a` | 1-1 | 1 less, 2 ties |

**Four of four on the moving material in ballot 1 (two pairs per clip there, not
three), six of six in ballot 2, ten of ten combined**, against splits or ties on the
static and cut-bearing clips both times. As a post-hoc subgroup that is P = 0.001 and
must be labelled as one - the subgroup was chosen after seeing round 1.

It is also mechanism-*shaped*: a local tone map that re-solves per frame is what
would cost on continuous motion and a cross-dissolve while being invisible on a
static shot. It lines up with the metrics, which had `shipped-tone-0` closer to the
source on the synthetic motion clip (+4.83 dB PSNR, -4.02 dE).

**One observation cuts against the temporal half of that story, and it comes from
this ballot's own sampling.** Three of ballot 2's pairs sit at frames 2, 4 and 8,
inside the window where the guide generator has just reset its history - the guard
is 0.1 s, about two frames, sized to keep this corpus's short shots usable at all.
A still with no temporal history cannot show temporal instability, yet two of those
three went to the knob-off arm (`e6281e` game-motion frame 4, `59b6f2` dissolve
frame 8; the third, `a4059c` faces frame 2, was a tie). So the preference may be
**spatial** - the look of the tone map itself - rather than the per-frame instability
the pattern suggested. Round 3 has to separate those, and until it does,
"mechanism-shaped" is as far as this evidence reaches.

## Round 3, pre-registered here - and a prerequisite the corpus forces

**Ballot 2 was not a null, so the earlier "a wash closes Q7" clause does not apply
to this data and is retired.** What is on the table now is whether a shipped default
changes, and these are the terms, fixed before any pairs exist:

1. **Ties are excluded from n.** The test is one-sided on decided pairs, and the bar
   is the smallest count whose binomial probability is at or below 2 % - for 9
   decided pairs that is 8, for 12 it is 10.
2. **Confidence 1-5 is required.** Two ballots have now arrived without it, so the
   "at confidence >= 3" half of every rule so far has never been checkable.
3. **What each outcome means, stated now.** A sweep on the motion clips *with* the
   static and cut-bearing clips still splitting or tying is what would justify
   changing the global default - the player cannot pick a tone strength per shot, so
   a motion-only win still has to be taken globally or not at all. A wash on the
   motion clips retires the subgroup as noise and leaves the default where it is.
4. **A second judge if one is available.** One pair of eyes, twice, is one pair of
   eyes.

**The prerequisite: this corpus cannot supply an independent motion ballot.**
`orig-game-motion` is a single 65-frame shot of 2.2 s and `orig-dissolve` is 71
frames; six pairs per clip would sample neighbours of frames the same judge has
already scored. Raising the guard to give each still real temporal history makes it
worse, measured across the camera-original set:

| clip | duration | candidate frames at guard 0.1 s | 0.35 s | 0.5 s | 1.0 s |
|---|---|---|---|---|---|
| `orig-game-motion` | 2.20 s | 49 | 42 | 37 | 22 |
| `orig-game-cuts` | 3.03 s | 40 | 26 | 21 | 6 |
| `orig-faces` | 4.21 s | 36 | 11 | 4 | **0** |
| `orig-film-cuts-a` | 5.46 s | 32 | 16 | 8 | **0** |
| `orig-film-cuts-b` | 2.92 s | 21 | 9 | 5 | **0** |
| `orig-dissolve` | 2.96 s | 17 | 5 | **0** | **0** |
| `orig-film-fade` | 1.29 s | **0** | **0** | **0** | **0** |

### Round 3 as built (2026-09-15): the clips were cut, and two sampling defects fell out

Two new camera-original clips now exist, found by sweeping the registered sources for
spans with no proposed cut, at least 240 frames, and a median |dY| of 1.5 or more:

| clip | source | frames | seconds | median &#124;dY&#124; | character |
|---|---|---|---|---|---|
| `orig-film-motion-a` | godfather | 272 | 11.3 | 1.81 | dim interior, faces and skin, gentle camera |
| `orig-film-motion-b` | lawrence | 258 | 10.8 | **4.72** | exterior tracking shot, foliage streaming, real motion blur |

`orig-film-motion-b` carries the strongest sustained motion of any camera-original
clip here - 4.72 against 3.9 for the 2.2 s `orig-game-motion`. Both are one
continuous shot, verified the way this corpus requires: the five largest internal
pairs of each were inspected by eye and are the same shot in motion, histogram
overlap never below 0.82. A third qualifying span (lawrence 1694-2159, 19.4 s) was
rejected for being 38 % near-static. Both clips are in
`tools/benchmark/camera-original.digests.json`, so their pixels are now load-bearing:
`corpus.py --check` verifies nine clips, not seven.

**The first build of ballot 3 was invalid and the tool now refuses to build it that
way.** Two defects, both of which would have inflated the tally the bar is set
against:

- Runs from ballot 2 were still on disk under the same profile names, so
  `orig-game-motion` - the 2.2 s clip these long ones were cut to replace - joined
  the ballot uninvited. `blind.py` takes `--clips` now.
- Frames were drawn with `rng.choice` per pair over the whole pool, so a single-shot
  clip handed the judge the same comparison twice: the first build drew frames 34,
  34, 50 and 51 out of 66. Sampling is now without replacement with at least one
  excerpt length between frames, and when a clip cannot supply the pairs asked for it
  says so instead of repeating itself.

Ballot 3 is therefore **10 pairs, five per clip**, 1.5 s excerpts with a 1.0 s guard,
seed 3, frames 42-48 apart within each clip so no two excerpts overlap:
`orig-film-motion-a` at 48, 90, 151, 195, 256 and `orig-film-motion-b` at 29, 83,
137, 189, 228. `orig-film-motion-b` could only supply five, and said so.

**The bar, by the rule fixed above: 9 of 10 decided pairs** (P = 0.0107; 8 of 10 is
P = 0.055 and does not qualify). If ties reduce the decided count the bar moves with
it: 8 of 9 (P = 0.0195), and at 8 decided pairs 7 does not qualify (P = 0.035) so a
clean 8 of 8 is needed. A sweep still clears the bar at 7 of 7 (0.0078) and 6 of 6
(0.0156); at five or fewer decided pairs nothing does, which would make the round
inconclusive by construction rather than by result.

Differences span mean absolute 0.55 to 7.20 levels. One pair (`a4c357`, 0.55 mean,
4 % of pixels) is near-identical - a dim interior frame where the knob has little to
work on - and is left in rather than dropped, because dropping the pairs where an
effect is invisible is how a preference gets manufactured.

---

The original prerequisite, for the record: round 3 needed **longer continuous-motion
spans cut from the same publisher sources** - `tools/benchmark/fetch_camera_original.ps1` already fetches them, and
`corpus.py` cuts them; what is missing is a 10-15 s motion span rather than a 2 s
one. With that, a 0.5-1.0 s guard becomes affordable and twelve pairs are twelve
independent samples instead of twelve views of the same two seconds. `blind.py` now
takes `--guard-seconds` and records it in `key.json`, so whichever value round 3
uses is part of its record rather than a constant somebody edited.

Artefacts: `ballot-2/ballot.csv`, `ballot-2/key.json` (unsealed), `ballot-2/result.json`.

Unscored as of 2026-09-15. The instrument is built, asserted and sealed; the judgement
is owed.
