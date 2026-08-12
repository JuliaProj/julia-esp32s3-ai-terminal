#!/usr/bin/env python3
"""Build the Julia outsourced UI asset checklist as an Excel workbook."""

import csv
import datetime as dt
import re
import sys
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "file" / "Julia_UI素材交付清单.csv"
OUTPUT = ROOT / "file" / "Julia_UI外包素材需求清单.xlsx"


def cell_ref(row: int, column: int) -> str:
    letters = ""
    while column:
        column, remainder = divmod(column - 1, 26)
        letters = chr(65 + remainder) + letters
    return f"{letters}{row}"


def cell_xml(row: int, column: int, value, style: int = 0) -> str:
    ref = cell_ref(row, column)
    text = "" if value is None else str(value)
    text = escape(text)
    return f'<c r="{ref}" s="{style}" t="inlineStr"><is><t xml:space="preserve">{text}</t></is></c>'


def sheet_xml(rows, widths, freeze=True, filter_row=None, filter_end=None):
    row_xml = []
    for r_index, row in enumerate(rows, 1):
        cells = []
        for c_index, item in enumerate(row, 1):
            if isinstance(item, tuple):
                value, style = item
            else:
                value, style = item, 0
            cells.append(cell_xml(r_index, c_index, value, style))
        height = ' ht="30" customHeight="1"' if r_index == 1 else ''
        row_xml.append(f'<row r="{r_index}"{height}>' + "".join(cells) + "</row>")

    cols = "".join(
        f'<col min="{index}" max="{index}" width="{width}" customWidth="1"/>'
        for index, width in enumerate(widths, 1)
    )
    view = ('<pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>'
            '<selection pane="bottomLeft" activeCell="A2" sqref="A2"/>') if freeze else '<selection activeCell="A1" sqref="A1"/>'
    auto_filter = ""
    if filter_row and filter_end:
        auto_filter = f'<autoFilter ref="A{filter_row}:{cell_ref(filter_end, len(widths))}"/>'
    dimension = f'A1:{cell_ref(max(1, len(rows)), max(1, len(widths)))}'
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <dimension ref="{dimension}"/>
  <sheetViews><sheetView workbookViewId="0">{view}</sheetView></sheetViews>
  <sheetFormatPr defaultRowHeight="18"/>
  <cols>{cols}</cols>
  <sheetData>{''.join(row_xml)}</sheetData>
  {auto_filter}
  <pageMargins left="0.25" right="0.25" top="0.5" bottom="0.5" header="0.2" footer="0.2"/>
</worksheet>'''


def workbook_xml():
    return '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets>
    <sheet name="素材交付清单" sheetId="1" r:id="rId1"/>
    <sheet name="规格与验收" sheetId="2" r:id="rId2"/>
    <sheet name="数量汇总" sheetId="3" r:id="rId3"/>
  </sheets>
</workbook>'''


STYLES = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <fonts count="3">
    <font><sz val="10"/><name val="Microsoft YaHei"/></font>
    <font><b/><color rgb="FFFFFFFF"/><sz val="10"/><name val="Microsoft YaHei"/></font>
    <font><b/><sz val="12"/><name val="Microsoft YaHei"/></font>
  </fonts>
  <fills count="5">
    <fill><patternFill patternType="none"/></fill>
    <fill><patternFill patternType="gray125"/></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FF2F5597"/><bgColor indexed="64"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFD9EAF7"/><bgColor indexed="64"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFFFE699"/><bgColor indexed="64"/></patternFill></fill>
  </fills>
  <borders count="2">
    <border><left/><right/><top/><bottom/><diagonal/></border>
    <border><left style="thin"><color rgb="FFD9E2F3"/></left><right style="thin"><color rgb="FFD9E2F3"/></right><top style="thin"><color rgb="FFD9E2F3"/></top><bottom style="thin"><color rgb="FFD9E2F3"/></bottom><diagonal/></border>
  </borders>
  <cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
  <cellXfs count="6">
    <xf numFmtId="0" fontId="0" fillId="0" borderId="1" xfId="0"><alignment vertical="center" wrapText="1"/></xf>
    <xf numFmtId="0" fontId="1" fillId="2" borderId="1" xfId="0"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
    <xf numFmtId="0" fontId="2" fillId="3" borderId="1" xfId="0"><alignment vertical="center" wrapText="1"/></xf>
    <xf numFmtId="0" fontId="0" fillId="3" borderId="1" xfId="0"><alignment vertical="center" wrapText="1"/></xf>
    <xf numFmtId="0" fontId="0" fillId="4" borderId="1" xfId="0"><alignment vertical="center" wrapText="1"/></xf>
    <xf numFmtId="0" fontId="2" fillId="0" borderId="1" xfId="0"><alignment vertical="center" wrapText="1"/></xf>
  </cellXfs>
  <cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>
