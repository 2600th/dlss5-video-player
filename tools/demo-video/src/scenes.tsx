import React from 'react';
import {AbsoluteFill, Img, Interactive, interpolate, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {Split, Plate} from './Split';
import {Caption, clamp, ease, ink} from './brand';

// A paused frame with the divider already across it, sweeping on. The first
// thing a silent autoplay shows is the comparison itself - no logo, no title
// card in front of it.
export const Sweep: React.FC<{
  plate: Plate;
  title: string;
  detail: string;
  // Push-in over the scene, around `origin` in the layer's own pixels.
  zoom: [number, number];
  origin: string;
  from?: number;
  to?: number;
}> = ({plate, title, detail, zoom, origin, from = 1, to = 0.5}) => {
  const frame = useCurrentFrame();
  const {durationInFrames} = useVideoConfig();
  const divider = interpolate(frame, [0, 36, durationInFrames], [from, to, to - 0.05], {...clamp, easing: ease});
  const scale = interpolate(frame, [0, durationInFrames], zoom, clamp);
  return (
    <AbsoluteFill>
      <Split plate={plate} divider={divider} scale={scale} origin={origin} />
      <Caption title={title} detail={detail} delay={14} />
    </AbsoluteFill>
  );
};

// Continuous playback with the divider held still: proof that it survives
// motion, not just a chosen pause.
export const Motion: React.FC<{plate: Plate; title: string; detail: string; divider?: number}> = ({
  plate,
  title,
  detail,
  divider = 0.5,
}) => (
  <AbsoluteFill>
    <Split plate={plate} divider={divider} />
    <Caption title={title} detail={detail} delay={4} />
  </AbsoluteFill>
);

// The player's own window, as captured: its title bar, the picture and the
// compare bar. The status row below is cropped off, so no measured rate taken
// on a shared GPU is shown. Only crop, scale and push-in happen here.
const WINDOW = {width: 1902, height: 858};
const SCALE = 0.96;
export const PlayerView: React.FC<{src: string; title: string; detail: string; zoom: [number, number]; origin: string; punch?: boolean}> = ({
  src,
  title,
  detail,
  zoom,
  origin,
  punch = false,
}) => {
  const frame = useCurrentFrame();
  const {durationInFrames} = useVideoConfig();
  // A punch-in holds the whole window for a beat, then moves in fast and settles.
  const scale = punch
    ? interpolate(frame, [0, 22, 46, durationInFrames], [zoom[0], zoom[0], zoom[1], zoom[1] * 1.03], {...clamp, easing: ease})
    : interpolate(frame, [0, durationInFrames], zoom, clamp);
  const left = (1920 - WINDOW.width * SCALE) / 2;
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground, overflow: 'hidden'}}>
      <div
        style={{position: 'absolute', left, top: 28, width: WINDOW.width, height: WINDOW.height, overflow: 'hidden',
          scale: String(SCALE), transformOrigin: '0 0', outline: `1px solid ${ink.line}`}}
      >
        <Img src={staticFile(src)} style={{position: 'absolute', left: 0, top: 0, scale: String(scale), transformOrigin: origin}} />
      </div>
      <Caption title={title} detail={detail} delay={6} />
    </AbsoluteFill>
  );
};

// The render band filling during a live session: window captures taken every
// 200 ms, played at twice that rate, and labelled so. Two crops
// of each capture are shown - the picture, and the timeline - with the status
// row between them left out.
export const Band: React.FC<{frames: number; title: string; detail: string}> = ({frames, title, detail}) => {
  const frame = useCurrentFrame();
  const {durationInFrames} = useVideoConfig();
  const index = Math.min(frames - 1, Math.floor((frame / durationInFrames) * frames));
  const src = staticFile(`band-${String(index).padStart(3, '0')}.png`);
  const picture = {top: 75, height: 712};
  const bar = {top: 972, height: 40};
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      <div style={{position: 'absolute', left: 9, top: 30, width: 1902, height: picture.height, overflow: 'hidden'}}>
        <Img src={src} style={{position: 'absolute', left: 0, top: -picture.top}} />
      </div>
      <div style={{position: 'absolute', left: 9, top: 30 + picture.height + 40, width: 1902, height: bar.height, overflow: 'hidden', outline: `1px solid ${ink.line}`}}>
        <Img src={src} style={{position: 'absolute', left: 0, top: -bar.top}} />
      </div>
      <Interactive.Div
        name="BandKicker"
        style={{position: 'absolute', right: 40, top: 30 + picture.height + 92, fontFamily: 'JetBrains Mono', fontSize: 22, letterSpacing: 2, color: ink.flag}}
      >
        2× SPEED
      </Interactive.Div>
      <Caption title={title} detail={detail} delay={4} />
    </AbsoluteFill>
  );
};

export const EndCard: React.FC = () => {
  const frame = useCurrentFrame();
  const rise = (at: number) => ({
    opacity: interpolate(frame, [at, at + 10], [0, 1], {...clamp, easing: ease}),
    translate: interpolate(frame, [at, at + 14], ['0px 18px', '0px 0px'], {...clamp, easing: ease}),
  });
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      <Interactive.Div name="EndKicker" style={{position: 'absolute', left: 120, top: 290, fontFamily: 'JetBrains Mono', fontSize: 26, letterSpacing: 4, color: ink.flag, ...rise(0)}}>
        FREE · OPEN SOURCE · WINDOWS · RTX
      </Interactive.Div>
      <Interactive.Div
        name="EndHeadline"
        style={{position: 'absolute', left: 116, top: 340, fontFamily: 'Archivo', fontWeight: 800, fontSize: 150, lineHeight: 0.95, letterSpacing: -5, color: ink.text, ...rise(3)}}
      >
        DLSS 5 Video Player
      </Interactive.Div>
      <Interactive.Div name="EndClaim" style={{position: 'absolute', left: 120, top: 505, fontFamily: 'Archivo', fontSize: 40, color: ink.dim, ...rise(6)}}>
        Any video through NVIDIA&rsquo;s neural renderer, checked frame by frame.
      </Interactive.Div>
      <Interactive.Div name="EndUrl" style={{position: 'absolute', left: 120, top: 590, fontFamily: 'JetBrains Mono', fontSize: 44, color: ink.text, ...rise(9)}}>
        github.com/2600th/<span style={{color: ink.flag}}>dlss5-video-player</span>
      </Interactive.Div>
      <Interactive.Div name="EndMeta" style={{position: 'absolute', left: 120, top: 690, fontFamily: 'Archivo', fontSize: 30, lineHeight: 1.5, color: ink.dim, ...rise(12)}}>
        Needs an RTX card and driver 610.47+ · community project, not an NVIDIA product.
        <br />
        <span style={{fontSize: 24, color: ink.meta}}>
          Rendered at default settings on an RTX 4080 SUPER · development build after v0.25.0
        </span>
      </Interactive.Div>
    </AbsoluteFill>
  );
};
