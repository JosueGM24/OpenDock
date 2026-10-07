"""Genera el sitio estático de OpenDock en docs/ (GitHub Pages) a partir de site/page.html.

    py site/build_site.py

- docs/index.html            la página, con CSS y JS en archivos aparte
- docs/assets/site.css, site.js
- docs/assets/logo.png, favicon.png, apple-touch-icon.png, og.png
- docs/assets/sounds/*.wav   los sonidos de la demo (de sounds/, a 24 kHz)
- docs/gracias.html          destino de los formularios sin JavaScript
- docs/privacidad.html       política de privacidad (la pide SignPath Foundation)
- docs/firma.html            política de firma de código (la pide SignPath Foundation)
- docs/install.sh            el instalador de Linux (curl -fsSL .../install.sh | sh), de linux/install.sh
Se publica en Netlify (Netlify Forms) conectando el repo; ver netlify.toml.
Las imágenes se generan con Pillow; sin Pillow, o en Netlify (sin las fuentes Segoe), se usan
las de docs/assets que ya están en el repo. No necesita audioop (Python 3.13 lo quitó).
REPO y SITE son las únicas direcciones del sitio.
"""
import os, sys, io, wave, re, array
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    Image = None
# Netlify compila en Linux sin las fuentes de la imagen social: ahí valen las del repo
IMAGES = Image is not None and not os.environ.get('NETLIFY')


def resample(raw, src, dst):
    """PCM mono de 16 bits de src a dst Hz, con interpolación lineal (sustituye a audioop.ratecv)."""
    a = array.array('h'); a.frombytes(raw)
    if sys.byteorder == 'big': a.byteswap()
    n = max(1, int(len(a) * dst / src))
    out = array.array('h', bytes(2 * n))
    for i in range(n):
        x = i * src / dst; j = int(x); f = x - j
        v0 = a[min(j, len(a) - 1)]; v1 = a[min(j + 1, len(a) - 1)]
        out[i] = int(round(v0 + (v1 - v0) * f))
    if sys.byteorder == 'big': out.byteswap()
    return out.tobytes()

REPO = 'https://github.com/JosueGM24/OpenDock'
SITE = 'https://open-dock.netlify.app/'   # cámbialo si usas otro dominio
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
    raw = resample(w.readframes(w.getnframes()), w.getframerate(), 24000)
    o = wave.open(os.path.join(assets, 'sounds', name + '.wav'), 'wb')
    o.setnchannels(1); o.setsampwidth(2); o.setframerate(24000); o.writeframes(raw); o.close()
    js = js.replace(f"d: '__SND_{src}__'", f"d: 'assets/sounds/{name}.wav'")
assert '__SND' not in js
js = js.replace("    const bin = atob(s.d), bytes = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);\n    return await c.decodeAudioData(bytes.buffer);",
                "    const r = await fetch(s.d); return await c.decodeAudioData(await r.arrayBuffer());")
assert 'atob' not in js

# imágenes
if IMAGES:
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
    d.text((90, 420), 'Notch, barra superior y dock para Windows y Linux.', font=mid, fill=(170, 177, 190))
    d.text((90, 470), 'Código abierto · sin telemetría · menos de 15 MB de memoria', font=mid, fill=(120, 128, 142))
    og.save(os.path.join(assets, 'og.png'), optimize=True)
