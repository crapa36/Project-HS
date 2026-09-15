import {execFileSync} from 'node:child_process';
import {existsSync, mkdirSync, writeFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {resolve, dirname, join} from 'node:path';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const work = join(root,'Build/tooling');
const vs = execFileSync(join(process.env['ProgramFiles(x86)'],'Microsoft Visual Studio/Installer/vswhere.exe'),
  ['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],
  {encoding:'utf8',windowsHide:true}).trim();
const clangd = process.env.HS_CLANGD ?? join(vs,'VC/Tools/Llvm/x64/bin/clangd.exe');
const database = join(root,'Build/msvc-core');
if (!existsSync(clangd) || !existsSync(join(database,'compile_commands.json'))) {
  throw new Error('clangd and the verify-core compile database are required. Run Tools/verify.ps1 first.');
}
mkdirSync(work,{recursive:true});
process.env.LSP_TOOLS_MCP_PROJECT_CONFIG = join(work,'lsp-client.json');
writeFileSync(process.env.LSP_TOOLS_MCP_PROJECT_CONFIG, JSON.stringify({lsp:{clangd:{
  command:[clangd,`--compile-commands-dir=${database}`,'--background-index','--log=error'],extensions:['.c','.cpp','.h','.hpp']
}}}));
process.chdir(root);
const {runMcpStdioServer} = await import('../../Build/tooling/codex-lsp/packages/lsp-tools-mcp/dist/mcp.js');
const {disposeDefaultLspManager} = await import('../../Build/tooling/codex-lsp/packages/lsp-tools-mcp/dist/lsp/manager.js');
try { await runMcpStdioServer(); } finally { await disposeDefaultLspManager(); }
