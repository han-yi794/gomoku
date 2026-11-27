// _dbg/verify_drift.js - NNUE 夹具漂移点的对手侧地面真值核验（协议第 3 步）
// 方法：每个漂移点 replay 到该局面 → 分别走「新着法」与「旧着法」→ 黑方以古典引擎
// 深搜 20s → 分数对比。黑方视角分越低 = 我方候选越好。古典引擎是已验证基准，
// 不用 NNUE 当真值（避免自证循环）。
const { execSync } = require('child_process');

const CLI = 'D:/gomoku/native/gomoku_cli.exe';
const BUDGET = 20000;

// [标签, 前 N 手（fixtures 的 n 截断）, 新着法(NNUE v7p 原生 20s 选点), 旧着法(期望)]
const CASES = [
  { tag: 'battery-n15', line: [[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]].slice(0, 15), newM: [9,6], oldM: [5,10] },
  { tag: 'game11-m10', line: [[7,7],[8,7],[8,6],[7,6],[6,6],[8,8],[6,8],[9,5],[6,7]], newM: [6,9], oldM: [6,5] },
  { tag: 'game13-m11', line: [[7,7],[7,8],[6,8],[5,9],[8,6],[5,10],[9,5],[10,4],[9,7],[10,8],[10,6]], newM: [5,8], oldM: [5,11] },
];

function searchAfter(line, move) {
  const moves = [...line, move].map(([x, y]) => `${x},${y}`).join(' ');
  const out = execSync(`"${CLI}" search --level 3 --budget ${BUDGET} ${moves}`, { encoding: 'utf8' });
  const m = out.match(/move=\((\d+),(\d+)\) score=(-?\d+) depth=(\d+) nodes=(\d+) ms=(\d+)/);
  if (!m) throw new Error('parse failed: ' + out);
  return { reply: [Number(m[1]), Number(m[2])], score: Number(m[3]), depth: Number(m[4]), ms: Number(m[6]) };
}

for (const c of CASES) {
  const a = searchAfter(c.line, c.newM);
  const b = searchAfter(c.line, c.oldM);
  // 黑方视角分越低 = 我方候选越好；平分局看谁的分低
  const winner = a.score < b.score ? `NEW (${c.newM})` : (a.score > b.score ? `OLD (${c.oldM})` : 'TIE');
  console.log(
    `${c.tag.padEnd(12)} NEW(${c.newM.join(',')}) blackScore=${a.score} (reply ${a.reply.join(',')} d${a.depth})` +
    ` | OLD(${c.oldM.join(',')}) blackScore=${b.score} (reply ${b.reply.join(',')} d${b.depth})` +
    `  => better: ${winner}`
  );
}
console.log('=== drift verification done (lower black score = better for white) ===');