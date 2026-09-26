#!/usr/bin/env python3
"""Export the RGS v1 companion as a paginated PDF.

Requires ReportLab: python -m pip install reportlab
Run from any directory: python tools/export_rgs_spec.py
Render the result for inspection: pdftoppm -r 130 -png output/pdf/rgs-v1-specification.pdf page
"""
from __future__ import annotations

import argparse
import html
from pathlib import Path
import re
from urllib.parse import urljoin

from reportlab.lib import colors
from reportlab.lib.enums import TA_RIGHT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfgen import canvas
from reportlab.platypus import (
    Flowable, PageBreak, Paragraph, Preformatted,
    SimpleDocTemplate, Spacer, Table, TableStyle,
)

ROOT = Path(__file__).resolve().parents[1]
INK = colors.HexColor('#172D3D')
TEAL = colors.HexColor('#087F87')
MUTED = colors.HexColor('#536975')
RULE = colors.HexColor('#D7E1E5')
PALE = colors.HexColor('#F0F5F6')
WHITE = colors.white
WIDTH, HEIGHT = A4
MARGIN = 47
CONTENT_WIDTH = WIDTH - 2 * MARGIN

# Topic boundaries are deliberate: diagrams, tables and arithmetic stay with
# their explanations. New sections require an explicit pagination decision.
PAGES = [
    ['The units of the format'],
    ['A file begins with twelve bytes', 'Each frame starts with its own state'],
    ['Finding the slices', 'Bits inside a slice'],
    ['Extracting codes and padding', 'Turning codes into residuals'],
    ['Predict, reconstruct, update', 'A complete 42-byte example'],
    ['Validation is part of decoding', 'What the encoder may change'],
]
PAGE_NAMES = ['Overview and limits', 'File headers and predictor state',
              'Channel layout and slice bits', 'Codes and dequantization',
              'Reconstruction and worked example',
              'Validation and integration boundaries']


def plain_ascii(text: str) -> str:
    return text.translate(str.maketrans({'\u2010': '-', '\u2011': '-', '\u2012': '-',
        '\u2013': '-', '\u2014': '-', '\u2212': '-', '\u2018': "'", '\u2019': "'",
        '\u201c': '"', '\u201d': '"', '\u00a0': ' ', '\u2026': '...'}))


class Rule(Flowable):
    def __init__(self, width=CONTENT_WIDTH):
        Flowable.__init__(self)
        self.width, self.height = width, 9

    def draw(self):
        self.canv.setStrokeColor(RULE)
        self.canv.setLineWidth(.6)
        self.canv.line(0, 5, self.width, 5)


class NumberedCanvas(canvas.Canvas):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._pages = []

    def showPage(self):
        self._pages.append(dict(self.__dict__))
        self._startPage()

    def save(self):
        count = len(self._pages)
        if count != len(PAGES):
            raise ValueError(f'Expected {len(PAGES)} pages, got {count}; review the page plan before publishing')
        for state in self._pages:
            self.__dict__.update(state)
            self.setStrokeColor(RULE)
            self.setLineWidth(.6)
            self.line(MARGIN, 39, WIDTH - MARGIN, 39)
            self.setFillColor(MUTED)
            self.setFont('Helvetica', 8)
            self.drawString(MARGIN, 26, 'RGS v1  |  Reverse Gravity Signal')
            self.drawRightString(WIDTH - MARGIN, 26, f'{self._pageNumber} / {count}')
            super().showPage()
        super().save()


def styles():
    base = dict(fontName='Helvetica', textColor=INK, fontSize=9.5, leading=12.4,
                spaceAfter=6, allowWidows=0, allowOrphans=0)
    return {
        'body': ParagraphStyle('Body', **base),
        'h2': ParagraphStyle('Section', fontName='Helvetica-Bold', fontSize=13.4,
                             leading=16.5, textColor=INK, spaceBefore=7, spaceAfter=6,
                             keepWithNext=True),
        'cell': ParagraphStyle('Cell', fontName='Helvetica', fontSize=8.7, leading=11.5,
                               textColor=INK, spaceAfter=0),
        'cell_right': ParagraphStyle('CellRight', fontName='Helvetica', fontSize=8.7,
                                     leading=11.5, textColor=INK, alignment=TA_RIGHT),
        'thead': ParagraphStyle('TableHeader', fontName='Helvetica-Bold', fontSize=8.7,
                                leading=11.5, textColor=WHITE),
        'thead_right': ParagraphStyle('TableHeaderRight', fontName='Helvetica-Bold', fontSize=8.7,
                                      leading=11.5, textColor=WHITE, alignment=TA_RIGHT),
        'code': ParagraphStyle('Code', fontName='Courier', fontSize=8.5, leading=10,
                               textColor=INK, spaceAfter=0),
        'list': ParagraphStyle('List', **{**base, 'leftIndent': 13, 'firstLineIndent': -13,
                                          'spaceAfter': 5.7}),
    }


