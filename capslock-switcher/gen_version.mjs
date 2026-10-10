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
// 版本号两份：字符串给 C++/资源里的文案，4 段数字给 VERSIONINFO 的 FILEVERSION。
const numbers = pkg.version.split('.').map((part) => parseInt(part, 10) || 0)
while (numbers.length < 4) {
  numbers.push(0)
}

fs.writeFileSync(
  path.join(here, 'version.h'),
  `#define APP_VERSION "${pkg.version}"${os.EOL}` +
    `#define APP_VERSION_NUM ${numbers.slice(0, 4).join(',')}${os.EOL}`,
)
