// test_session_state.js
// 会话隔离回归：置换表不得跨局 / 跨悔棋残留。
//
// 背景：主置换表 g_tt 曾从不清理。实测（中等难度、同一进程）：
//   第一盘 (7,7) -> 白应 (6,6)；玩过一盘再新开一局，(7,7) -> 白应 (8,6)。
//   两个应手评分相同（并列），置换表残留打破了并列取舍，造成"同样局面应手漂移"。
// 修复：initGame / undo / undoN 时清空主置换表与威胁搜索置换表。
//
// 契约：
//   1) 新开一局必须回到干净状态：同一开局的应手与全新进程一致。
//   2) 悔棋后重下同一位置，连续两次重下的应手必须一致（清表后都为"新鲜搜索"）。
const M = require('./build/gomoku.js');
const sleep = ms => new Promise(r => setTimeout(r, ms));

function aiPlay() {
  const before = [];
  for (let y = 0; y < 15; y++) for (let x = 0; x < 15; x++) if (M._getCell(x, y) !== 0) before.push(x + ',' + y);
  M._aiMove();
  for (let y = 0; y < 15; y++) for (let x = 0; x < 15; x++) {
    const c = M._getCell(x, y);
    if (c !== 0 && before.indexOf(x + ',' + y) < 0) return x + ',' + y;
  }
  return 'none';
}

(async () => {
  await sleep(800);
  let failed = 0;
  const check = (ok, msg) => { console.log((ok ? 'PASS: ' : 'FAIL: ') + msg); if (!ok) failed++; };

  // 场景 1：跨局残留 —— 玩过一盘后新开一局，同一开局的应手必须与第一盘一致
  M._initGame(); M._setDifficultyLevel(1);
  M._playerMove(7, 7); const r1 = aiPlay();
  M._playerMove(6, 6); aiPlay();   // 继续在中心区搜索（污染置换表；占位时落子失败也无妨）
  M._playerMove(8, 8); aiPlay();
  M._initGame(); M._setDifficultyLevel(1);
  M._playerMove(7, 7); const r2 = aiPlay();
  check(r1 === r2, 'new game starts from a clean table (reply ' + r1 + ' reproduced, got ' + r2 + ')');

  // 场景 2：悔棋重下的稳定性 —— 连续两次「悔棋 + 重下」应手必须一致
  M._initGame(); M._setDifficultyLevel(1);
  M._playerMove(7, 7); aiPlay();
  M._playerMove(5, 5); aiPlay();
  M._undo(); M._undo();
  M._playerMove(5, 5); const w2a = aiPlay();
  M._undo(); M._undo();
  M._playerMove(5, 5); const w2b = aiPlay();
  check(w2a === w2b, 'replay after undo is stable (' + w2a + ' == ' + w2b + ')');

  console.log(failed === 0 ? 'All session-state tests passed.' : failed + ' session-state test(s) FAILED.');
  process.exit(failed === 0 ? 0 : 1);
})();
