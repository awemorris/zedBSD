// Compile editable SVG screen templates into the installer's RGB24 BMP assets.
// Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
const fs = require('node:fs');
const path = require('node:path');
const { Resvg } = require('@resvg/resvg-js');
const out = __dirname;
const background = fs.readFileSync(path.join(out, 'background.jpg')).toString('base64');
const font = process.argv[2] || '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf';
const esc = text => text.replaceAll('&', '&amp;').replaceAll('<', '&lt;');
const label = (x,y,size,text,color='#f7fcf8') => `<text x="${x}" y="${y}" font-size="${size}" fill="${color}" font-family="DejaVu Sans">${esc(text)}</text>`;
function screen(kind, focused) {
    const image = `<image href="data:image/jpeg;base64,${background}" x="0" y="0" width="640" height="480" preserveAspectRatio="xMidYMid slice"/>`;
    let svg = `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480" viewBox="0 0 640 480"><defs><filter id="blur" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="9"/></filter><clipPath id="panel"><rect x="32" y="56" width="576" height="396" rx="16"/></clipPath></defs>${image}<g clip-path="url(#panel)"><g filter="url(#blur)">${image}</g><rect x="32" y="56" width="576" height="396" fill="#142e25" fill-opacity=".78"/></g><rect x="32" y="56" width="576" height="396" rx="16" fill="none" stroke="#c5eedb" stroke-width="1.1"/>${label(24,35,25,'zedBSD')}`;
    for (let i=0;i<5;i++) {
        const x=64+i*105;
        svg+=`<circle cx="${x}" cy="83" r="5" fill="${focused ? '#a9f9dd' : 'none'}" stroke="#c5d4c9"/>`;
        svg+=label(x+11,87,10,['Source','Mode','Storage','Review','Install'][i]);
    }
    if (kind==='menu') {
        for(let i=0;i<3;i++) {
            const y=198+i*50;
            svg+=`<rect x="60" y="${y}" width="520" height="42" rx="7" fill="#d1f8e0" fill-opacity=".04" stroke="${focused?'#a9f9dd':'#a4b0a5'}" stroke-width="${focused?2:1}"/><circle cx="79" cy="${y+21}" r="7" fill="${focused?'#a9f9dd':'none'}" stroke="#c5d4c9"/>`;
        }
    }
    if(kind==='progress') svg+=`<rect x="60" y="295" width="520" height="15" rx="6" fill="#10291f" stroke="#7b9587"/>`;
    svg+=`<rect x="60" y="389" width="120" height="34" rx="6" fill="#d2eddb" fill-opacity=".12" stroke="${focused?'#a9f9dd':'#819589'}" stroke-width="${focused?2:1}"/><rect x="438" y="389" width="142" height="34" rx="6" fill="#a9f9dd" stroke="${focused?'#ffffff':'#a9f9dd'}" stroke-width="${focused?2:1}"/>`;
    svg+=label(60,441,10,kind==='progress' ? 'Working. Keep the installation media connected.' : 'Tab / arrows: Move     Enter: Select     Esc: Cancel');
    return svg+'</svg>';
}
function bmp(image) {
    const w=image.width,h=image.height,stride=(w*3+3)&~3;
    const bytes=Buffer.alloc(54+stride*h);
    bytes.write('BM');bytes.writeUInt32LE(bytes.length,2);bytes.writeUInt32LE(54,10);
    bytes.writeUInt32LE(40,14);bytes.writeInt32LE(w,18);bytes.writeInt32LE(h,22);
    bytes.writeUInt16LE(1,26);bytes.writeUInt16LE(24,28);bytes.writeUInt32LE(stride*h,34);
    const rgba=image.pixels;
    for(let y=0;y<h;y++) for(let x=0;x<w;x++) {
        const src=(y*w+x)*4,dst=54+(h-y-1)*stride+x*3;
        if(rgba[src+3]!==255) throw Error('nonopaque screen');
        bytes[dst]=rgba[src+2];bytes[dst+1]=rgba[src+1];bytes[dst+2]=rgba[src];
    }
    return bytes;
}
for(const kind of ['menu','review','progress']) for(const focused of [false,true]) {
    const name=kind+(focused?'-focus':'');
    const svg=screen(kind,focused);
    // SVGs remain directly inspectable; the target needs only the BMPs.
    fs.writeFileSync(path.join(out,name+'.svg'),svg.replaceAll('data:image/jpeg;base64,'+background, 'background.jpg'));
    const image=new Resvg(svg,{font:{loadSystemFonts:false,fontFiles:[font]}}).render();
    fs.writeFileSync(path.join(out,name+'.bmp'),bmp(image));
    fs.writeFileSync(path.join(out,name+'.png'),image.asPng());
}