def inline(text, source_url):
    """Convert the small Markdown inline subset used by the specification."""
    saved = []

    def stash(value):
        saved.append(value)
        return f'INLINEPLACEHOLDER{len(saved)-1}END'

    text = plain_ascii(text)
    text = re.sub(r'`([^`]+)`', lambda m: stash('<font name="Courier" size="8.5">' + html.escape(m[1]) + '</font>'), text)
    text = re.sub(r'\[([^]]+)\]\(([^)]+)\)', lambda m: stash(
        '<link href="' + html.escape(urljoin(source_url, m[2]), quote=True) + '" color="#087F87">' +
        html.escape(m[1]) + '</link>'), text)
    text = html.escape(text)
    text = re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', text)
    text = re.sub(r'\*([^*]+)\*', r'<i>\1</i>', text)
    for i, value in enumerate(saved):
        text = text.replace(f'INLINEPLACEHOLDER{i}END', value)
    return text


def table(rows, sty, source_url):
    columns = len(rows[0])
    if columns == 5:
        widths = [CONTENT_WIDTH * .14] + [CONTENT_WIDTH * .215] * 4
    elif columns == 3:
        widths = [CONTENT_WIDTH * .17, CONTENT_WIDTH * .15, CONTENT_WIDTH * .68]
        if rows[0][0] == 'Field':
            widths = [CONTENT_WIDTH * .34, CONTENT_WIDTH * .33, CONTENT_WIDTH * .33]
        elif rows[0][0] == 'High nibble':
            widths = [CONTENT_WIDTH * .17, CONTENT_WIDTH * .24, CONTENT_WIDTH * .59]
    else:
        widths = [CONTENT_WIDTH * .55, CONTENT_WIDTH * .45]
    data = [[Paragraph(inline(cell, source_url), sty['thead_right'] if r == 0 and columns == 5 else
                       sty['thead'] if r == 0 else
                       sty['cell_right'] if columns == 5 else sty['cell'])
             for cell in row] for r, row in enumerate(rows)]
    result = Table(data, colWidths=widths, repeatRows=1, hAlign='LEFT')
    result.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, 0), INK),
        ('ROWBACKGROUNDS', (0, 1), (-1, -1), [WHITE, PALE]),
        ('LINEBELOW', (0, -1), (-1, -1), .5, RULE),
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('LEFTPADDING', (0, 0), (-1, -1), 8),
        ('RIGHTPADDING', (0, 0), (-1, -1), 8),
        ('TOPPADDING', (0, 0), (-1, -1), 3.2),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 3.2),
    ]))
    return [result, Spacer(1, 8)]


def blocks(lines, sty, source_url):
    result = []
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            i += 1
        elif line.startswith('```'):
            code = []
            i += 1
            while i < len(lines) and not lines[i].startswith('```'):
                code.append(plain_ascii(lines[i]))
                i += 1
            i += 1
            box = Table([[Preformatted('\n'.join(code), sty['code'])]], colWidths=[CONTENT_WIDTH], hAlign='LEFT')
            box.setStyle(TableStyle([('BACKGROUND', (0, 0), (-1, -1), PALE),
                ('BOX', (0, 0), (-1, -1), .5, RULE),
                ('LEFTPADDING', (0, 0), (-1, -1), 10),
                ('RIGHTPADDING', (0, 0), (-1, -1), 10),
                ('TOPPADDING', (0, 0), (-1, -1), 6),
                ('BOTTOMPADDING', (0, 0), (-1, -1), 6)]))
            result.extend([box, Spacer(1, 8)])
        elif line.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].strip().startswith('|'):
                row = [cell.strip() for cell in lines[i].strip().strip('|').split('|')]
                if not all(re.fullmatch(r':?-+:?', cell) for cell in row):
                    rows.append(row)
                i += 1
            result.extend(table(rows, sty, source_url))
        elif re.match(r'^(?:- |\d+\. )', line):
            item = line
            i += 1
            while i < len(lines) and lines[i].strip() and not re.match(r'^(?:- |\d+\. )', lines[i].strip()):
                item += ' ' + lines[i].strip()
                i += 1
            result.append(Paragraph(inline(item, source_url), sty['list']))
        else:
            paragraph = line
            i += 1
            while i < len(lines) and lines[i].strip() and not lines[i].startswith(('```', '|', '#')):
                paragraph += ' ' + lines[i].strip()
                i += 1
            result.append(Paragraph(inline(paragraph, source_url), sty['body']))
    return result


