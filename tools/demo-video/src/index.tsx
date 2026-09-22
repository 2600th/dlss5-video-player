import {AbsoluteFill, Composition, Interactive, Sequence, interpolate, registerRoot, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {loadFont} from '@remotion/fonts';
import {Claim, EndCard, Motion, Sweep} from './scenes';
import {clamp, ink} from './brand';

// What the video has to do, in order: show the difference before saying
// anything, say what it is, show it survives motion, show it up close, then say
// where to get it. A silent autoplay in a README gets about two seconds, so the
// first frame is already the comparison.
//
// Every picture is the player's own output: the original is the source file,
// the neural side is the render the shipping player wrote to its cache for
// that file (tools/demo-video/README.md). prepare-inputs cuts both at the same
// frame indices with the same crop; nothing here retouches either side.

void Promise.all([
  loadFont({family: 'Archivo', url: staticFile('archivo-latin.woff2'), weight: '400 900'}),
  loadFont({family: 'JetBrains Mono', url: staticFile('jbmono-latin.woff2'), weight: '400 700'}),
]);

const trinity = {kind: 'still', original: 'matrix-2116-original.png', neural: 'matrix-2116-neural.png', x: 350, y: 130} as const;
const lucia = {kind: 'still', original: 'gta6-1940-original.png', neural: 'gta6-1940-neural.png', x: 130, y: 0} as const;

// Seconds. Each clip scene is exactly as long as its clip, so nothing loops,
// holds or changes speed.
const timeline = [
  {id: 'trinity', at: 0, length: 4.5},
  {id: 'claim', at: 4.5, length: 2.4},
  {id: 'lucia-motion', at: 6.9, length: 2.8},
  {id: 'lucia-2x', at: 9.7, length: 3.6},
  {id: 'florida', at: 13.3, length: 3.0},
  {id: 'end', at: 16.3, length: 3.4},
] as const;
const total = timeline[timeline.length - 1].at + timeline[timeline.length - 1].length;

const scene = (id: (typeof timeline)[number]['id']) => {
  switch (id) {
    case 'trinity':
      return <Sweep plate={trinity} zoom={[1.25, 1.45]} origin="1310px 460px" title="The Matrix (1999)" detail="paused frame · 2560×1440 source · same pixels both sides" />;
    case 'claim':
      return <Claim still={trinity.neural} x={trinity.x} y={trinity.y} />;
    case 'lucia-motion':
      return (
        <Motion
          plate={{kind: 'clip', original: 'lucia-original.mp4', neural: 'lucia-neural.mp4'}}
          title="Grand Theft Auto VI"
          detail="playing at 30 fps · Trailer 2 · 1:1 source pixels"
        />
      );
    case 'lucia-2x':
      return <Sweep plate={lucia} zoom={[1.9, 2.1]} origin="1090px 380px" from={0} to={0.5} title="Up close, 2×" detail="skin, hair, catchlights · one paused frame" />;
    case 'florida':
      return (
        <Motion
          plate={{kind: 'clip', original: 'florida-original.mp4', neural: 'florida-neural.mp4'}}
          divider={0.47}
          title="Daylight, mid-sentence"
          detail="Grand Theft Auto VI · Trailer 2 · playback"
        />
      );
    case 'end':
      return <EndCard version="0.25.0" size="312 MB" />;
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
        Unofficial community build, not affiliated with NVIDIA. The Matrix © Warner Bros. · Grand Theft Auto VI © Rockstar Games.
      </Interactive.Div>
      <Interactive.Div
        name="Progress"
        style={{position: 'absolute', left: 0, bottom: 0, height: 4, backgroundColor: ink.flag, width: interpolate(frame, [0, durationInFrames - 1], [0, 1920], clamp)}}
      />
    </AbsoluteFill>
  );
};

registerRoot(() => (
  <Composition id="Demo" component={Demo} width={1920} height={1080} fps={30} durationInFrames={Math.round(total * 30)} />
));
