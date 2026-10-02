# Genera app.ico: el logo de OpenDock. Un cuadrado (esquinas apenas suavizadas, como los
# iconos de Windows 11) con el notch negro colgando del borde superior, con sus hombros
# curvos como uno real. Los tamaños pequeños llevan un notch más grande para leerse a 16 px.
# Requiere Pillow y numpy.
from PIL import Image
import numpy as np

def sd_rrect(x, y, cx, cy, hw, hh, r):
    qx = np.abs(x - cx) - (hw - r); qy = np.abs(y - cy) - (hh - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r

def cov(d, aa):
    return np.clip(0.5 - d / aa, 0, 1)

def render(n):
    ss = 8
    N = n * ss
    y, x = (np.mgrid[0:N, 0:N] + 0.5) / N
    aa = 1.5 / N
    small = n < 48
    # el azulejo
    tile = cov(sd_rrect(x, y, .5, .5, .47, .47, .11 if not small else .09), aa)
    top = .03
    # fondo: degradado del acento, más claro arriba a la izquierda
    t = np.clip((x * .45 + y * .75) / 1.2, 0, 1)[..., None]
    c0, c1 = np.array([150, 190, 255]), np.array([84, 100, 240])
    col = c0 * (1 - t) + c1 * t
    # brillo suave arriba
    col = col + (255 - col) * (np.clip(.35 - y, 0, .35) * .5)[..., None]
    # notch: cuerpo colgado del borde superior + hombros cóncavos
    w, h, r, s = (.5, .27, .1, .0) if small else (.40, .2, .1, .09)
    cx = .5
    hh = (h + .3) * .5                                   # sobresale por arriba: solo se ven las esquinas de abajo
    body = sd_rrect(x, y, cx, top + h - hh, w * .5, hh, r)
    notch = cov(body, aa)
    if s > 0:
        for side in (-1, 1):
            ex = cx + side * w * .5                      # borde lateral del notch
            ccx, ccy = ex + side * s, top + s            # centro del círculo del hombro
            box = sd_rrect(x, y, ex + side * s * .5, top + s * .5 - .05, s * .5, s * .5 + .05, 0)
            hole = s - np.hypot(x - ccx, y - ccy)
            notch = np.maximum(notch, cov(np.maximum(box, hole), aa))
    notch_col = np.array([4, 5, 8])
    col = col * (1 - notch[..., None]) + notch_col * notch[..., None]
    a = tile
    img = np.dstack([col, a * 255]).clip(0, 255).astype(np.uint8)
    return Image.fromarray(img, 'RGBA').resize((n, n), Image.LANCZOS)

sizes = [16, 20, 24, 32, 48, 64, 128, 256]
imgs = {s: render(s) for s in sizes}
imgs[256].save("app.ico", sizes=[(s, s) for s in sizes], append_images=[imgs[s] for s in sizes[:-1]])
imgs[256].save("icon_preview.png")
render(512).save("icon_512.png")
print("ok")
