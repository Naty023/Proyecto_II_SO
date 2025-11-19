# Proyecto_II_SO
## Cómo compilar en Unix / WSL

En una terminal Unix (Linux, macOS o WSL):

gcc -Wall -Wextra -std=c11 buscadup.c -o buscadup

## 3. Pruebas rápidas (para que veas que funciona)

En una terminal:

# 1. Crear un directorio de pruebas
mkdir -p pruebas_dup/subdir

# 2. Crear archivos y duplicados
echo "hola mundo" > pruebas_dup/a.txt
cp pruebas_dup/a.txt pruebas_dup/subdir/b.txt

echo "otro archivo" > pruebas_dup/c.txt
cp pruebas_dup/c.txt pruebas_dup/copia_c.txt

# 3. Ejecutar SOLO listado de duplicados
./buscadup pruebas_dup

# 4. Ejecutar listado + borrado de duplicados
./buscadup pruebas_dup -d


En la primera corrida deberías ver mensajes tipo:

[Duplicado encontrado]

Original : ...

Duplicado: ...

Accion : Solo se reporta, no se borra.

En la segunda corrida, los duplicados deberían borrarse (verás fue BORRADO).

4. Cómo probarlo “desde Visual”

Si usas Visual Studio Code con WSL o Linux:

Abre la carpeta donde está buscadup.c.

Abre una terminal integrada (Vista → Terminal).

Compila con:

gcc -Wall -Wextra -std=c11 buscadup.c -o buscadup


En la misma terminal, corre las pruebas del punto 3:

./buscadup pruebas_dup
./buscadup pruebas_dup -d
