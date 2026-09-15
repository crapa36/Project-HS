import {executeLspTool} from '../../Build/tooling/codex-lsp/packages/lsp-tools-mcp/dist/tools.js';
import {disposeDefaultLspManager} from '../../Build/tooling/codex-lsp/packages/lsp-tools-mcp/dist/lsp/manager.js';
import {readFileSync, writeFileSync} from 'node:fs';
import {resolve} from 'node:path';
process.env.LSP_TOOLS_MCP_PROJECT_CONFIG = resolve('Build/tooling/lsp-client.json');
const filePath = resolve('Source/Gameplay/Private/simulation_combat.cpp');
const lines = readFileSync(filePath,'utf8').split('\n');
const line = lines.findIndex(x=>x.includes('SimulationWorld::'));
const character = lines[line].indexOf('SimulationWorld');
const measurements=[];
try {
  for (const [name,params] of [
    ['lsp.status',{}],
    ['lsp.goto_definition',{filePath,line:line+1,character}],
    ['lsp.goto_definition',{filePath,line:line+1,character}],
    ['lsp.symbols',{filePath,scope:'document'}],
    ['lsp.diagnostics',{filePath,severity:'error'}],
    ['lsp.diagnostics',{filePath:resolve('Build/tooling/lsp-probe.cpp'),severity:'error'}],
  ]) {
    const start=performance.now();
    const result=await executeLspTool(name.replace('lsp.',''),params);
    const record={name,ms:performance.now()-start,result};
    measurements.push(record);
    console.log(JSON.stringify({name,ms:record.ms,result:JSON.stringify(result).slice(0,900)}));
  }
  writeFileSync('Build/tooling/lsp-measurements.json',JSON.stringify(measurements,null,2));
} finally { await disposeDefaultLspManager(); }
