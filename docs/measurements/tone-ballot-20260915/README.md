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

| | value |
|---|---|
| `NeuralWorker.exe` sha256 | `729836f0ab7c866e...` |
| built | 2026-09-15T03:53 |
| card / driver | RTX 4080 SUPER, 610.47 (32.0.16.1047) |

A rebuild of the worker does not invalidate the eight pairs on disk - they are
finished files - but re-running the two `run.py` lines above against a different
worker is a different measurement, and the digests in the table above would move.

## How to score it

The artefacts are under `build-upscaling/benchmark-work/blind/` (untracked - renders,
not documentation):

- `pairs/<id>-A.png`, `pairs/<id>-B.png` - matched stills, same frame index
- `pairs/<id>-A.mp4`, `pairs/<id>-B.mp4` - the excerpt from that frame, clipped to
  the shot so it never crosses an edit. **These carry the question the stills cannot**:
  local tone is a temporal-stability risk as much as a look choice.
- `pairs/<id>.txt` - what to judge, with no identities in it
- `view/<id>-full.png`, `view/<id>-crop.png` - viewing aids added by this session: A
  left, B right, and a 2x blow-up of the 480x270 tile where the two arms differ most.
  The tile is chosen by difference magnitude alone, which says where to look and
  nothing about which side is better.
- `ballot.csv` - one row per pair: `preferred` = `A`, `B` or `tie`, `confidence` 1-5
- `key.json` - sealed. Do not open it before the ballot is filled in.

Then:

```
python tools/benchmark/blind.py --score build-upscaling/benchmark-work/blind/ballot.csv
```

## What each outcome means

- **Shipped side wins, or a tie:** Q7 closes as it stands. The metric verdict and the
  human verdict agree and `NRLocalTone=1.0` is the right default on real footage.
- **Candidate side wins with confidence:** a *look* result overturns a sub-dE metric
  wash, which is the only way this default changes. It would be the first verdict in
  the project decided by eye, so it needs the ballot filled in blind and the key
  opened afterwards - not the other way round.

Unscored as of 2026-09-15. The instrument is built, asserted and sealed; the judgement
is owed.
