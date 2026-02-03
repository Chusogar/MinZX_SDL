# Uso de TR-DOS ROM en MinZX_SDL

## Guía rápida

### 1. Obtener TR-DOS ROM

Necesitas un archivo de ROM TR-DOS (16KB). Puedes encontrarlos en sitios de recursos ZX Spectrum.
Versiones comunes:
- TR-DOS 5.03 (más compatible)
- TR-DOS 5.04T
- TR-DOS 6.10

### 2. Colocar la ROM

Opción A: Coloca `trdos.rom` en el mismo directorio que `minzx`
Opción B: Especifica la ruta con `--trdos-rom`

### 3. Ejecutar

```bash
# Método 1: Auto-carga (trdos.rom en directorio actual)
./minzx disk.trd

# Método 2: Especificar ROM
./minzx disk.trd --trdos-rom /ruta/a/trdos.rom

# Método 3: Con múltiples discos
./minzx disco1.trd disco2.scl --trdos-rom trdos.rom
```

### 4. Usar TR-DOS

1. **Iniciar**: El emulador arranca con ROM ZX Spectrum
2. **Activar TR-DOS**: Presiona `F9` para cambiar a ROM TR-DOS
3. **Usar comandos**: Ahora puedes usar comandos TR-DOS:
   - `LIST` - Listar archivos
   - `RUN "nombre"` - Ejecutar programa BASIC
   - `LOAD "nombre"` - Cargar archivo
   - `CAT` - Mostrar catálogo

4. **Volver a ZX ROM**: Presiona `F9` nuevamente

### 5. Teclas útiles

- `F9` - Alternar ROM TR-DOS ON/OFF
- `F8` - Listar discos montados y archivos
- `F12` - Reset del sistema
- `ESC` - Salir

## Ejemplo de sesión

```bash
$ ./minzx juegos.trd

# Salida en consola:
# TRD: Opened 'juegos.trd' - 80 tracks, 2 sides, 5/5 files loaded
# Mounted TRD to drive 0
# TR-DOS enabled. Keys: F8=List disks, F12=Reset
# TR-DOS ROM loaded: trdos.rom

# En el emulador:
# 1. Presionar F9 -> "TR-DOS ROM: ACTIVE"
# 2. El sistema arranca en TR-DOS
# 3. Escribir: LIST
# 4. Ver lista de archivos del disco
# 5. Escribir: RUN "JUEGO"
# 6. El juego se carga desde el disco
```

## Solución de problemas

**"TR-DOS ROM not loaded"**
- Verifica que trdos.rom existe y tiene 16384 bytes exactos
- Usa --trdos-rom para especificar ruta completa

**"Warning: Could not load TR-DOS ROM"**
- El archivo no existe o no es accesible
- Verifica permisos de lectura

**No puedo ver archivos en disco**
- Presiona F8 para ver si el disco está montado
- Presiona F9 para activar ROM TR-DOS
- Verifica que el archivo .trd es válido

## Notas técnicas

- La ROM TR-DOS se mapea en 0x0000-0x3FFF cuando está activa
- La conmutación es manual con F9 (no automática)
- Ambas ROMs (ZX48 y TR-DOS) se mantienen en memoria
- El cambio es instantáneo y no afecta la RAM
