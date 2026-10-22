# FAT32-FUSE - Guía Técnica de Implementación

## Estructura del Código

FUSE, la librería que permite crear filesystems desde el espacio de usuario, es lo suficientemente flexible como para permitir cualquier tipo de funcionalidad, siempre que se implementen las operaciones requeridas en la estructura `fuse_operations`. Para entender qué hace el sistema de archivos, se debe comenzar por entender esta estructura.

Cuando se monta el sistema de archivos, se invocan las funciones del archivo `fat_fuse.c`, que crea un nuevo `fat_volume` a partir de la información de la imagen. Durante este proceso, al leer la tabla FAT, el sistema construye el árbol de directorios en un TAD `fat_tree`. Luego, se llama a la función `fuse_main`, que efectivamente monta el nuevo sistema de archivos.

Una vez que el sistema está corriendo, cada vez que se invoca una llamada al sistema que involucra el filesystem, FUSE llama a las funciones implementadas en `fat_fuse_ops.c`. En estas funciones sólo se implementa la lógica de alto nivel del filesystem que asegura la consistencia de directorios y archivos. Las operaciones de lectura y escritura de bajo nivel están implementadas en `fat_file.c`.

Toda esta información es guardada en estructuras en memoria mientras el filesystem está corriendo, y se destruye cuando el proceso termina o cuando se desmonta el sistema de archivos. Por ello, al implementar las operaciones de escritura, se debe **mantener la consistencia entre la información en memoria y la información en la imagen**.

### Limitaciones del Sistema

* Sólo tiene soporte para FAT32
* Sólo acepta nombres de 8 caracteres más una extensión de 3 caracteres (no tiene soporte para VFAT/nombres largos)
* Sólo soporta un único hilo (single-threaded)

## Compilar y Ejecutar

### Instalación de Dependencias

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install libfuse-dev libglib2.0-dev pkg-config build-essential check
```

**macOS:**
```bash
brew install macfuse glib pkg-config check
```

Ver el [README.md](../README.md) en la raíz del proyecto para instrucciones detalladas de instalación.

### Compilación

```bash
make
```

### Ejecución

```bash
mkdir mnt
./fat-fuse path/to/image.img ./mnt
```

### Desmontaje

**Linux:**
```bash
fusermount -u ./mnt
```

**macOS:**
```bash
umount ./mnt
```

Si el programa falla inesperadamente, el sistema de archivos puede no desmontarse correctamente. En ese caso, es necesario desmontarlo manualmente usando los comandos nativos del sistema operativo (ver sección de troubleshooting en README.md principal).

## Tests

El proyecto incluye tests unitarios y funcionales usando el framework Check.

### Ejecutar Todos los Tests

```bash
make test
```

### Tests Individuales

```bash
make test-ht     # Tests unitarios: hierarchy tree
make test-ft     # Tests unitarios: FAT filesystem tree
make test-ops    # Tests funcionales: operaciones del filesystem
```

### Cobertura de Tests

**Tests Unitarios:**
- `test-ht`: Operaciones del árbol jerárquico (BST + listas enlazadas)
- `test-ft`: Gestión del árbol de directorios FAT
- Total: 37 checks

**Tests Funcionales (`test-ops`):**
- Operaciones básicas de archivos (crear, leer, escribir, sobrescribir)
- Operaciones multi-cluster (archivos de 512B, 2KB, 10KB)
- Operaciones de directorios (mkdir, directorios anidados)
- Operaciones de eliminación (unlink, rmdir, casos de error)
- Total: 13 checks que cubren operaciones end-to-end

### Implementación de Tests Funcionales

Los tests funcionales usan un enfoque híbrido de fixtures:

**Unchecked fixtures** (lentos, 1 vez por suite):
- Crear y montar imagen FAT32 temporal
- Ejecutado una sola vez antes de todos los tests

**Checked fixtures** (rápidos, por cada test):
- Limpiar archivos creados por test anterior
- Garantiza que cada test comienza con FS limpio

**Ventajas:**
- ✅ Ejecución rápida: ~5 segundos para todos los tests funcionales
- ✅ Tests atómicos: cada test es independiente
- ✅ Aislamiento: fallos no afectan otros tests
- ✅ Debuggeable: se pueden correr tests individuales

## Crear y Montar una Imagen de Prueba

### Linux

```bash
# Crear una imagen vacía de 35MB (FAT32 requiere mínimo ~33MB)
dd if=/dev/zero of=file.img count=35 bs=1M

