// 古典（wasm，NNUE 默认关）夹具复现：电池 + 第 11/13 局关键点。
// HANDOFF_V7 §1 引用的脚本此前未入库，此处重建（本地脚本，pgitignored）。
// 用法: node _dbg/fixtures_replay.js 20000
const M = require('../build/gomoku.js');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const budget = parseInt(process.argv[2] || '20000');
const mvB = [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]];
const gm  = [[7,7],[8,7],[8,6],[7,6],[6,6],[8,8],[6,8],[9,5],[6,7],[6,9],[5,7],[7,9],[7,5],[8,4]];
const g13 = [[7,7],[7,8],[6,8],[5,9],[8,6],[5,10],[9,5],[10,4],[9,7],[10,8],[10,6]];
function run(moves, tag) {
  M._initGame(); M._setDifficultyLevel(2); M._setTimeLimit(budget);
  for (const m of moves) M._playerMove(m[0], m[1]);
  console.log('CASE ' + tag + ' (after ' + moves.length + ' plies)');
  M._aiMove();
}
(async () => {
  await sleep(800);
  run(mvB.slice(0, 1), 'n1');
  run(mvB.slice(0, 13), 'n13');
  run(mvB.slice(0, 15), 'n15');
  run(mvB.slice(0, 17), 'n17');
  run(gm.slice(0, 7), 'g11-m8');
  run(gm.slice(0, 9), 'g11-m10');
  run(gm.slice(0, 11), 'g11-m12');
  run(g13.slice(0, 11), 'g13-m12');
  console.log('FIXTURES done');
  process.exit(0);
})();