#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "../NDS.h"
#include "../GPU.h"
#include "../FIFO.h"
#include "../GPU3D_Soft.h"
#include "../Platform.h"
#include "../GPU3D.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

extern const u8 CmdNumParams[256];

void GPU3D::WriteToGXFIFO(u32 val) noexcept
{
    if (NumCommands == 0)
    {
        NumCommands = 4;
        CurCommand = val;
        ParamCount = 0;
        TotalParams = CmdNumParams[CurCommand & 0xFF];

        if (TotalParams > 0) return;
    }
    else
        ParamCount++;

    for (;;)
    {
        if ((CurCommand & 0xFF) || (NumCommands == 4 && CurCommand == 0))
        {
            CmdFIFOEntry entry;
            entry.Command = CurCommand & 0xFF;
            entry.Param = val;
            CmdFIFOWrite(entry);
        }

        if (ParamCount >= TotalParams)
        {
            CurCommand >>= 8;
            NumCommands--;
            if (NumCommands == 0) break;

            ParamCount = 0;
            TotalParams = CmdNumParams[CurCommand & 0xFF];
        }
        if (ParamCount < TotalParams)
            break;
    }
}


u8 GPU3D::Read8(u32 addr) noexcept
{
    switch (addr)
    {
    case 0x04000600:
        Run();
        return GXStat & 0xFF;
    case 0x04000601:
        {
            Run();
            return ((GXStat >> 8) & 0xFF) |
                   (PosMatrixStackPointer & 0x1F) |
                   ((ProjMatrixStackPointer & 0x1) << 5);
        }
    case 0x04000602:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return fifolevel & 0xFF;
        }
    case 0x04000603:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return ((GXStat >> 24) & 0xFF) |
                   (fifolevel >> 8) |
                   (fifolevel < 128 ? (1<<1) : 0) |
                   (fifolevel == 0  ? (1<<2) : 0);
        }
    }

    Log(LogLevel::Debug, "unknown GPU3D read8 %08X\n", addr);
    return 0;
}

u16 GPU3D::Read16(u32 addr) noexcept
{
    switch (addr)
    {
    case 0x04000060:
        return DispCnt;

    case 0x04000320:
        return 46; // TODO, eventually

    case 0x04000600:
        {
            Run();

            return (GXStat & 0xFFFF) |
                   ((PosMatrixStackPointer & 0x1F) << 8) |
                   ((ProjMatrixStackPointer & 0x1) << 13);
        }
    case 0x04000602:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return (GXStat >> 16) |
                   fifolevel |
                   (fifolevel < 128 ? (1<<9) : 0) |
                   (fifolevel == 0  ? (1<<10) : 0);
        }

    case 0x04000604:
        return NumPolygons;
    case 0x04000606:
        return NumVertices;

    case 0x04000630: return VecTestResult[0];
    case 0x04000632: return VecTestResult[1];
    case 0x04000634: return VecTestResult[2];
    }

    Log(LogLevel::Debug, "unknown GPU3D read16 %08X\n", addr);
    return 0;
}

u32 GPU3D::Read32(u32 addr) noexcept
{
    switch (addr)
    {
    case 0x04000060:
        return DispCnt;

    case 0x04000320:
        return 46; // TODO, eventually

    case 0x04000600:
        {
            Run();

            u32 fifolevel = CmdFIFO.Level();

            return GXStat |
                   ((PosMatrixStackPointer & 0x1F) << 8) |
                   ((ProjMatrixStackPointer & 0x1) << 13) |
                   (fifolevel << 16) |
                   (fifolevel < 128 ? (1<<25) : 0) |
                   (fifolevel == 0  ? (1<<26) : 0);
        }

    case 0x04000604:
        return NumPolygons | (NumVertices << 16);

    case 0x04000620: return PosTestResult[0];
    case 0x04000624: return PosTestResult[1];
    case 0x04000628: return PosTestResult[2];
    case 0x0400062C: return PosTestResult[3];

    case 0x04000680: return VecMatrix[0];
    case 0x04000684: return VecMatrix[1];
    case 0x04000688: return VecMatrix[2];
    case 0x0400068C: return VecMatrix[4];
    case 0x04000690: return VecMatrix[5];
    case 0x04000694: return VecMatrix[6];
    case 0x04000698: return VecMatrix[8];
    case 0x0400069C: return VecMatrix[9];
    case 0x040006A0: return VecMatrix[10];
    }

    if (addr >= 0x04000640 && addr < 0x04000680)
    {
        UpdateClipMatrix();
        return ClipMatrix[(addr & 0x3C) >> 2];
    }

    //printf("unknown GPU3D read32 %08X\n", addr);
    return 0;
}

