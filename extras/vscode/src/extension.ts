// Copyright (c) 2026 The Sea Authors
// SPDX-License-Identifier: BSD-3-Clause

// The Sea extension: starts `sea lsp` (the language server built into the
// sea binary) and connects VS Code to it. Diagnostics, formatting, hover,
// go to definition, completion and document symbols all come from the server;
// this file only finds the binary, starts it and restarts it on demand.

import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let output: vscode.OutputChannel | undefined;

function seaPath(): string {
    const cfg = vscode.workspace.getConfiguration('sea');
    return cfg.get<string>('executablePath') || 'sea';
}

async function start(ctx: vscode.ExtensionContext): Promise<void> {
    const command = seaPath();
    const serverOptions: ServerOptions = {
        run: { command, args: ['lsp'], transport: TransportKind.stdio },
        debug: { command, args: ['lsp'], transport: TransportKind.stdio },
    };
    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'sea' },
            { scheme: 'untitled', language: 'sea' },
        ],
        outputChannel: output,
        // the server reports its own problems on stderr: show them, don't pop up dialogs
        revealOutputChannelOn: 4, // RevealOutputChannelOn.Never
    };
    client = new LanguageClient('sea', 'Sea Language Server', serverOptions, clientOptions);
    try {
        await client.start();
    } catch (err) {
        client = undefined;
        const msg = `Could not start "${command} lsp": ${err instanceof Error ? err.message : err}. ` +
            'Set "sea.executablePath" to the sea binary.';
        const choice = await vscode.window.showErrorMessage(msg, 'Open Settings');
        if (choice === 'Open Settings') {
            vscode.commands.executeCommand('workbench.action.openSettings', 'sea.executablePath');
        }
    }
}

async function stop(): Promise<void> {
    if (client) {
        const c = client;
        client = undefined;
        await c.stop();
    }
}

export async function activate(ctx: vscode.ExtensionContext): Promise<void> {
    output = vscode.window.createOutputChannel('Sea Language Server');
    ctx.subscriptions.push(output);

    ctx.subscriptions.push(
        vscode.commands.registerCommand('sea.restartServer', async () => {
            await stop();
            await start(ctx);
            vscode.window.setStatusBarMessage('Sea language server restarted', 3000);
        }),
        vscode.workspace.onDidChangeConfiguration(async e => {
            if (e.affectsConfiguration('sea.executablePath')) {
                await stop();
                await start(ctx);
            }
        }),
    );

    await start(ctx);
}

export function deactivate(): Thenable<void> | undefined {
    return client ? client.stop() : undefined;
}
