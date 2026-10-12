# aquamarine-omalauncher

Repositorio: <https://github.com/AlejandroFloresArroyo/aquamarine-omalauncher>. El artefacto y la
ruta de instalación se llaman `aquamarine-shm`.

Este parche es para **aquamarine v0.15.1** (commit `f31c47a1b9d3…`), el backend de Hyprland
0.56.2. Con él, Hyprland puede correr **anidado** dentro de un compositor Wayland que no ofrece
`zwp_linux_dmabuf_v1` (aquí, el compositor de omalauncher en `/run/user/0/wayland-0`) y
seguir **renderizando en la GPU**.

Se compila como un `libaquamarine.so.14` de reemplazo. Se instala en `/opt/aquamarine-shm` y
se carga con `LD_LIBRARY_PATH`. Hyprland no se toca.

```
patches/0001-wayland-wl_shm-presentation-fallback.patch   el parche (git apply en la raíz de aquamarine v0.15.1)
.github/workflows/build.yml                                 build en GitHub Actions (Arch Linux ARM, aarch64)
tests/shm_copy_test.cpp                                     test de CPU de la lógica de copia, del bookkeeping y del bucle de frames
```

## El problema

Sin el parche, Hyprland anidado aborta así:

```
Wayland backend cannot start: Missing protocols
CRIT Cannot open backend: no allocator available
```

El backend Wayland de aquamarine falla por tres motivos:

- Exige `zwp_linux_dmabuf_v1` (`src/backend/Wayland.cpp:147-150`).
- Saca el nodo DRM del `main_device` de ese protocolo (`Wayland.cpp:382-466`). Sin él no hay fd
  DRM, y por tanto no hay allocator GBM (`src/backend/Backend.cpp:163-179`).
- Solo sabe presentar frames como `wl_buffer` dmabuf (`Wayland.cpp:864-883`).

El compositor de omalauncher solo tiene `wl_shm`, con XRGB8888 y ARGB8888.

## Qué hace el parche

Si el host no tiene un `zwp_linux_dmabuf_v1` utilizable, o si se fuerza con
`AQ_WAYLAND_SHM_PRESENT=1`, el backend Wayland pasa a presentar por `wl_shm` ("modo wl_shm"):

1. **Nodo de render.** Abre `AQ_WAYLAND_RENDER_NODE` (por defecto `/dev/dri/renderD128`) y lo
   devuelve en `drmFD()` y `drmRenderNodeFD()`.
   - Antes de abrirlo comprueba con `stat` que es un *render node*: major 226, minor ≥ 128.
     Nunca abre `/dev/dri/card*`, que en el teléfono son la pantalla real de Android.
   - Con ese fd, aquamarine crea su allocator GBM igual que antes, y Hyprland su contexto EGL.
2. **Formatos.** `getRenderFormats()` ofrece solo XRGB8888 y ARGB8888 con
   `DRM_FORMAT_MOD_LINEAR`, para que la CPU pueda leer los buffers con un layout conocido.
   - El byte layout de XRGB8888/ARGB8888 en `wl_shm` es idéntico al de DRM: no hay swizzle.
   - En este modo GBM reserva con `GBM_BO_USE_RENDERING` sin `SCANOUT`: justo la combinación
     verificada con zink, `gbm_bo_create_with_modifiers2(XRGB8888, LINEAR, RENDERING)`.
