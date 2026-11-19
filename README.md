# Wireless Corne 3x5

Variante de 36 teclas: columnas exteriores cortadas de la PCB Corne. Usa el layout 5 columnas incluido en ZMK v0.3. El keymap conserva las funciones de las teclas restantes; combos ajustados a las nuevas posiciones.

<p align="center">
  <img width="900" alt="Wireless Corne" src="https://github.com/user-attachments/assets/0e4540cb-65be-4db0-9063-eaaef82d1726" />
</p>

<h1 align="center">Wireless HotSwap MX Ergo Split Corne Keyboard</h1>
<p align="center">Compacto, inalámbrico y pensado para aprovechar al máximo cada tecla</p>

### Layer previews

#### Base
<img width="1333" height="435" alt="image" src="https://github.com/user-attachments/assets/37bead11-6a41-495a-8050-b5f490de531b" />

#### Lower
<img width="1323" height="439" alt="image" src="https://github.com/user-attachments/assets/9eb23cf9-94f8-4bd1-9b5f-3b1fc6ae5e78" />

#### Raise
<img width="1323" height="433" alt="image" src="https://github.com/user-attachments/assets/bfa2a616-b5c0-42fd-81db-6ebc022ea842" />

#### Adjust
<img width="1323" height="433" alt="image" src="https://github.com/user-attachments/assets/99f0bac7-8d1f-44e1-ba15-74e243d0b068" />

### Display izquierdo: batería y tiempo de uso

La pantalla nice!view izquierda usa el diseño de `wcorne-trackpad`:

- Arriba: salida USB/Bluetooth, perfil/conexión y capa activa.
- Centro: batería izquierda (`I`) y derecha (`D`) en porcentaje.
- Abajo: tiempo activo con batería de cada mitad, en horas:minutos.
- `D --:--` indica que aún no llegó el tiempo derecho; `*` marca un dato sin actualizar durante al menos 65 segundos. La consulta BLE se realiza cada 30 segundos.

Cada mitad conserva su contador al apagar o reiniciar. Se guarda cada cinco
minutos y al entrar en idle/sleep; un corte brusco puede perder hasta cinco
minutos. No cuenta reposo ni tiempo conectado a USB. Con los tiempos actuales,
el período activo incluye hasta 30 minutos de inactividad antes de idle.

Conectar USB reinicia el contador de esa mitad y desconectarlo inicia el nuevo
período. Esto indica uso desde la última conexión de carga, no desde una carga
completa: el firmware no detecta cuándo la batería terminó de cargar.

Hay que actualizar **ambas mitades** para ver ambos contadores. La pantalla
original derecha y ZMK Studio izquierdo se conservan. La matriz `build.yaml`
agrega `config/corne_display.conf` solamente a la compilación izquierda;
`config/corne.conf` mantiene la configuración compartida.