</styleSheet>'''


CONTENT_TYPES = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/worksheets/sheet3.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>'''


ROOT_RELS = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>'''


WORKBOOK_RELS = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet3.xml"/>
  <Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>'''


def main():
    with SOURCE.open("r", encoding="utf-8-sig", newline="") as handle:
        csv_rows = list(csv.reader(handle))
    if not csv_rows:
        raise RuntimeError(f"Empty source: {SOURCE}")

    checklist = [[(value, 1 if row_index == 0 else (4 if value == "必须" else 0))
                  for value in row]
                 for row_index, row in enumerate(csv_rows)]

    specifications = [
        [("Julia UI 外包素材规格与验收", 2), ("内容", 2)],
        [("设备画布", 3), "360 x 360，适配 1.85 寸圆形 LCD"],
        [("源文件", 3), "静态图交付 640 x 640 PNG；动画交付 640 x 640 PNG 序列 + MP4 + 可编辑工程"],
        [("设备动画格式", 3), "研发端转换为 RGB565 RLE .trn；外包不得只交 .trn"],
        [("状态循环基线", 3), "33 帧 / 12 FPS / 2.75 秒，无缝循环"],
        [("状态转换基线", 3), "17 帧 / 12 FPS / 1.42 秒，首尾匹配状态锚点"],
        [("独立动作基线", 3), "28 帧 / 12 FPS / 2.33 秒"],
        [("角色一致性", 3), "脸型、发型、发色、服装、发饰、瞳色、身体比例必须跨状态一致"],
        [("眼睛要求", 3), "统一金色/琥珀色；禁止绿色瞳孔、绿色遮罩、色键残留及矩形修补区域"],
        [("画面质量", 3), "禁止坏点、透明黑边、压缩块、人物清晰度突变、道具闪烁或变形"],
        [("动作完整性", 3), "必须包含看书、喝水/喝茶、伸懒腰、发呆；所有动作须能复位"],
        [("交付验收", 3), "逐帧检查 + 真机烧入 + 完整 showcase 录屏确认"],
        [("交付日期", 3), "由项目负责人和外包方确认"],
        [("验收状态", 3), "未提交 / 待检查 / 需修改 / 已通过"],
    ]

    summary = [
        [("类别", 1), ("必须数量", 1), ("说明", 1)],
        ["静态状态图", "20", "20 个子状态视觉锚点"],
        ["状态循环动画", "21", "20 个状态；S2.1 包含看书与喝水/喝茶两套"],
        ["主状态转换动画", "12", "覆盖 S0-S5 主要转换路径"],
        ["独立待机动作", "4", "必须：伸懒腰、喝水、看书、发呆；建议扩展至 8 段"],
        ["启动/休眠/充电/网络/语音/兜底", "至少 14", "按实际功能分别提供静态图或短循环"],
        ["分层脸部基础素材", "至少 14", "眼睛、瞳孔、嘴型、脸部及发梢等"],
        ["图标与覆盖层", "至少 12 + 4 类动效", "网络、电量、充电、麦克风、音量、设置等"],
        [("最低完整交付量", 5), ("约 83 个视觉成品项", 5), ("不含序列帧、可编辑工程及多分辨率导出", 5)],
    ]

    now = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(timespec="seconds")
    core = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>Julia UI 外包素材需求清单</dc:title><dc:creator>Julia Project</dc:creator><dcterms:created xsi:type="dcterms:W3CDTF">{now}</dcterms:created></cp:coreProperties>'''
    app = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>Julia UI Asset Workbook Builder</Application></Properties>'''

    sheet1 = sheet_xml(checklist, [16, 14, 34, 9, 22, 18, 11, 8, 9, 10, 12, 48], True, 1, len(checklist))
    sheet2 = sheet_xml(specifications, [24, 92], True)
    sheet3 = sheet_xml(summary, [28, 24, 66], True, 1, len(summary))

    with zipfile.ZipFile(OUTPUT, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml", CONTENT_TYPES)
        archive.writestr("_rels/.rels", ROOT_RELS)
        archive.writestr("xl/workbook.xml", workbook_xml())
        archive.writestr("xl/_rels/workbook.xml.rels", WORKBOOK_RELS)
        archive.writestr("xl/styles.xml", STYLES)
        archive.writestr("xl/worksheets/sheet1.xml", sheet1)
        archive.writestr("xl/worksheets/sheet2.xml", sheet2)
        archive.writestr("xl/worksheets/sheet3.xml", sheet3)
        archive.writestr("docProps/core.xml", core)
        archive.writestr("docProps/app.xml", app)

    print(f"created={OUTPUT}")
    print(f"checklist_rows={len(csv_rows) - 1}")
    print("sheets=3")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"error={exc}", file=sys.stderr)
        raise