3. **Cada frame, sin bloquear a Hyprland.** Hyprland sigue renderizando en la GPU sobre la
   swapchain GBM. En `commit()`:
   - Se apunta el daño del frame que pasa Hyprland (coordenadas de buffer, recortado al buffer;
     un commit sin daño cuenta como frame completo). Los commits sin buffer (DPMS, tests de
     modo...) no generan tráfico Wayland.
   - El frame queda **pendiente** y "en vuelo": Hyprland no recibe `frame` ni vuelve a renderizar
     en la swapchain hasta que el frame llega al host y su `wl_surface.frame` termina.
   - **Fence.** Hyprland entrega en cada frame un *in-fence* (sync_file de
     `EGL_ANDROID_native_fence_sync`). El parche no lo espera con `poll()` bloqueante: guarda un
     `dup` y lo mete en un `epoll` propio, cuyo fd se añade a los que Hyprland ya vigila
     (`pollFDs()`). Cuando el fence señala, se copia y se presenta. El hilo de Hyprland nunca se
     queda esperando a la GPU. **Nunca** se crea ni se exporta un sync_file o un syncobj.
   - **Copia.** Se elige un buffer `wl_shm` libre (memfd + `wl_shm_pool` por buffer, máximo 3 por
     output) y se copian en él **solo los rectángulos que ese buffer tiene desactualizados**. Con
     el host de omalauncher, que copia y suelta el buffer enseguida, normalmente basta un buffer y
     lo copiado es el daño del frame.
   - **Lectura.** Por defecto con `gbm_bo_map` (el camino verificado en el teléfono). Con
     `AQ_WAYLAND_SHM_MMAP=1`, con un `mmap` persistente del dmabuf, entre
     `DMA_BUF_IOCTL_SYNC(START)` y `(END)` como exige la uAPI de dma-buf (salvo
     `AQ_WAYLAND_SHM_DMABUF_SYNC=0`).
   - **Envío.** `attach`, `wl_surface.damage` (v1) solo con lo que cambió desde la última copia
     del host (por encima de 32 rectángulos, su caja envolvente), `frame` si no hay uno en vuelo,
     `commit` y un único `wl_display_flush`: un `sendmsg` por frame y ningún roundtrip.
   - **Frames sin cambios.** Si Hyprland hace commit sin daño (lo hace para mandar los frame
     callbacks de sus clientes) y no hay `wl_surface.frame` en vuelo, se vuelve a adjuntar un buffer
     al día con 1 píxel de daño. Así el host siempre ve un commit normal con buffer y contesta
     con su frame callback. **Nunca** se manda un commit sin `attach`.
   - **Frames sin fence** (si `eglDupNativeFenceFDANDROID` fallara, o en el primer modeset): se
     copian enseguida tras `DMA_BUF_IOCTL_SYNC(START|READ)`, que solo espera los fences implícitos
     que el driver ya haya puesto. Esos píxeles quedan "sin verificar" y se vuelven a copiar desde
     el mismo buffer de la swapchain 25 ms después (un timerfd en el mismo epoll), o con el
     siguiente frame si llega antes. Se avisa una vez en el log.
   - **Fence que no llega.** Se sigue esperando (sin copiar nada sin terminar) y al pasar 1 s se
     avisa en el log. Solo sin epoll (no debería pasar) se espera de forma síncrona, como mucho
     1 s, y entonces el frame se descarta y se pide otro.
4. **`test()`** rechaza en este modo cualquier buffer que no sea un buffer GBM del propio
   aquamarine (los de su swapchain: LINEAR XRGB8888/ARGB8888, un plano). Así el *direct scanout*
   de Hyprland nunca se activa, aunque `render:direct_scanout` esté a 1 o 2: los buffers de
   clientes se leerían sin fence. `commit()` rechaza esos buffers igual.
5. **Liberación de buffers.** Los `wl_buffer.release` se siguen uno a uno. Nunca se escribe en
   un buffer que el host aún tiene. Si el host retiene los 3, el frame espera al siguiente
   `release`, sin bloquear.
6. **Cursor.** `capabilities()` devuelve 0, sin cursor hardware. Hyprland dibuja un cursor
   software dentro del frame, y el cursor del host se oculta al entrar con
   `wl_pointer.set_cursor(serial, NULL)`. (Hyprland renderiza el cursor hardware sin flush ni
   fence, `src/render/GLRenderer.cpp:105-106`: la CPU no sabría cuándo leerlo.)
7. **Orden xdg-shell.** En este modo, el `xdg_toplevel.configure` se aplica en el
   `xdg_surface.configure`, **después** del `ack_configure` y en el mismo dispatch. Así el
   primer buffer nunca llega a una superficie sin configurar.
