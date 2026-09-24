import React from 'react';
import {Easing, Interactive, interpolate, useCurrentFrame} from 'remotion';

// The site's palette and type (site/src/styles.css), so the video and the page
// it plays on read as one thing: true black, one ink.
export const ink = {
  ground: '#050506',
  text: '#ecebe8',
  dim: '#a4a099',
  meta: '#837e77',
  flag: '#ff6a1a',
  line: '#ffffff26',
};

export const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'} as const;
export const ease = Easing.bezier(0.16, 1, 0.3, 1);

// Bottom-left lower third: what the footage is, then how it was made.
export const Caption: React.FC<{title: string; detail: string; delay?: number}> = ({title, detail, delay = 8}) => {
  const frame = useCurrentFrame();
  return (
    <Interactive.Div
      name="Caption"
      style={{
        position: 'absolute',
        left: 56,
        bottom: 60,
        padding: '18px 24px 16px',
        backgroundColor: '#050506d9',
        borderLeft: `3px solid ${ink.flag}`,
        opacity: interpolate(frame, [delay, delay + 10], [0, 1], {...clamp, easing: ease}),
        translate: interpolate(frame, [delay, delay + 14], ['0px 14px', '0px 0px'], {...clamp, easing: ease}),
      }}
    >
      <div style={{fontFamily: 'Archivo', fontSize: 40, fontWeight: 700, letterSpacing: -0.6, color: ink.text, lineHeight: 1.1}}>
        {title}
      </div>
      <div style={{fontFamily: 'JetBrains Mono', fontSize: 21, color: ink.dim, marginTop: 8, letterSpacing: 0.4}}>{detail}</div>
    </Interactive.Div>
  );
};
