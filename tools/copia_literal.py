"""Generates the literal copies of guest functions used by the self-checking guards of the native code.

Some game functions have a native C++ replacement (see app/src/nfsmw_efecto_pasada_nativo.cpp and
app/src/nfsmw_pegamento_nativo.cpp). Their guards run the native version against a copy of the recompiled original
in which every outgoing call goes through a recording policy, so both can be compared without side effects. Those
copies are code translated from the game's executable, so they are not part of this repository: this tool extracts
them from your own generated code.

Run it after the code generator and after llamadas_directas.py:

    python tools/copia_literal.py app/generated/default app/src/copias_literales
    python tools/copia_literal.py app_usa/generated/default app_usa/src/copias_literales --tabla app_usa/tabla.tsv

The addresses below are those of the PAL Spanish executable. For another edition, --tabla takes the address table
used by tools/editions/crear_arbol.py and translates them.

It writes one .inc file per copy. It fails if a function is missing or if a call it has to rewrite is not found the
expected number of times, which means the generated code does not match the one the native code was written for.
"""
import os
import re
import sys

# name of the copy, guest function, and how each outgoing call is rewritten (calls are matched with or without the
# __imp__ prefix that llamadas_directas.py adds)
COPIES = [
    ('CopiaOriginal', 'sub_82448E80', {
        'sub_8259B9B8': 'NFSMW_PASADA_LLAMAR(ctx, base, kLiberar);',
        'sub_8259C150': 'NFSMW_PASADA_LLAMAR(ctx, base, kSombreadorVertices);',
        'sub_8259BDC0': 'NFSMW_PASADA_LLAMAR(ctx, base, kSombreadorPixeles);',
    }, 'NFSMW_PASADA_INDIRECTA'),
    ('CopiaFlujos', 'sub_82452690', {
        'sub_8258D968': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kFlujo);',
        'sub_8258DA60': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kIndices);',
    }, None),
    ('CopiaDibujo', 'sub_8244ED58', {
        'sub_826992F0': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kEfecto);',
        'sub_82593C50': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kDibujar);',
        'sub_8244EDF8': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kSegundo);',
    }, 'NFSMW_PEGAMENTO_INDIRECTA'),
    ('CopiaPegamento', 'sub_82452730', {
        'sub_8244EA48': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kCambioEstado);',
        'sub_82453E20': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kVistaE20);',
        'sub_82453D60': 'NFSMW_PEGAMENTO_LLAMAR(ctx, base, kVistaD60);',
        'sub_82452690': 'NFSMW_PEGAMENTO_FLUJOS(ctx, base);',
        'sub_8244ED58': 'NFSMW_PEGAMENTO_DIBUJO(ctx, base);',
    }, None),
    ('CopiaBucle', 'sub_82454B50', {
        'sub_82452730': 'NFSMW_BUCLE_LLAMAR(ctx, base);',
    }, None),
]

# number of indirect calls rewritten in each copy
INDIRECT = {'CopiaOriginal': 2, 'CopiaDibujo': 1}


def find_functions(generated):
    functions = {}
    for name in sorted(os.listdir(generated)):
        if not name.endswith('.cpp'):
            continue
        text = open(os.path.join(generated, name), encoding='utf-8').read()
        for m in re.finditer(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\) \{\n(.*?)\n\}\n', text, re.M | re.S):
            functions[m.group(1)] = m.group(2)
    return functions


def build_copy(body, calls, indirect_macro, name):
    lines = []
    seen = {target: 0 for target in calls}
    indirect = 0
    for line in body.split('\n'):
        line = re.sub(r'\s*//.*$', '', line).rstrip()
        if not line.strip():
            continue
        indent = line[:len(line) - len(line.lstrip())]
        m = re.fullmatch(r'(?:__imp__)?(sub_[0-9A-F]+)\(ctx, base\);', line.strip())
        if m and m.group(1) in calls:
            seen[m.group(1)] += 1
            line = indent + calls[m.group(1)]
        else:
            m = re.fullmatch(r'REX_CALL_INDIRECT_FUNC\((.+)\);', line.strip())
            if m and indirect_macro:
                indirect += 1
                line = f'{indent}{indirect_macro}(ctx, base, {m.group(1)});'
        lines.append(line)
    missing = [target for target, count in seen.items() if count != 1]
    if missing or indirect != INDIRECT.get(name, 0):
        raise SystemExit(f'{name}: the generated code does not match (calls {missing}, indirect {indirect})')
    return ('template <class Llamadas>\n'
            f'[[gnu::noinline]] void {name}(PPCContext& __restrict ctx, uint8_t* base) {{\n'
            + '\n'.join(lines) + '\n}\n')


def load_table(path):
    """PAL address -> address in the other edition, from the table of tools/editions (tab separated, one header
    line: pal, section, other, state; '-' when there is no match)."""
    table = {}
    for line in open(path, encoding='utf-8').read().splitlines()[1:]:
        pal, _, other, _ = line.split('\t')
        if other != '-':
            table[int(pal, 16)] = int(other, 16)
    return table


def translate(name, table):
    if table is None:
        return name
    address = int(name[4:], 16)
    if address not in table:
        raise SystemExit(f'{name} is not in the address table')
    return f'sub_{table[address]:08X}'


def main():
    args = sys.argv[1:]
    table = None
    if '--tabla' in args:
        i = args.index('--tabla')
        table = load_table(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    generated, output = args
    functions = find_functions(generated)
    os.makedirs(output, exist_ok=True)
    for name, function, calls, indirect_macro in COPIES:
        function = translate(function, table)
        calls = {translate(target, table): text for target, text in calls.items()}
        if function not in functions:
            raise SystemExit(f'{function} not found in {generated}')
        text = build_copy(functions[function], calls, indirect_macro, name)
        with open(os.path.join(output, f'{name}.inc'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('// Generated by tools/copia_literal.py from the recompiled game code. Do not edit.\n' + text)
        print(f'{name}.inc <- {function}')


if __name__ == '__main__':
    main()
