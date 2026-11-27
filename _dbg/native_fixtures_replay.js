// _dbg/native_fixtures_replay.js - 原生 CLI 侧 9 夹具复现（NNUE + 快照优化后的选点）
// 与 _dbg/nnue_fixtures_replay.js 同一批夹具/期望，但走 native search（avoid wasm/native 双轨偏差）
// 用法: node _dbg/native_fixtures_replay.js [预算ms]
const { execSync } = require('child_process');
const CLI = 'D:/gomoku/native/gomoku_cli.exe';
const BIN = process.env.NNUE_BIN || 'D:/gomoku/native/nnue/nnue_v6.bin';
const BUDGET = process.argv[2] ? Number(process.argv[2]) : 20000;

const FIXTURES = [
  { tag: 'battery-n1',  line: [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]], n: 1,  expect: [[7,8]],       note: 'n1' },
  { tag: 'battery-n13', line: [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]], n: 13, expect: [[4,10]],      note: 'n13' },
  { tag: 'battery-n15', line: [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]], n: 15, expect: [[5,10]],      note: 'n15 b3 保护生效点：不得选 (4,6)' },
  { tag: 'battery-n17', line: [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]], n: 17, expect: [[3,9]],       note: 'n17 全证败，评分须落败带', lossBand: true },
  { tag: 'game11-m8',  line: [[7,7],[8,7],[8,6],[7,6],[6,6],[8,8],[6,8],[9,5],[6,7],[6,9],[5,7],[7,9],[7,5],[8,4]], n: 7,  expect: [[9,5]],  note: 'm8' },
  { tag: 'game11-m10', line: [[7,7],[8,7],[8,6],[7,6],[6,6],[8,8],[6,8],[9,5],[6,7],[6,9],[5,7],[7,9],[7,5],[8,4]], n: 9,  expect: [[6,5]],  note: 'm10' },
  { tag: 'game11-m12', line: [[7,7],[8,7],[8,6],[7,6],[6,6],[8,8],[6,8],[9,5],[6,7],[6,9],[5,7],[7,9],[7,5],[8,4]], n: 11, expect: [[6,5]],  note: 'm12（旧选 (9,8) 被黑 6.1s 证杀）' },
  { tag: 'game12-m13', line: [[7,7],[7,8],[8,6],[6,8],[8,8],[9,9],[9,7],[8,7],[9,6],[10,6],[9,5],[9,4],[7,5]], n: 13, expect: [[6,4]],  note: '20s 必须选 (6,4)；10s/15s 会错选 (10,8)' },
  { tag: 'game13-m11', line: [[7,7],[7,8],[6,8],[5,9],[8,6],[5,10],[9,5],[10,4],[9,7],[10,8],[10,6]], n: 11, expect: [[5,11],[6,11],[5,12],[7,11],[4,11]], note: '不得选 (8,8)（黑 43ms 证杀）；b3 应换到幸存手', forbid: [[8,8]] },
];
const LOSS_BAND_LO = -(10000000 + 999);
const LOSS_BAND_HI = -(10000000 - 999);

let match = 0;
for (const fx of FIXTURES) {
  const moves = fx.line.slice(0, fx.n).map(([x, y]) => `${x},${y}`).join(' ');
  const out = execSync(`"${CLI}" search --level 2 --budget ${BUDGET} --nnue "${BIN}" ${moves}`, { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] });
  const m = out.match(/move=\((\d+),(\d+)\) score=(-?\d+) depth=(\d+) nodes=(\d+) ms=(\d+)/);
  if (!m) { console.log(`${fx.tag}: PARSE FAIL: ${out.slice(0, 200)}`); continue; }
  const mv = [Number(m[1]), Number(m[2])];
  const ok = fx.expect.some(([ex, ey]) => ex === mv[0] && ey === mv[1]);
  const forbidden = fx.forbid && fx.forbid.some(([ex, ey]) => ex === mv[0] && ey === mv[1]);
  const inBand = fx.lossBand && Number(m[3]) >= LOSS_BAND_LO && Number(m[3]) <= LOSS_BAND_HI;
  const pass = ok || Boolean(inBand) || (fx.lossBand && forbidden === false);
  if (pass && !forbidden) match++;
  console.log(`${pass ? 'MATCH' : 'DIFF '} ${fx.tag.padEnd(13)} move=(${mv.join(',')}) score=${m[3]} depth=${m[4]} nodes=${m[5]} ms=${m[6]}  [${fx.note}]`);
}
console.log(`=== done: ${match}/${FIXTURES.length} match (native, NNUE v6+snapshot, ${BUDGET}ms) ===`);