import html
import itertools

_FACE = "#FAEEDA"
_TOP = "#FAC775"
_SIDE = "#EF9F27"
_LINE = "#854F0B"
_EDGE = "#633806"

_counter = itertools.count()


# At depth t the band slice is front-top-left -> front-top-right ->
# front-bottom-right, each shifted by t*(33, -33).
def _sheet(t):
    dx, dy = 33 * t, -33 * t
    return (f'<polyline points="{55+dx},{72+dy} {145+dx},{72+dy} {145+dx},{185+dy}" '
            f'fill="none" stroke="{_EDGE}" stroke-width="1" opacity=".78"/>')


def _bands(b):
    if b <= 1:
        return ""
    if b < 10:
        return "".join(_sheet(k / b) for k in range(1, b))
    fronts = [0.06, 0.12, 0.18, 0.24] if b <= 50 else \
             [0.04, 0.07, 0.10, 0.13, 0.16, 0.19]
    sheets = "".join(_sheet(t) for t in fronts) + _sheet(0.88)
    dots = "".join(f'<circle cx="{105+i*8}" cy="50" r="2.1" fill="{_EDGE}"/>'
                   for i in range(3))
    return sheets + dots


def _cube(b, x, y):
    return (
        '<svg width="100%" viewBox="0 0 215 205" '
        f'role="img" aria-label="shortcog image cube, {b} bands">'
        f'<polygon points="55,72 88,39 178,39 145,72" fill="{_TOP}" '
        f'stroke="{_LINE}" stroke-width="1.3"/>'
        f'<polygon points="145,72 178,39 178,152 145,185" fill="{_SIDE}" '
        f'stroke="{_LINE}" stroke-width="1.3"/>'
        f'{_bands(b)}'
        f'<rect x="55" y="72" width="90" height="113" fill="{_FACE}" '
        f'stroke="{_EDGE}" stroke-width="1.5"/>'
        f'<g stroke="{_LINE}" stroke-width="0.5" opacity=".35">'
        '<line x1="85" y1="72" x2="85" y2="185"/>'
        '<line x1="115" y1="72" x2="115" y2="185"/>'
        '<line x1="55" y1="110" x2="145" y2="110"/>'
        '<line x1="55" y1="147" x2="145" y2="147"/></g>'
        f'<text x="100" y="199" text-anchor="middle" font-size="10.5" '
        f'font-family="monospace" fill="currentColor" opacity=".7">X: {x}</text>'
        f'<text x="44" y="128" text-anchor="middle" font-size="10.5" '
        f'font-family="monospace" fill="currentColor" opacity=".7" '
        f'transform="rotate(-90,44,128)">Y: {y}</text>'
        f'<text x="182" y="37" font-size="10.5" font-family="monospace" '
        f'fill="currentColor" opacity=".7">B: {b}</text>'
        '</svg>'
    )


_CSS = """
#ID{font-family:ui-monospace,Menlo,monospace;font-size:13px;color:inherit;
 display:inline-block;line-height:1.5}
#ID .box{display:flex;gap:6px;align-items:center;
 background:rgba(128,128,128,.06);border:1px solid rgba(128,128,128,.25);
 border-radius:8px;padding:12px 16px}
#ID .hdr{margin-bottom:9px}
#ID .cls{opacity:.6}
#ID .dim{font-weight:600}
#ID table{border-collapse:collapse;font-size:12.5px}
#ID td.k{opacity:.6;padding:2px 16px 2px 0}
#ID td.sub{opacity:.45;padding-left:10px}
#ID .g{flex:0 0 auto;width:170px}
"""


def _wrap(inner):
    uid = f"scog{next(_counter)}"
    return (f'<div class="shortcog-repr" id="{uid}"><style>'
            f'{_CSS.replace("#ID", f"#{uid}")}</style>{inner}</div>')


def text(f):
    if not f["ok"]:
        return "<shortcog.Spec (unreadable)>"
    tw, tl = f["tile"]
    return "\n".join([
        f"<shortcog.Spec ({f['b']}, {f['y']}, {f['x']})>",
        f"  dtype      : {f['dtype']}",
        f"  tile       : {tw} x {tl}",
        f"  tiles      : {f['tiles']}",
        f"  tiles/band : {f['across'] * f['down']}",
        f"  codec      : {f['codec']}",
    ])


def html_(f):
    if not f["ok"]:
        return _wrap('<div class="hdr"><span class="cls">shortcog.Spec</span> '
                     '<span class="dim">(unreadable)</span></div>')
    tw, tl = f["tile"]
    e = html.escape
    rows = (
        f'<tr><td class="k">dtype</td><td>{e(f["dtype"])}</td></tr>'
        f'<tr><td class="k">tile</td><td>{tw} \u00d7 {tl}</td></tr>'
        f'<tr><td class="k">tiles</td><td><b>{f["tiles"]:,}</b></td></tr>'
        f'<tr><td class="k">tiles/band</td><td>{f["across"] * f["down"]:,}</td></tr>'
        f'<tr><td class="k">codec</td><td>{e(f["codec"])}</td></tr>'
    )
    meta = (f'<div><div class="hdr"><span class="cls">shortcog.Spec</span> '
            f'<span class="dim">({f["b"]}, {f["y"]}, {f["x"]})</span></div>'
            f'<table>{rows}</table></div>')
    return _wrap(f'<div class="box">{meta}'
                 f'<div class="g">{_cube(f["b"], f["x"], f["y"])}</div></div>')