void GPU3D::Write8(u32 addr, u8 val) noexcept
{
    if (!RenderingEnabled && addr >= 0x04000320 && addr < 0x04000400) return;
    if (!GeometryEnabled  && addr >= 0x04000400 && addr < 0x04000700) return;

    switch (addr)
    {
    case 0x04000340:
        AlphaRefVal = val & 0x1F;
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000601:
        if (val & 0x80)
        {
            GXStat &= ~0x8000;
            ProjMatrixStackPointer = 0;
            //PosMatrixStackPointer = 0;
            TexMatrixStackPointer = 0; // CHECKME
        }
        return;
    case 0x04000603:
        val &= 0xC0;
        GXStat &= 0x3FFFFFFF;
        GXStat |= (val << 24);
        CheckFIFOIRQ();
        return;
    }

    if (addr >= 0x04000330 && addr < 0x04000340)
    {
        ((u8*)EdgeTable)[addr - 0x04000330] = val;
        return;
    }

    if (addr >= 0x04000360 && addr < 0x04000380)
    {
        FogDensityTable[addr - 0x04000360] = val & 0x7F;
        return;
    }

    if (addr >= 0x04000380 && addr < 0x040003C0)
    {
        ((u8*)ToonTable)[addr - 0x04000380] = val;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU3D write8 %08X %02X\n", addr, val);
}

void GPU3D::Write16(u32 addr, u16 val) noexcept
{
    if (!RenderingEnabled && addr >= 0x04000320 && addr < 0x04000400) return;
    if (!GeometryEnabled  && addr >= 0x04000400 && addr < 0x04000700) return;

    switch (addr)
    {
    case 0x04000060:
        DispCnt = (val & 0x4FFF) | (DispCnt & 0x3000);
        if (val & (1<<12)) DispCnt &= ~(1<<12);
        if (val & (1<<13)) DispCnt &= ~(1<<13);
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000340:
        AlphaRefVal = val & 0x1F;
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000350:
        ClearAttr1 = (ClearAttr1 & 0xFFFF0000) | val;
        return;
    case 0x04000352:
        ClearAttr1 = (ClearAttr1 & 0xFFFF) | (val << 16);
        return;
    case 0x04000354:
        ClearAttr2 = (ClearAttr2 & 0xFFFF0000) | val;
        return;
    case 0x04000356:
        ClearAttr2 = (ClearAttr2 & 0xFFFF) | (val << 16);
        return;

    case 0x04000358:
        FogColor = (FogColor & 0xFFFF0000) | val;
        return;
    case 0x0400035A:
        FogColor = (FogColor & 0xFFFF) | (val << 16);
        return;
    case 0x0400035C:
        FogOffset = val & 0x7FFF;
        return;

    case 0x04000600:
        if (val & 0x8000)
        {
            GXStat &= ~0x8000;
            ProjMatrixStackPointer = 0;
            //PosMatrixStackPointer = 0;
            TexMatrixStackPointer = 0; // CHECKME
        }
        return;
    case 0x04000602:
        val &= 0xC000;
        GXStat &= 0x3FFFFFFF;
        GXStat |= (val << 16);
        CheckFIFOIRQ();
        return;

    case 0x04000610:
        val &= 0x7FFF;
        ZeroDotWLimit = (val * 0x200) + 0x1FF;
        return;
    }

    if (addr >= 0x04000330 && addr < 0x04000340)
    {
        EdgeTable[(addr - 0x04000330) >> 1] = val;
        return;
    }

    if (addr >= 0x04000360 && addr < 0x04000380)
    {
        addr -= 0x04000360;
        FogDensityTable[addr] = val & 0x7F;
        FogDensityTable[addr+1] = (val >> 8) & 0x7F;
        return;
    }

    if (addr >= 0x04000380 && addr < 0x040003C0)
    {
        ToonTable[(addr - 0x04000380) >> 1] = val;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU3D write16 %08X %04X\n", addr, val);
}

void GPU3D::Write32(u32 addr, u32 val) noexcept
{
    if (!RenderingEnabled && addr >= 0x04000320 && addr < 0x04000400) return;
    if (!GeometryEnabled  && addr >= 0x04000400 && addr < 0x04000700) return;

    switch (addr)
    {
    case 0x04000060:
        DispCnt = (val & 0x4FFF) | (DispCnt & 0x3000);
        if (val & (1<<12)) DispCnt &= ~(1<<12);
        if (val & (1<<13)) DispCnt &= ~(1<<13);
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000340:
        AlphaRefVal = val & 0x1F;
        AlphaRef = (DispCnt & (1<<2)) ? AlphaRefVal : 0;
        return;

    case 0x04000350:
        ClearAttr1 = val;
        return;
    case 0x04000354:
        ClearAttr2 = val;
        return;

    case 0x04000358:
        FogColor = val;
        return;
    case 0x0400035C:
        FogOffset = val & 0x7FFF;
        return;

    case 0x04000600:
        if (val & 0x8000)
        {
            GXStat &= ~0x8000;
            ProjMatrixStackPointer = 0;
            //PosMatrixStackPointer = 0;
            TexMatrixStackPointer = 0; // CHECKME
        }
        val &= 0xC0000000;
        GXStat &= 0x3FFFFFFF;
        GXStat |= val;
        CheckFIFOIRQ();
        return;

    case 0x04000610:
        val &= 0x7FFF;
        ZeroDotWLimit = (val * 0x200) + 0x1FF;
        return;
    }

    if (addr >= 0x04000400 && addr < 0x04000440)
    {
        WriteToGXFIFO(val);
        return;
    }

    if (addr >= 0x04000440 && addr < 0x040005CC)
    {
        CmdFIFOEntry entry;
        entry.Command = (addr & 0x1FC) >> 2;
        entry.Param = val;
        CmdFIFOWrite(entry);
        return;
    }

    if (addr >= 0x04000330 && addr < 0x04000340)
    {
        addr = (addr - 0x04000330) >> 1;
        EdgeTable[addr] = val & 0xFFFF;
        EdgeTable[addr+1] = val >> 16;
        return;
    }

    if (addr >= 0x04000360 && addr < 0x04000380)
    {
        addr -= 0x04000360;
        FogDensityTable[addr] = val & 0x7F;
        FogDensityTable[addr+1] = (val >> 8) & 0x7F;
        FogDensityTable[addr+2] = (val >> 16) & 0x7F;
        FogDensityTable[addr+3] = (val >> 24) & 0x7F;
        return;
    }

    if (addr >= 0x04000380 && addr < 0x040003C0)
    {
        addr = (addr - 0x04000380) >> 1;
        ToonTable[addr] = val & 0xFFFF;
        ToonTable[addr+1] = val >> 16;
        return;
    }

    Log(LogLevel::Debug, "unknown GPU3D write32 %08X %08X\n", addr, val);
}


} // namespace melonDS