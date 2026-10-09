import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// 路径按本脚本所在位置解析，而不是当前工作目录，这样 MSBuild 或
// Visual Studio 从任何地方调用 node 都能正常工作。
const here = path.dirname(fileURLToPath(import.meta.url))
const pkg = JSON.parse(fs.readFileSync(path.join(here, '..', 'package.json'), 'utf8'))

// 用 os.EOL 是为了保持工作区干净：以前写裸 LF 会让 version.h
// 在每次编译后都显示成已修改。
fs.writeFileSync(path.join(here, 'version.h'), `#define APP_VERSION "${pkg.version}"${os.EOL}`)
