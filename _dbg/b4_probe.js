// _dbg/b4_probe.js - b4 落子深证验证探针（node _dbg/b4_probe.js <tag>）
// 用法示例：
//   node _dbg/b4_probe.js m12        game11-m12 位：期望 (6,5)、闸门触发则 ms≈35s
//   node _dbg/b4_probe.js m12verify  (7,5) 后黑方 15s 深搜：期望证出 mate（b4 谓词闭环）
//   node _dbg/b4_probe.js m13        game12-m13 位：期望 (6,4)（闸门应跳过：自评已够负）
//   node _dbg/b4_probe.js g13m12     game13-m12 位：期望非 (8,8)
//   node _dbg/b4_probe.js all        依次跑全部
const { execSync } = require('child_process');
const CLI = 'D:/gomoku/native/gomoku_cli.exe';

const CASES = {
  // game11 第 12 手（白方）：正解 (6,5)，历史败着 (7,5)
  m12:       'search --level 3 --budget 20000 7,7 8,7 8,6 7,6 6,6 8,8 6,8 9,5 6,7 6,9 5,7',
  // 白方走出历史败着 (7,5) 后，黑方 15s 全引擎深搜（b4 的深证预算）——期望 mate 分
  m12verify: 'search --level 3 --budget 15000 7,7 8,7 8,6 7,6 6,6 8,8 6,8 9,5 6,7 6,9 5,7 7,5',
  // game12 第 14 手（白方）：期望 (6,4)
  m13:       'search --level 3 --budget 20000 7,7 7,8 8,6 6,8 8,8 9,9 9,7 8,7 9,6 10,6 9,5 9,4 7,5',
  // game13 第 12 手（白方）：期望非 (8,8)
  g13m12:    'search --level 3 --budget 20000 7,7 7,8 6,8 5,9 8,6 5,10 9,5 10,4 9,7 10,8 10,6',
  // 10s 档复现第 11 局败线（b4 闸门在预算 >=8s 时生效）：观察是否改走幸存者
  m12s10:    'search --level 3 --budget 10000 7,7 8,7 8,6 7,6 6,6 8,8 6,8 9,5 6,7 6,9 5,7',
};

const EXPECT = {
  m12:       'pick (6,5); ms>30000 = gate fired, no mate',
  m12verify: 'mate score (>9990000) = b4 predicate proven end-to-end',
  m13:       'pick (6,4); gate should SKIP (self-eval already ~-9M)',
  g13m12:    'pick != (8,8)',
  m12s10:    '10s repro: pick must not be the losing (7,5)',
};

function run(tag) {
  const args = CASES[tag];
  if (!args) { console.log('unknown case: ' + tag); return; }
  const out = execSync(CLI + ' ' + args, { encoding: 'utf8' });
  const line = out.split('\n').find(s => s.includes('move=(')) || out;
  console.log(tag + ': ' + line.trim());
  console.log('   expect: ' + EXPECT[tag]);
}

const what = process.argv[2] || 'all';
if (what === 'all') { ['m12', 'm13', 'g13m12', 'm12verify', 'm12s10'].forEach(run); }
else { run(what); }