# Formatear como FAT32
mkfs.vfat -F 32 -v ./file.img

# Montar con el driver nativo (opcional, para agregar archivos)
mkdir native_mnt
sudo mount -t vfat file.img native_mnt/ -o umask=000

# Agregar contenido de prueba
echo "Hello, this is a file" > native_mnt/test_file
mkdir native_mnt/testdir

# Desmontar
sudo umount native_mnt/
rmdir native_mnt
```

### macOS

```bash
# Crear imagen vacía
dd if=/dev/zero of=file.img count=35 bs=1M

# Adjuntar sin montar
hdiutil attach -nomount file.img
# Anotar el nombre del dispositivo (ej: /dev/disk4)

# Formatear como FAT32
newfs_msdos -F 32 -v TESTFAT /dev/disk4

# Desadjuntar
hdiutil detach /dev/disk4

# Para agregar archivos (opcional)
hdiutil attach file.img
echo "Hello, this is a file" > /Volumes/TESTFAT/test_file
hdiutil detach /Volumes/TESTFAT
```

## Arquitectura y Módulos

### fat_fuse.c
Contiene la función `main` y se encarga de llamar a FUSE y montar el volumen.

### fat_volume.c
Contiene las funciones que permiten montar el sistema de archivos. Esta operación involucra:
- Abrir el archivo de imagen con los permisos correspondientes
- Leer los componentes del sistema de archivos FAT:
  - Parámetros del BIOS (BIOS Parameter Block)
  - Sector de arranque (boot sector)
  - Múltiples copias de la FAT (File Allocation Table)
- Inicializar la estructura de datos `fat_tree` que representa el árbol de directorios con el directorio raíz

Esta información se almacena en la estructura de datos `fat_volume`.

### fat_fuse_ops.c
Contiene las operaciones que implementan el comportamiento de las distintas llamadas al sistema relacionadas con el sistema de archivos. Estas son las funciones que FUSE invoca cuando el usuario interactúa con el filesystem montado.

Operaciones principales:
- `fat_fuse_readdir()` - Listar contenido de directorios
- `fat_fuse_read()` - Leer archivos
- `fat_fuse_write()` - Escribir archivos
- `fat_fuse_mkdir()` - Crear directorios
- `fat_fuse_mknod()` - Crear archivos
- `fat_fuse_unlink()` - Eliminar archivos
- `fat_fuse_rmdir()` - Eliminar directorios

### fat_fs_tree.c
Define el TAD `fat_tree` que abstrae el árbol de directorios. Internamente, `fat_tree` utiliza un `h_tree` cuyos datos son de tipo `fat_file`. `fat_tree` hace de interfaz entre el `h_tree` (que es un árbol genérico) y `fat_fuse_ops`, manejando las funciones particulares de `fat_file`.

Esta estructura es una combinación de árbol de búsqueda binario y listas enlazadas. Esto permite:
- Búsquedas por nombre de archivo en tiempo O(log n)
- Recorrido eficiente de los archivos de un directorio
- Acceso al path completo de un archivo (directorios "ancestros")

### fat_file.c
Define el TAD `fat_file` que abstrae la información y las funciones necesarias para manipular archivos y directorios. Tiene una copia de la entrada de directorio (directory entry) leída del cluster de datos de su directorio padre.

Funciones principales:
- `fat_file_pread()` - Lectura de bajo nivel desde clusters
- `fat_file_pwrite()` - Escritura de bajo nivel a clusters
- `fat_file_read_children()` - Cargar hijos de un directorio
- `fat_file_init()` - Inicializar nuevos archivos
- Documentación completa en formato Doxygen en `fat_file.h`

### fat_table.c
Define el TAD `fat_table`, que abstrae la lógica de las cadenas de clusters y las operaciones de escritura/lectura de la tabla FAT.

Funciones principales:
- `fat_table_read_cluster_value()` - Leer valor de un cluster en la FAT
- `fat_table_set_next_cluster()` - Establecer siguiente cluster en una cadena
- `fat_table_get_next_free_cluster()` - Buscar cluster libre
- `fat_table_seek_cluster()` - Navegar cadena de clusters
- Documentación completa en formato Doxygen en `fat_table.h`

### hierarchy_tree.c
Implementa un árbol genérico (`h_tree`) que es una estructura híbrida:
- Árbol binario de búsqueda (BST) para búsquedas eficientes
- Listas enlazadas para mantener relaciones padre-hijo

Utilizado por `fat_fs_tree.c` para construir el árbol de directorios en memoria.

### fat_filename_util.c
Funciones auxiliares para convertir entre nombres de archivo estándar y el formato 8.3 de FAT (8 caracteres de nombre base + 3 de extensión).

### fat_util.c
Funciones generales de utilidad para manejo de timestamps, conversiones de endianness, etc.

## Debugging

### Ejecutar en Foreground

FUSE puede correrse tanto en background como en foreground. Si está en background, al llamarse a la función `fuse_main` en `fat_fuse.c`, se pasa el control de ejecución a la librería y se redirigen `stderr` y `stdout`. Al llamar a `fusermount` (Linux) o `umount` (macOS), el control retorna a la aplicación (que ya está corriendo como daemon) y se desmonta el sistema.

Para facilitar el debugging, si llamamos a `fuse_main` con la opción `-f`, podemos correr FUSE en foreground y ver los prints que realizamos desde el código:

```bash
./fat-fuse -f path/to/image.img ./mnt
```

### Mensajes DEBUG

El sistema cuenta con un macro `DEBUG` que puede utilizarse para imprimir mensajes opcionales. Para desactivar estos mensajes, compilar con el flag `-DNDEBUG`:

```bash
make clean
CFLAGS="-DNDEBUG" make
```

### Debugging Avanzado con FUSE

Para un nivel mayor de debugging, se puede correr el código con `-d` y mostrará los mensajes provenientes de FUSE:

```bash
./fat-fuse -f -d path/to/image.img ./mnt
```

### Funciones de Debugging

El código incluye varias funciones útiles para debugging:
- `fat_table_print()` - Imprimir estado de la tabla FAT
- `fat_tree_print_preorder()` - Imprimir árbol de directorios en preorden
- `fat_file_print_dentry()` - Imprimir detalles de una directory entry

## Estilo de Código

El proyecto usa `clang-format` para mantener un estilo de código consistente.

Después de agregar cambios, antes de hacer un commit:

```bash
git-clang-format *.c *.h
```

Luego re-agregar los archivos que hayan sido modificados. Con `git diff` se pueden ver los cambios realizados por clang-format.

## Problemas Conocidos y Limitaciones

### Bugs Conocidos

* **Timestamps incorrectos**: La hora de los directorios y archivos creados puede mostrar valores incorrectos debido a manejo de zonas horarias (UTC vs local).

* **Clusters de directorio llenos**: Al crear archivos, si el cluster de datos del directorio se llena de entradas, se devuelve un error. Falta implementar la lógica para buscar un nuevo cluster de datos automáticamente.

* **Nombres largos**: Hay un error al intentar crear archivos o directorios con más de 8 caracteres. El directorio/archivo es creado con el nombre truncado a 8 caracteres. Luego FUSE trata de buscar el nuevo directorio/archivo con el nombre original y devuelve un error, ya que no existe.

* **Tipos de archivo limitados**: No hay soporte para otros tipos de archivos que no sean archivos regulares o directorios (no hay soporte para symlinks, devices, etc.).

* **Detección de ciclos**: No se controla que no existan ciclos de subdirectorios, ni que los paths no tengan más de 4096 caracteres (puede causar segmentation fault).

### Notas Técnicas de Implementación

* La tabla FAT se mapea en memoria con `mmap()`. Otros datos del filesystem, como datos de clusters, se leen con `pread()`.

* Cuando se necesita un hijo de un directorio, todos los hijos se leen al mismo tiempo y se insertan en un árbol binario de búsqueda ordenado por nombre (incluyendo extensión, si está presente).

* El código del filesystem **no es thread-safe**, por lo que las operaciones concurrentes del filesystem serán serializadas (FUSE se ejecuta con flag `-s`).

* El código del filesystem no intenta detectar estructuras de directorios cíclicas (que son posibles en FAT). Simplemente hará recursión indefinida si se encuentra un ciclo.

## Referencias

* [Diseño del Sistema de Archivos FAT](https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system)
* [Documentación de FUSE](https://libfuse.github.io/doxygen/)
* [Repositorio original fat-fuse](https://github.com/ebiggers/fat-fuse)
