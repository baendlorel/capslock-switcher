import fs from 'node:fs'
const content = fs.readFileSync('../package.json', 'utf8');
const pkg = JSON.parse(content);
fs.writeFileSync('version.h', `#define APP_VERSION "${pkg.version}"\n`);