8. **Correcciones.**
   - Ningún proxy dmabuf nulo se desreferencia (`setCursor`, `CWaylandBuffer`).
   - `drmGetDeviceNameFromFd2()` puede devolver NULL cuando sysfs no tiene `uevent` para el nodo
     (el /sys falso de proot), y se asignaba a un `std::string` sin comprobar
     (`src/allocator/GBM.cpp:359-360`).

Si el host **sí** tiene un `zwp_linux_dmabuf_v1` que funciona y `AQ_WAYLAND_SHM_PRESENT` no
está, el comportamiento es el de siempre. Solo cambia la comprobación de NULL del punto 8.

Cuando el modo está activo, el log de Hyprland lo dice con una línea WARNING:

```
Wayland backend: presenting through wl_shm copies (the host has no usable zwp_linux_dmabuf_v1). Rendering on /dev/dri/renderD128, ..., reading through gbm_bo_map
```

### Por qué `wl_surface.damage` y no `damage_buffer`

El host anuncia `wl_compositor` v4 y recorta el bind de aquamarine (v6) a v4, así que
`damage_buffer` (v4) también existiría. Aun así se usa `damage` (v1): esta superficie nunca cambia
el buffer scale (1) ni el transform (normal), así que las dos peticiones son equivalentes, y v1 es
la que cualquier host implementa. El camino dmabuf original sigue usando `damage_buffer`.

### Versiones de protocolo y lo que se le pide al host

Todo lo que se envía existe en las versiones del host (`wl_compositor` 4, `xdg_wm_base` 3,
`wl_seat` 5, `wl_shm` 1): `wl_shm.create_pool`, `wl_shm_pool.create_buffer`/`destroy`,
`wl_buffer.destroy`, `wl_surface.attach`/`damage`/`frame`/`commit`, `xdg_surface.ack_configure`
y `wl_pointer.set_cursor`, todas v1.

El host debe:
- aceptar `wl_pointer.set_cursor` con superficie NULL (así se oculta su cursor);
- aceptar pools `wl_shm` de unos 13 MB (2304x1440x4);
- contestar el frame callback de un commit que trae buffer, también si el daño es de 1 píxel y el
  buffer es uno que ya había soltado;
- mantener un frame callback pedido en un commit aunque llegue otro commit (con buffer y sin
  `frame`) antes de presentarlo. aquamarine de serie ya hace eso.

Una salvedad que no viene del parche: al **destruir** el backend, los destructores generados por
hyprwayland-scanner mandan `wl_shm.release` (v2) y `wl_compositor.release` (v7), que este host no
tiene. Solo pasa al cerrar, justo antes de `wl_display_disconnect()`, que no hace flush, así que
en la práctica no llegan al host.

## 1. El repositorio y el build

