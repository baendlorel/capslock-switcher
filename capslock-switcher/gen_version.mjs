import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// Resolve against this script instead of the current working directory, so the
// build still works wherever MSBuild or Visual Studio happens to invoke node from.
const here = path.dirname(fileURLToPath(import.meta.url))
const pkg = JSON.parse(fs.readFileSync(path.join(here, '..', 'package.json'), 'utf8'))

// os.EOL keeps the checkout clean: a bare LF used to leave version.h showing as
// modified after every single build.
fs.writeFileSync(path.join(here, 'version.h'), `#define APP_VERSION "${pkg.version}"${os.EOL}`)