def read_sections(markdown):
    sections = {'intro': []}
    current = 'intro'
    for line in markdown.splitlines():
        if line.startswith('# '):
            continue
        if line.startswith('## '):
            current = line[3:].strip()
            if current in sections:
                raise ValueError('Duplicate section: ' + current)
            sections[current] = []
        else:
            sections[current].append(line)
    expected = ({'intro'} | {heading for group in PAGES for heading in group}) - {'Extracting codes and padding'}
    if set(sections) != expected:
        raise ValueError('Update the page plan for changed headings: ' + str(set(sections) ^ expected))
    bits = sections['Bits inside a slice']
    split = next(i for i, line in enumerate(bits) if line.startswith('For code width'))
    sections['Bits inside a slice'] = bits[:split]
    sections['Extracting codes and padding'] = bits[split:]
    return sections


def verify_residual_table(article, normative):
    magnitudes = []
    for line in article.splitlines():
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if len(cells) == 5 and all(re.fullmatch(r'\d+', cell) for cell in cells):
            if int(cells[0]) != len(magnitudes):
                raise ValueError('Dequantizer scale rows are out of order')
            magnitudes.append(list(map(int, cells[1:])))
    reference = [list(map(int, re.findall(r'-?\d+', line))) for line in normative.splitlines()
                 if line.strip().startswith('{') and len(re.findall(r'-?\d+', line)) == 8]
    expanded = [[value for n in row for value in (n, -n)] for row in magnitudes]
    if len(reference) != 16 or expanded != reference:
        raise ValueError('Companion residual table does not match the normative specification')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'docs/posts/rgs-specification.md')
    parser.add_argument('--normative', type=Path, default=ROOT / 'docs/rgs_format.md')
    parser.add_argument('--output', type=Path, default=ROOT / 'output/pdf/rgs-v1-specification.pdf')
    parser.add_argument('--source-url', default='https://github.com/superwendel/rg_audio/blob/main/docs/posts/rgs-specification.md')
    args = parser.parse_args()
    article = args.source.read_text(encoding='utf-8')
    normative = args.normative.read_text(encoding='utf-8')
    verify_residual_table(article, normative)
    sections = read_sections(article)
    sty = styles()
    story = []
    for page, headings in enumerate(PAGES):
        if page:
            story.append(PageBreak())
        eyebrow = ParagraphStyle('Eyebrow', fontName='Helvetica-Bold', fontSize=8,
            leading=11, textColor=TEAL, spaceAfter=7, charSpace=1)
        story.append(Paragraph('RGS / FORMAT VERSION 1', eyebrow))
        if page == 0:
            title = ParagraphStyle('Title', fontName='Helvetica-Bold', fontSize=28,
                leading=31, textColor=INK, spaceAfter=5)
            story.append(Paragraph('RGS v1, byte by byte', title))
            story.append(Paragraph('A complete decoder reference', ParagraphStyle('Subtitle',
                fontName='Helvetica', fontSize=12, leading=16, textColor=MUTED, spaceAfter=9)))
            story.append(Rule())
            story.extend(blocks(sections['intro'], sty, args.source_url))
        else:
            story.append(Paragraph(PAGE_NAMES[page], ParagraphStyle('PageTitle',
                fontName='Helvetica-Bold', fontSize=21, leading=25, textColor=INK, spaceAfter=9)))
            story.append(Rule())
        for heading in headings:
            story.append(Paragraph(plain_ascii(heading), sty['h2']))
            story.extend(blocks(sections[heading], sty, args.source_url))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # SimpleDocTemplate adds six points of frame padding on each side.
    # Compensate so paragraphs, full-width tables, rules and footers align.
    doc = SimpleDocTemplate(str(args.output), pagesize=A4, leftMargin=MARGIN - 6,
        rightMargin=MARGIN - 6, topMargin=36, bottomMargin=53,
        title='RGS v1 - Byte-level decoder reference', author='Reverse Gravity',
        subject='Companion to the normative RGS version 1 format specification',
        keywords='RGS, PCM16, audio, decoder, specification, version 1',
        pageCompression=1)
    doc.build(story, canvasmaker=NumberedCanvas)
    print(args.output)


if __name__ == '__main__':
    main()
