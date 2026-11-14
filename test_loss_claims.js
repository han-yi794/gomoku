// test_loss_claims.js
// 「全证败」抽查回归（偶层视界假证败的防护）：
//  T1 假证败夹具（2026-10-31 第二局）：黑 (9,6) 后白方不输（黑方深搜无杀），
//     引擎不得报「损失带」结论。
//  T2 真败保留：电池 m18 局面应为真证败，仍须落在损失带（防过度否决）。
//  T3 信息项：黑 (9,5) 分支（存在活路 (8,9)）观察抽查效果（只打印，不判定）。
const M = require('./build/gomoku.js');
const sleep = ms => new Promise(r => setTimeout(r, ms));

const P10 = [[7,7],[7,8],[8,6],[6,8],[8,8],[9,9],[9,7],[8,7],[10,6],[7,9]];
const MOVES_B = [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]];
const LOSS_BAND = -9999743;   // = -(WIN_SCORE_FLOOR)

function playAndSearch(mv) {
  M._initGame();
  M._setDifficultyLevel(2);
  for (const m of mv) M._playerMove(m[0], m[1]);
  M._aiMove();
  const score = Number(M._wasmGetLastSearchScore());
  const w = [];
  for (let y = 0; y < 15; y++) for (let x = 0; x < 15; x++) if (M._getCell(x, y) === 2) w.push(x + ',' + y);
  return { score, whites: w.join(' ') };
}

(async () => {
  await sleep(800);
  let failed = 0;
  const check = (ok, msg) => { console.log((ok ? 'PASS: ' : 'FAIL: ') + msg); if (!ok) failed++; };

  // T1: 假证败夹具
  const t1 = playAndSearch(P10.concat([[9, 6]]));
  check(t1.score > LOSS_BAND, 'F1 false-loss rejected (score ' + t1.score + ' not in loss band)');

  // T2: 真败保留（m18 电池局面）
  const t2 = playAndSearch(MOVES_B.slice(0, 17));
  check(t2.score <= LOSS_BAND, 'm18 true loss preserved (score ' + t2.score + ')');

  // T3: 信息项
  const t3 = playAndSearch(P10.concat([[9, 5]]));
  console.log('INFO: F2 (9,5)-branch -> whites=' + t3.whites + ' score=' + t3.score);

  console.log(failed === 0 ? 'All loss-claim tests passed.' : failed + ' loss-claim test(s) FAILED.');
  process.exit(failed === 0 ? 0 : 1);
})();
