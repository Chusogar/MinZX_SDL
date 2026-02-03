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

### 4. Usar TR-DOS - AUTOMÁTICO

**¡IMPORTANTE!** La ROM TR-DOS se activa automáticamente cuando es necesario:

1. **Iniciar**: El emulador arranca con ROM ZX Spectrum
2. **Uso normal**: Escribe comandos BASIC normales
3. **Activación automática**: Cuando ejecutas comandos que acceden al disco (CAT, LOAD, RUN, etc.), la ROM TR-DOS se activa automáticamente
4. **Desactivación automática**: Cuando termina la operación de disco, vuelve a ROM ZX Spectrum

**No necesitas presionar F9** - el sistema detecta automáticamente cuando se llama a TR-DOS.

### 5. Teclas útiles

- `F8` - Listar discos montados y archivos
- `F9` - Toggle manual ROM (solo para debugging - normalmente no necesario)
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
# 1. Sistema arranca en ZX Spectrum BASIC
# 2. Escribir: CAT
# 3. ROM TR-DOS se activa AUTOMÁTICAMENTE
# 4. Se muestra el catálogo del disco
# 5. Escribir: RUN "JUEGO"
# 6. El juego se carga desde el disco
# 7. ROM vuelve a ZX Spectrum automáticamente
```

## Cómo funciona la activación automática

El emulador monitorea el Program Counter (PC) del Z80:

- **Activación**: Cuando `PC & 0xFF00 == 0x3D00` (rango 0x3D00-0x3DFF)
- **Desactivación**: Cuando PC sale de ese rango

Este rango de direcciones (0x3D00-0x3DFF) es el punto de entrada estándar de TR-DOS en sistemas ZX Spectrum. Cuando cualquier programa llama a TR-DOS (mediante RANDOMIZE USR 15616 o llamadas del sistema), el PC entra en este rango y la ROM se activa automáticamente.

## Solución de problemas

**"TR-DOS ROM not loaded"**
- Verifica que trdos.rom existe y tiene 16384 bytes exactos
- Usa --trdos-rom para especificar ruta completa

**"Warning: Could not load TR-DOS ROM"**
- El archivo no existe o no es accesible
- Verifica permisos de lectura

**No puedo ver archivos en disco**
- Presiona F8 para ver si el disco está montado
- Verifica que el archivo .trd es válido
- La ROM TR-DOS debería activarse automáticamente al usar comandos de disco

**La ROM no se activa automáticamente**
- Verifica que trdos.rom se cargó correctamente
- Los programas deben llamar a TR-DOS mediante el punto de entrada estándar (0x3D00)
- Usa F9 para toggle manual si es necesario (debugging)

## Notas técnicas

- La ROM TR-DOS se mapea en 0x0000-0x3FFF cuando está activa
- La activación es automática basada en el valor del PC (Program Counter)
- Condición: `(PC & 0xFF00) == 0x3D00`
- Ambas ROMs (ZX48 y TR-DOS) se mantienen en memoria
- El cambio es instantáneo y no afecta la RAM
- F9 permite override manual (útil para debugging)
