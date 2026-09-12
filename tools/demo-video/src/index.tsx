import React from 'react';
import {
  AbsoluteFill,
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

// What the video has to do, in order: say what this is, prove it on a face,
// prove it survives motion, then say where to get it. Every shot is a screen
// recording of the shipping player; `trim` is where a scene enters its
// recording and the only edit made to it.
//
// The proof shots use the player's own inspection controls, on their measured
// instants: 'Z' magnifies 2x around the pointer at `zoom` seconds, and
// Video > Compare > Wipe splits the magnified frame with a white divider at
// `wipe` seconds - original to the left of it, render to the right.
type Scene = {
  start: number;
  length: number;
  asset?: string;
  trim?: number;
  zoom?: number;
  wipe?: number;
  heading: string;
  detail: string;
  badge: string;
  card?: 'cta';
};

const scenes: Scene[] = [
  {
    start: 0,
    length: 3.4,
    asset: 'playback-godfather.mp4',
    trim: 3.4,
    heading: 'Re-render\nyour videos\nwith DLSS 5.',
    detail: 'An open-source player for sharper video on Windows.',
    badge: 'Live, on an RTX 5090',
  },
  {
    // Enter each proof just before its zoom: the setup is not the evidence, and
    // the labelled split has to be on screen long enough to inspect.
    start: 3.4,
    length: 5.6,
    asset: 'compare-godfather.mp4',
    trim: 1.2,
    zoom: 1.71,
    wipe: 3.48,
    heading: 'The Godfather,\n1972',
    detail: 'One paused frame, magnified 2x in the player.',
    badge: '2560x1440 source',
  },
  {
    start: 9,
    length: 5.6,
    asset: 'compare-gta6.mp4',
    trim: 1.1,
    zoom: 1.57,
    wipe: 3.33,
    heading: 'Grand Theft\nAuto VI',
    detail: 'Same frame, same single keypress.',
    badge: '2560x1440 source',
  },
  {
    start: 14.6,
    length: 4.8,
    asset: 'playback.mp4',
    trim: 0.1,
    heading: 'Neural rendering\nduring playback.',
    detail: 'Faces, cuts and motion, while the render runs ahead.',
    badge: '1440p on an RTX 5090',
  },
  {
    start: 19.4,
    length: 3.2,
    card: 'cta',
    heading: 'Download\nfor Windows',
    detail: 'github.com/2600th/dlss5-video-player',
    badge: 'Free, unofficial, community build',
  },
];

const total = scenes[scenes.length - 1].start + scenes[scenes.length - 1].length;
const lime = '#b6f36b';
const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'} as const;
const ease = Easing.bezier(0.16, 1, 0.3, 1);
// The recordings are the player window at its captured size, so no pixel of the
// application is covered, cropped or rescaled.
const playerStyle: React.CSSProperties = {
  position: 'absolute',
  left: 462,
  top: 74,
  width: 1442,
  height: 932,
  border: '1px solid #34414b',
  boxShadow: '0 26px 70px #0008',
};

// Half-labels for the wipe. The divider is drawn by the player at the middle of
// its magnified frame; these say which side is which, because the picture alone
// does not.
const Chip: React.FC<{left: number; text: string; light?: boolean}> = ({left, text, light}) => (
  <Interactive.Div
    name={`Chip-${text}`}
    style={{
      position: 'absolute',
      left,
      top: 152,
      padding: '12px 22px',
      borderRadius: 4,
      fontSize: 36,
      fontWeight: 650,
      letterSpacing: 0.5,
      backgroundColor: light ? '#b6f36bee' : '#0b1117dd',
      color: light ? '#0b1117' : '#f0f4f8',
    }}
  >
    {text}
  </Interactive.Div>
);

const Demo = () => {
  const frame = useCurrentFrame();
  const {fps, durationInFrames} = useVideoConfig();
  const second = frame / fps;
  const index = scenes.reduce<number>((current, scene, at) => (second >= scene.start ? at : current), 0);
  const scene = scenes[index];
  const sceneSecond = second - scene.start;
  const trim = scene.trim ?? 0;
  // Where the recording itself is, so a measured instant can be compared to it.
  const sourceSecond = sceneSecond + trim;
  const zoomed = scene.zoom !== undefined && sourceSecond >= scene.zoom;
  const wiped = scene.wipe !== undefined && sourceSecond >= scene.wipe;

  return (
    <AbsoluteFill style={{backgroundColor: '#0b1117', color: '#f0f4f8', fontFamily: 'Segoe UI, sans-serif'}}>
      <Interactive.Div
        name="Wordmark"
        style={{position: 'absolute', left: 56, top: 77, width: 340, color: lime, fontSize: 22, fontWeight: 600, letterSpacing: 2}}
      >
        DLSS 5<br />VIDEO PLAYER
      </Interactive.Div>
      <Interactive.Div
        name="Heading"
        style={{
          position: 'absolute',
          left: 56,
          top: 230,
          width: 390,
          whiteSpace: 'pre-line',
          fontSize: 62,
          lineHeight: 1.06,
          letterSpacing: -2.2,
          fontWeight: 700,
          opacity: interpolate(sceneSecond * fps, [0, 9], [0, 1], {...clamp, easing: ease}),
          translate: interpolate(sceneSecond * fps, [0, 13], ['0px 16px', '0px 0px'], {...clamp, easing: ease}),
        }}
      >
        {scene.heading}
      </Interactive.Div>
      <Interactive.Div
        name="Detail"
        style={{
          position: 'absolute',
          left: 58,
          top: 560,
          width: 380,
          color: '#c6d2dd',
          fontSize: 34,
          lineHeight: 1.34,
          opacity: interpolate(sceneSecond * fps, [4, 15], [0, 1], {...clamp, easing: ease}),
        }}
      >
        {scene.detail}
      </Interactive.Div>
      <Interactive.Div
        name="Badge"
        style={{position: 'absolute', left: 58, top: 742, width: 380, fontSize: 25, lineHeight: 1.45, color: lime}}
      >
        {scene.badge}
      </Interactive.Div>

      {scenes.map((item) =>
        item.asset ? (
          <Sequence key={item.asset} from={item.start * fps} durationInFrames={item.length * fps}>
            <Video
              src={staticFile(item.asset)}
              muted
              objectFit="contain"
              trimBefore={Math.round((item.trim ?? 0) * fps)}
              style={playerStyle}
            />
          </Sequence>
        ) : null,
      )}

      {/* One chip while the magnified render is alone on screen, two once the
          player's divider splits it. Both sit clear of the divider, which the
          player draws near the middle of the magnified frame. */}
      {zoomed && !wiped && <Chip left={492} text="NEURAL RENDERED - 2x" light />}
      {wiped && (
        <>
          <Chip left={492} text="ORIGINAL" />
          <Chip left={1130} text="NEURAL RENDERED" light />
        </>
      )}

      {scene.card === 'cta' && (
        <Interactive.Div
          name="CtaPanel"
          style={{
            ...playerStyle,
            display: 'flex',
            flexDirection: 'column',
            justifyContent: 'center',
            paddingLeft: 96,
            backgroundColor: '#111a22',
            border: `1px solid ${lime}55`,
          }}
        >
          <div style={{fontSize: 84, fontWeight: 700, letterSpacing: -2.6, lineHeight: 1.05}}>
            Download for Windows
            <br />
            <span style={{color: lime}}>&rarr; GitHub Releases</span>
          </div>
          <div style={{fontSize: 40, color: '#e2e9f0', marginTop: 34, lineHeight: 1.4}}>
            Needs an RTX card and NVIDIA driver 610.47+
          </div>
          <div style={{fontSize: 27, color: '#9fb0bd', marginTop: 26, lineHeight: 1.5}}>
            v0.21.2 - dlss5-video-player-v0.21.2-win64.zip, 308 MB with the pinned runtime.
            <br />Screen recordings of the shipping build: no simulated UI, no speed changes, no sound.
          </div>
        </Interactive.Div>
      )}

      <Interactive.Div name="Attribution" style={{position: 'absolute', left: 58, top: 1019, fontSize: 15, color: '#8fa0ae'}}>
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
  <Composition id="Demo" component={Demo} width={1920} height={1080} fps={30} durationInFrames={Math.round(total * 30)} />
));
