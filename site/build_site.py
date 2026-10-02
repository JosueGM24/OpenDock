"""Genera el sitio estático de OpenDock en docs/ (GitHub Pages) a partir de site/page.html.

    py site/build_site.py

- docs/index.html            la página, con CSS y JS en archivos aparte
- docs/assets/site.css, site.js
- docs/assets/logo.png, favicon.png, apple-touch-icon.png, og.png
- docs/assets/sounds/*.wav   los sonidos de la demo (de sounds/, a 24 kHz)
- docs/gracias.html          destino de los formularios sin JavaScript
Se publica en Netlify (Netlify Forms) con .github/workflows/web.yml.
Requiere Pillow. REPO y SITE son las únicas direcciones del sitio.
"""
import os, io, wave, audioop, re
from PIL import Image, ImageDraw, ImageFont

REPO = 'https://github.com/JosueGM24/OpenDock'
SITE = 'https://opendock.netlify.app/'   # cámbialo si tu sitio de Netlify tiene otro nombre o dominio
DOWNLOAD = REPO + '/releases/latest/download/OpenDock.exe'

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
out = os.path.join(root, 'docs')
assets = os.path.join(out, 'assets')
os.makedirs(os.path.join(assets, 'sounds'), exist_ok=True)

page = open(os.path.join(here, 'page.html'), encoding='utf-8').read()

# CSS y JS a archivos
css = re.search(r'<style>\n(.*?)</style>', page, re.S).group(1)
js = re.search(r'<script>\n(.*?)</script>', page, re.S).group(1)
body = page[page.index('<div class="wrap">'):page.index('<script>')]

# sonidos: archivos en vez de base64
SND = {'simple2': 'eco-suave', 'simple1': 'nota', 'deco1': 'destello', 'ambient': 'ambiente', 'celeb1': 'logro'}
for src, name in SND.items():
    w = wave.open(os.path.join(root, 'sounds', src + '.wav'))
    raw, _ = audioop.ratecv(w.readframes(w.getnframes()), 2, 1, w.getframerate(), 24000, None)
    o = wave.open(os.path.join(assets, 'sounds', name + '.wav'), 'wb')
    o.setnchannels(1); o.setsampwidth(2); o.setframerate(24000); o.writeframes(raw); o.close()
    js = js.replace(f"d: '__SND_{src}__'", f"d: 'assets/sounds/{name}.wav'")
assert '__SND' not in js
js = js.replace("    const bin = atob(s.d), bytes = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);\n    return await c.decodeAudioData(bytes.buffer);",
                "    const r = await fetch(s.d); return await c.decodeAudioData(await r.arrayBuffer());")
assert 'atob' not in js

# imágenes
logo = Image.open(os.path.join(root, 'icon_512.png')).convert('RGBA')
logo.resize((160, 160), Image.LANCZOS).save(os.path.join(assets, 'logo.png'), optimize=True)
logo.resize((64, 64), Image.LANCZOS).save(os.path.join(assets, 'favicon.png'), optimize=True)
bg = Image.new('RGBA', (180, 180), (0, 0, 0, 255)); bg.alpha_composite(logo.resize((150, 150), Image.LANCZOS), (15, 15))
bg.convert('RGB').save(os.path.join(assets, 'apple-touch-icon.png'), optimize=True)
og = Image.new('RGB', (1200, 630), (0, 0, 0)); d = ImageDraw.Draw(og)
for y in range(630):   # brillo frío arriba a la derecha, como la página
    for x in range(0, 1200, 4):
        k = max(0.0, 1 - ((x - 980) ** 2 + (y - 80) ** 2) ** .5 / 700) * .35
        if k > 0: d.line([(x, y), (x + 3, y)], fill=(int(40 * k), int(70 * k), int(160 * k)))
og.paste(logo.resize((150, 150), Image.LANCZOS), (90, 90), logo.resize((150, 150), Image.LANCZOS))
def font(names, size):
    for n in names:
        try: return ImageFont.truetype(n, size)
        except OSError: pass
    return ImageFont.load_default()
