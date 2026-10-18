// Copyright (c) 2026 The Sea Authors
// SPDX-License-Identifier: BSD-3-Clause

// npm test: runs suite.js inside the installed VS Code (the `code` command),
// with this folder as a development extension and a throwaway profile.
// Needs a display. SEA overrides the sea binary (default: ../../bin/sea).

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ext = path.resolve(__dirname, '..');
const sea = path.resolve(process.env.SEA || path.join(ext, '..', '..', 'bin', 'sea'));
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'sea-vscode-profile-'));
const result = path.join(tmp, 'result.txt');
fs.mkdirSync(path.join(tmp, 'user', 'User'), { recursive: true });
fs.writeFileSync(path.join(tmp, 'user', 'User', 'settings.json'),
    JSON.stringify({ 'sea.executablePath': sea, 'workbench.startupEditor': 'none' }));

const args = [
    '--new-window', '--disable-extensions', '--skip-welcome', '--skip-release-notes',
    '--user-data-dir=' + path.join(tmp, 'user'), '--extensions-dir=' + path.join(tmp, 'ext'),
    '--extensionDevelopmentPath=' + ext, '--extensionTestsPath=' + path.join(__dirname, 'suite.js'),
];
// the Electron binary itself: the `code` script hands the window to a running instance
const electron = process.env.CODE || path.join(path.dirname(fs.realpathSync(cp.execSync('command -v code', { shell: '/bin/sh' }).toString().trim())), '..', 'code');
const r = cp.spawnSync(electron, args, {
    stdio: 'inherit', env: { ...process.env, SEA_VSCODE_RESULT: result }, timeout: 120000,
});
const out = fs.existsSync(result) ? fs.readFileSync(result, 'utf8') : '';
fs.rmSync(tmp, { recursive: true, force: true });
if (out === '') {
    console.error('FAIL: the suite did not finish (exit ' + r.status + ')');
    process.exit(1);
}
process.stdout.write(out + 'PASS\n');
