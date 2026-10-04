#!/usr/bin/env node
// Generates each console's music and sound effects with ElevenLabs (music_v2_5 and the
// sound-effects model), into audio/masters/<console>/<name>.mp3. Existing files are kept
// (pass --force to make them again, or a name filter to make only some).
//   ELEVENLABS_API_KEY=... node tools/audio/gen.mjs [--force] [filter]
//
// Every console has its own retro sound, the way each has its own look:
//   gba  pocket chiptune (pulse and triangle waves, noise drums)
//   ds   bright handheld pop (marimba, glockenspiel, slap bass)
//   psp  glossy late-night arcade synth (pads, analog bass, breakbeats)
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const KEY = process.env.ELEVENLABS_API_KEY;
const force = process.argv.includes('--force');
const filter = process.argv.slice(2).find((a) => !a.startsWith('--'));
if (!KEY) throw new Error('Set ELEVENLABS_API_KEY');

const LOOP = 'instrumental, steady tempo from the first beat to the last, no intro build-up, no fade in, no fade out, no ending, made to loop seamlessly';
const STYLE = {
  gba: '16-bit portable console chiptune sound: pulse-wave leads, triangle-wave bass, noise-channel drums',
  ds: 'mid-2000s portable console soundtrack: clean sampled instruments, bright and light',
  psp: 'mid-2000s portable console electronic soundtrack: glossy synths, analog bass, crisp breakbeats',
};

const MUSIC = {
  gba: {
    lobby: [`Upbeat menu theme for a card battle game: bright square-wave lead melody, bubbly arpeggios, bouncy triangle bass, crisp noise drums, 140 BPM, major key, catchy and playful. ${STYLE.gba}. ${LOOP}.`, 44000],
    game: [`Focused theme for a turn-based card battle in progress: steady 118 BPM groove, pulse-wave bass ostinato, a soft square-wave melody that leaves room to think, light noise hi-hats, gently tense but friendly. ${STYLE.gba}. ${LOOP}.`, 60000],
    win: [`A short triumphant victory jingle, about 5 seconds: a rising square-wave fanfare with a sparkly arpeggio, landing on a held major chord that rings out. ${STYLE.gba}.`, 6000],
    lose: [`A short defeat jingle, about 4 seconds: a descending, good-humoured square-wave phrase ending on a soft minor chord, not sad. ${STYLE.gba}.`, 5000],
  },
  ds: {
    lobby: [`Cheerful menu theme for a card battle game: bouncy marimba and glockenspiel melody, slap bass, bright synth brass stabs, punchy drum machine, 128 BPM, sunny major key. ${STYLE.ds}. ${LOOP}.`, 44000],
    game: [`Light, curious groove for a strategy card battle in progress: plucky pizzicato strings and electric piano, round synth bass, soft shaker and kick, 112 BPM, playful thinking music. ${STYLE.ds}. ${LOOP}.`, 60000],
    win: [`A bright victory jingle, about 5 seconds: a brass and glockenspiel fanfare with a cymbal swell, landing on a ringing major chord. ${STYLE.ds}.`, 6000],
    lose: [`A gentle defeat jingle, about 4 seconds: a descending marimba phrase with a muted horn, light and good-humoured. ${STYLE.ds}.`, 5000],
  },
  psp: {
    lobby: [`Late-night arcade menu theme for a card battle game: shimmering pads, chunky analog bass, crisp breakbeat drums, a bright synth lead hook, 124 BPM, energetic and cool. ${STYLE.psp}. ${LOOP}.`, 44000],
    game: [`Stylish, focused electronic groove for a card battle in progress: pulsing sidechained bass, minimal synth arpeggios, deep kick and snappy snare, 116 BPM, tense but cool. ${STYLE.psp}. ${LOOP}.`, 60000],
    win: [`A victory sting, about 5 seconds: a big synth chord stab, a rising arpeggio and a reverse cymbal, landing on a wide ringing chord. ${STYLE.psp}.`, 6000],
    lose: [`A defeat sting, about 4 seconds: a falling filtered synth line into a low warm pad, not sad. ${STYLE.psp}.`, 5000],
  },
};

