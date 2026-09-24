import {AbsoluteFill, Composition, Interactive, Sequence, interpolate, registerRoot, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {loadFont} from '@remotion/fonts';
import {Band, EndCard, Motion, PlayerView, Sweep} from './scenes';
import {clamp, ink} from './brand';
import {Card, Square, squareTimeline} from './social';

// What the video has to do, in order: show the difference before saying
// anything, show it survives motion, show the tools the player gives you to
// check it (Difference, Side by side, the loupe), show that it renders while you
// watch, then say where to get it. A silent autoplay in a README gets about two
// seconds, so the first frame is already the comparison.
//
// Every picture is the player's own output. The split scenes use the source
// file and the render the player wrote to its cache for that file, cut at the
// same frame with the same crop (prepare-inputs.py). The player scenes are
// window captures of the player itself, made with capture-window.ps1's method;
// here they are only cropped, scaled and pushed in, never retouched.

void Promise.all([
  loadFont({family: 'Archivo', url: staticFile('archivo-latin.woff2'), weight: '400 900'}),
  loadFont({family: 'JetBrains Mono', url: staticFile('jbmono-latin.woff2'), weight: '400 700'}),
]);

// 007 First Light, frame 1122: the 1920x1080 window sits over the face.
const bond = {kind: 'still', original: 'bond-1122-original.png', neural: 'bond-1122-neural.png', x: 560, y: 180} as const;

// Seconds. Each clip scene is exactly as long as its clip, so nothing loops,
// holds or changes speed - except the render band, which says it runs at 2x:
// 50 window captures taken 200 ms apart (10 s) shown over 5 s.
const timeline = [
  {id: 'bond', at: 0, length: 4.0},
  {id: 'lucia-motion', at: 4.0, length: 2.8},
  {id: 'difference', at: 6.8, length: 3.0},
  {id: 'side-by-side', at: 9.8, length: 2.8},
  {id: 'loupe', at: 12.6, length: 3.4},
  {id: 'band', at: 16.0, length: 5.0},
  {id: 'end', at: 21.0, length: 3.8},
] as const;
const total = timeline[timeline.length - 1].at + timeline[timeline.length - 1].length;

const scene = (id: (typeof timeline)[number]['id']) => {
  switch (id) {
    case 'bond':
      return (
        <Sweep plate={bond} zoom={[1.0, 1.12]} origin="1520px 560px" from={0.56} to={0.49}
          title="007 First Light" detail="one paused frame · 2560×1440 source · same pixels both sides" />
      );
    case 'lucia-motion':
      return (
        <Motion plate={{kind: 'clip', original: 'lucia-original.mp4', neural: 'lucia-neural.mp4'}}
          title="Grand Theft Auto VI" detail="playing at 30 fps · Trailer 2 · 1:1 source pixels" />
      );
    case 'difference':
      return <PlayerView src="difference.png" zoom={[1.25, 1.5]} origin="1060px 430px" title="Difference" detail="where the model changed the picture · ×4 · in the player" />;
    case 'side-by-side':
      return <PlayerView src="side-by-side.png" zoom={[1.3, 1.38]} origin="951px 425px" title="Side by side" detail="one frame, one timestamp · Video > Compare" />;
    case 'loupe':
      return <PlayerView src="loupe.png" zoom={[1.0, 1.8]} origin="1032px 240px" punch title="Loupe" detail="4× rendered pixels · original left, DLSS 5 right" />;
    case 'band':
      return <Band frames={50} title="Renders while you watch" detail="teal is rendered, blue is played · 16 s clip, shown at 2× speed" />;
    case 'end':
      return <EndCard />;
  }
};

const Demo = () => {
  const frame = useCurrentFrame();
  const {fps, durationInFrames} = useVideoConfig();
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      {timeline.map((item) => (
        <Sequence key={item.id} name={item.id} from={Math.round(item.at * fps)} durationInFrames={Math.round(item.length * fps)}>
          {scene(item.id)}
        </Sequence>
      ))}
      <Interactive.Div
        name="Attribution"
        style={{position: 'absolute', right: 28, bottom: 14, padding: '4px 12px', backgroundColor: '#050506b3', fontFamily: 'Archivo', fontSize: 15, color: ink.dim}}
      >
        Unofficial community build, not affiliated with NVIDIA. 007 First Light © IO Interactive · Grand Theft Auto VI © Rockstar Games.
      </Interactive.Div>
      <Interactive.Div
        name="Progress"
        style={{position: 'absolute', left: 0, bottom: 0, height: 4, backgroundColor: ink.flag, width: interpolate(frame, [0, durationInFrames - 1], [0, 1920], clamp)}}
      />
    </AbsoluteFill>
  );
};

const squareTotal = squareTimeline[squareTimeline.length - 1].at + squareTimeline[squareTimeline.length - 1].length;

registerRoot(() => (
  <>
    <Composition id="Demo" component={Demo} width={1920} height={1080} fps={30} durationInFrames={Math.round(total * 30)} />
    <Composition id="Card" component={Card} width={1200} height={630} fps={30} durationInFrames={1} />
    <Composition id="Square" component={Square} width={1080} height={1080} fps={30} durationInFrames={Math.round(squareTotal * 30)} />
  </>
));
