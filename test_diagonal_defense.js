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
      const before = snapshotBoard();
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
}, 1000);

function snapshotBoard() {
  return boardSnapshot();
}
