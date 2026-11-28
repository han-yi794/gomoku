// _dbg/b4_trace.js - b4 gate tracing (temporary instrumentation; remove after diagnosis)
// usage: node _dbg/b4_trace.js <budget> [--nnue] <moves...>
// prints: the search's move line + all [b4] stderr traces
const { spawnSync } = require('child_process');
const args = process.argv.slice(2);
const budget = args.shift();
const useNnue = args[0] === '--nnue';
if (useNnue) { args.shift(); }
const nnueArgs = useNnue ? ['--nnue', '_dbg/nnue_v7.bin'] : [];
const cli = 'D:/gomoku/native/gomoku_cli.exe';
const r = spawnSync(cli, ['search', '--level', '3', '--budget', budget].concat(nnueArgs).concat(args), { encoding: 'utf8' });
const moveLine = (r.stdout || '').split('\n').find(s => s.includes('move=(')) || '(no move line)';
console.log('budget=' + budget + (useNnue ? ' +nnue' : '') + ' moves=' + args.join(' '));
console.log('  ' + moveLine.trim());
for (const l of (r.stderr || '').split('\n')) {
  const t = l.trim();
  if (t.includes('[b4]')) console.log('  ' + t);
}