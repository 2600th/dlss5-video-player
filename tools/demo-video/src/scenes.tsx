import React from 'react';
import {AbsoluteFill, Img, Interactive, interpolate, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {Split, Plate} from './Split';
import {Caption, clamp, ease, ink} from './brand';

// A paused frame, opened as the original and wiped across to the render. The
// first thing a silent autoplay shows is the comparison itself - no logo, no
// title card in front of it.
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
  const divider = interpolate(frame, [6, 40, durationInFrames], [from, to, to - 0.06], {...clamp, easing: ease});
  const scale = interpolate(frame, [0, durationInFrames], zoom, clamp);
  return (
    <AbsoluteFill>
      <Split plate={plate} divider={divider} scale={scale} origin={origin} />
      <Caption title={title} detail={detail} delay={20} />
    </AbsoluteFill>
  );
};

// Continuous playback with the divider held still: proof that it survives
// motion, focus pulls and cuts, not just a chosen pause.
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

// The claim, stated once, over the render it is about.
export const Claim: React.FC<{still: string; x: number; y: number}> = ({still, x, y}) => {
  const frame = useCurrentFrame();
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground, overflow: 'hidden'}}>
      <Img
        src={staticFile(still)}
        style={{position: 'absolute', left: -x, top: -y, width: 2560, height: 1440, opacity: 0.34, scale: String(interpolate(frame, [0, 75], [1.06, 1.1], clamp))}}
      />
      <Interactive.Div
        name="ClaimKicker"
        style={{position: 'absolute', left: 120, top: 330, fontFamily: 'JetBrains Mono', fontSize: 26, letterSpacing: 4, color: ink.flag,
          opacity: interpolate(frame, [0, 8], [0, 1], clamp)}}
      >
        DLSS 5 VIDEO PLAYER
      </Interactive.Div>
      <Interactive.Div
        name="ClaimHeadline"
        style={{
          position: 'absolute', left: 116, top: 380, whiteSpace: 'nowrap', fontFamily: 'Archivo', fontWeight: 800, fontSize: 112,
          lineHeight: 0.98, letterSpacing: -4.5, color: ink.text,
          opacity: interpolate(frame, [2, 12], [0, 1], {...clamp, easing: ease}),
          translate: interpolate(frame, [2, 16], ['0px 24px', '0px 0px'], {...clamp, easing: ease}),
        }}
      >
        Put any video through
        <br />
        NVIDIA&rsquo;s neural renderer.
      </Interactive.Div>
      <Interactive.Div
        name="ClaimDetail"
        style={{position: 'absolute', left: 120, top: 660, fontFamily: 'Archivo', fontSize: 40, color: ink.dim,
          opacity: interpolate(frame, [10, 20], [0, 1], {...clamp, easing: ease})}}
      >
        Films, trailers, your own clips. Watch it live, or export it.
      </Interactive.Div>
    </AbsoluteFill>
  );
};

export const EndCard: React.FC<{version: string; size: string}> = ({version, size}) => {
  const frame = useCurrentFrame();
  const rise = (at: number) => ({
    opacity: interpolate(frame, [at, at + 10], [0, 1], {...clamp, easing: ease}),
    translate: interpolate(frame, [at, at + 14], ['0px 18px', '0px 0px'], {...clamp, easing: ease}),
  });
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      <Interactive.Div name="EndKicker" style={{position: 'absolute', left: 120, top: 300, fontFamily: 'JetBrains Mono', fontSize: 26, letterSpacing: 4, color: ink.flag, ...rise(0)}}>
        FREE · OPEN SOURCE · WINDOWS
      </Interactive.Div>
      <Interactive.Div
        name="EndHeadline"
        style={{position: 'absolute', left: 116, top: 350, fontFamily: 'Archivo', fontWeight: 800, fontSize: 150, lineHeight: 0.95, letterSpacing: -5, color: ink.text, ...rise(3)}}
      >
        DLSS 5 Video Player
      </Interactive.Div>
      <Interactive.Div name="EndUrl" style={{position: 'absolute', left: 120, top: 540, fontFamily: 'JetBrains Mono', fontSize: 44, color: ink.text, ...rise(8)}}>
        github.com/2600th/<span style={{color: ink.flag}}>dlss5-video-player</span>
      </Interactive.Div>
      <Interactive.Div name="EndMeta" style={{position: 'absolute', left: 120, top: 640, fontFamily: 'Archivo', fontSize: 34, lineHeight: 1.45, color: ink.dim, ...rise(12)}}>
        v{version} · {size} with the neural runtime · needs an RTX card and driver 610.47+
        <br />
        Community project, not an NVIDIA product.
        <br />
        <span style={{fontSize: 26, color: ink.meta}}>
          Rendered on an RTX 4080 SUPER · Intensity, Local tone and Local structure at 2.0 (defaults 1.0)
        </span>
      </Interactive.Div>
    </AbsoluteFill>
  );
};
