import React from 'react';
import {AbsoluteFill, Composition, Img, Sequence, interpolate, registerRoot, staticFile, useCurrentFrame} from 'remotion';
import {Video} from '@remotion/media';

const lime = '#b6f36b';
const clamp = {extrapolateLeft: 'clamp', extrapolateRight: 'clamp'} as const;
const playerStyle: React.CSSProperties = {position: 'absolute', left: 426, top: 74, width: 1442, height: 932, border: '1px solid #34414b', boxShadow: '0 26px 70px #0008'};

const Demo = () => {
  const f = useCurrentFrame();
  const chapter = f < 90 ? 0 : f < 360 ? 1 : f < 810 ? 2 : 3;
  const headings = ['Render once.\nCompare\ninstantly.', 'One frame.\nTwo views.', 'Neural On.\nKeep\nwatching.', 'Download.\nPlay.\nCompare.'];
  const details = ['Original and cached neural playback in a native Windows player.', 'Pause and switch the same timestamp. Look at the face, hair and lighting.', 'The neural view stays enabled as playback continues.', 'Try the experimental v0.13.0 Windows build.'];
  return <AbsoluteFill style={{backgroundColor: '#0b1117', color: '#f0f4f8', fontFamily: 'Segoe UI, sans-serif'}}>
    <div style={{position: 'absolute', left: 64, top: 77, width: 290, color: lime, fontSize: 21, fontWeight: 600, letterSpacing: 2}}>DLSS 5<br/>VIDEO PLAYER</div>
    <div style={{position: 'absolute', left: 64, top: 246, width: 332, whiteSpace: 'pre-line', fontSize: 52, lineHeight: 1.1, letterSpacing: -1.8, fontWeight: 650, opacity: interpolate(f, [0, 15], [0, 1], clamp)}}>{headings[chapter]}</div>
    <div style={{position: 'absolute', left: 67, top: 513, width: 292, color: '#bac6d1', fontSize: 27, lineHeight: 1.42}}>{details[chapter]}</div>
    <div style={{position: 'absolute', left: 67, top: 741, width: 290, fontSize: 20, lineHeight: 1.5, color: '#93a3b1'}}><span style={{color: lime}}>RECORDED ON RTX 5090</span><br/>1080p source · Upscaling off<br/>Neutral playback adjustments</div>
    <div style={{position: 'absolute', left: 67, top: 889, width: 292, fontSize: 20, lineHeight: 1.4, color: '#b9c6d0'}}>Prepared cache comparison.<br/>No live neural processing.</div>
    <Sequence durationInFrames={90}><Img src={staticFile('hero.jpg')} style={playerStyle}/></Sequence>
    <Sequence from={90} durationInFrames={270}><Video src={staticFile('face-take.mp4')} muted objectFit="contain" style={playerStyle}/></Sequence>
    <Sequence from={360} durationInFrames={450}><Video src={staticFile('playback-take.mp4')} muted objectFit="contain" style={playerStyle}/></Sequence>
    <Sequence from={810} durationInFrames={90}><Img src={staticFile('hero.jpg')} style={playerStyle}/></Sequence>
    <div style={{position: 'absolute', left: 67, top: 1017, fontSize: 16, color: '#8fa0ae'}}>Unofficial RenoDX/ReShade experiment. NVIDIA components belong to NVIDIA. The Witcher IV footage © CD PROJEKT RED.</div>
    <div style={{position: 'absolute', left: 0, bottom: 0, height: 4, width: interpolate(f, [0, 899], [0, 1920]), background: lime}}/>
  </AbsoluteFill>;
};

registerRoot(() => <Composition id="Demo" component={Demo} width={1920} height={1080} fps={30} durationInFrames={900}/>);
