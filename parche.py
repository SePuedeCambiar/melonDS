#!/usr/bin/env python3
"""
Parche para completar la extracción de GPU3D_IO.cpp
Ejecutar después de que el script maestro falló en la fase IO
"""

import os
import re

def main():
    print("=" * 60)
    print("PARCHE: Completando GPU3D_IO.cpp")
    print("=" * 60)
    
    # Leer el GPU3D.cpp actual (ya tiene las otras secciones extraídas)
    print("\n[1/4] Leyendo src/GPU3D.cpp...")
    with open("src/GPU3D.cpp", "r", encoding="utf-8") as f:
        contenido = f.read()
    
    # Buscar desde WriteToGXFIFO hasta el final del archivo
    print("[2/4] Extrayendo bloque IO...")
    patron = r"void GPU3D::WriteToGXFIFO\(u32 val\) noexcept.*"
    match = re.search(patron, contenido, re.DOTALL)
    
    if not match:
        print("  ❌ ERROR: No se encontró WriteToGXFIFO")
        return
    
    # Extraer todo desde WriteToGXFIFO hasta el final
    start_pos = match.start()
    io_code = contenido[start_pos:]
    
    # Eliminar el cierre del namespace al final
    io_code = re.sub(r"\n\}\s*$", "", io_code).strip()
    
    print(f"  ✅ Bloque IO extraído ({len(io_code)} bytes)")
    
    # Crear GPU3D_IO.cpp
    print("[3/4] Creando src/gpu3d/GPU3D_IO.cpp...")
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

extern const u8 CmdNumParams[256];

{io_code}

}} // namespace melonDS
"""
    
    with open("src/gpu3d/GPU3D_IO.cpp", "w", encoding="utf-8") as f:
        f.write(plantilla)
    
    print("  ✅ GPU3D_IO.cpp creado")
    
    # Limpiar GPU3D.cpp
    print("[4/4] Limpiando GPU3D.cpp...")
    contenido_limpio = contenido[:start_pos]
    
    # Asegurar que termine correctamente
    contenido_limpio = contenido_limpio.rstrip() + "\n\n} // namespace melonDS\n"
    
    with open("src/GPU3D.cpp", "w", encoding="utf-8") as f:
        f.write(contenido_limpio)
    
    print("  ✅ GPU3D.cpp limpiado")
    
    # Actualizar CMakeLists.txt
    print("\n[EXTRA] Actualizando CMakeLists.txt...")
    with open("src/CMakeLists.txt", "r", encoding="utf-8") as f:
        cmake = f.read()
    
    if "gpu3d/GPU3D_IO.cpp" not in cmake:
        cmake = cmake.replace(
            "    gpu3d/GPU3D_Geometry.cpp",
            "    gpu3d/GPU3D_IO.cpp\n    gpu3d/GPU3D_Geometry.cpp"
        )
        with open("src/CMakeLists.txt", "w", encoding="utf-8") as f:
            f.write(cmake)
        print("  ✅ CMakeLists.txt actualizado")
    else:
        print("  ℹ️  CMakeLists.txt ya estaba actualizado")
    
    print("\n" + "=" * 60)
    print("✅ PARCHE COMPLETADO")
    print("=" * 60)
    print("\nAhora puedes compilar:")
    print("  cd build")
    print("  cmake ..")
    print("  make -j$(nproc)")
    print("=" * 60)

if __name__ == "__main__":
    main()
