// _dbg/mix_v7.js - 拼接 v7 训练混合集（v7 = 30s 教师深标签）
// 浅 12621(v2) + 800ms 2927(v4) + 3s 消毒 6756(v6)×3 + 10s 深 1946(v6deep)×3 + 30s 深(v7)×3
const fs = require('fs');
const nn = 'D:/gomoku/native/nnue/';

function dataLines(file) {
  return fs.readFileSync(file, 'utf8').split(/\r?\n/).filter((l) => {
    if (!l || l.startsWith('#')) return false;
    const parts = l.split('\t');
    if (parts.length !== 2) return false;
    const feats = parts[1].split(' ');
    return feats.length === 62 && feats.every((f) => f !== '' && Number.isFinite(Number(f)));
  });
}
function loadOr(parts, name) {
  const out = [];
  for (const p of parts) out.push(...dataLines(p));
  if (out.length < 50) throw new Error(name + ' too small: ' + out.length);
  return out;
}

const shallow = dataLines(nn + 'dump_v2.txt');
const mid = dataLines(nn + 'dump_v4.txt');
const deep3s = loadOr([nn + 'dump_v6.txt', 'D:/gomoku/_dbg/v6_more.txt'], '3s-sanitized');
const deep10s = loadOr([nn + 'dump_v6deep.txt', 'D:/gomoku/_dbg/v6deep_more.txt'], '10s-deep');
const deep30s = loadOr([nn + 'dump_v7.txt', nn + 'dump_v7b.txt', nn + 'dump_v7c.txt', nn + 'dump_v7d.txt'], '30s-deep');

const out = [...shallow, ...mid, ...deep3s, ...deep3s, ...deep3s, ...deep10s, ...deep10s, ...deep10s, ...deep30s, ...deep30s, ...deep30s];
fs.writeFileSync('D:/gomoku/_dbg/mix_v7.txt', out.join('\n') + '\n');
console.log(`mix_v7: shallow=${shallow.length} mid=${mid.length} 3s=${deep3s.length}x3 10s=${deep10s.length}x3 30s=${deep30s.length}x3 total=${out.length}`);