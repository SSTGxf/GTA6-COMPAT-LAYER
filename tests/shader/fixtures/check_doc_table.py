"""Fail if the doc's memory table differs from a conformance run, in any respect.

Usage: check_doc_table.py <conformance-log> [doc-path]

The printed table is `== asserted operand tokens per field ==`: one line per field
(field, asserted, word-only, excluded, rows), zero or more indented reason lines
("N excluded: <reason>"), then the `all memory fields` totals line. The doc table
must have exactly the same field set, row order, columns (no doc-only column),
numbers, and reason text; the reason cell is the printed reason lines verbatim,
joined with "<br>" when a field has more than one.
"""
import io, re, sys

log_path = sys.argv[1] if len(sys.argv) > 1 else 'out/shader_tests/conformance_doc_check.log'
doc_path = sys.argv[2] if len(sys.argv) > 2 else 'docs/SHADER-PIPELINE-CONFORMANCE.md'

row_re = re.compile(r'^ {3}(\w[\w ]*?)\s+asserted\s+(\d+)\s+word-only\s+(\d+)\s+excluded\s+(\d+)\s+rows\s+(\d+)\s*$')
tot_re = re.compile(r'^ {3}all memory fields\s+asserted\s+(\d+)\s+word-only\s+(\d+)\s+excluded\s+(\d+)\s*$')
reason_re = re.compile(r'^ {6,}(\d+) excluded: (.*)$')

printed, printed_total, reasons, current = [], None, {}, None
for raw in io.open(log_path, encoding='utf-8', errors='replace'):
    line = raw.rstrip('\r\n')
    if tot_re.match(line):
        g = tot_re.match(line)
        printed_total = (g.group(1), g.group(2), g.group(3))
        current = None
        continue
    m = row_re.match(line)
    if m:
        printed.append((m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)))
        current = m.group(1)
        reasons.setdefault(current, [])
        continue
    r = reason_re.match(line)
    if r and current is not None:
        reasons[current].append(r.group(1) + ' excluded: ' + r.group(2).strip())

doc_lines = io.open(doc_path, encoding='utf-8').read().split('\n')
start = next(i for i, l in enumerate(doc_lines)
             if l.startswith('| field | asserted | word-only | excluded | rows |'))
header = [c.strip() for c in doc_lines[start].strip().strip('|').split('|')]
doc_rows, doc_total, sep_seen = [], None, False
for line in doc_lines[start + 1:]:
    if not line.startswith('|'):
        break
    cells = [c.strip().replace('**', '') for c in line.strip().strip('|').split('|')]
    if not sep_seen and all(set(c) <= set('-') for c in cells):
        sep_seen = True
        continue
    if cells[0] == 'all memory fields':
        doc_total = (cells[1].replace(',', ''), cells[2].replace(',', ''), cells[3].replace(',', ''))
        continue
    doc_rows.append(cells)

problems = []
EXPECTED_HEADER = ['field', 'asserted', 'word-only', 'excluded', 'rows', 'exclusions']
if header != EXPECTED_HEADER:
    problems.append('header/column set differs: doc=%r expected=%r' % (header, EXPECTED_HEADER))
if len(doc_rows) != len(printed):
    problems.append('row count: doc=%d printed=%d' % (len(doc_rows), len(printed)))
for idx, (cells, pr) in enumerate(zip(doc_rows, printed)):
    if len(cells) != len(EXPECTED_HEADER):
        problems.append('row %d (%s) has %d columns, expected %d'
                        % (idx, pr[0], len(cells), len(EXPECTED_HEADER)))
        continue
    got = (cells[0], cells[1].replace(',', ''), cells[2].replace(',', ''), cells[3].replace(',', ''),
           cells[4].replace(',', ''))
    if got != pr:
        problems.append('row %d differs: doc=%r printed=%r' % (idx, got, pr))
    want_reason = '<br>'.join(reasons.get(pr[0], []))
    if cells[5].strip() != want_reason:
        problems.append('row %d (%s) reason differs:\n  doc=%r\n  printed=%r'
                        % (idx, pr[0], cells[5].strip(), want_reason))
if doc_total != printed_total:
    problems.append('totals differ: doc=%r printed=%r' % (doc_total, printed_total))
for p in problems:
    print('DIFF ' + p)
print('doc table vs printed: %s (%d field rows, totals %s)'
      % ('MATCH' if not problems else 'DIFFERS (%d problem(s))' % len(problems), len(printed), printed_total))
sys.exit(0 if not problems else 1)
