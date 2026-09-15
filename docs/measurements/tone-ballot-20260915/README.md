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

Unscored as of 2026-09-15. The instrument is built, asserted and sealed; the judgement
is owed.
