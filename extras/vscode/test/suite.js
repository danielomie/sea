// Copyright (c) 2026 The Sea Authors
// SPDX-License-Identifier: BSD-3-Clause

// Integration test, run inside VS Code by run.js: opens a Sea file and checks
// that each feature of `sea lsp` reaches the editor.

const vscode = require('vscode');
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const SRC = [
    'local util = require("./util")',
    'local x = 1',
    'print(y)',
    '',
    '-- add(a, b) → the sum',
    'local function add(a, b) return a + b end',
    'print(add(1, 2), util.twice(3))',
    'local   z=add( 1,2 )',
    'print(z)',
    '',
].join('\n');

const UTIL = [
    'local M = {}',
    '',
    '-- util.twice(n) → 2n',
    'function M.twice(n)',
    '    return n * 2',
    'end',
    '',
    'return M',
    '',
].join('\n');

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function until(what, fn, ms = 15000) {
    const end = Date.now() + ms;
    for (;;) {
        const v = await fn();
        if (v) { return v; }
        if (Date.now() > end) { throw new Error('timed out waiting for ' + what); }
        await sleep(100);
    }
}

async function run() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sea-vscode-'));
    const file = path.join(dir, 'main.sea');
    fs.writeFileSync(file, SRC);
    fs.writeFileSync(path.join(dir, 'util.sea'), UTIL);
    const doc = await vscode.workspace.openTextDocument(file);
    await vscode.window.showTextDocument(doc);
    assert.strictEqual(doc.languageId, 'sea');
    const results = [];
    const ok = (name) => { results.push('ok ' + name); };

    // diagnostics: the unused x and the undefined y
    const diags = await until('diagnostics', () => {
        const d = vscode.languages.getDiagnostics(doc.uri);
        return d.length >= 2 ? d : null;
    });
    const msgs = diags.map(d => d.message).join(' | ');
    assert.ok(/'x' is never used/.test(msgs), msgs);
    assert.ok(/'y'/.test(msgs) && diags.some(d => d.severity === vscode.DiagnosticSeverity.Error), msgs);
    ok('diagnostics: ' + msgs);

    // hover over add (line 6, col 7) shows its doc comment
    const hovers = await vscode.commands.executeCommand('vscode.executeHoverProvider', doc.uri, new vscode.Position(6, 7));
    const htext = hovers.map(h => h.contents.map(c => c.value || c).join('')).join('');
    assert.ok(htext.includes('the sum'), htext);
    ok('hover');

    // definition of add → line 5; of util.twice → util.sea line 3
    let defs = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', doc.uri, new vscode.Position(6, 7));
    const d0 = defs[0];
    assert.strictEqual((d0.range || d0.targetRange).start.line, 5);
    defs = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', doc.uri, new vscode.Position(6, 23));
    const d1 = defs[0];
    assert.ok((d1.uri || d1.targetUri).fsPath.endsWith('util.sea'));
    assert.strictEqual((d1.range || d1.targetRange).start.line, 3);
    ok('definition (local and in a required module)');

    // completion after "util." offers twice
    const edit = new vscode.WorkspaceEdit();
    edit.insert(doc.uri, new vscode.Position(9, 0), 'util.');
    await vscode.workspace.applyEdit(edit);
    const list = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', doc.uri, new vscode.Position(9, 5), '.');
    const labels = list.items.map(i => typeof i.label === 'string' ? i.label : i.label.label);
    assert.ok(labels.includes('twice'), labels.slice(0, 20).join(','));
    ok('completion');
    await vscode.commands.executeCommand('undo');

    // document symbols
    const syms = await vscode.commands.executeCommand('vscode.executeDocumentSymbolProvider', doc.uri);
    const names = syms.map(s => s.name);
    assert.ok(names.includes('add'), names.join(','));
    ok('document symbols: ' + names.join(', '));

    // formatting fixes "local   z=add( 1,2 )"
    const edits = await vscode.commands.executeCommand('vscode.executeFormatDocumentProvider', doc.uri, { tabSize: 4, insertSpaces: true });
    assert.ok(edits && edits.length > 0, 'no formatting edits');
    const fixed = new vscode.WorkspaceEdit();
    fixed.set(doc.uri, edits);
    await vscode.workspace.applyEdit(fixed);
    assert.ok(doc.getText().includes('local z = add(1, 2)'), doc.getText());
    ok('formatting');

    fs.writeFileSync(process.env.SEA_VSCODE_RESULT, results.join('\n') + '\n');
}

module.exports = { run };