const SFX = {
  gba: {
    move: ['8-bit chiptune menu cursor blip: one tiny, short square-wave tick', 0.5],
    select: ['8-bit chiptune confirm sound: a quick rising two-note square-wave chirp', 0.5],
    back: ['8-bit chiptune cancel sound: a quick falling two-note square-wave blip', 0.5],
    place: ['8-bit chiptune card placed on the board: a short punchy noise thump with a pitch drop', 0.5],
    capture: ['8-bit chiptune capture sound: a quick sparkly rising square-wave arpeggio', 0.8],
    turn: ['8-bit chiptune "your turn" chime: two bright square-wave notes, rising', 0.8],
    error: ['8-bit chiptune error buzz: a short low square-wave double beep', 0.5],
    start: ['8-bit chiptune game start: a fast rising noise sweep ending in a bright ping', 1.0],
  },
  ds: {
    move: ['Handheld game menu cursor: one tiny bright marimba plink, very short, clean', 0.5],
    select: ['Handheld game confirm: a bright two-note glockenspiel chime, rising, clean', 0.6],
    back: ['Handheld game cancel: a soft descending two-note marimba, clean', 0.5],
    place: ['A playing card slapped onto a wooden table: short, crisp, close up', 0.5],
    capture: ['A card flipping over with a quick swish and a sparkly glockenspiel chime', 0.8],
    turn: ['A loud, bright two-note vibraphone chime, rising, close-miked and clear: a friendly game alert', 0.8],
    error: ['A soft muted cartoon bonk, short, gentle, "not allowed"', 0.5],
    start: ['A quick swoosh into a bright cymbal sparkle, game start, handheld game', 1.0],
  },
  psp: {
    move: ['Sleek synth menu tick: one tiny glassy click, very short, modern game UI', 0.5],
    select: ['Punchy synth confirm sound with a short shimmering tail, modern game UI', 0.6],
    back: ['Low soft synth back blip, short, modern game UI', 0.5],
    place: ['A heavy card slammed onto a table with a deep bass thump, short, punchy', 0.6],
    capture: ['An electric card flip: a quick zap with a rising synth sweep, short', 0.8],
    turn: ['A glassy two-note synth alert, "your turn", bright, modern game UI', 0.8],
    error: ['A short low distorted synth buzz, "not allowed", modern game UI', 0.5],
    start: ['A short cinematic synth riser into a punchy impact, game start', 1.2],
  },
};

async function call(url, body, out) {
  for (let attempt = 1; attempt <= 3; attempt++) {
    const res = await fetch(url, { method: 'POST', headers: { 'xi-api-key': KEY, 'content-type': 'application/json' }, body: JSON.stringify(body) });
    if (res.ok) {
      mkdirSync(dirname(out), { recursive: true });
      writeFileSync(out, Buffer.from(await res.arrayBuffer()));
      return;
    }
    const text = await res.text();
    console.log(`  ${res.status} ${text.slice(0, 300)}`);
    if (res.status < 500 && res.status !== 429) throw new Error(`${out}: ${res.status}`);
    await new Promise((r) => setTimeout(r, 5000 * attempt));
  }
  throw new Error(`${out}: gave up`);
}

const jobs = [];
for (const [con, set] of Object.entries(MUSIC))
  for (const [name, [prompt, ms]] of Object.entries(set))
    jobs.push({ out: join(ROOT, 'audio/masters', con, `${name}.mp3`), label: `${con}/${name}`,
                url: 'https://api.elevenlabs.io/v1/music?output_format=mp3_44100_128', body: { prompt, music_length_ms: ms, model_id: 'music_v2_5', force_instrumental: true } });
for (const [con, set] of Object.entries(SFX))
  for (const [name, [text, secs]] of Object.entries(set))
    jobs.push({ out: join(ROOT, 'audio/masters', con, `sfx-${name}.mp3`), label: `${con}/sfx-${name}`,
                url: 'https://api.elevenlabs.io/v1/sound-generation?output_format=mp3_44100_128', body: { text, duration_seconds: secs, prompt_influence: 0.55 } });

const todo = jobs.filter((j) => (!filter || j.label.includes(filter)) && (force || !existsSync(j.out)));
console.log(`${todo.length} to make`);
// a few at a time: music takes a while, sound effects are quick
const queue = [...todo];
await Promise.all(Array.from({ length: 4 }, async () => {
  while (queue.length) {
    const j = queue.shift();
    const t0 = Date.now();
    try {
      await call(j.url, j.body, j.out);
      console.log(`made ${j.label} (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
    } catch (e) {
      console.log(`FAILED ${j.label}: ${e.message}`);
    }
  }
}));