else:
    for f in ('logo.png', 'favicon.png', 'apple-touch-icon.png', 'og.png'):
        assert os.path.exists(os.path.join(assets, f)), f'falta docs/assets/{f} (genéralo con Pillow)'

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
desc = 'OpenDock pone tus notificaciones en un notch y añade una barra superior con centro de control y un dock a Windows 10 y 11 y a Linux. Código abierto, sin telemetría, menos de 15 MB de memoria.'
html = f'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>OpenDock — notch, barra superior y dock para Windows y Linux</title>
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
<script>document.documentElement.classList.add('js')</script>
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
# versión por contenido: si cambia el CSS o el JS cambia su URL, y ningún navegador mezcla
# una página nueva con un archivo viejo de su caché
import hashlib
ver = lambda t: hashlib.sha256(t.encode('utf-8')).hexdigest()[:10]
CSS_URL, JS_URL = f'assets/site.css?v={ver(css)}', f'assets/site.js?v={ver(js)}'
html = html.replace('href="assets/site.css"', f'href="{CSS_URL}"').replace('src="assets/site.js"', f'src="{JS_URL}"')
open(os.path.join(out, 'index.html'), 'w', encoding='utf-8', newline='\n').write(html)
open(os.path.join(assets, 'site.css'), 'w', encoding='utf-8', newline='\n').write(css)
open(os.path.join(assets, 'site.js'), 'w', encoding='utf-8', newline='\n').write(js)
# el instalador de Linux, siempre con LF (un CR rompe el script en sh)
inst = open(os.path.join(root, 'linux', 'install.sh'), encoding='utf-8').read()
assert inst.startswith('#!/bin/sh') and inst.rstrip().endswith('main "$@"')
open(os.path.join(out, 'install.sh'), 'w', encoding='utf-8', newline='\n').write(inst)
gracias = f'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gracias — OpenDock</title>
<meta name="robots" content="noindex">
<link rel="icon" type="image/png" href="assets/favicon.png">
{fonts}
<link rel="stylesheet" href="{CSS_URL}">
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

def doc_page(name, title, body):
    """Página de texto (privacidad, firma) con el aspecto del sitio."""
    html = f'''<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} — OpenDock</title>
<link rel="canonical" href="{SITE}{name}">
<link rel="icon" type="image/png" href="assets/favicon.png">
{fonts}
<link rel="stylesheet" href="{CSS_URL}">
<style>
.doc {{ max-width: 760px; margin: 0 auto; padding-block: 56px 72px; display: grid; gap: 14px; }}
.doc h1 {{ font: 700 clamp(30px, 5vw, 44px)/1.1 var(--display); letter-spacing: -.02em; margin: 18px 0 6px; }}
.doc h2 {{ font: 700 20px/1.3 var(--display); margin: 22px 0 0; }}
.doc p, .doc li {{ color: #C9CED8; font-size: 16px; line-height: 1.6; }}
.doc p {{ margin: 0; }}
.doc ul {{ margin: 0; padding-left: 20px; display: grid; gap: 6px; }}
.doc a {{ color: var(--ink); }}
.doc .meta {{ color: var(--mute); font-size: 13px; }}
.doc blockquote {{ margin: 0; padding: 14px 18px; border-left: 3px solid #3A414D; background: #0E1116; border-radius: 0 12px 12px 0; color: var(--ink); }}
</style>
</head>
<body>
<main class="wrap doc">
  <a href="./" style="display:inline-flex;align-items:center;gap:10px;text-decoration:none;color:var(--ink);font-weight:700"><img src="assets/logo.png" alt="" width="32" height="32">OpenDock</a>
{body.rstrip()}
  <p class="meta">Última actualización: 7 de octubre de 2026 · <a href="./">Volver a OpenDock</a></p>
</main>
</body>
</html>
'''
    open(os.path.join(out, name), 'w', encoding='utf-8', newline='\n').write(html)

