#!/bin/bash
# Script de pruebas para el Servidor CSV RESTful
# IC6600 - Principios de Sistemas Operativos

HOST="localhost"
PORT="8080"
BASE_URL="http://$HOST:$PORT"

echo "=========================================="
echo "  Pruebas del Servidor CSV RESTful"
echo "=========================================="
echo ""

# Esperar a que el servidor este listo
sleep 1

echo "1. GET /items - Obtener todos los registros"
echo "-------------------------------------------"
curl -s -X GET "$BASE_URL/items" | head -20
echo ""
echo ""

echo "2. GET /items/1 - Obtener registro con ID=1"
echo "--------------------------------------------"
curl -s -X GET "$BASE_URL/items/1"
echo ""
echo ""

echo "3. GET /items/999 - Obtener registro inexistente"
echo "-------------------------------------------------"
curl -s -X GET "$BASE_URL/items/999"
echo ""
echo ""

echo "4. POST /items - Crear nuevo registro"
echo "--------------------------------------"
curl -s -X POST "$BASE_URL/items" \
     -H "Content-Type: text/plain" \
     -d "Luis,Hernandez,40,Puntarenas"
echo ""
echo ""

echo "5. GET /items - Verificar nuevo registro"
echo "-----------------------------------------"
curl -s -X GET "$BASE_URL/items" | head -20
echo ""
echo ""

echo "6. PUT /items/1 - Actualizar registro ID=1"
echo "-------------------------------------------"
curl -s -X PUT "$BASE_URL/items/1" \
     -H "Content-Type: text/plain" \
     -d "Juan,Perez,26,San Jose ACTUALIZADO"
echo ""
echo ""

echo "7. GET /items/1 - Verificar actualizacion"
echo "------------------------------------------"
curl -s -X GET "$BASE_URL/items/1"
echo ""
echo ""

echo "8. DELETE /items/2 - Eliminar registro ID=2"
echo "--------------------------------------------"
curl -s -X DELETE "$BASE_URL/items/2"
echo ""
echo ""

echo "9. GET /items - Verificar eliminacion"
echo "--------------------------------------"
curl -s -X GET "$BASE_URL/items" | head -20
echo ""
echo ""

echo "10. GET /items/2 - Intentar obtener registro eliminado"
echo "-------------------------------------------------------"
curl -s -X GET "$BASE_URL/items/2"
echo ""
echo ""

echo "=========================================="
echo "  Pruebas completadas"
echo "=========================================="



