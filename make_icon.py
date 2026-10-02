# Genera app.ico: el logo de OpenDock. Un azulejo negro OLED con la isla (notch) arriba
# y el dock abajo. Los tamaños pequeños (bandeja, barra) se dibujan aparte, sin detalles,
# para que sigan leyéndose a 16 px.
from PIL import Image
import math

def sd_rrect(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r); qy = abs(py - cy) - (hh - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))

def cov(d, aa):
    return max(0.0, min(1.0, 0.5 - d / aa))

def render(n, detail):
    ss = 3 if n >= 256 else 4 if n >= 64 else 8
    N = n * ss
    img = Image.new("RGBA", (N, N))
    p = img.load()
    aa = 1.0 / N
    for y in range(N):
        fy = (y + .5) / N
        for x in range(N):
            fx = (x + .5) / N
            d = sd_rrect(fx, fy, .5, .5, .47, .47, .235)
            a = cov(d, aa * 1.5)
            if a <= 0: p[x, y] = (0, 0, 0, 0); continue
            # fondo: negro con un brillo frío muy leve arriba
            col = mix((30, 35, 46), (3, 4, 6), min(1, fy * 1.25))
            # filo superior
            if detail and d > -.011: col = mix(col, (78, 86, 104), max(0, .6 - fy * 1.1))
            # isla: pastilla blanca arriba
            isl = sd_rrect(fx, fy, .5, .215, .15 if detail else .2, .048 if detail else .075, .048 if detail else .075)
            col = mix(col, (238, 241, 246), cov(isl, aa * 1.5))
            # dock: barra de acento abajo
            dk = sd_rrect(fx, fy, .5, .74, .34 if detail else .36, .105 if detail else .12, .07 if detail else .07)
            acc = mix((143, 184, 255), (98, 124, 255), (fx - .18) / .64)
            col = mix(col, acc, cov(dk, aa * 1.5))
            if detail:
                # tres apps en el dock; la del medio, "al frente"
                for i, cx in enumerate((.355, .5, .645)):
                    ap = sd_rrect(fx, fy, cx, .728, .048, .048, .016)
                    col = mix(col, (255, 255, 255), cov(ap, aa * 1.5) * (.92 if i == 1 else .5))
                dot = sd_rrect(fx, fy, .5, .804, .026, .008, .008)
                col = mix(col, (20, 26, 52), cov(dot, aa * 1.5))
            p[x, y] = (int(col[0]), int(col[1]), int(col[2]), int(255 * a))
    return img.resize((n, n), Image.LANCZOS)

sizes = [16, 20, 24, 32, 48, 64, 128, 256]
imgs = {s: render(s, s >= 48) for s in sizes}
imgs[256].save("app.ico", sizes=[(s, s) for s in sizes], append_images=[imgs[s] for s in sizes[:-1]])
imgs[256].save("icon_preview.png")
render(512, True).save("icon_512.png")
print("ok")
