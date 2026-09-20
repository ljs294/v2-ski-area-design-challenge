import { lstat, readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const root = process.cwd();
const ignoredDirectories = new Set([
  'Binaries',
  'Intermediate',
  'Saved',
  'DerivedDataCache',
  '__pycache__',
  '.git',
  '.idea',
  '.vs',
  '.vscode',
  'coverage',
  'dist',
  'dist-electron',
  'dist-ssr',
  'dist-web',
  'node_modules',
  'playwright-report',
  'release',
  'scratchpad',
  'test-results',
]);
const maxAgentLines = 150;
const maxAgentCharacters = 8_000;
const maxRoutingRows = 20;
const errors = [];

function relative(file) {
  return path.relative(root, file).replaceAll(path.sep, '/');
}

async function filesBelow(directory) {
  const found = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    if (entry.isDirectory() && ignoredDirectories.has(entry.name)) continue;
    const target = path.join(directory, entry.name);
    if (entry.isDirectory()) found.push(...await filesBelow(target));
    else if (entry.isFile()) found.push(target);
  }
  return found;
}

async function exists(target) {
  try {
    await lstat(target);
    return true;
  } catch (error) {
    if (error?.code === 'ENOENT' || error?.code === 'ENOTDIR') return false;
    throw error;
  }
}

function normalizedText(text) {
  return text.replace(/^\uFEFF/, '').replaceAll('\r\n', '\n');
}

function lineCount(text) {
  const normalized = normalizedText(text);
  if (!normalized) return 0;
  return normalized.endsWith('\n')
    ? normalized.slice(0, -1).split('\n').length
    : normalized.split('\n').length;
}

async function checkGuidancePairs(files) {
  const agentFiles = files.filter((file) => path.basename(file) === 'AGENTS.md');
  const claudeFiles = files.filter((file) => path.basename(file) === 'CLAUDE.md');
  const agentDirectories = new Set(agentFiles.map(path.dirname));
  const claudeDirectories = new Set(claudeFiles.map(path.dirname));

  if (!agentDirectories.has(root)) errors.push('Missing root AGENTS.md.');
  if (!claudeDirectories.has(root)) errors.push('Missing root CLAUDE.md.');

  for (const directory of new Set([...agentDirectories, ...claudeDirectories])) {
    const label = relative(directory) || '.';
    if (!agentDirectories.has(directory)) {
      errors.push(`${label}/CLAUDE.md has no paired AGENTS.md.`);
      continue;
    }
    if (!claudeDirectories.has(directory)) {
      errors.push(`${label}/AGENTS.md has no paired CLAUDE.md.`);
      continue;
    }

    const agentFile = path.join(directory, 'AGENTS.md');
    const claudeFile = path.join(directory, 'CLAUDE.md');
    const agentText = normalizedText(await readFile(agentFile, 'utf8'));
    const claudeText = normalizedText(await readFile(claudeFile, 'utf8'));
    const lines = lineCount(agentText);

    if (claudeText !== '@AGENTS.md\n' && claudeText !== '@AGENTS.md') {
      errors.push(`${relative(claudeFile)} must contain exactly @AGENTS.md and an optional final newline.`);
    }
    if (lines > maxAgentLines) {
      errors.push(`${relative(agentFile)} has ${lines} lines; maximum is ${maxAgentLines}.`);
    }
    if (agentText.length > maxAgentCharacters) {
      const estimate = Math.ceil(agentText.length / 4);
      errors.push(`${relative(agentFile)} has ${agentText.length} characters (~${estimate} tokens); maximum proxy is ${maxAgentCharacters} characters (~2,000 tokens).`);
    }
  }
}

async function checkRootRoutingTable() {
  const agentFile = path.join(root, 'AGENTS.md');
  if (!await exists(agentFile)) return;

  const lines = normalizedText(await readFile(agentFile, 'utf8')).split('\n');
  const heading = lines.findIndex((line) => line.trim() === '## Routing');
  if (heading === -1) {
    errors.push('AGENTS.md must contain a ## Routing section.');
    return;
  }

  const nextHeading = lines.findIndex((line, index) => index > heading && /^##\s+/.test(line));
  const section = lines.slice(heading + 1, nextHeading === -1 ? undefined : nextHeading);
  const tableLines = section.map((line) => line.trim()).filter((line) => line.startsWith('|'));
  if (tableLines.length < 2) {
    errors.push('AGENTS.md ## Routing must contain a Markdown table.');
    return;
  }

  const routingRows = tableLines.slice(2).length;
  if (routingRows > maxRoutingRows) {
    errors.push(`AGENTS.md has ${routingRows} routing-table data rows; maximum is ${maxRoutingRows}.`);
  }
}

function markdownTargets(text) {
  const targets = [];
  const linkPattern = /!?\[[^\]]*\]\((<[^>]+>|[^\s)]+)(?:\s+["'][^"']*["'])?\)/g;
  for (const match of text.matchAll(linkPattern)) {
    targets.push(match[1].replace(/^<|>$/g, ''));
  }
  return targets;
}

function isExternal(target) {
  return /^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(target);
}

async function checkLocalLinks(files) {
  const markdownFiles = files.filter((file) => file.toLowerCase().endsWith('.md'));
  for (const file of markdownFiles) {
    const text = await readFile(file, 'utf8');
    for (const target of markdownTargets(text)) {
      if (!target || target.startsWith('#') || isExternal(target)) continue;
      const pathPart = target.split('#', 1)[0].split('?', 1)[0];
      if (!pathPart) continue;

      let decoded;
      try {
        decoded = decodeURIComponent(pathPart);
      } catch {
        errors.push(`${relative(file)} contains a malformed encoded link: ${target}`);
        continue;
      }

      const resolved = path.resolve(path.dirname(file), decoded);
      if (!await exists(resolved)) {
        errors.push(`${relative(file)} links to missing local path: ${target}`);
      }
    }
  }
}

const files = await filesBelow(root);
await checkGuidancePairs(files);
await checkRootRoutingTable();
await checkLocalLinks(files);

if (errors.length > 0) {
  console.error('Agent documentation checks failed:');
  for (const error of errors) console.error(`- ${error}`);
  process.exitCode = 1;
} else {
  const guidanceCount = files.filter((file) => path.basename(file) === 'AGENTS.md').length;
  const markdownCount = files.filter((file) => file.toLowerCase().endsWith('.md')).length;
  console.log(`Agent documentation checks passed (${guidanceCount} guidance pair, ${markdownCount} Markdown files).`);
}