El build ocurre en GitHub Actions, en
[AlejandroFloresArroyo/aquamarine-omalauncher](https://github.com/AlejandroFloresArroyo/aquamarine-omalauncher).
GitHub solo ejecuta workflows de `.github/workflows/` en la raíz del repositorio. Para un fork o
un repositorio propio: subir este directorio como raíz (`git init -b main`, `git add .`,
`git commit`, `gh repo create <nombre> --public --source . --push`).

El push lanza el workflow. Se sigue con:

```sh
gh run watch -R AlejandroFloresArroyo/aquamarine-omalauncher   # o en la pestaña Actions de la web
```

Qué hace el workflow (`ubuntu-24.04-arm` con el contenedor `menci/archlinuxarm:base-devel`):

1. Instala las dependencias (`pacman --disable-sandbox`): `base-devel cmake pkgconf git
   linux-api-headers hyprwayland-scanner hyprutils wayland wayland-protocols libglvnd mesa libdrm
   seatd libinput systemd-libs pixman libdisplay-info hwdata`.
2. Clona aquamarine en `v0.15.1` y **falla** si el tag no apunta exactamente a
   `f31c47a1b9d300847d8fc3108ad959448103dfc1`.
3. Aplica el parche (`git apply --check` y luego `git apply`).
4. `cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/aquamarine-shm`, compila e
   instala en un directorio de staging.
5. Comprueba la librería y **falla** si:
   - el soname no es `libaquamarine.so.14`;
   - sus `NEEDED` no son exactamente los del `libaquamarine.so.14` de serie del teléfono (por
     ejemplo, otro soname de hyprutils porque el mirror avanzó);
   - necesita símbolos más nuevos que `GLIBC_2.43`, `GLIBCXX_3.4.35` o `CXXABI_1.3.17`, lo
     máximo que tiene el teléfono;
   - no contiene `CWaylandOutput::presentShm`.
6. Compila y ejecuta `tests/shm_copy_test.cpp`.
7. Empaqueta `opt/aquamarine-shm` en `aquamarine-shm-0.15.1-aarch64.tar.gz` (con
   `BUILDINFO.txt` dentro) y lo sube como artefacto `aquamarine-shm-0.15.1-aarch64`.

## 2. Descargar el artefacto

```sh
mkdir -p ~/aq-artifact && cd ~/aq-artifact
gh run download -R AlejandroFloresArroyo/aquamarine-omalauncher -n aquamarine-shm-0.15.1-aarch64
sha256sum -c aquamarine-shm-0.15.1-aarch64.tar.gz.sha256
```

(`-R` hace falta porque no se ejecuta dentro de un clon. Sin `gh`: bajar el zip del artefacto
desde la página del run en la web y descomprimirlo aquí.)

## 3. Instalar en `/opt/aquamarine-shm`

```sh
tar -xzf ~/aq-artifact/aquamarine-shm-0.15.1-aarch64.tar.gz -C /     # crea /opt/aquamarine-shm
cat /opt/aquamarine-shm/BUILDINFO.txt
pacman -Q hyprland aquamarine hyprutils
LD_LIBRARY_PATH=/opt/aquamarine-shm/lib ldd /usr/bin/Hyprland | grep -E 'aquamarine|hyprutils|not found'
```

- `ldd` debe mostrar `libaquamarine.so.14 => /opt/aquamarine-shm/lib/libaquamarine.so.14` y
  ningún `not found`. `Hyprland` no tiene RPATH/RUNPATH, así que `LD_LIBRARY_PATH` manda.
- El artefacto vale **solo** para las versiones exactas con las que se compiló: `hyprland
  0.56.2`, `aquamarine 0.15.1` (soname `.so.14`) y `hyprutils` con `libhyprutils.so.13`. Compara
  `BUILDINFO.txt` con `pacman -Q`.
- Tras cualquier `pacman -Syu` que toque `hyprland`, `aquamarine` o `hyprutils`, vuelve a
  comprobar `BUILDINFO.txt` y `ldd`. Si algo cambió, **no** lo arregles con `pacman -Syu`:
  hay que ajustar el parche y el workflow (`AQ_TAG`, `EXPECTED_NEEDED`) a las versiones nuevas y
  recompilar. Mezclar sonames de hyprutils en un proceso acaba en crash.

Para desinstalar basta `rm -rf /opt/aquamarine-shm`: sin `LD_LIBRARY_PATH`, Hyprland usa el
aquamarine del sistema.

## 4. Ejecutar Hyprland anidado

Guarda la configuración mínima en `~/.config/hypr/hyprland-nested.conf` (Hyprland sale con
"Config file ... is invalid" si el fichero de `-c` no existe):

```sh
mkdir -p ~/.config/hypr
cat > ~/.config/hypr/hyprland-nested.conf <<'EOF'
# Hyprland 0.56.2 anidado en omalauncher, con aquamarine-shm
# La salida se llama WAYLAND-1. Su tamaño lo decide el configure del host
# (si el host manda 0x0, aquamarine usa 1280x720).
monitor = WAYLAND-1, preferred, 0x0, 1

# Un terminal al arrancar. El táctil no llega a Hyprland: hace falta teclado y/o ratón.
exec-once = foot
bind = SUPER, Return, exec, foot
bind = CTRL ALT, T, exec, foot
bind = SUPER, Q, killactive
bind = SUPER SHIFT, E, exit
bind = CTRL ALT, BackSpace, exit

cursor {
    no_hardware_cursors = 1      # redundante con este parche (no hay cursor hardware), pero explícito
}

render {
    direct_scanout = 0           # el parche lo impide de todos modos (test())
    new_render_scheduling = false
}

debug {
    damage_tracking = 2          # por defecto; sin daño real cada frame sería una copia completa
    vfr = true                   # por defecto
}

decoration {
    blur {
        enabled = false          # el blur agranda el daño, y con él las copias
    }
    shadow {
        enabled = false
    }
}

animations {
    enabled = false              # opcional: menos frames con daño grande
}

xwayland {
    enabled = false              # opcional: Xwayland/glamor arrancaría otra instancia zink
}
EOF
```

`SUPER` puede no llegar si Android se queda la tecla Meta: por eso también hay atajos con
`CTRL ALT`. Para usar tu configuración normal en vez de esta, añade a ella las líneas `monitor`,
`cursor` y `render` de arriba y pásala con `-c`.

Después, en un terminal del teléfono:

```sh
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=/opt/aquamarine-shm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# GPU: zink sobre radv-xclipse (la pila verificada)
export MESA_LOADER_DRIVER_OVERRIDE=zink
export RADV_DEBUG=nodma
export VK_DRIVER_FILES=/opt/radv-xclipse-540/share/vulkan/icd.d/radeon_icd.aarch64.json
unset LIBGL_ALWAYS_SOFTWARE          # obligatorio: forzaría llvmpipe

export HYPRLAND_NO_RT=1
# opcional, para depurar: export AQ_TRACE=1 HYPRLAND_TRACE=1

# La copia corre en el hilo principal de Hyprland: mejor en los núcleos grandes. Las CPUs 4-7
# son Cortex-A720 (0xd81) y 0-3 Cortex-A520 (0xd80), pero el cpuset del launcher solo deja
# 0-6, así que la 7 no existe para nosotros.
taskset -c 4-6 Hyprland --i-am-really-stupid -c ~/.config/hypr/hyprland-nested.conf 2>&1 | tee /tmp/hyprland-nested.log
```

- `--i-am-really-stupid` hace falta porque se ejecuta como root (Hyprland `src/main.cpp:203-208`).
- **No** pongas `LIBSEAT_BACKEND=noop`. Con él, aquamarine intentaría hacer KMS sobre
  `/dev/dri/card0`, que es la pantalla real de Android.
- Todos los clientes que lance Hyprland heredan estas variables.
- El log completo también queda en `$XDG_RUNTIME_DIR/hypr/<instancia>/hyprland.log`.

Qué mirar en el log:

| Línea | Significado |
|---|---|
| `presenting through wl_shm copies (...) reading through gbm_bo_map` | el modo wl_shm está activo |
| `frames arrive without an explicit fence` | Hyprland no da fences: la presentación no está sincronizada con la GPU y se apoya en la re-copia a los 25 ms |
| `the frame's GPU fence has not signaled after N ms` | la GPU no termina el frame (¿colgada?) |
| `the host holds all 3 buffers` | el host tarda en soltar buffers; los frames esperan |
| `cannot present this buffer` | algo intentó presentar un buffer que no es de la swapchain |

**Opcional, cuando lo anterior funcione:** probar la lectura directa con `AQ_WAYLAND_SHM_MMAP=1`
y compararla con la de por defecto sobre algo que cambie en cada frame (por ejemplo `watch -n 0.1
date` en el `foot` anidado). Si con `=1` salen píxeles viejos o corruptos, quédate con el valor
por defecto. Si `DMA_BUF_IOCTL_SYNC` resultara caro en este driver, `AQ_WAYLAND_SHM_DMABUF_SYNC=0`
lo quita (bajo tu responsabilidad).

### Variables de entorno del parche

| Variable | Valores | Efecto |
|---|---|---|
| `AQ_WAYLAND_SHM_PRESENT` | sin definir / `1` / `0` | Automático: se activa si el host no tiene dmabuf. `1` lo fuerza. `0` lo desactiva y deja el error de antes. |
| `AQ_WAYLAND_RENDER_NODE` | ruta | Nodo de render. Por defecto `/dev/dri/renderD128`; solo acepta render nodes. |
| `AQ_WAYLAND_SHM_MMAP` | sin definir / `1` | Sin definir: lee con `gbm_bo_map` (verificado). `1`: `mmap` directo del dmabuf entre `DMA_BUF_IOCTL_SYNC`. |
| `AQ_WAYLAND_SHM_DMABUF_SYNC` | sin definir / `1` / `0` | `DMA_BUF_IOCTL_SYNC` alrededor de la lectura. Sin definir: en frames sin fence, en las re-copias y con `AQ_WAYLAND_SHM_MMAP=1`. `1`: siempre. `0`: nunca. |
| `AQ_WAYLAND_SHM_RECOPY` | sin definir / `1` / `0` | Re-copia a los 25 ms. Sin definir: solo de frames sin fence. `1`: de todos (útil si el último frame de un cambio a veces queda viejo). `0`: nunca. |

## Tests

```sh
git clone --branch v0.15.1 https://github.com/hyprwm/aquamarine && cd aquamarine
git apply ../aquamarine-omalauncher/patches/0001-wayland-wl_shm-presentation-fallback.patch
g++ -std=c++23 -O2 -I src/backend -I /usr/include/pixman-1 \
    ../aquamarine-omalauncher/tests/shm_copy_test.cpp -lhyprutils -lpixman-1 -o shm_copy_test
./shm_copy_test
```

El test no toca la GPU ni Wayland.

- `clippedRects` + `copyRects` copian exactamente el daño recortado, desde una fuente con stride
  con relleno y offset, como un plano dmabuf. `hostDamageRects` devuelve la región, su caja o
  1 píxel si está vacía.
- Una simulación paso a paso (un paso = un frame del host) de `presentShm`, `advanceShm`, la
  re-copia, `onFrameDone` y el `release`, con los mismos helpers (`src/backend/WaylandShm.hpp`) y
  las mismas decisiones, contra:
  - un host que copia los rectángulos dañados de cada commit en su propia copia y suelta los
    buffers al momento, tarde, al azar o con bloqueos largos;
  - una GPU que termina cada frame 0-3 frames del host después. Los frames llegan con fence, sin
    fence (se copian enseguida y pueden leer el contenido viejo del buffer en su daño) o mezclados;
  - un consumidor que a veces renderiza con un frame en vuelo (como tras un `xdg_surface.configure`).
- Comprueba: ningún buffer cambia mientras el host lo tiene; un frame en vuelo siempre tiene un
  `wl_surface.frame` o un frame pendiente que lo termina; cada commit del consumidor recibe su
  evento `frame`; todo commit al host lleva buffer y daño; cuando no queda nada pendiente ni sin
  verificar, la copia del host es igual al último frame.

Resultado típico:

```
immediate/fenced       frames  1783  commits  1724  recopies    0  waits 1157  buffers 2  copied/damaged 0.996
late/fenced            frames  1822  commits  1748  recopies    0  waits 1696  buffers 3  copied/damaged 1.252
random/fenced          frames  1785  commits  1730  recopies    0  waits 1412  buffers 3  copied/damaged 1.323
stalls/fenced          frames  1756  commits  1698  recopies    0  waits 1117  buffers 3  copied/damaged 1.127
immediate/no-fence     frames  2162  commits  3313  recopies 1152  waits    0  buffers 2  copied/damaged 3.256
...
all tests passed
```

Para comprobar que el test sirve se rompió la lógica a propósito de 11 maneras: no ensuciar los
buffers retenidos, elegir un buffer ocupado, no esperar al fence, no apuntar los píxeles sin
verificar, no volver a dañarlos al re-copiar, no pasarlos al frame siguiente, re-copiar en el
siguiente frame callback (que puede ser de un commit anterior), re-copiar en el mismo frame, no
re-copiar nunca, commit sin `attach` para frames sin cambios y `release` que no reanuda el frame
pendiente. El test detecta las 11.

## Limitaciones conocidas

- **No hay touch.** El backend Wayland de aquamarine nunca pide `wl_touch` (`Wayland.cpp:347-364`),
  así que el táctil del teléfono no llega a Hyprland. Hacen falta teclado y ratón. Arreglarlo
  requiere otro parche: un `CWaylandTouch` que emita eventos `ITouch`.
- **Coste de CPU.** La copia GPU→CPU corre en el hilo principal de Hyprland (ya no la espera a la
  GPU).
  - Un frame completo de 2304x1440 cuesta unos 4,7 ms en un A720 y entre 86 y 135 ms en un A520.
    De ahí `taskset -c 4-6`.
  - Hay copias completas al arrancar, tras cada cambio de tamaño (dos o tres), al cambiar de
    workspace y con `forceFullFrames`.
  - El daño que da Hyprland es el del buffer de su swapchain de 3 (buffer age 3), así que puede ser
    hasta unas 3 veces el cambio real.
  - Mejoras posibles: un hilo de copia, o una swapchain de 1 buffer con age 1 en modo wl_shm.
- **Primer modeset y cambios de tamaño.** Hyprland hace commit con un buffer aún sin renderizar.
  Si el host ya configuró la superficie, se ve un frame negro o basura hasta el primer frame real;
  el camino dmabuf original hace lo mismo.
- **Plataforma EGL.** Hyprland preferirá probablemente `EGL_PLATFORM_DEVICE_EXT`: en el /sys falso,
  `card0` y `renderD128` cuelgan del mismo dispositivo PCI. Solo se ha verificado la plataforma
  GBM con zink.
- **Fences.** La seguridad de los fences por frame depende de que zink→radv-xclipse nunca exporte
  un sync_file desde un syncobj sin materializar. Esa protección está en el driver, no aquí. Si el
  driver devolviera un fence ya señalado sin que la GPU haya terminado, el último frame de un
  cambio podría quedarse viejo: `AQ_WAYLAND_SHM_RECOPY=1` lo cubre.
- **Byte X.** El host debe tratar el byte X de XRGB8888 como opaco. Hyprland no garantiza alfa 255
  en ese byte.
- **Tamaño.** Si el host configura 0x0, aquamarine usa 1280x720, y ese tamaño se impone a la
  regla `monitor` de Hyprland.
- **Dependencia de sysfs.** `reopenDRMNode` (`Backend.cpp:330-387`) depende de que el /sys falso
  conserve `/sys/dev/char/226:128/device/drm`.
- **Sin `wp_presentation`.** El evento `present` se emite en cada `wl_callback.done`, sin timestamp
  del host (como antes).

## Qué está verificado y qué no

Verificado en local, sin GPU ni Wayland:
- El parche aplica con `git apply --check` sobre una copia limpia de v0.15.1 (f31c47a).
- Con las cabeceras reales de protocolo (hyprwayland-scanner 0.4.6, el paquete firmado de Arch
  Linux ARM extraído en un directorio temporal, sin instalar nada), compila **toda** la librería
  sin warnings (`-Wall -Wextra -Wpedantic`, `-O3`) y enlaza con `-Wl,--no-undefined` y el mismo
  soname. Sus `NEEDED` son idénticos a los del `/usr/lib/libaquamarine.so.14` de serie y exporta
  los 47 símbolos que importa `/usr/bin/Hyprland` y todos los que importa `libhyprtoolkit`. De
  los 1710 que exporta la librería de serie faltan 4 instancias inline/plantilla (p. ej. el
  destructor de una struct anónima de `CWaylandOutput`) que nadie importa.
- El test de CPU pasa, y las roturas a propósito lo hacen fallar.
- El paso "Check the library" del workflow, ejecutado en local: pasa con la librería de serie en
  NEEDED y versiones de símbolos, y falla si cambia un NEEDED o si el objetivo tiene un glibc,
  GLIBCXX o CXXABI más viejo.

No verificado:
- Un build real con CMake en CI.
- Cualquier ejecución contra el host, la GPU o Hyprland (por las reglas de esta tarea no se
  ejecutó nada de eso): que el host conteste frame callbacks como se espera, que el epoll con el
  sync_file despierte a Hyprland en este kernel, la plataforma EGL que elija Hyprland, y la lectura
  con `AQ_WAYLAND_SHM_MMAP=1` en sgpu.
