# Genera app.ico: una "pantalla" oscura con una esquina redondeada resaltada.
from PIL import Image
import math
def sdf_box(px, py, cx, cy, hw, hh, r):
    qx = abs(px-cx)-(hw-r); qy = abs(py-cy)-(hh-r)
    return math.hypot(max(qx,0), max(qy,0)) + min(max(qx,qy),0) - r
def render(n, ss=4):
    N = n*ss; img = Image.new("RGBA", (N, N)); p = img.load()
    for y in range(N):
        for x in range(N):
            fx, fy = (x+.5)/N, (y+.5)/N
            d = sdf_box(fx, fy, .5, .5, .46, .46, .22)
            if d > 0: p[x,y] = (0,0,0,0); continue
            col = (24,26,38)
            # arco de acento: esquina superior izquierda
            ring = abs(sdf_box(fx, fy, .62, .62, .40, .40, .30)) < .055
            if ring and fx < .62 and fy < .62: col = (110,150,255)
            p[x,y] = (*col, 255)
    return img.resize((n, n), Image.LANCZOS)
big = render(256)
big.save("app.ico", sizes=[(16,16),(20,20),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
big.save("icon_preview.png")
