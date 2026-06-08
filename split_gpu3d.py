#!/usr/bin/env python3
"""
Script maestro para dividir GPU3D.cpp en 5 archivos modulares.
Ejecutar desde la raíz del proyecto: python3 dividir_gpu3d.py
"""

import os
import re

def main():
    print("=" * 60)
    print("DIVISOR DE GPU3D.cpp - Script Maestro")
    print("=" * 60)
    
    # Paso 1: Restaurar archivos originales
    print("\n[1/6] Restaurando archivos originales desde Git...")
    os.system("git checkout origin/master -- src/GPU3D.cpp src/CMakeLists.txt")
    
    # Paso 2: Leer el archivo original
    print("[2/6] Leyendo src/GPU3D.cpp...")
    with open("src/GPU3D.cpp", "r", encoding="utf-8") as f:
        contenido_original = f.read()
    
    # Paso 3: Crear directorio gpu3d
    os.makedirs("src/gpu3d", exist_ok=True)
    
    # Paso 4: Definir las secciones que vamos a extraer
    # Cada sección tiene: nombre del archivo, patrón de inicio, patrón de fin
    
    secciones = {
        "GPU3D_Math.cpp": {
            "inicio": r"void MatrixLoadIdentity\(s32\* m\)\n\{",
            "fin": r"void GPU3D::UpdateClipMatrix\(\) noexcept\n\{",
            "extra_al_final": "UpdateClipMatrix"  # Incluir esta función también
        },
        "GPU3D_Geometry.cpp": {
            "inicio": r"template<int comp, s32 plane, bool attribs>\nvoid ClipSegment",
            "fin": r"void GPU3D::CmdFIFOWrite\(const CmdFIFOEntry& entry\) noexcept"
        },
        "GPU3D_Cmd.cpp": {
            "inicio": r"void GPU3D::CmdFIFOWrite\(const CmdFIFOEntry& entry\) noexcept",
            "fin": r"s32 GPU3D::CyclesToRunFor\(\) const noexcept"
        },
        "GPU3D_IO.cpp": {
            "inicio": r"void GPU3D::WriteToGXFIFO\(u32 val\) noexcept",
            "fin": r"\} // namespace melonDS"
        }
    }
    
    # Paso 5: Extraer cada sección
    archivos_creados = []
    contenido_restante = contenido_original
    
    for nombre_archivo, config in secciones.items():
        print(f"\n[3/6] Extrayendo {nombre_archivo}...")
        
        # Buscar el bloque de código
        patron = config["inicio"] + r"(.*?)(?=" + config["fin"] + r")"
        match = re.search(patron, contenido_original, re.DOTALL)
        
        if not match:
            print(f"  ❌ ERROR: No se encontró el bloque para {nombre_archivo}")
            return
        
        codigo_extraido = match.group(0)
        
        # Si necesita UpdateClipMatrix, extraerlo también
        if config.get("extra_al_final") == "UpdateClipMatrix":
            match_ucm = re.search(
                r"void GPU3D::UpdateClipMatrix\(\) noexcept\n\{.*?\n\}",
                contenido_original,
                re.DOTALL
            )
            if match_ucm:
                codigo_extraido += "\n\n" + match_ucm.group(0)
        
        # Crear el archivo
        if nombre_archivo == "GPU3D_Cmd.cpp":
            # Cmd necesita la tabla CmdNumParams
            match_cmdnum = re.search(
                r"const u8 CmdNumParams\[256\] =\n\{.*?\n\};",
                contenido_original,
                re.DOTALL
            )
            if match_cmdnum:
                tabla_cmd = match_cmdnum.group(0)
                # Convertir a extern en el archivo principal
                contenido_restante = contenido_restante.replace(
                    tabla_cmd,
                    "extern const u8 CmdNumParams[256];"
                )
                # Añadir la tabla al archivo Cmd
                codigo_extraido = tabla_cmd + "\n\n" + codigo_extraido
        
        # Escribir el archivo
        plantilla = f"""#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "NDS.h"
#include "GPU.h"
#include "FIFO.h"
#include "GPU3D_Soft.h"
#include "Platform.h"
#include "GPU3D.h"

namespace melonDS
{{
using Platform::Log;
using Platform::LogLevel;

{codigo_extraido}

}} // namespace melonDS
"""
        
        with open(f"src/gpu3d/{nombre_archivo}", "w", encoding="utf-8") as f:
            f.write(plantilla)
        
        archivos_creados.append(nombre_archivo)
        print(f"  ✅ {nombre_archivo} creado ({len(codigo_extraido)} bytes)")
        
        # Eliminar el código extraído del contenido restante
        contenido_restante = contenido_restante.replace(codigo_extraido, "")
    
    # Paso 6: Limpiar el archivo GPU3D.cpp restante
    print("\n[4/6] Limpiando GPU3D.cpp central...")
    
    # Eliminar la tabla CmdNumParams si aún está
    contenido_restante = re.sub(
        r"const u8 CmdNumParams\[256\] =\n\{.*?\n\};\n*",
        "extern const u8 CmdNumParams[256];\n",
        contenido_restante,
        flags=re.DOTALL
    )
    
    # Asegurar que tenga los includes y namespace correctos
    if "extern const u8 CmdNumParams[256];" not in contenido_restante:
        # Insertar después del namespace
        contenido_restante = re.sub(
            r"(namespace melonDS\n\{)",
            r"\1\nusing Platform::Log;\nusing Platform::LogLevel;\n\nextern const u8 CmdNumParams[256];\n",
            contenido_restante
        )
    
    # Añadir prototipos de funciones matemáticas
    prototipos_math = """
void MatrixLoadIdentity(s32* m);
void MatrixLoad4x4(s32* m, s32* s);
void MatrixLoad4x3(s32* m, s32* s);
void MatrixMult4x4(s32* m, s32* s);
void MatrixMult4x3(s32* m, s32* s);
void MatrixMult3x3(s32* m, s32* s);
void MatrixScale(s32* m, s32* s);
void MatrixTranslate(s32* m, s32* s);
"""
    
    # Insertar prototipos antes de la primera función que los use
    if "void MatrixLoadIdentity(s32* m);" not in contenido_restante:
        contenido_restante = re.sub(
            r"(namespace melonDS\n\{.*?\n)",
            r"\1" + prototipos_math,
            contenido_restante,
            count=1,
            flags=re.DOTALL
        )
    
    # Escribir el GPU3D.cpp limpio
    with open("src/GPU3D.cpp", "w", encoding="utf-8") as f:
        f.write(contenido_restante)
    
    print("  ✅ GPU3D.cpp central limpiado")
    
    # Paso 7: Actualizar CMakeLists.txt
    print("\n[5/6] Actualizando CMakeLists.txt...")
    with open("src/CMakeLists.txt", "r", encoding="utf-8") as f:
        cmake = f.read()
    
    # Añadir los nuevos archivos
    archivos_cmake = "\n".join([f"    gpu3d/{arch}" for arch in archivos_creados])
    
    if "gpu3d/GPU3D_Math.cpp" not in cmake:
        cmake = cmake.replace(
            "    GPU3D.cpp",
            f"    GPU3D.cpp\n{archivos_cmake}"
        )
    
    # Cambiar INTERFACE a PUBLIC para que los headers sean accesibles
    cmake = cmake.replace(
        'INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"',
        'PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"'
    )
    
    with open("src/CMakeLists.txt", "w", encoding="utf-8") as f:
        f.write(cmake)
    
    print("  ✅ CMakeLists.txt actualizado")
    
    # Paso 8: Resumen
    print("\n" + "=" * 60)
    print("✅ DIVISIÓN COMPLETADA EXITOSAMENTE")
    print("=" * 60)
    print("\nArchivos creados:")
    for arch in archivos_creados:
        print(f"  - src/gpu3d/{arch}")
    print("\nArchivo modificado:")
    print("  - src/GPU3D.cpp (versión central limpia)")
    print("\nPróximos pasos:")
    print("  cd build")
    print("  cmake ..")
    print("  make -j$(nproc)")
    print("=" * 60)

if __name__ == "__main__":
    main()