big = font(['segoeuib.ttf', 'arialbd.ttf'], 92); mid = font(['segoeui.ttf', 'arial.ttf'], 36)
d.text((90, 300), 'OpenDock', font=big, fill=(238, 241, 246))
d.text((90, 420), 'Notch, barra superior y dock para Windows.', font=mid, fill=(170, 177, 190))
d.text((90, 470), 'Código abierto · sin red · menos de 15 MB de memoria', font=mid, fill=(120, 128, 142))
og.save(os.path.join(assets, 'og.png'), optimize=True)

# enlaces reales
body = body.replace('src="__LOGO__"', 'src="assets/logo.png"')
body = body.replace('<a class="btn btn-primary" href="#instalar">Instalar OpenDock</a>',
                    f'<a class="btn btn-primary" href="{DOWNLOAD}">Descargar para Windows</a>')
body = body.replace('<a class="btn btn-ghost" href="#codigo">Ver el código</a>',
                    f'<a class="btn btn-ghost" href="{REPO}">Ver en GitHub</a>')
body = body.replace('<a href="#codigo">Código</a>', f'<a href="{REPO}">GitHub</a>')
body = body.replace('<li><b>Abre OpenDock.exe</b>', f'<li><b><a href="{DOWNLOAD}">Descarga OpenDock.exe</a> y ábrelo</b>')
body = body.replace('<span class="meta">Hecho en C, sin frameworks.</span>',
                    f'<span class="meta"><a href="{REPO}">GitHub</a> · <a href="{REPO}/releases">Versiones</a> · <a href="{REPO}/blob/main/LICENSE">Licencia MIT</a></span>')
assert '__LOGO__' not in body and DOWNLOAD in body

fonts = page[page.index('<link rel="preconnect"'):page.index('<style>')].strip()
desc = 'OpenDock pone tus notificaciones en un notch y añade una barra superior con centro de control y un dock a Windows 10 y 11. Código abierto, sin red, menos de 15 MB de memoria.'
html = f'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>OpenDock — notch, barra superior y dock para Windows</title>
<meta name="description" content="{desc}">
<meta name="theme-color" content="#000000">
<link rel="canonical" href="{SITE}">
<link rel="icon" type="image/png" href="assets/favicon.png">
<link rel="apple-touch-icon" href="assets/apple-touch-icon.png">
<meta property="og:type" content="website">
<meta property="og:title" content="OpenDock">
<meta property="og:description" content="{desc}">
<meta property="og:url" content="{SITE}">
<meta property="og:image" content="{SITE}assets/og.png">
<meta name="twitter:card" content="summary_large_image">
{fonts}
<link rel="stylesheet" href="assets/site.css">
</head>
<body>
{body.rstrip()}
<script src="assets/site.js" defer></script>
</body>
</html>
'''
# el reset que antes ponía el visor de artifacts
css = ':root { padding-top: env(safe-area-inset-top, 0px); padding-bottom: env(safe-area-inset-bottom, 0px); }\nbody { margin: 0; }\nimg { max-width: 100%; }\n[hidden] { display: none !important; }\n' + css
open(os.path.join(out, 'index.html'), 'w', encoding='utf-8', newline='\n').write(html)
open(os.path.join(assets, 'site.css'), 'w', encoding='utf-8', newline='\n').write(css)
open(os.path.join(assets, 'site.js'), 'w', encoding='utf-8', newline='\n').write(js)
gracias = f'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gracias — OpenDock</title>
<meta name="robots" content="noindex">
<link rel="icon" type="image/png" href="assets/favicon.png">
{fonts}
<link rel="stylesheet" href="assets/site.css">
</head>
<body>
<div class="wrap" style="min-height:100vh;display:grid;place-content:center;gap:18px;text-align:center;justify-items:center">
  <img src="assets/logo.png" alt="" width="72" height="72">
  <h2>Gracias, ya me llegó tu mensaje</h2>
  <p style="color:#B3B9C5;max-width:44ch;margin:0">Lo reviso en los próximos días. Si dejaste tu correo, te escribo en cuanto tenga novedades.</p>
  <a class="btn btn-primary" href="./">Volver a OpenDock</a>
</div>
</body>
</html>
'''
open(os.path.join(out, 'gracias.html'), 'w', encoding='utf-8', newline='\n').write(gracias)
nj = os.path.join(out, '.nojekyll')
if os.path.exists(nj): os.remove(nj)
total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(out) for f in fs)
print('docs/ listo,', total // 1024, 'KB')
