import React from 'react';
import {AbsoluteFill, Img, Interactive, Sequence, interpolate, staticFile, useCurrentFrame, useVideoConfig} from 'remotion';
import {Split, Plate} from './Split';
import {clamp, ease, ink} from './brand';

// The social cuts reuse the demo's inputs - the same verified source/render
// pair and the same player captures - only framed for other shapes. Nothing
// here is new footage.

// 007 First Light frame 1122, scaled to 0.62 so the face fits the 1200x630 card
// right of the text, with the divider through it; both halves scale together.
const cardPlate: Plate = {kind: 'still', original: 'bond-1122-original.png', neural: 'bond-1122-neural.png', x: 340, y: 170};

// A 1200x630 link card: the split face, the name and one line.
export const Card: React.FC = () => (
  <AbsoluteFill style={{backgroundColor: ink.ground}}>
    <Split plate={cardPlate} divider={0.69} scale={0.62} origin="600px 170px" />
    <AbsoluteFill style={{background: 'linear-gradient(90deg, #050506f2 0%, #050506d9 34%, #05050600 58%)'}} />
    <Interactive.Div name="CardKicker" style={{position: 'absolute', left: 56, top: 212, fontFamily: 'JetBrains Mono', fontSize: 17, letterSpacing: 3, color: ink.flag}}>
      FREE · OPEN SOURCE · WINDOWS · RTX
    </Interactive.Div>
    <Interactive.Div name="CardTitle" style={{position: 'absolute', left: 52, top: 242, fontFamily: 'Archivo', fontWeight: 800, fontSize: 64, lineHeight: 1, letterSpacing: -2, color: ink.text}}>
      DLSS 5
      <br />
      Video Player
    </Interactive.Div>
    <Interactive.Div name="CardLine" style={{position: 'absolute', left: 56, top: 390, width: 520, fontFamily: 'Archivo', fontSize: 24, lineHeight: 1.3, color: ink.dim}}>
      Any video through NVIDIA&rsquo;s neural renderer, checked frame by frame.
    </Interactive.Div>
    <Interactive.Div name="CardMeta" style={{position: 'absolute', left: 56, bottom: 30, fontFamily: 'JetBrains Mono', fontSize: 13, color: ink.meta}}>
      007 First Light, one frame · player render, default settings · not an NVIDIA product
    </Interactive.Div>
  </AbsoluteFill>
);

const SquareCaption: React.FC<{title: string; detail: string}> = ({title, detail}) => {
  const frame = useCurrentFrame();
  return (
    <Interactive.Div
      name="SquareCaption"
      style={{position: 'absolute', left: 40, bottom: 56, padding: '14px 20px 12px', backgroundColor: '#050506d9', borderLeft: `3px solid ${ink.flag}`,
        opacity: interpolate(frame, [4, 14], [0, 1], {...clamp, easing: ease})}}
    >
      <div style={{fontFamily: 'Archivo', fontSize: 40, fontWeight: 700, color: ink.text, lineHeight: 1.1}}>{title}</div>
      <div style={{fontFamily: 'JetBrains Mono', fontSize: 20, color: ink.dim, marginTop: 6}}>{detail}</div>
    </Interactive.Div>
  );
};

// A player capture framed square: (cx, cy) in the capture's pixels sits at the
// centre, at `zoom` over the scene. Crop and scale only.
const SquareCapture: React.FC<{src: string; cx: number; cy: number; zoom: [number, number]; label: string; detail: string}> = ({src, cx, cy, zoom, label, detail}) => {
  const frame = useCurrentFrame();
  const {durationInFrames} = useVideoConfig();
  const scale = interpolate(frame, [0, durationInFrames], zoom, {...clamp, easing: ease});
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground, overflow: 'hidden'}}>
      <Img src={staticFile(src)} style={{position: 'absolute', left: 540 - cx, top: 540 - cy, scale: String(scale), transformOrigin: `${cx}px ${cy}px`}} />
      <SquareCaption title={label} detail={detail} />
    </AbsoluteFill>
  );
};

// The PNG the player's Save comparison image wrote for the 2 x 2 view, footer
// and all, scaled to the square's width.
const Saved: React.FC = () => {
  const frame = useCurrentFrame();
  const scale = 1080 / 1266;
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      <Img src={staticFile('saved-2x2.png')} style={{position: 'absolute', left: 0, top: 150, width: 1266, height: 790, scale: String(scale), transformOrigin: '0 0',
        opacity: interpolate(frame, [0, 6], [0, 1], clamp)}} />
      <Interactive.Div name="SavedKicker" style={{position: 'absolute', left: 40, top: 80, fontFamily: 'JetBrains Mono', fontSize: 22, letterSpacing: 2, color: ink.flag}}>
        SAVED BY THE PLAYER, WITH ITS PROVENANCE
      </Interactive.Div>
      <SquareCaption title="Save comparison image" detail="2 × 2 · original, DLSS 5, difference, mix 50%" />
    </AbsoluteFill>
  );
};

