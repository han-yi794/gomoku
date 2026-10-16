// test_engine.js - WASM 引擎测试脚本
const fs = require('fs');
const path = require('path');

const Module = require('./build/gomoku.js');

setTimeout(function() {
    console.log("=== Gomoku Engine WASM Test ===\n");
    
    // 1. Init game
    console.log("1. Init game...");
    Module._initGame();
    Module._setDifficultyLevel(1);
    console.log("   Status: OK");
    
    // 2. Basic info
    console.log("\n2. Basic info...");
    var size = Module._getBoardSize();
    var player = Module._getCurrentPlayer();
    var moveCount = Module._getMoveCount();
    console.log("   Board size: " + size + "x" + size);
    console.log("   Current player: " + (player === 1 ? "BLACK" : "WHITE"));
    console.log("   Move count: " + moveCount);
    
    // 3. Player move
    console.log("\n3. Player move test...");
    var r1 = Module._playerMove(7, 7);
    console.log("   Move (7,7): result=" + r1);
    var cell = Module._getCell(7, 7);
    console.log("   Verify (7,7): " + (cell === 1 ? "BLACK OK" : "ERROR"));
    
    // 4. AI move
    console.log("\n4. AI move test...");
    console.log("   AI thinking...");
    var r2 = Module._aiMove();
    console.log("   AI result: " + r2);
    
    var aiX = -1, aiY = -1;
    for (var y = 0; y < 15; y++) {
        for (var x = 0; x < 15; x++) {
            if (Module._getCell(x, y) === 2) {
                aiX = x; aiY = y;
                break;
            }
        }
        if (aiY >= 0) break;
    }
    console.log("   AI position: (" + aiX + "," + aiY + ")");
    
    // 5. Game status
    console.log("\n5. Game status...");
    var status = Module._getGameStatus();
    console.log("   Status: " + (status === 0 ? "playing" : status === 1 ? "BLACK wins" : status === 2 ? "WHITE wins" : "draw"));
    
    // 6. Undo
    console.log("\n6. Undo test...");
    var before = Module._getMoveCount();
    Module._undo();
    var after = Module._getMoveCount();
    console.log("   Before: " + before + ", After: " + after);
    
    // 7. AI defense test
    console.log("\n7. AI defense test...");
    Module._initGame();
    Module._setDifficultyLevel(0);
    
    // Black has 4 in a row with the left endpoint already blocked:
    // (5,7)(6,7)(7,7)(8,7), white at (4,7)
    Module._playerMove(5, 7);
    Module._playerMove(4, 7);
    Module._playerMove(6, 7);
    Module._playerMove(5, 6);
    Module._playerMove(7, 7);
    Module._playerMove(6, 6);
    Module._playerMove(8, 7);
    
    console.log("   Black has 4-in-a-row, AI should block (9,7)");
    Module._aiMove();
    
    var blocked = Module._getCell(9, 7) === 2;
    console.log("   AI blocked correctly: " + (blocked ? "YES" : "NO"));
    if (!blocked) process.exitCode = 1;
    
    // 8. Evaluation test
    console.log("\n8. Evaluation test...");
    Module._initGame();
    Module._setDifficultyLevel(1);
    Module._playerMove(7, 7);
    Module._playerMove(6, 7);
    
    var evalScore = Module._evaluateBoard();
    console.log("   Eval score: " + evalScore);
    
    var candCount = Module._getCandidateCount(20);
    console.log("   Candidate count (top 20): " + candCount);
    
    // 9. AI vs AI test
    console.log("\n9. AI vs AI test (10 moves)...");
    Module._initGame();
    Module._setDifficultyLevel(0);
    
    for (var i = 0; i < 10; i++) {
        var s = Module._getGameStatus();
        if (s !== 0) {
            console.log("   Move " + (i+1) + ": game over (" + (s === 1 ? "BLACK" : "WHITE") + " wins)");
            break;
        }
        Module._aiMove();
        var p = Module._getCurrentPlayer();
        console.log("   Move " + (i+1) + ": " + (p === 1 ? "BLACK" : "WHITE") + " to move");
    }
    
    var fs2 = Module._getGameStatus();
    var fc = Module._getMoveCount();
    console.log("   Final: " + (fs2 === 0 ? "playing" : fs2 === 1 ? "BLACK wins" : "WHITE wins") + ", moves: " + fc);
    
    console.log("\n=== Test Complete ===");
}, 1000);
