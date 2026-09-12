import React from 'react';
import {
  AbsoluteFill,
  CanvasImage,
  Composition,
  Easing,
  Interactive,
  Sequence,
  interpolate,
  registerRoot,
  staticFile,
  useCurrentFrame,
  useVideoConfig,
} from 'remotion';
import {Video} from '@remotion/media';

// Chapter boundaries in seconds; the two middle chapters are the lengths of the
// genuine recordings in public/, so nothing is stretched or slowed.
const chapters = [0, 3, 13, 28, 30] as const;
const lime = '#b6f36b';
const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'} as const;
// The recordings are the player window at its captured size; titles and notices
// sit outside it, so no pixel of the application is covered or rescaled.
const playerStyle: React.CSSProperties = {
  position: 'absolute',
  left: 426,
  top: 74,
  width: 1442,
  height: 932,
  border: '1px solid #34414b',
  boxShadow: '0 26px 70px #0008',
};

const headings = [
  'Render once.\nCompare\ninstantly.',
  'One frame.\nTwo views.',
  'Neural On.\nKeep\nwatching.',
  'Download.\nPlay.\nCompare.',
];

const details = [
  'A native Windows player that re-renders video through the DLSS 5 neural runtime.',
  'The Godfather at 01:14, paused. The real toggle switches Neural Rendering from Off to On.',
  'GTA VI gameplay footage from An Extended Look, playing with the neural view left on.',
  'Try the v0.21.0 Windows build. 1440p sources, an RTX card and a driver from 610.47.',
];

const Demo = () => {
  const frame = useCurrentFrame();
  const {fps, durationInFrames} = useVideoConfig();
  const second = frame / fps;
  const chapter = chapters.reduce<number>((current, start, index) => (second >= start ? index : current), 0);

  return (
    <AbsoluteFill style={{backgroundColor: '#0b1117', color: '#f0f4f8', fontFamily: 'Segoe UI, sans-serif'}}>
      <Interactive.Div
        name="Wordmark"
        style={{position: 'absolute', left: 64, top: 77, width: 290, color: lime, fontSize: 21, fontWeight: 600, letterSpacing: 2}}
      >
        DLSS 5<br />VIDEO PLAYER
      </Interactive.Div>
      <Interactive.Div
        name="Heading"
        style={{
          position: 'absolute',
          left: 64,
          top: 246,
          width: 332,
          whiteSpace: 'pre-line',
          fontSize: 52,
          lineHeight: 1.1,
          letterSpacing: -1.8,
          fontWeight: 650,
          opacity: interpolate(frame, [0, 15], [0, 1], {...clamp, easing: Easing.bezier(0.16, 1, 0.3, 1)}),
        }}
      >
        {headings[chapter]}
      </Interactive.Div>
      <Interactive.Div
        name="Detail"
        style={{position: 'absolute', left: 67, top: 513, width: 292, color: '#bac6d1', fontSize: 25, lineHeight: 1.42}}
      >
        {details[chapter]}
      </Interactive.Div>
      <Interactive.Div
        name="Conditions"
        style={{position: 'absolute', left: 67, top: 757, width: 290, fontSize: 20, lineHeight: 1.5, color: '#93a3b1'}}
      >
        <span style={{color: lime}}>RECORDED ON RTX 5090</span>
        <br />2560x1440 sources · Upscaling off
        <br />Neutral playback adjustments
      </Interactive.Div>
      <Interactive.Div
        name="Honesty"
        style={{position: 'absolute', left: 67, top: 889, width: 292, fontSize: 20, lineHeight: 1.4, color: '#b9c6d0'}}
      >
        Live neural rendering, recorded
        <br />as it ran. No simulated UI.
      </Interactive.Div>

      <Sequence durationInFrames={chapters[1] * fps}>
        <CanvasImage src={staticFile('hero.jpg')} style={playerStyle} />
      </Sequence>
      <Sequence from={chapters[1] * fps} durationInFrames={(chapters[2] - chapters[1]) * fps}>
        <Video src={staticFile('face-take.mp4')} muted objectFit="contain" style={playerStyle} />
      </Sequence>
      <Sequence from={chapters[2] * fps} durationInFrames={(chapters[3] - chapters[2]) * fps}>
        <Video src={staticFile('playback-take.mp4')} muted objectFit="contain" style={playerStyle} />
      </Sequence>
      <Sequence from={chapters[3] * fps}>
        <CanvasImage src={staticFile('hero-godfather.jpg')} style={playerStyle} />
      </Sequence>

      <Interactive.Div name="Attribution" style={{position: 'absolute', left: 67, top: 1017, fontSize: 16, color: '#8fa0ae'}}>
        Unofficial RenoDX/ReShade experiment. NVIDIA components belong to NVIDIA. Grand Theft Auto VI footage (C) Rockstar
        Games; The Godfather footage (C) Paramount Pictures.
      </Interactive.Div>
      <Interactive.Div
        name="Progress"
        style={{
          position: 'absolute',
          left: 0,
          bottom: 0,
          height: 4,
          width: interpolate(frame, [0, durationInFrames - 1], [0, 1920], clamp),
          background: lime,
        }}
      />
    </AbsoluteFill>
  );
};

registerRoot(() => (
  <Composition id="Demo" component={Demo} width={1920} height={1080} fps={30} durationInFrames={900} />
));
