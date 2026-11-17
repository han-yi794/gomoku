const assert = require('assert');
const Module = require('./build/gomoku.js');

const blackMoves = [
  [7, 7],
  [8, 6],
  [9, 5],
  [10, 4],
  [11, 3],
];

function boardSnapshot() {
  const board = [];
  for (let y = 0; y < 15; y += 1) {
    const row = [];
    for (let x = 0; x < 15; x += 1) row.push(Module._getCell(x, y));
    board.push(row);
  }
  return board;
}

function changedCell(before) {
  for (let y = 0; y < 15; y += 1) {
    for (let x = 0; x < 15; x += 1) {
      if (Module._getCell(x, y) !== before[y][x]) return [x, y];
    }
  }
  return null;
}

setTimeout(() => {
  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(150);

  let blackResult;
  for (let i = 0; i < blackMoves.length; i += 1) {
    const [x, y] = blackMoves[i];
    blackResult = Module._playerMove(x, y);

    if (i < blackMoves.length - 1 && blackResult === 0) {
      const before = boardSnapshot();
      Module._aiMove();
      const aiMove = changedCell(before);
      assert(aiMove, `AI move ${i + 1} should change exactly one board cell`);
    }
  }

  assert.notStrictEqual(
    blackResult,
    1,
    'AI must prevent the recorded diagonal win: black (7,7),(8,6),(9,5),(10,4),(11,3)'
  );

  console.log('PASS: AI prevents the recorded diagonal win');

  for (let attempt = 0; attempt < 5; attempt += 1) {
    Module._initGame();
    Module._setDifficultyLevel(0);
    Module._setTimeLimit(500);
    const verticalThreeSetup = [
      [9, 8],
      [9, 11],
      [9, 9],
      [6, 8],
      [9, 10],
    ];
    for (const [x, y] of verticalThreeSetup) {
      assert.strictEqual(Module._playerMove(x, y), 0, `setup move (${x},${y}) should be legal`);
    }
    Module._aiMove();
    assert.strictEqual(
      Module._getCell(9, 7),
      2,
      `AI must block the open end of a vertical three on attempt ${attempt + 1}`
    );
  }
  console.log('PASS: AI blocks the open end of a one-ended three across 5 searches');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const forkSetup = [
    [7, 7], [8, 6], [8, 8], [7, 6], [6, 6], [5, 5], [9, 9], [10, 10],
    [9, 7], [6, 7], [10, 8], [8, 5], [9, 4], [7, 5], [6, 5], [8, 4],
    [8, 7], [5, 8], [4, 9], [8, 3], [8, 2],
  ];
  for (const [x, y] of forkSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `fork setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(8, 10) === 2 || Module._getCell(9, 8) === 2 || Module._getCell(11, 7) === 2 || Module._getCell(12, 6) === 2,
    'AI must block an endpoint before the opponent can create an open four fork'
  );
  console.log('PASS: AI blocks the open-four fork setup');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const diagonalThreeSetup = [
    [7, 7], [2, 2], [8, 6], [2, 12], [9, 5], [12, 2], [12, 12],
  ];
  for (const [x, y] of diagonalThreeSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `diagonal setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(6, 8) === 2 || Module._getCell(10, 4) === 2,
    'AI must block an open-three endpoint before black completes the diagonal five'
  );
  console.log('PASS: AI blocks the diagonal open three');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const horizontalThreeSetup = [
    [7, 7], [2, 2], [8, 7], [2, 12], [9, 7], [12, 2], [12, 12],
  ];
  for (const [x, y] of horizontalThreeSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `horizontal setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(6, 7) === 2 || Module._getCell(10, 7) === 2,
    'AI must block an endpoint before black completes the horizontal five'
  );
  console.log('PASS: AI blocks the horizontal open three');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const verticalThreeSetup = [
    [7, 7], [7, 5], [6, 8], [8, 6], [5, 9], [4, 10], [7, 9], [5, 7],
    [6, 10],
  ];
  for (const [x, y] of verticalThreeSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `vertical setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(6, 9) === 2,
    'AI must occupy the shared gap before black creates a vertical/horizontal double three'
  );
  console.log('PASS: AI blocks the vertical-horizontal cross gap');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const diagonalSplitThreeSetup = [
    [6, 6], [2, 2], [8, 6], [12, 2], [6, 8], [2, 12], [8, 8],
  ];
  for (const [x, y] of diagonalSplitThreeSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `diagonal split-three setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(7, 7) === 2,
    'AI must occupy the shared diagonal gap before black creates a diagonal double split-three'
  );
  console.log('PASS: AI blocks the diagonal shared-gap double split-three');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const diagonalFourSetup = [
    [7, 7], [7, 5], [8, 8], [6, 6], [5, 7], [8, 7], [6, 8], [8, 6],
    [7, 9], [4, 6], [7, 8], [7, 6], [5, 6], [5, 8], [8, 10], [9, 11],
    [6, 10],
  ];
  for (const [x, y] of diagonalFourSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `diagonal four setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(9, 7) === 2 || Module._getCell(5, 11) === 2,
    'AI must block an endpoint before black completes the diagonal five'
  );
  console.log('PASS: AI blocks the diagonal open-four endpoint');

  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(500);
  const columnBlockSetup = [
    [7, 7], [8, 6], [8, 8], [6, 6], [9, 9], [6, 8], [8, 7], [9, 6],
    [9, 7], [6, 7], [10, 7], [11, 7], [6, 9], [7, 5], [10, 6], [7, 9],
    [10, 8],
  ];
  for (const [x, y] of columnBlockSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `column block setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  assert(
    Module._getCell(10, 9) === 2,
    'AI must block the column before black builds an unstoppable double threat'
  );
  console.log('PASS: AI blocks the column three before the double threat');

  // Sequential double threat: one move that forces a reply AND plants a broken
  // three, so the opponent is dead even after taking the forced block.
  // Black (3,4) completes the diagonal (2,3)-(3,4)-(4,5)-(5,6)-(6,7) into an
  // open four while also making y=4 a double-gap three at (3,4)(5,4)(7,4).
  // Needs depth 7 to see, so this one runs at hard difficulty.
  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(2000);
  const sequentialThreatSetup = [
    [7, 7], [7, 6], [6, 7], [8, 7], [6, 5], [6, 6], [5, 6], [7, 8],
    [4, 7], [3, 8], [7, 4], [8, 3], [5, 7], [3, 7], [5, 5], [5, 8],
    [5, 4], [5, 3], [4, 5], [3, 5],
  ];
  for (const [x, y] of sequentialThreatSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `sequential threat setup move (${x},${y}) should be legal`);
  }
  assert.strictEqual(Module._getCurrentPlayer(), 1, 'black must be on move for this fixture');
  Module._aiMove();
  assert(
    Module._getCell(3, 4) === 1,
    'AI must find the forcing point that wins two moves later'
  );
  console.log('PASS: AI finds the sequential double threat (forced block then five)');

  // The all-lost verdict occupies [-(WIN_THRESHOLD+999), -(WIN_THRESHOLD-999)]:
  // a terminal loss is -WIN_THRESHOLD with the tactical tie-break clamped to
  // +/-999.
  // 将杀距离编码后，终局败分 = -(10000000-1-k*8) + tie，k 在 20..32（深度<=12），tie 在 +-3，
  // 即落在 [-9999843, -9999741] 这条窄带里。「所有着法已证败」必然落在带内；
  // 活候选虽可能因战术加成取到很负的值（实测 -1741 万），但不落在这条窄带里。
  const inAllLostBand = (score) => score >= -9999843 && score <= -9999741;

  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(8000);
  const partialScanSetup = [
    [7, 7], [7, 6], [6, 7], [8, 7], [6, 5], [6, 6], [5, 6], [7, 8],
    [4, 7], [3, 8], [7, 4], [8, 3], [5, 7], [3, 7], [5, 5], [5, 8],
    [5, 4], [5, 3], [4, 5],
  ];
  for (const [x, y] of partialScanSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `partial scan setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  const partialScanScore = Module._wasmGetLastSearchScore();
  assert(
    !inAllLostBand(partialScanScore),
    `AI must not call this position lost off a partial scan (got ${partialScanScore})`
  );
  console.log('PASS: AI does not claim a proven loss from a partially scanned root');

  // ...and the fix must not suppress the real thing: a position that actually
  // is lost still has to be reported as lost.
  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(2000);
  const provenLostSetup = [
    [7, 7], [7, 6], [6, 7], [8, 7], [6, 5], [6, 6], [5, 6], [7, 8],
    [4, 7], [3, 8], [7, 4], [8, 3], [5, 7], [3, 7], [5, 5], [5, 8],
    [5, 4], [5, 3], [4, 5], [3, 5], [3, 4], [2, 3], [4, 4],
  ];
  for (const [x, y] of provenLostSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `lost setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  const lostScore = Module._wasmGetLastSearchScore();
  assert(
    inAllLostBand(lostScore),
    `AI must still report this forced loss (got ${lostScore})`
  );
  console.log('PASS: AI still reports a proven loss when the position really is lost');

  // VCF (continuous-four) win: black forces with (8,5) [a four the defender must
  // block at (9,5)], then (11,8) makes an open four and the win is unavoidable.
  // That is a 5-ply chain, so the easy engine's depth-4 full-width search cannot
  // see it - only the four-only VCF search reaches it.
  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(1000);
  const vcfWinSetup = [
    [5, 5], [0, 0], [6, 5], [2, 0], [7, 5], [0, 2], [9, 6], [2, 2],
    [10, 7], [4, 5],
  ];
  for (const [x, y] of vcfWinSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `vcf win setup move (${x},${y}) should be legal`);
  }
  assert.strictEqual(Module._getCurrentPlayer(), 1, 'black must be on move for this fixture');
  assert.strictEqual(Module._wasmHasVcfWin(), 1, 'black must have a continuous-four win in this fixture');
  Module._aiMove();
  assert(
    Module._wasmGetLastSearchScore() >= 10000000,
    'AI must claim the continuous-four win instead of evaluating it as unclear'
  );
  assert.strictEqual(Module._getCell(8, 5), 1, 'AI must start the four chain at (8,5)');
  console.log('PASS: AI finds the continuous-four (VCF) win');

  // ...and the other direction: the same structure one ply earlier must be
  // defused. Whatever white plays, black must not be left with a VCF win.
  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(1000);
  const vcfDefenseSetup = [
    [5, 5], [0, 0], [6, 5], [2, 0], [7, 5], [0, 2], [9, 6], [2, 2],
    [10, 7], [4, 5], [14, 14],
  ];
  for (const [x, y] of vcfDefenseSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `vcf defense setup move (${x},${y}) should be legal`);
  }
  assert.strictEqual(Module._getCurrentPlayer(), 2, 'white must be on move for this fixture');
  Module._aiMove();
  assert.strictEqual(
    Module._wasmHasVcfWin(), 0,
    'white must take away the opponent continuous-four win'
  );
  console.log('PASS: AI defuses the opponent continuous-four (VCF) win');

  // premise check: the threat above is real - a wasted white move hands black the win
  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(1000);
  for (const [x, y] of vcfDefenseSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `vcf premise move (${x},${y}) should be legal`);
  }
  assert.strictEqual(Module._playerMove(7, 7), 0, 'wasted white move should be legal');
  assert.strictEqual(
    Module._wasmHasVcfWin(), 1,
    'the fixture threat must be real: a wasted move leaves black a VCF win'
  );
  console.log('PASS: the VCF defensive fixture really is dangerous');

  // VCT: threats beyond pure four-chains. Black has two open threes waiting at
  // (7,7) and a diagonal one: (6,6) wins - if white blocks the diagonal, (7,7)
  // makes a double three; if white takes (7,7), the diagonal becomes an open
  // four. A four-only search sees none of this (the first move is not a four),
  // so a forced-win claim here only comes from the threat search.
  Module._initGame();
  Module._setDifficultyLevel(0);
  Module._setTimeLimit(1000);
  const vctSetup = [
    [5, 7], [0, 0], [6, 7], [2, 0], [7, 5], [0, 2], [7, 6], [2, 2],
  ];
  for (const [x, y] of vctSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `vct setup move (${x},${y}) should be legal`);
  }
  assert.strictEqual(Module._getCurrentPlayer(), 1, 'black must be on move for this fixture');
  Module._aiMove();
  assert(
    Module._wasmGetLastSearchScore() >= 10000000,
    'AI must claim the threat-sequence win instead of evaluating it as unclear'
  );
  assert(
    Number(Module._wasmGetLastSearchTimeMs()) < 500,
    'the threat search must prove it quickly, not eat the whole budget'
  );
  console.log('PASS: AI finds the threat-sequence (VCT) win');

  // The deep chain from the 2026-10-17 record: after white's (9,10) black wins by
  // force, but the full-width search cannot see it (it needs depth 10, the engine
  // reaches 8). Only the threat search proves it, and only after the defender's
  // counter-four is handled properly instead of disqualifying every three.
  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(12000);
  const deepChainSetup = [
    [7, 7], [8, 6], [8, 8], [6, 6], [7, 6], [7, 5], [9, 7], [5, 7],
    [8, 4], [7, 9], [8, 7], [10, 7], [9, 8], [6, 5], [10, 9], [11, 10],
    [9, 9], [9, 10],
  ];
  for (const [x, y] of deepChainSetup) {
    assert.strictEqual(Module._playerMove(x, y), 0, `deep chain setup move (${x},${y}) should be legal`);
  }
  assert.strictEqual(Module._getCurrentPlayer(), 1, 'black must be on move for this fixture');
  Module._aiMove();
  assert(
    Module._wasmGetLastSearchScore() >= 10000000,
    'AI must prove the deep threat chain the full-width search cannot reach'
  );
  console.log('PASS: AI proves a win that needs more depth than the full search has');

  // ===== 中盘让步基线（2026-10-19 两盘实战） =====
  // 这两个局面里白棋（AI）其实已经必败：黑棋后续用「双活三点」组合取胜。
  // 在评估函数学会感知这类潜在结构之前，这里锁定的契约是「必须如实上报败局」：
  //   - 报告分必须落在将杀距离的败势带里（引擎知道自己输、且知道输得多慢）
  //   - 绝不允许回到历史上那种「必败局面评成 +4600 万大优」的误判
  // 评估层改进（潜在威胁感知）落地后，这两条会开始失败——届时把它们改成
  // 「必须不落入败势带」的正向断言。
  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(12000);
  const concession1 = [
    [7, 7], [5, 6], [6, 8], [8, 6], [6, 6], [8, 8], [6, 7], [6, 9],
    [5, 7], [4, 7], [6, 5], [6, 4], [8, 7],
  ];
  for (const [x, y] of concession1) {
    assert.strictEqual(Module._playerMove(x, y), 0, `concession1 setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  const score1 = Number(Module._wasmGetLastSearchScore());
  assert(
    inAllLostBand(score1),
    `AI must honestly report the midgame concession (got ${score1})`
  );
  console.log('PASS: midgame concession honestly reported (game 1, lost at ~ply 30)');

  // game 2: white was already lost before its move 26 (9,5)/(5,5) both lose)
  Module._initGame();
  Module._setDifficultyLevel(2);
  Module._setTimeLimit(12000);
  const concession2 = [
    [7, 7], [8, 6], [7, 5], [5, 6], [7, 6], [7, 4], [6, 6], [8, 8],
    [5, 7], [8, 4], [7, 8], [7, 9], [4, 8], [3, 9], [8, 7], [9, 7],
    [10, 6], [9, 8], [6, 7], [4, 7], [6, 5], [6, 8], [8, 5], [6, 10],
    [5, 11],
  ];
  for (const [x, y] of concession2) {
    assert.strictEqual(Module._playerMove(x, y), 0, `concession2 setup move (${x},${y}) should be legal`);
  }
  Module._aiMove();
  const score2 = Number(Module._wasmGetLastSearchScore());
  assert(
    inAllLostBand(score2),
    `AI must honestly report the midgame concession in game 2 (got ${score2})`
  );
  console.log('PASS: midgame concession honestly reported (game 2, lost at ~ply 30)');
}, 1000);
