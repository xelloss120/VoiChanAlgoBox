# 画面の説明文（ツールチップ）が 3 行に収まっているかを数える。3 行を超えるものを出す（0 件なら問題なし）
#   python tooltip_lines.py
# 1 行は全角約 27 字（ツールチップの幅 460px、17px の文字）。選択欄は「全体の説明＋選んでいる項目の説明」を合わせて数える
import glob, os, re, sys, unicodedata
root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'Source'))
PER_LINE = 27.0  # ツールチップの 1 行（幅 460px、17px の文字で全角約 27 字）

def width(t):
    return sum(1 if unicodedata.east_asian_width(c) in 'WFA' else 0.55 for c in t)

def lines(t):
    # 改行で区切り、それぞれを 1 行の幅で折り返したときの行数
    return sum(max(1, -(-width(part) // PER_LINE)) for part in t.split('\\n'))

out = []
# 1) 文字列そのもの（Params.cpp と Plugin の .cpp）。隣り合う文字列の連結もまとめる
files = [os.path.join(root, 'Core', 'Params.cpp'), os.path.join(root, 'Core', 'Choices.cpp')] + glob.glob(os.path.join(root, 'Plugin', '*.cpp'))
for path in files:
    s = open(path, encoding='utf-8').read()
    for m in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+', s):
        t = ''.join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(0)))
        if any(ord(c) > 0x3000 for c in t) and lines(t) > 3:
            out.append((os.path.basename(path), s.count('\n', 0, m.start()) + 1, lines(t), t[:40]))

# 2) 選択欄: 全体の説明 + 改行 + 「選択肢の名前」説明
s = open(os.path.join(root, 'Core', 'Params.cpp'), encoding='utf-8').read()
desc = dict(re.findall(r'case P::(\w+):\s+return "((?:[^"\\]|\\.)*)";', s))
spec_choices = {}
for m in re.finditer(r'\{ P::(\w+),\s+"\w+",\s+"[^"]*",\s+T::choice,[^{]*\{([^}]*)\}', s):
    spec_choices[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
cd = s[s.index('const char* choiceDescription'):]
cd = cd[:cd.index('\n}')]
for m in re.finditer(r'((?:case P::\w+:\s*)+)return pick \(\{(.*?)\}\);', cd, re.S):
    ids = re.findall(r'P::(\w+)', m.group(1))
    items = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
    for pid in ids:
        base = desc.get(pid, '')
        names = spec_choices.get(pid, [])
        for i, t in enumerate(items):
            name = names[i] if i < len(names) else '?'
            total = lines(base) + lines('「' + name + '」' + t)
            if total > 3:
                out.append(('choice', pid, total, name))
for o in out:
    print(*o, sep='\t')
print(f'3 行を超えるもの: {len(out)} 件')