doc_page('privacidad.html', 'Privacidad', f'''
  <h1>Política de privacidad</h1>
  <p>OpenDock es software libre que se ejecuta solo en tu equipo. Esta página explica qué datos trata el programa y qué datos trata este sitio web.</p>
  <h2>El programa (Windows y Linux)</h2>
  <ul>
    <li><b>No recoge ni envía datos.</b> OpenDock no tiene cuenta, ni telemetría, ni estadísticas de uso.</li>
    <li><b>Actualizaciones (Windows), solo si las activas.</b> Con "Buscar actualizaciones" encendido (lo propone al instalar y se cambia en Configuración), una vez al día pide a la API pública de GitHub cuál es la última versión, identificándose solo como <code>OpenDock/&lt;versión&gt;</code>; también lo hace si eliges "Buscar actualizaciones" en el menú de la bandeja. Al pulsar "Actualizar" descarga el .exe de esa versión desde GitHub y comprueba su SHA-256 antes de usarlo. GitHub ve esas peticiones como las de cualquier navegador (ver su política más abajo). Sin activarlo, OpenDock no se conecta a nada.</li>
    <li><b>Notificaciones.</b> Para mostrarlas en el notch, en Windows lee en solo lectura la base local de notificaciones de Windows, y en Linux las recibe como servidor de notificaciones del escritorio. No se copian ni se suben a ningún sitio.</li>
    <li><b>Ajustes.</b> Se guardan en tu equipo: en Windows en el registro de tu usuario (HKCU), en Linux en <code>~/.config/opendock</code>. Al desinstalar puedes borrarlos.</li>
    <li>Puedes comprobar todo lo anterior en el <a href="{REPO}">código fuente</a>.</li>
  </ul>
  <h2>Este sitio web</h2>
  <ul>
    <li><b>Alojamiento.</b> El sitio se sirve desde Netlify, que como cualquier servidor web procesa tu dirección IP para entregarte las páginas (ver la <a href="https://www.netlify.com/privacy/" rel="noopener">privacidad de Netlify</a>).</li>
    <li><b>Lista de versiones.</b> Tu navegador pide a la API pública de GitHub la lista de versiones, así que GitHub ve esa petición (ver la <a href="https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement" rel="noopener">privacidad de GitHub</a>). Las descargas también vienen de GitHub.</li>
    <li><b>Formularios.</b> Si reportas un error o sugieres una función, lo que escribes (y tu nombre, correo o imagen si los añades, todos opcionales) se guarda en Netlify Forms y solo se usa para revisar tu mensaje y responderte. Pide que lo borre escribiendo por el mismo formulario.</li>
    <li>El sitio no usa cookies de seguimiento, ni analítica, ni publicidad.</li>
  </ul>
  <h2>Contacto</h2>
  <p>Daniel Godínez, mantenedor de OpenDock: usa el formulario de la <a href="./#opinion">página principal</a> o abre un tema en <a href="{REPO}/issues">GitHub</a>.</p>
''')

doc_page('firma.html', 'Política de firma de código', f'''
  <h1>Política de firma de código</h1>
  <blockquote>Free code signing provided by <a href="https://about.signpath.io/" rel="noopener">SignPath.io</a>, certificate by <a href="https://signpath.org/" rel="noopener">SignPath Foundation</a>.</blockquote>
  <p class="meta">Solicitud en trámite: hasta que SignPath Foundation la apruebe, las versiones se publican sin firmar y con su SHA-256.</p>
  <h2>Qué se firma</h2>
  <ul>
    <li>Solo <code>OpenDock.exe</code>, el programa para Windows de cada versión publicada en <a href="{REPO}/releases">GitHub Releases</a>.</li>
    <li>Se compila en GitHub Actions (<a href="{REPO}/blob/main/.github/workflows/release.yml">release.yml</a>) a partir de una etiqueta <code>v*</code> de este repositorio, con MSVC y protecciones comprobadas en cada compilación. Nada compilado en un equipo personal se firma.</li>
    <li>Cada versión publica también la huella SHA-256 del archivo.</li>
  </ul>
  <h2>Equipo</h2>
  <ul>
    <li><b>Autores y revisores:</b> <a href="{REPO}/graphs/contributors">colaboradores del repositorio</a>.</li>
    <li><b>Aprobación de cada firma:</b> <a href="https://github.com/JosueGM24">Daniel Godínez (JosueGM24)</a>, mantenedor.</li>
  </ul>
  <h2>Privacidad</h2>
  <p>This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it. Ver la <a href="privacidad.html">política de privacidad</a>.</p>
''')
nj = os.path.join(out, '.nojekyll')
if os.path.exists(nj): os.remove(nj)
total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(out) for f in fs)
print('docs/ listo,', total // 1024, 'KB')
