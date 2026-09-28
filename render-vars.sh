#!/bin/sh
#
# 打包前把源文件里的 ${VAR} 占位符渲染成 debian/variables.env 中的实际取值。
#
# 工作区级权威取值见 omv-plugins 工作区 project_rules.md 的「全局变量定义表」；
# 本文件只保留构建真正需要的最小变量，值必须与其保持一致。
#
# 用法：  sh ./render-vars.sh   （就地改写工作区文件；CI 已在 dpkg-buildpackage 前自动执行）
#
# This file is part of OpenMediaVault openmediavault-pluginmgr packaging.
# @license https://www.gnu.org/licenses/gpl.html GPL Version 3

set -eu

cd "$(dirname "$0")"

ENV_FILE="debian/variables.env"
if [ ! -f "$ENV_FILE" ]; then
	echo "ERROR: $ENV_FILE not found" >&2
	exit 1
fi

# 渲染：只替换 variables.env 里声明过的占位符，避免误伤脚本自身的 ${1}/${VAR} 等。
while IFS= read -r line; do
	case "$line" in
		''|\#*) continue ;;
	esac
	key=${line%%=*}
	value=${line#*=}
	if [ -z "$key" ]; then
		continue
	fi
	files=$(grep -rl --fixed-strings --exclude-dir=.git "\${$key}" . || true)
	if [ -z "$files" ]; then
		echo "  ${key}: no placeholder found (already rendered?)"
		continue
	fi
	for f in $files; do
		sed -i "s|\${$key}|$value|g" "$f"
	done
	echo "  ${key}: rendered in $(echo "$files" | wc -l | tr -d ' ') file(s)"
done < "$ENV_FILE"

# 渲染后不得残留任何已声明的占位符。
while IFS= read -r line; do
	case "$line" in
		''|\#*) continue ;;
	esac
	key=${line%%=*}
	if [ -z "$key" ]; then
		continue
	fi
	if grep -rn --fixed-strings --exclude-dir=.git "\${$key}" .; then
		echo "ERROR: unresolved placeholder \${$key} remains" >&2
		exit 1
	fi
done < "$ENV_FILE"

echo "OK: all build variables rendered."