const squarePlate: Plate = {kind: 'still', original: 'bond-1122-original.png', neural: 'bond-1122-neural.png', x: 980, y: 180};

const SquareSweep: React.FC = () => {
  const frame = useCurrentFrame();
  const {durationInFrames} = useVideoConfig();
  const divider = interpolate(frame, [0, 40, durationInFrames], [0.62, 0.46, 0.43], {...clamp, easing: ease});
  const scale = interpolate(frame, [0, durationInFrames], [1.0, 1.1], clamp);
  return (
    <AbsoluteFill>
      <Split plate={squarePlate} divider={divider} scale={scale} origin="1520px 560px" />
      <SquareCaption title="007 First Light" detail="one frame · same pixels both sides" />
    </AbsoluteFill>
  );
};

const SquareEnd: React.FC = () => {
  const frame = useCurrentFrame();
  const rise = (at: number) => ({opacity: interpolate(frame, [at, at + 10], [0, 1], {...clamp, easing: ease})});
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      <Interactive.Div name="SqKicker" style={{position: 'absolute', left: 80, top: 330, fontFamily: 'JetBrains Mono', fontSize: 24, letterSpacing: 3, color: ink.flag, ...rise(0)}}>
        FREE · OPEN SOURCE · WINDOWS · RTX
      </Interactive.Div>
      <Interactive.Div name="SqTitle" style={{position: 'absolute', left: 76, top: 372, fontFamily: 'Archivo', fontWeight: 800, fontSize: 124, lineHeight: 0.95, letterSpacing: -4, color: ink.text, ...rise(3)}}>
        DLSS 5
        <br />
        Video Player
      </Interactive.Div>
      <Interactive.Div name="SqUrl" style={{position: 'absolute', left: 80, top: 640, fontFamily: 'JetBrains Mono', fontSize: 34, color: ink.text, ...rise(8)}}>
        github.com/2600th/<span style={{color: ink.flag}}>dlss5-video-player</span>
      </Interactive.Div>
      <Interactive.Div name="SqMeta" style={{position: 'absolute', left: 80, top: 710, width: 920, fontFamily: 'Archivo', fontSize: 26, lineHeight: 1.45, color: ink.dim, ...rise(12)}}>
        Default settings, RTX 4080 SUPER. Community project, not an NVIDIA product.
      </Interactive.Div>
    </AbsoluteFill>
  );
};

// 1080x1080, 13 s: the split face, the Difference view, 2 x 2, the name.
export const squareTimeline = [
  {id: 'sweep', at: 0, length: 4.0},
  {id: 'difference', at: 4.0, length: 3.0},
  {id: 'quad', at: 7.0, length: 3.0},
  {id: 'end', at: 10.0, length: 3.0},
] as const;

export const Square: React.FC = () => {
  const {fps, durationInFrames} = useVideoConfig();
  const frame = useCurrentFrame();
  const scene = (id: (typeof squareTimeline)[number]['id']) => {
    switch (id) {
      case 'sweep':
        return <SquareSweep />;
      case 'difference':
        return <SquareCapture src="difference.png" cx={1075} cy={420} zoom={[1.45, 1.6]} label="Difference" detail="where the model changed it · ×4" />;
      case 'quad':
        return <Saved />;
      case 'end':
        return <SquareEnd />;
    }
  };
  return (
    <AbsoluteFill style={{backgroundColor: ink.ground}}>
      {squareTimeline.map((item) => (
        <Sequence key={item.id} name={item.id} from={Math.round(item.at * fps)} durationInFrames={Math.round(item.length * fps)}>
          {scene(item.id)}
        </Sequence>
      ))}
      <Interactive.Div name="SqAttribution" style={{position: 'absolute', right: 20, bottom: 12, fontFamily: 'Archivo', fontSize: 14, color: ink.dim}}>
        007 First Light © IO Interactive · unofficial, not affiliated with NVIDIA
      </Interactive.Div>
      <div style={{position: 'absolute', left: 0, bottom: 0, height: 4, backgroundColor: ink.flag, width: interpolate(frame, [0, durationInFrames - 1], [0, 1080], clamp)}} />
    </AbsoluteFill>
  );
};
