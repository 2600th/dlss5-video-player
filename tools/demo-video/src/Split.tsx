import React from 'react';
import {AbsoluteFill, Img, Interactive, staticFile, useVideoConfig} from 'remotion';
import {Video} from '@remotion/media';
import {ink} from './brand';

// One frame, twice: the render fills the picture and the original is clipped
// to the left of the divider. Both layers get the same source-pixel crop and
// the same `scale`, so any scaling applies identically to both halves - the
// divider is the only thing that differs between them.
export type Plate = {
  // Stills are full 2560x1440 PNGs; clips are pre-cut 1920x1080 MP4s.
  kind: 'still' | 'clip';
  original: string;
  neural: string;
  // Still only: where the composition's window sits inside the 2560x1440 frame.
  x?: number;
  y?: number;
};

const layer = (plate: Plate, which: 'original' | 'neural', scale: number, origin: string) => {
  const common: React.CSSProperties = {position: 'absolute', scale: String(scale), transformOrigin: origin};
  const src = staticFile(plate[which]);
  return plate.kind === 'clip' ? (
    <Video src={src} muted style={{...common, left: 0, top: 0, width: 1920, height: 1080}} />
  ) : (
    <Img src={src} style={{...common, left: -(plate.x ?? 0), top: -(plate.y ?? 0), width: 2560, height: 1440}} />
  );
};

export const Split: React.FC<{plate: Plate; divider: number; scale?: number; origin?: string; labels?: number}> = ({
  plate,
  divider,
  scale = 1,
  origin = '50% 50%',
  labels = 1,
}) => {
  // The composition's own size, so the same split serves the 1920x1080 demo,
  // the 1200x630 card and the 1080x1080 clip.
  const {width, height} = useVideoConfig();
  const x = divider * width;
  return (
    <AbsoluteFill style={{overflow: 'hidden', backgroundColor: ink.ground}}>
      {layer(plate, 'neural', scale, origin)}
      <AbsoluteFill style={{clipPath: `inset(0 ${width - x}px 0 0)`}}>{layer(plate, 'original', scale, origin)}</AbsoluteFill>
      <div style={{position: 'absolute', left: x - 1, top: 0, width: 2, height, backgroundColor: ink.text, opacity: divider > 0.001 && divider < 0.999 ? 0.9 : 0}} />
      <Interactive.Div
        name="LabelOriginal"
        style={{position: 'absolute', right: width - x + 22, top: 44, opacity: labels * (divider > 0.08 ? 1 : 0), ...chip, color: ink.text, backgroundColor: '#050506cc'}}
      >
        ORIGINAL
      </Interactive.Div>
      <Interactive.Div
        name="LabelNeural"
        style={{position: 'absolute', left: x + 22, top: 44, opacity: labels * (divider < 0.92 ? 1 : 0), ...chip, color: '#120602', backgroundColor: ink.flag}}
      >
        DLSS 5
      </Interactive.Div>
    </AbsoluteFill>
  );
};

const chip: React.CSSProperties = {
  padding: '10px 18px 9px',
  fontFamily: 'JetBrains Mono',
  fontSize: 26,
  fontWeight: 600,
  letterSpacing: 2,
  whiteSpace: 'nowrap',
};
