# Firmware minero para tres Heltec WiFi LoRa 32 V3

Esta carpeta es independiente del firmware estable de los ESP32 WROOM. Contiene
la migracion inicial para tres readers Heltec V3:

- `heltec_rx01`: RX-01, portal/sala de control.
- `heltec_rx02`: RX-02, rampa.
- `heltec_rx03`: RX-03, frente de trabajo.

Los tres perfiles usan la placa PlatformIO `heltec_wifi_lora_32_V3`, almacenan
la interfaz web en LittleFS, escanean los tres iBeacon y reservan LoRa P2P en
915 MHz para compartir observaciones entre readers.

## Advertencias antes de energizar o transmitir

1. Confirmar que las tres placas sean **V3** y para la banda **915 MHz**.
2. Conectar la antena LoRa incluida antes de encender o flashear cada placa.
3. No conectar ni flashear las tres placas a la vez durante la identificacion.
4. No seleccionar una placa Heltec V2 ni el perfil `esp32dev`.
5. Esta es una plataforma de monitoreo; no sustituye un sistema certificado de
   seguridad o anticolision.

## Primera carga de cada equipo

Cerrar primero cualquier Monitor serial que este usando el puerto COM.

```powershell
cd "C:\Users\fvergram\OneDrive - NTT DATA EMEAL\Desktop\beacons\firmware_heltec_mineria"

# Primer Heltec
pio run -e heltec_rx01 -t upload
pio run -e heltec_rx01 -t uploadfs
pio device monitor -e heltec_rx01

# Segundo Heltec
pio run -e heltec_rx02 -t upload
pio run -e heltec_rx02 -t uploadfs
pio device monitor -e heltec_rx02

# Tercer Heltec
pio run -e heltec_rx03 -t upload
pio run -e heltec_rx03 -t uploadfs
pio device monitor -e heltec_rx03
```

Si PlatformIO no elige el puerto correcto, listar primero:

```powershell
pio device list
```

Y usar temporalmente, por ejemplo:

```powershell
pio run -e heltec_rx01 -t upload --upload-port COM6
pio run -e heltec_rx01 -t uploadfs --upload-port COM6
```

Si la placa no entra en modo de carga: mantener presionado `PRG/BOOT`, pulsar y
soltar `RST`, soltar `PRG/BOOT` y repetir `Upload`.

## Acceso al portal

Cada perfil crea la red abierta `MINA-LOCAL` y aloja el portal en:

```text
http://192.168.4.1
```

El panel de diagnostico queda disponible en:

```text
http://192.168.4.1/diagnostico
```

La consola debe informar como minimo:

```text
[RED] MINA-LOCAL disponible
[LORA] activo en 915.0 MHz
[WEB] ... http://192.168.4.1
```

## Orden recomendado de laboratorio

1. Compilar `heltec_rx01` sin conectar hardware.
2. Flashear RX-01 y verificar portal, BLE y LoRa.
3. Repetir con RX-02 y luego con RX-03.
4. Encender los tres separados unos metros y comprobar `/diagnostico`.
5. Mover un beacon entre readers y validar que cambie su `reader_id`.

La primera compilacion requiere que PlatformIO instale o encuentre sus
dependencias. Si queda detenido por procesos antiguos, cerrar todas las ventanas
de VS Code, esperar unos segundos y abrir solamente esta carpeta antes de volver
a compilar.
